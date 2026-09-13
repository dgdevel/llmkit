#include "tools.h"
#include "htmlmd.h"
#include "jsonrpc.h"
#include "platform.h"
#include "srv.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <cJSON.h>

#define TOOLS_SERVER_NAME "llmkit-tools"

/* Cap on a single HTTP download (DoS guard for online_fetch/online_search). */
#define TOOLS_MAX_DOWNLOAD (32 * 1024 * 1024)

/* Timeout for outbound HTTP requests of both built-in tools. */
#define TOOLS_HTTP_TIMEOUT_MS 30000

/* Returned markdown longer than this is truncated (with a note) so a big
 * page cannot flood the LLM context. */
#define TOOLS_FETCH_MAX_CHARS 100000

/* DuckDuckGo's free, unauthenticated HTML search endpoint. */
#define TOOLS_DDG_URL "https://html.duckduckgo.com/html/?q="

/* file_scan result limits: at most 20 records are returned (a "<N> more
 * files matching" line is appended beyond that) and at most 50 matching
 * line numbers are listed per record (a trailing "+" marks more). */
#define TOOLS_SCAN_MAX_RECORDS      20
#define TOOLS_SCAN_MAX_LISTED_LINES 50

/* Bytes sniffed at the start of a file to tell text from binary (a NUL
 * byte means binary), and the largest file that is fully read for line
 * counting / content matching (larger files are listed without lines). */
#define TOOLS_SCAN_SNIFF_BYTES    8192
#define TOOLS_SCAN_MAX_TEXT_BYTES (32 * 1024 * 1024)

/* Longest relative path collected by the file_scan walker. */
#define TOOLS_SCAN_PATH_MAX 4096

/* A browser User-Agent: the DDG HTML endpoint (and many sites) reject
 * requests without one. */
#define TOOLS_USER_AGENT                                                      \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/124.0.0.0 Safari/537.36"

const char *const TOOLS_BUILTIN_NAMES[] = {
    "online_search", "online_fetch", "file_scan",   "exec",      "exec_status",
    "sleep",         "file_read",    "file_create", "file_edit", NULL};

/* ------------------------------------------------------------------ */
/*  HTTP GET helper                                                    */
/* ------------------------------------------------------------------ */

static void ensure_curl_init(void) {
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = true;
    }
}

/* curl WRITEFUNCTION backed by a growbuf, capped at TOOLS_MAX_DOWNLOAD. */
static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    util_growbuf *gb = userdata;
    size_t n = size * nmemb;
    if (gb->len + n + 1 > TOOLS_MAX_DOWNLOAD) {
        return 0; /* non-zero expected: aborts the transfer */
    }
    util_growbuf_append(gb, ptr, n);
    return n;
}

/* Perform a GET request. On CURLE_OK fills *out_body (malloc'd, caller
 * frees), *out_status (HTTP code), *out_content_type (malloc'd copy of the
 * Content-Type header or NULL, caller frees) and *out_err (malloc'd
 * message). Returns EXIT_SUCCESS or EXIT_LLM_ERR on transport error. */
static int http_get(const char *url, char **out_body, long *out_status, char **out_content_type,
                    char **out_err) {
    *out_body = NULL;
    *out_err = NULL;

    ensure_curl_init();
    CURL *c = curl_easy_init();
    if (c == NULL) {
        *out_err = util_strdup("curl init failed");
        return EXIT_INTERNAL_ERR;
    }

    util_growbuf gb = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)TOOLS_HTTP_TIMEOUT_MS);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, TOOLS_USER_AGENT);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* transparent gzip/deflate */
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &gb);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        util_growbuf_free(&gb);
        size_t msg_len = strlen(curl_easy_strerror(rc)) + 32;
        *out_err = malloc(msg_len);
        if (*out_err != NULL) {
            snprintf(*out_err, msg_len, "%s", curl_easy_strerror(rc));
        }
        curl_easy_cleanup(c);
        return EXIT_LLM_ERR;
    }

    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, out_status);
    /* The Content-Type pointer is owned by the curl handle: copy it
     * before cleanup invalidates it. */
    *out_content_type = NULL;
    char *ct = NULL;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ct != NULL) *out_content_type = util_strdup(ct);
    curl_easy_cleanup(c);

    *out_body = util_growbuf_release(&gb);
    if (*out_body == NULL) *out_body = util_strdup("");
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Tool schemas                                                       */
/* ------------------------------------------------------------------ */

/* Build {"type":"object","properties":{...},"required":[...]} from
 * (name, type, description) triples plus the required property names. */
static char *build_tool_schema(const char *const *props, int prop_count,
                               const char *const *required, int required_count) {
    cJSON *schema = cJSON_CreateObject();
    if (schema == NULL) return NULL;
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *properties = cJSON_AddObjectToObject(schema, "properties");
    if (properties == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    for (int i = 0; i < prop_count; i += 3) {
        cJSON *p = cJSON_AddObjectToObject(properties, props[i]);
        if (p == NULL) continue;
        cJSON_AddStringToObject(p, "type", props[i + 1]);
        cJSON_AddStringToObject(p, "description", props[i + 2]);
    }

    if (required_count > 0) {
        cJSON *req = cJSON_AddArrayToObject(schema, "required");
        if (req != NULL) {
            for (int i = 0; i < required_count; i++) {
                cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
            }
        }
    }

    char *out = cJSON_PrintUnformatted(schema);
    cJSON_Delete(schema);
    return out;
}

/* Wrap a text payload as a successful tools/call result:
 * {"content":[{"type":"text","text":payload}],"isError":is_error}. */
static cJSON *build_tool_text_result(const char *payload, bool is_error) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    if (content == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", payload ? payload : "");
    cJSON_AddItemToArray(content, item);
    if (is_error) {
        cJSON_AddTrueToObject(result, "isError");
    } else {
        cJSON_AddFalseToObject(result, "isError");
    }
    return result;
}

/* Fetch an integer argument that may arrive as a JSON number or as a
 * numeric string (line numbers circulate as text once an LLM copies
 * them out of a file_read reply). Absent arguments return true with
 * *out left at its initial value; present-but-non-numeric arguments
 * return false. */
static bool tool_int_arg(const cJSON *args, const char *name, long *out) {
    cJSON *j = args ? cJSON_GetObjectItem(args, name) : NULL;
    if (j == NULL || cJSON_IsNull(j)) return true;
    if (cJSON_IsNumber(j)) {
        *out = (long)j->valuedouble;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring[0] != '\0') {
        char *end = NULL;
        long v = strtol(j->valuestring, &end, 10);
        if (end != j->valuestring && *end == '\0') {
            *out = v;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  online_search                                                      */
/* ------------------------------------------------------------------ */

#define SEARCH_DESC "Search the web"

/* Resolve a DuckDuckGo result href to the real target URL: unwraps the
 * //duckduckgo.com/l/?uddg=<encoded>&rut=... redirect and absolutizes
 * scheme-relative or root-relative links. Returns a malloc'd string. */
static char *unwrap_ddg_href(CURL *curl, const char *href) {
    if (href == NULL || href[0] == '\0') return util_strdup("");

    /* Redirect wrapper: extract and decode the uddg query parameter. */
    const char *lq = strstr(href, "/l/?");
    if (lq != NULL) {
        const char *query = lq + 4;
        const char *uddg = strstr(query, "uddg=");
        if (uddg != NULL) {
            const char *start = uddg + 5;
            const char *end = start + strcspn(start, "&");
            char *unescaped = curl_easy_unescape(curl, start, (int)(end - start), NULL);
            if (unescaped != NULL) {
                char *out = util_strdup(unescaped);
                curl_free(unescaped);
                return out;
            }
        }
    }
    if (strncmp(href, "//", 2) == 0) {
        char *out = malloc(strlen(href) + 9);
        if (out == NULL) return util_strdup("");
        snprintf(out, strlen(href) + 9, "https:%s", href);
        return out;
    }
    if (href[0] == '/') {
        char *out = malloc(strlen(href) + 32);
        if (out == NULL) return util_strdup("");
        snprintf(out, strlen(href) + 32, "https://duckduckgo.com%s", href);
        return out;
    }
    return util_strdup(href);
}

/* Render the parsed results in the user-visible list format. */
static char *format_ddg_results(const char *query, const htmlmd_match *anchors, int anchor_count,
                                const htmlmd_match *snippets, int snippet_count) {
    util_growbuf out = {0};
    if (anchor_count == 0) {
        util_growbuf_append_str(&out, "No results found for: ");
        util_growbuf_append_str(&out, query);
        return util_growbuf_release(&out);
    }
    for (int i = 0; i < anchor_count; i++) {
        const char *description = "";
        if (i < snippet_count && snippets[i].text != NULL) description = snippets[i].text;
        if (i > 0) util_growbuf_append_str(&out, "\n\n");
        util_growbuf_append_str(&out, "Title: ");
        util_growbuf_append_str(&out, anchors[i].text);
        util_growbuf_append_str(&out, "\nURL: ");
        if (anchors[i].attr != NULL) util_growbuf_append_str(&out, anchors[i].attr);
        util_growbuf_append_str(&out, "\nDescription: ");
        util_growbuf_append_str(&out, description);
    }
    return util_growbuf_release(&out);
}

/* Parse a DDG HTML results page and render the text list. Uses the
 * shared htmlmd parser; a curl handle is created for URL unescaping. */
char *tools_parse_search_results(const char *html, const char *query) {
    ensure_curl_init();
    CURL *curl = curl_easy_init();
    if (curl == NULL) return NULL;

    int anchor_count = 0;
    int snippet_count = 0;
    htmlmd_match *anchors = htmlmd_find_by_class(html, "result__a", "href", &anchor_count);
    htmlmd_match *snippets = htmlmd_find_by_class(html, "result__snippet", NULL, &snippet_count);

    /* Resolve redirect-wrapped hrefs in place. */
    for (int i = 0; i < anchor_count; i++) {
        char *real = unwrap_ddg_href(curl, anchors[i].attr);
        free(anchors[i].attr);
        anchors[i].attr = real;
    }
    curl_easy_cleanup(curl);

    char *text = format_ddg_results(query, anchors, anchor_count, snippets, snippet_count);
    htmlmd_matches_free(anchors, anchor_count);
    htmlmd_matches_free(snippets, snippet_count);
    return text;
}

static int tool_online_search(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *query_j = args ? cJSON_GetObjectItem(args, "query") : NULL;
    const char *query = (query_j && cJSON_IsString(query_j)) ? query_j->valuestring : "";
    if (query[0] == '\0') {
        *out_resp = srv_build_error(id_node, "online_search requires a string 'query' argument");
        return EXIT_SUCCESS;
    }

    /* Build the request URL with the percent-encoded query. */
    ensure_curl_init();
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        *out_resp = srv_build_error(id_node, "curl init failed");
        return EXIT_SUCCESS;
    }
    char *encoded = curl_easy_escape(curl, query, 0);
    size_t url_len = strlen(TOOLS_DDG_URL) + (encoded ? strlen(encoded) : 0) + 1;
    char *url = malloc(url_len);
    if (url == NULL || encoded == NULL) {
        curl_free(encoded);
        curl_easy_cleanup(curl);
        *out_resp = srv_build_error(id_node, "Out of memory");
        return EXIT_SUCCESS;
    }
    snprintf(url, url_len, "%s%s", TOOLS_DDG_URL, encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);

    char *body = NULL;
    long status = 0;
    char *content_type = NULL;
    char *err = NULL;
    int rc = http_get(url, &body, &status, &content_type, &err);
    free(content_type);
    free(url);
    if (rc != EXIT_SUCCESS) {
        size_t msg_len = strlen("Search failed: ") + (err ? strlen(err) : 0) + 1;
        char *msg = malloc(msg_len);
        if (msg != NULL) snprintf(msg, msg_len, "Search failed: %s", err ? err : "unknown error");
        free(err);
        cJSON *result = build_tool_text_result(msg, true);
        free(msg);
        if (result == NULL) return EXIT_INTERNAL_ERR;
        *out_resp = srv_build_response(id_node, result, NULL);
        return EXIT_SUCCESS;
    }

    char *text = tools_parse_search_results(body, query);
    free(body);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, "Out of memory");
        return EXIT_SUCCESS;
    }
    if (status != 200) {
        size_t msg_len = 64;
        char *msg = malloc(msg_len);
        if (msg != NULL) snprintf(msg, msg_len, "Search failed: HTTP Status %ld", status);
        free(text);
        cJSON *result = build_tool_text_result(msg ? msg : "Search failed", true);
        free(msg);
        if (result == NULL) return EXIT_INTERNAL_ERR;
        *out_resp = srv_build_response(id_node, result, NULL);
        return EXIT_SUCCESS;
    }

    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  online_fetch                                                       */
/* ------------------------------------------------------------------ */

#define FETCH_DESC "Fetch a web page"

static bool content_type_is_html(const char *ct) {
    if (ct == NULL) return true; /* assume HTML when the server says nothing */
    if (strstr(ct, "html") != NULL) return true;
    if (strstr(ct, "xml") != NULL) return true;
    return false;
}

static bool content_type_is_text(const char *ct) {
    if (ct == NULL) return true;
    if (strncmp(ct, "text/", 5) == 0) return true;
    if (strstr(ct, "json") != NULL) return true;
    if (strstr(ct, "javascript") != NULL) return true;
    if (strstr(ct, "xml") != NULL) return true;
    return false;
}

/* Truncate a UTF-8 string at a character boundary, appending a note. */
static char *truncate_markdown(char *text) {
    size_t len = strlen(text);
    if (len <= (size_t)TOOLS_FETCH_MAX_CHARS) return text;
    size_t cut = (size_t)TOOLS_FETCH_MAX_CHARS;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;
    const char *note = "\n\n[content truncated at 100000 characters]";
    size_t note_len = strlen(note);
    char *out = malloc(cut + note_len + 1);
    if (out == NULL) {
        free(text);
        return NULL;
    }
    memcpy(out, text, cut);
    memcpy(out + cut, note, note_len);
    out[cut + note_len] = '\0';
    free(text);
    return out;
}

static int tool_online_fetch(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *url_j = args ? cJSON_GetObjectItem(args, "url") : NULL;
    const char *url = (url_j && cJSON_IsString(url_j)) ? url_j->valuestring : "";
    if (url[0] == '\0') {
        *out_resp = srv_build_error(id_node, "online_fetch requires a string 'url' argument");
        return EXIT_SUCCESS;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        *out_resp = srv_build_error(id_node, "online_fetch requires an http(s) URL");
        return EXIT_SUCCESS;
    }

    char *body = NULL;
    long status = 0;
    char *content_type = NULL;
    char *err = NULL;
    int rc = http_get(url, &body, &status, &content_type, &err);
    if (rc != EXIT_SUCCESS) {
        size_t msg_len = strlen("Fetch failed: ") + (err ? strlen(err) : 0) + 1;
        char *msg = malloc(msg_len);
        if (msg != NULL) snprintf(msg, msg_len, "Fetch failed: %s", err ? err : "unknown error");
        free(err);
        cJSON *result = build_tool_text_result(msg, true);
        free(msg);
        if (result == NULL) return EXIT_INTERNAL_ERR;
        *out_resp = srv_build_response(id_node, result, NULL);
        return EXIT_SUCCESS;
    }
    free(err);

    /* Non-200: report the status per the tool contract. */
    if (status != 200) {
        free(body);
        free(content_type);
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP Status %ld", status);
        cJSON *result = build_tool_text_result(msg, false);
        if (result == NULL) return EXIT_INTERNAL_ERR;
        *out_resp = srv_build_response(id_node, result, NULL);
        return EXIT_SUCCESS;
    }

    /* HTML (and XML) goes through readability + markdown; other text
     * content is passed through as-is; binary content is not returned. */
    char *text = NULL;
    if (content_type_is_html(content_type)) {
        text = htmlmd_convert(body);
    } else if (content_type_is_text(content_type)) {
        text = body;
        body = NULL;
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "[non-text content: %s]",
                 content_type ? content_type : "unknown");
        text = util_strdup(msg);
    }
    free(body);
    free(content_type);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, "Out of memory");
        return EXIT_SUCCESS;
    }

    text = truncate_markdown(text);
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  file_scan                                                          */
/* ------------------------------------------------------------------ */

#define SCAN_DESC "Search local files by name glob, optionally by per-line regex"

/* Match one glob segment against one path component. Supports '*' (any
 * run of characters, including none) and '?' (exactly one character).
 * Returns true when the whole name is consumed. */
static bool scan_segment_matches(const char *pat, const char *name) {
    const char *p = pat;
    const char *n = name;
    const char *star_p = NULL;
    const char *star_n = NULL;
    while (*n != '\0') {
        if (*p == '*') {
            star_p = ++p;
            star_n = n;
        } else if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (star_p != NULL) {
            p = star_p;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* Split the glob pattern into '/'-separated segments (pointers into
 * *out_copy, which the caller owns). Backslashes become separators so
 * Windows-style patterns work everywhere; leading "./" and empty
 * segments are dropped. Absolute patterns and any ".." segment are
 * rejected: matching always starts at the working directory and can
 * never escape it. Returns the segment count, or -1 with *out_err set
 * on a rejected pattern. */
static int scan_parse_segments(const char *pattern, char ***out_segs, char **out_copy,
                               char **out_err) {
    *out_segs = NULL;
    *out_copy = NULL;
    *out_err = NULL;

    if (pattern[0] == '/' || pattern[0] == '\\') {
        *out_err = util_strdup("file_scan: pattern must be relative to the working directory");
        return -1;
    }
    char *copy = util_strdup(pattern);
    if (copy == NULL) {
        *out_err = util_strdup("Out of memory");
        return -1;
    }
    for (char *c = copy; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }

    char **segs = NULL;
    int count = 0;
    int cap = 0;
    char *saveptr = NULL;
    for (char *seg = strtok_r(copy, "/", &saveptr); seg != NULL;
         seg = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(seg, "..") == 0) {
            *out_err = util_strdup("file_scan: '..' is not allowed in the pattern");
            free(copy);
            free(segs);
            return -1;
        }
        if (strcmp(seg, ".") == 0) continue;
        if (count == cap) {
            int new_cap = cap ? cap * 2 : 8;
            char **tmp = realloc(segs, (size_t)new_cap * sizeof(*tmp));
            if (tmp == NULL) {
                *out_err = util_strdup("Out of memory");
                free(copy);
                free(segs);
                return -1;
            }
            segs = tmp;
            cap = new_cap;
        }
        segs[count++] = seg;
    }
    if (count == 0) {
        *out_err = util_strdup("file_scan: empty pattern");
        free(copy);
        free(segs);
        return -1;
    }
    *out_segs = segs;
    *out_copy = copy;
    return count;
}

/* A collected result: the relative path plus its rendered record. */
typedef struct {
    char *path;
    char *record;
} scan_result;

typedef struct {
    char **segs; /* pattern segments (owned via segs_copy) */
    char *segs_copy;
    int nsegs;
    regex_t *re; /* optional compiled content filter */
    scan_result *results;
    int count;
    int cap;
    bool oom;
} scan_state;

static void scan_state_free(scan_state *st) {
    for (int i = 0; i < st->count; i++) {
        free(st->results[i].path);
        free(st->results[i].record);
    }
    free(st->results);
    free(st->segs);
    free(st->segs_copy);
    if (st->re != NULL) {
        regfree(st->re);
        free(st->re);
    }
}

static void scan_add_result(scan_state *st, char *path, char *record) {
    if (st->count == st->cap) {
        int new_cap = st->cap ? st->cap * 2 : 32;
        scan_result *tmp = realloc(st->results, (size_t)new_cap * sizeof(*tmp));
        if (tmp == NULL) {
            st->oom = true;
            free(path);
            free(record);
            return;
        }
        st->results = tmp;
        st->cap = new_cap;
    }
    st->results[st->count].path = path;
    st->results[st->count].record = record;
    st->count++;
}

/* Human-readable size: plain bytes below 1 KiB, otherwise one decimal of
 * Kb/Mb/Gb ("512b", "12.3Kb", "1.5Mb", "2.0Gb"). */
static void scan_format_size(unsigned long long size, char *out, size_t out_len) {
    const unsigned long long UNIT = 1024;
    if (size < UNIT) {
        snprintf(out, out_len, "%llub", size);
        return;
    }
    static const char *const SUFFIXES[] = {"Kb", "Mb", "Gb"};
    unsigned long long scale = UNIT;
    for (int i = 0; i < 3; i++) {
        if (size < scale * UNIT || i == 2) {
            snprintf(out, out_len, "%llu.%llu%s", size / scale, (size % scale) * 10 / scale,
                     SUFFIXES[i]);
            return;
        }
        scale *= UNIT;
    }
}

/* Evaluate one glob-matching file and, when it satisfies the content
 * filter, collect its rendered record. Files that vanish mid-scan are
 * skipped; binary files are listed without lines and never match a
 * content filter. */
static void scan_evaluate(scan_state *st, const char *rel) {
    if (st->oom) return;

    struct stat sb;
    if (lstat(rel, &sb) != 0 || !S_ISREG(sb.st_mode)) return;
    unsigned long long size = (unsigned long long)sb.st_size;

    long line_count = 0;
    long match_count = 0;
    util_growbuf lines_list = {0};
    bool textual = size <= TOOLS_SCAN_MAX_TEXT_BYTES;
    if (textual) {
        FILE *fp = fopen(rel, "rb");
        if (fp == NULL) return;
        util_growbuf gb = {0};
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) util_growbuf_append(&gb, chunk, n);
        fclose(fp);
        size_t len = gb.len;
        char *buf = util_growbuf_release(&gb); /* NUL-terminated; NULL when empty */

        /* A NUL byte in the leading bytes marks a binary file. */
        size_t sniff = len < (size_t)TOOLS_SCAN_SNIFF_BYTES ? len : (size_t)TOOLS_SCAN_SNIFF_BYTES;
        if (buf != NULL && memchr(buf, '\0', sniff) != NULL) {
            textual = false;
        } else {
            /* Walk NUL-terminated lines in place (the newline bytes are
             * the terminator slots growbuf already provides). */
            char *p = buf;
            char *end = buf != NULL ? buf + len : NULL;
            while (p != NULL && p < end) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                char *line_end = nl ? nl : end;
                *line_end = '\0';
                line_count++;
                if (st->re != NULL && regexec(st->re, p, 0, NULL, 0) == 0) {
                    match_count++;
                    if (match_count <= TOOLS_SCAN_MAX_LISTED_LINES) {
                        if (match_count > 1) util_growbuf_append_str(&lines_list, ", ");
                        char num[32];
                        snprintf(num, sizeof(num), "%ld", line_count);
                        util_growbuf_append_str(&lines_list, num);
                    }
                }
                p = nl ? nl + 1 : NULL;
            }
            if (match_count > TOOLS_SCAN_MAX_LISTED_LINES) {
                util_growbuf_append_str(&lines_list, "+");
            }
            /* growbuf_append leaves the terminator slot unwritten: reading
             * the list as a C string below needs it explicit. */
            if (lines_list.buf != NULL) lines_list.buf[lines_list.len] = '\0';
        }
        free(buf);
    }

    /* A content filter excludes files with no matching line. */
    if (st->re != NULL && match_count == 0) {
        util_growbuf_free(&lines_list);
        return;
    }

    char size_str[48];
    scan_format_size(size, size_str, sizeof(size_str));
    util_growbuf record = {0};
    util_growbuf_append_str(&record, "Path: ");
    util_growbuf_append_str(&record, rel);
    util_growbuf_append_str(&record, "\nSize: ");
    util_growbuf_append_str(&record, size_str);
    if (textual) {
        char num[32];
        snprintf(num, sizeof(num), "\nLines: %ld", line_count);
        util_growbuf_append_str(&record, num);
    }
    if (st->re != NULL) {
        util_growbuf_append_str(&record, "\nMatching lines: ");
        util_growbuf_append_str(&record, lines_list.buf ? lines_list.buf : "");
    }
    util_growbuf_free(&lines_list);

    char *path_copy = util_strdup(rel);
    char *record_str = util_growbuf_release(&record);
    if (path_copy == NULL || record_str == NULL) {
        st->oom = true;
        free(path_copy);
        free(record_str);
        return;
    }
    scan_add_result(st, path_copy, record_str);
}

/* Add every regular file under dir_rel to the result set (filtered and
 * rendered like any other match). Used when the pattern ends in '**',
 * which matches any number of trailing path segments. */
static void scan_collect_all(scan_state *st, const char *dir_rel) {
    if (st->oom) return;
    DIR *dir = opendir(dir_rel[0] == '\0' ? "." : dir_rel);
    if (dir == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        char rel[TOOLS_SCAN_PATH_MAX];
        int printed = dir_rel[0] == '\0' ? snprintf(rel, sizeof(rel), "%s", name)
                                         : snprintf(rel, sizeof(rel), "%s/%s", dir_rel, name);
        if (printed < 0 || printed >= (int)sizeof(rel)) continue;
        struct stat sb;
        if (lstat(rel, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            scan_collect_all(st, rel);
        } else if (S_ISREG(sb.st_mode)) {
            scan_evaluate(st, rel);
        }
    }
    closedir(dir);
}

/* Depth-first walk from the working directory, matching one pattern
 * segment per path level so unrelated subtrees are pruned early.
 * Symlinks are never followed: lstat classifies them as neither
 * directories nor regular files, so nothing outside the working
 * directory can be reached. */
static void scan_walk(scan_state *st, const char *dir_rel, int seg_i) {
    if (st->oom || seg_i >= st->nsegs) return;

    const char *seg = st->segs[seg_i];
    bool deep = strcmp(seg, "**") == 0;
    bool last = seg_i == st->nsegs - 1;
    if (deep && last) {
        scan_collect_all(st, dir_rel);
        return;
    }
    if (deep) {
        /* '**' also matches zero directories: try the rest of the
         * pattern against this same directory. */
        scan_walk(st, dir_rel, seg_i + 1);
    }

    DIR *dir = opendir(dir_rel[0] == '\0' ? "." : dir_rel);
    if (dir == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        char rel[TOOLS_SCAN_PATH_MAX];
        int printed = dir_rel[0] == '\0' ? snprintf(rel, sizeof(rel), "%s", name)
                                         : snprintf(rel, sizeof(rel), "%s/%s", dir_rel, name);
        if (printed < 0 || printed >= (int)sizeof(rel)) continue;
        struct stat sb;
        if (lstat(rel, &sb) != 0) continue;

        if (deep) {
            /* A '**' segment absorbs one more directory level. */
            if (S_ISDIR(sb.st_mode)) scan_walk(st, rel, seg_i);
        } else if (scan_segment_matches(seg, name)) {
            if (last) {
                if (S_ISREG(sb.st_mode)) scan_evaluate(st, rel);
            } else if (S_ISDIR(sb.st_mode)) {
                scan_walk(st, rel, seg_i + 1);
            }
        }
    }
    closedir(dir);
}

static int scan_result_cmp(const void *a, const void *b) {
    const scan_result *ra = a;
    const scan_result *rb = b;
    return strcmp(ra->path, rb->path);
}

/* Run a file_scan query against the process working directory.
 * glob_pattern is mandatory; lines_regex (POSIX extended) is optional.
 * Returns a malloc'd report (records separated by blank lines, capped at
 * TOOLS_SCAN_MAX_RECORDS with a "<N> more files matching" note), or NULL
 * with *out_err set for a rejected pattern or regex. */
char *tools_file_scan(const char *glob_pattern, const char *lines_regex, char **out_err) {
    *out_err = NULL;
    if (glob_pattern == NULL || glob_pattern[0] == '\0') {
        *out_err = util_strdup("file_scan requires a non-empty 'filenames_glob'");
        return NULL;
    }

    scan_state st = {0};
    st.nsegs = scan_parse_segments(glob_pattern, &st.segs, &st.segs_copy, out_err);
    if (st.nsegs < 0) return NULL;

    if (lines_regex != NULL && lines_regex[0] != '\0') {
        st.re = malloc(sizeof(*st.re));
        if (st.re == NULL) {
            scan_state_free(&st);
            *out_err = util_strdup("Out of memory");
            return NULL;
        }
        int rc = regcomp(st.re, lines_regex, REG_EXTENDED | REG_NOSUB);
        if (rc != 0) {
            char msg[256];
            regerror(rc, st.re, msg, sizeof(msg));
            size_t len = strlen("file_scan: invalid 'content_lines_regex': ") + strlen(msg) + 1;
            *out_err = malloc(len);
            if (*out_err != NULL) {
                snprintf(*out_err, len, "file_scan: invalid 'content_lines_regex': %s", msg);
            }
            scan_state_free(&st);
            return NULL;
        }
    }

    scan_walk(&st, "", 0);

    char *out = NULL;
    if (st.oom) {
        *out_err = util_strdup("Out of memory");
    } else {
        qsort(st.results, (size_t)st.count, sizeof(*st.results), scan_result_cmp);
        /* Consecutive '**' segments can reach one file through several
         * pattern alignments: drop the duplicates. */
        int total = 0;
        for (int i = 0; i < st.count; i++) {
            if (total > 0 && strcmp(st.results[total - 1].path, st.results[i].path) == 0) {
                free(st.results[i].path);
                free(st.results[i].record);
                continue;
            }
            st.results[total++] = st.results[i];
        }
        st.count = total;

        if (total == 0) {
            util_growbuf gb = {0};
            util_growbuf_append_str(&gb, "No files matching: ");
            util_growbuf_append_str(&gb, glob_pattern);
            out = util_growbuf_release(&gb);
        } else {
            int shown = total < TOOLS_SCAN_MAX_RECORDS ? total : TOOLS_SCAN_MAX_RECORDS;
            util_growbuf gb = {0};
            for (int i = 0; i < shown; i++) {
                if (i > 0) util_growbuf_append_str(&gb, "\n\n");
                util_growbuf_append_str(&gb, st.results[i].record);
            }
            if (total > shown) {
                char note[64];
                snprintf(note, sizeof(note), "\n\n%d more files matching", total - shown);
                util_growbuf_append_str(&gb, note);
            }
            out = util_growbuf_release(&gb);
        }
    }

    scan_state_free(&st);
    return out;
}

static int tool_file_scan(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *glob_j = args ? cJSON_GetObjectItem(args, "filenames_glob") : NULL;
    const char *glob = (glob_j && cJSON_IsString(glob_j)) ? glob_j->valuestring : "";
    if (glob[0] == '\0') {
        *out_resp =
            srv_build_error(id_node, "file_scan requires a string 'filenames_glob' argument");
        return EXIT_SUCCESS;
    }
    cJSON *re_j = args ? cJSON_GetObjectItem(args, "content_lines_regex") : NULL;
    const char *re =
        (re_j && cJSON_IsString(re_j) && re_j->valuestring[0] != '\0') ? re_j->valuestring : NULL;

    char *err = NULL;
    char *text = tools_file_scan(glob, re, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  exec / exec_status                                                 */
/* ------------------------------------------------------------------ */

#define EXEC_DESC "Execute a shell command line"

#define EXEC_STATUS_DESC "Check the status of a pid"

/* How long exec waits for the command before replying with its pid and
 * leaving it running in the background for exec_status to poll. */
#define EXEC_WAIT_MS 10000

/* Output size returned to the caller: the tail of the full output, cut
 * at the first newline so the report never starts mid-line. */
#define EXEC_OUTPUT_KEEP (3 * 1024)

/* Directory (relative to the working directory) holding every command's
 * full output; the truncation note points into it. */
#define EXEC_OUTPUT_DIR ".output"

/* Bytes sniffed at the start of the output to tell text from binary
 * (a NUL byte means binary), matching file_scan's rule. */
#define EXEC_SNIFF_BYTES 8192

/* One tracked command. The exec tool server is single-threaded, so a
 * plain process-global array is safe; entries are never removed (they
 * are small and keep exec_status answers valid for the server's life). */
typedef struct {
    platform_process proc;
    char *out_path; /* full output file, relative to the cwd */
    int64_t start_ms;
    bool done;
    int exit_code;  /* valid once done */
    int64_t end_ms; /* valid once done */
} exec_record;

static exec_record *g_execs = NULL;
static int g_exec_count = 0;

/* Reap every finished background command so none linger as zombies when
 * the caller never polls a finished pid again. */
static void exec_sweep(void) {
    for (int i = 0; i < g_exec_count; i++) {
        if (g_execs[i].done) continue;
        int code = 0;
        if (platform_process_trywait(&g_execs[i].proc, &code) == 1) {
            g_execs[i].done = true;
            g_execs[i].exit_code = code;
            g_execs[i].end_ms = platform_now_ms();
        }
    }
}

static exec_record *exec_find(int pid) {
    for (int i = 0; i < g_exec_count; i++) {
        if (g_execs[i].proc.pid == pid) return &g_execs[i];
    }
    return NULL;
}

/* Human-readable duration: "350ms", "12.3s", "2m05s", "3h02m". */
static void exec_format_duration(int64_t ms, char *out, size_t out_len) {
    if (ms < 0) ms = 0;
    if (ms < 1000) {
        snprintf(out, out_len, "%lldms", (long long)ms);
    } else if (ms < 60000) {
        snprintf(out, out_len, "%.1fs", ms / 1000.0);
    } else if (ms < 3600000) {
        snprintf(out, out_len, "%dm%02ds", (int)(ms / 60000), (int)((ms / 1000) % 60));
    } else {
        snprintf(out, out_len, "%dh%02dm", (int)(ms / 3600000), (int)((ms / 60000) % 60));
    }
}

/* Streaming reader over an exec output file: tracks the total size, the
 * text/binary sniff, the newline count, and always keeps the last
 * EXEC_OUTPUT_KEEP bytes so arbitrarily large outputs can be rendered
 * without buffering them. */
typedef struct {
    char tail[EXEC_OUTPUT_KEEP];
    size_t tail_len;
    unsigned long long total;
    long long lines;
    bool textual;
} exec_out_reader;

static void exec_out_init(exec_out_reader *r) {
    r->tail_len = 0;
    r->total = 0;
    r->lines = 0;
    r->textual = true;
}

static void exec_out_feed(exec_out_reader *r, const char *data, size_t n) {
    if (r->total < EXEC_SNIFF_BYTES) {
        size_t sniff = EXEC_SNIFF_BYTES - (size_t)r->total;
        if (sniff > n) sniff = n;
        if (memchr(data, '\0', sniff) != NULL) r->textual = false;
    }
    if (r->textual) {
        for (size_t i = 0; i < n; i++) {
            if (data[i] == '\n') r->lines++;
        }
    }
    r->total += n;

    if (n >= EXEC_OUTPUT_KEEP) {
        memcpy(r->tail, data + n - EXEC_OUTPUT_KEEP, EXEC_OUTPUT_KEEP);
        r->tail_len = EXEC_OUTPUT_KEEP;
    } else if (r->tail_len + n > EXEC_OUTPUT_KEEP) {
        size_t drop = r->tail_len + n - EXEC_OUTPUT_KEEP;
        memmove(r->tail, r->tail + drop, r->tail_len - drop);
        memcpy(r->tail + (r->tail_len - drop), data, n);
        r->tail_len = EXEC_OUTPUT_KEEP;
    } else {
        memcpy(r->tail + r->tail_len, data, n);
        r->tail_len += n;
    }
}

/* Render the shared report ("Exit code / Duration / Output" plus the
 * output tail). When the whole output did not fit in the tail, a note
 * points at the full output file, with a line count for text files. */
static char *exec_build_report(const exec_record *rec) {
    exec_out_reader r;
    exec_out_init(&r);
    FILE *fp = fopen(rec->out_path, "rb");
    if (fp != NULL) {
        char chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) exec_out_feed(&r, chunk, n);
        fclose(fp);
    }

    int64_t end_ms = rec->end_ms;
    if (!rec->done) end_ms = platform_now_ms();
    int64_t duration = end_ms - rec->start_ms;
    char dur[32];
    exec_format_duration(duration, dur, sizeof(dur));

    util_growbuf out = {0};
    char line[128];
    snprintf(line, sizeof(line), "Exit code: %d\n", rec->exit_code);
    util_growbuf_append_str(&out, line);
    snprintf(line, sizeof(line), "Duration: %s\n", dur);
    util_growbuf_append_str(&out, line);
    util_growbuf_append_str(&out, "Output:\n");
    /* The report is a C string headed into a JSON text payload: show NUL
     * bytes as '.' so a binary tail cannot cut the report (and the
     * truncation note below) short. The full output file stays raw. */
    for (size_t i = 0; i < r.tail_len; i++) {
        if (r.tail[i] == '\0') r.tail[i] = '.';
    }
    if (r.tail_len > 0) util_growbuf_append(&out, r.tail, r.tail_len);

    if (r.total > EXEC_OUTPUT_KEEP) {
        if (r.tail_len == 0 || r.tail[r.tail_len - 1] != '\n') {
            util_growbuf_append_str(&out, "\n");
        }
        char size_str[48];
        scan_format_size(r.total, size_str, sizeof(size_str));
        util_growbuf_append_str(&out, "Output truncated, full output in file ");
        util_growbuf_append_str(&out, rec->out_path);
        util_growbuf_append_str(&out, " (size ");
        util_growbuf_append_str(&out, size_str);
        if (r.textual) {
            snprintf(line, sizeof(line), ", %lld lines", r.lines);
            util_growbuf_append_str(&out, line);
        }
        util_growbuf_append_str(&out, ")\n");
    }
    return util_growbuf_release(&out);
}

/* Run cmdline in a subshell. Returns the report once the command either
 * finished within EXEC_WAIT_MS or was left running in the background
 * (a "PID: ... still running" reply). Returns NULL with *out_err set for
 * a rejected argument or a spawn failure. */
char *tools_exec(const char *cmdline, char **out_err) {
    *out_err = NULL;
    if (cmdline == NULL || cmdline[0] == '\0') {
        *out_err = util_strdup("exec requires a non-empty 'cmdline' argument");
        return NULL;
    }

    exec_sweep();

#ifdef _WIN32
    if (_mkdir(EXEC_OUTPUT_DIR) != 0 && errno != EEXIST) {
#else
    if (mkdir(EXEC_OUTPUT_DIR, 0777) != 0 && errno != EEXIST) {
#endif
        *out_err = util_strdup("exec: cannot create the " EXEC_OUTPUT_DIR " directory");
        return NULL;
    }

    /* Every command gets a unique file so separate server runs (and
     * concurrent servers) sharing a working directory never overwrite
     * each other's full output. */
    char uid[37];
    util_uuid_v4(uid);
    char *path = malloc(strlen(EXEC_OUTPUT_DIR) + 48);
    if (path == NULL) {
        *out_err = util_strdup("Out of memory");
        return NULL;
    }
    snprintf(path, strlen(EXEC_OUTPUT_DIR) + 48, EXEC_OUTPUT_DIR "/exec_%s.log", uid);

    platform_process proc;
    memset(&proc, 0, sizeof(proc));
    if (platform_shell_spawn(cmdline, path, &proc) != 0) {
        int spawn_errno = errno;
        size_t msg_len =
            strlen("exec: failed to spawn the command: ") + strlen(strerror(spawn_errno)) + 1;
        *out_err = malloc(msg_len);
        if (*out_err != NULL) {
            snprintf(*out_err, msg_len, "exec: failed to spawn the command: %s",
                     strerror(spawn_errno));
        }
        free(path);
        return NULL;
    }

    exec_record *tmp = realloc(g_execs, (size_t)(g_exec_count + 1) * sizeof(*tmp));
    if (tmp == NULL) {
        *out_err = util_strdup("Out of memory");
        free(path);
        return NULL;
    }
    g_execs = tmp;
    exec_record *rec = &g_execs[g_exec_count++];
    memset(rec, 0, sizeof(*rec));
    rec->proc = proc;
    rec->out_path = path;
    rec->start_ms = platform_now_ms();

    int code = 0;
    if (platform_process_wait(&rec->proc, EXEC_WAIT_MS, &code) == 0) {
        rec->done = true;
        rec->exit_code = code;
        rec->end_ms = platform_now_ms();
        return exec_build_report(rec);
    }

    /* Still running after EXEC_WAIT_MS: hand the pid to the caller and
     * leave the command in the background for exec_status to poll. */
    util_growbuf out = {0};
    char line[128];
    snprintf(line, sizeof(line), "PID: %d\n", rec->proc.pid);
    util_growbuf_append_str(&out, line);
    util_growbuf_append_str(&out, "Process still running, use exec_status(");
    snprintf(line, sizeof(line), "%d) to get the current status.\n", rec->proc.pid);
    util_growbuf_append_str(&out, line);
    return util_growbuf_release(&out);
}

/* Report on a previously exec'd command: its report once finished, or a
 * "still running, started ... ago" note while it is not. Unknown pids
 * get a plain answer (NULL only on OOM). */
char *tools_exec_status(int pid, char **out_err) {
    (void)out_err;
    *out_err = NULL;

    exec_sweep();

    exec_record *rec = exec_find(pid);
    if (rec == NULL) {
        char msg[96];
        snprintf(msg, sizeof(msg), "No exec process with pid %d.", pid);
        return util_strdup(msg);
    }

    if (!rec->done) {
        char dur[32];
        exec_format_duration(platform_now_ms() - rec->start_ms, dur, sizeof(dur));
        util_growbuf out = {0};
        char head[64];
        snprintf(head, sizeof(head), "PID %d still running, started ", rec->proc.pid);
        util_growbuf_append_str(&out, head);
        util_growbuf_append_str(&out, dur);
        util_growbuf_append_str(&out, " ago.");
        return util_growbuf_release(&out);
    }

    return exec_build_report(rec);
}

static int tool_exec(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *cmd_j = args ? cJSON_GetObjectItem(args, "cmdline") : NULL;
    const char *cmdline = (cmd_j && cJSON_IsString(cmd_j)) ? cmd_j->valuestring : "";
    if (cmdline[0] == '\0') {
        *out_resp = srv_build_error(id_node, "exec requires a string 'cmdline' argument");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_exec(cmdline, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

static int tool_exec_status(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    /* Accept a JSON number or a numeric string: pids circulate as text
     * once the LLM copies them out of an exec reply. */
    cJSON *pid_j = args ? cJSON_GetObjectItem(args, "pid") : NULL;
    int pid = 0;
    if (pid_j != NULL && cJSON_IsNumber(pid_j)) {
        pid = (int)pid_j->valuedouble;
    } else if (pid_j != NULL && cJSON_IsString(pid_j) && pid_j->valuestring[0] != '\0') {
        char *end = NULL;
        long v = strtol(pid_j->valuestring, &end, 10);
        if (end != pid_j->valuestring && *end == '\0') pid = (int)v;
    }
    if (pid <= 0) {
        *out_resp = srv_build_error(id_node, "exec_status requires an integer 'pid' argument");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_exec_status(pid, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  sleep                                                              */
/* ------------------------------------------------------------------ */

#define SLEEP_DESC "Wait for given seconds"

/* Upper bound on a single sleep: the tools server is single-threaded,
 * so a request blocks every other call while it runs. */
#define TOOLS_SLEEP_MAX_SECONDS 60

/* Sleep for the given number of seconds (fractions allowed). Returns a
 * malloc'd "Slept for N seconds." reply, or NULL with *out_err set for
 * an out-of-range request. Exported for tests. */
char *tools_sleep(double seconds, char **out_err) {
    *out_err = NULL;
    if (seconds < 0.0 || seconds > (double)TOOLS_SLEEP_MAX_SECONDS) {
        *out_err = util_strdup("sleep: 'seconds' must be between 0 and 60");
        return NULL;
    }
    platform_sleep_ms((int64_t)((seconds * 1000.0) + 0.5));

    char num[32];
    if ((double)(long long)seconds == seconds) {
        snprintf(num, sizeof(num), "%lld", (long long)seconds);
    } else {
        snprintf(num, sizeof(num), "%g", seconds);
    }
    util_growbuf out = {0};
    util_growbuf_append_str(&out, "Slept for ");
    util_growbuf_append_str(&out, num);
    util_growbuf_append_str(&out, strcmp(num, "1") == 0 ? " second." : " seconds.");
    return util_growbuf_release(&out);
}

static int tool_sleep(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    /* Accept a JSON number or a numeric string, like exec_status's pid. */
    cJSON *s_j = args ? cJSON_GetObjectItem(args, "seconds") : NULL;
    double seconds = 0.0;
    bool present = false;
    if (s_j != NULL && cJSON_IsNumber(s_j)) {
        seconds = s_j->valuedouble;
        present = true;
    } else if (s_j != NULL && cJSON_IsString(s_j) && s_j->valuestring[0] != '\0') {
        char *end = NULL;
        double v = strtod(s_j->valuestring, &end);
        if (end != s_j->valuestring && *end == '\0') {
            seconds = v;
            present = true;
        }
    }
    if (!present) {
        *out_resp = srv_build_error(id_node, "sleep requires a number 'seconds' argument");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_sleep(seconds, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  file tools: shared helpers                                         */
/* ------------------------------------------------------------------ */

#define FILE_READ_DESC "Read lines from a text file"

#define FILE_CREATE_DESC "Create or overwrite a text file"

#define FILE_EDIT_DESC "Replace old_string with new_string"

/* Largest file the file tools will read or write. */
#define TOOLS_FILE_MAX_BYTES (32 * 1024 * 1024)

/* Bytes sniffed at the start of a file to tell text from binary (a NUL
 * byte means binary), matching file_scan's rule. */
#define TOOLS_FILE_SNIFF_BYTES 8192

/* file_read page size: at most 2000 lines and 100000 characters are
 * returned per call so a huge file cannot flood the LLM context. */
#define TOOLS_READ_MAX_LINES 2000
#define TOOLS_READ_MAX_CHARS 100000

/* Longest relative path accepted from the file tools. */
#define TOOLS_FILE_PATH_MAX 4096

/* Format an error message prefixed with the tool name, in the style of
 * file_scan's ("file_read: ..."). Returns a malloc'd string, or NULL
 * only on OOM. */
static char *tool_prefixed_err(const char *tool_name, const char *fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    size_t len = strlen(tool_name) + strlen(body) + 4;
    char *out = malloc(len);
    if (out != NULL) snprintf(out, len, "%s: %s", tool_name, body);
    return out;
}

/* Validate a file tool's path argument and normalize it to a clean
 * relative path: backslashes become separators, '.' and empty segments
 * are dropped, and absolute paths, Windows drive prefixes and any '..'
 * segment are rejected. Existing components are lstat'ed one by one so
 * a symbolic link anywhere along the way is refused too: with absolute
 * paths and '..' banned, no link means the path cannot leave the
 * working directory. Returns a malloc'd clean path, or NULL with
 * *out_err set. */
static char *filetool_clean_path(const char *tool_name, const char *path, char **out_err) {
    *out_err = NULL;

    if (path[0] == '\0') {
        *out_err = tool_prefixed_err(tool_name, "a non-empty 'filepath' is required");
        return NULL;
    }
    if (path[0] == '/' || path[0] == '\\') {
        *out_err = tool_prefixed_err(tool_name, "path must be relative to the working directory");
        return NULL;
    }
    if (path[1] == ':' &&
        ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'))) {
        *out_err = tool_prefixed_err(tool_name, "path must be relative to the working directory");
        return NULL;
    }

    char *copy = util_strdup(path);
    if (copy == NULL) {
        *out_err = util_strdup("Out of memory");
        return NULL;
    }
    for (char *c = copy; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }

    util_growbuf clean = {0};
    char acc[TOOLS_FILE_PATH_MAX];
    size_t acc_len = 0;
    bool failed = false;
    char *saveptr = NULL;
    for (char *seg = strtok_r(copy, "/", &saveptr); seg != NULL && !failed;
         seg = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(seg, "..") == 0) {
            *out_err = tool_prefixed_err(tool_name, "'..' is not allowed in the path");
            failed = true;
            break;
        }
        if (strcmp(seg, ".") == 0) continue;

        /* Accumulate the cleaned prefix so every existing component can
         * be checked for a symbolic link before the file is opened. */
        size_t seg_len = strlen(seg);
        if (acc_len + seg_len + 2 > sizeof(acc)) {
            *out_err = tool_prefixed_err(tool_name, "path is too long");
            failed = true;
            break;
        }
        if (acc_len > 0) acc[acc_len++] = '/';
        memcpy(acc + acc_len, seg, seg_len + 1);
        acc_len += seg_len;
        struct stat sb;
        if (lstat(acc, &sb) == 0 && S_ISLNK(sb.st_mode)) {
            *out_err = tool_prefixed_err(tool_name, "path traverses the symbolic link '%s'", seg);
            failed = true;
            break;
        }

        if (clean.len > 0) util_growbuf_append_str(&clean, "/");
        util_growbuf_append_str(&clean, seg);
    }
    free(copy);

    if (!failed && clean.len == 0) {
        *out_err = tool_prefixed_err(tool_name, "a non-empty 'filepath' is required");
        failed = true;
    }
    if (failed) {
        util_growbuf_free(&clean);
        if (*out_err == NULL) *out_err = util_strdup("Out of memory");
        return NULL;
    }
    char *out = util_growbuf_release(&clean);
    if (out == NULL) *out_err = util_strdup("Out of memory");
    return out;
}

/* Count lines the way the file tools report them: one per '\n' plus a
 * final line when the content does not end with one (empty content has
 * 0 lines). */
static long filetool_count_lines(const char *content, size_t len) {
    long lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (content[i] == '\n') lines++;
    }
    if (len > 0 && content[len - 1] != '\n') lines++;
    return lines;
}

/* Read the whole regular file at rel (already cleaned by
 * filetool_clean_path) into a NUL-terminated malloc'd buffer. Returns 0
 * with the content and length filled on success, 0 with *out_binary set
 * (and no content) when the file sniffs as binary, and -1 with *out_err
 * set for a missing file, a non-regular file, an oversized file or an
 * I/O error. */
static int filetool_read_whole(const char *tool_name, const char *rel, char **out_content,
                               size_t *out_len, bool *out_binary, char **out_err) {
    *out_content = NULL;
    *out_len = 0;
    *out_binary = false;
    *out_err = NULL;

    struct stat sb;
    if (lstat(rel, &sb) != 0) {
        *out_err = tool_prefixed_err(tool_name, "cannot open '%s': %s", rel, strerror(errno));
        return -1;
    }
    if (!S_ISREG(sb.st_mode)) {
        *out_err = tool_prefixed_err(tool_name, "'%s' is not a regular file", rel);
        return -1;
    }
    if ((unsigned long long)sb.st_size > TOOLS_FILE_MAX_BYTES) {
        *out_err = tool_prefixed_err(tool_name, "'%s' is too large (over 32MiB)", rel);
        return -1;
    }

    FILE *fp = fopen(rel, "rb");
    if (fp == NULL) {
        *out_err = tool_prefixed_err(tool_name, "cannot open '%s': %s", rel, strerror(errno));
        return -1;
    }
    util_growbuf gb = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) util_growbuf_append(&gb, chunk, n);
    bool io_err = ferror(fp) != 0;
    fclose(fp);
    if (io_err) {
        util_growbuf_free(&gb);
        *out_err = tool_prefixed_err(tool_name, "read error on '%s'", rel);
        return -1;
    }

    size_t len = gb.len;
    char *buf = util_growbuf_release(&gb); /* NUL-terminated; NULL when empty */
    if (buf == NULL) {
        buf = util_strdup("");
        if (buf == NULL) {
            *out_err = util_strdup("Out of memory");
            return -1;
        }
    }

    size_t sniff = len < (size_t)TOOLS_FILE_SNIFF_BYTES ? len : (size_t)TOOLS_FILE_SNIFF_BYTES;
    if (memchr(buf, '\0', sniff) != NULL) {
        free(buf);
        *out_binary = true;
        return 0;
    }
    *out_content = buf;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  file_read                                                          */
/* ------------------------------------------------------------------ */

/* Read a text file relative to the working directory and render the
 * requested 1-based line range: line_offset <= 0 means line 1,
 * lines_length <= 0 means TOOLS_READ_MAX_LINES, and at most 2000 lines
 * and 100000 characters are returned per call. Paths escaping the
 * working directory and symbolic links are refused (see
 * filetool_clean_path), as are binary files. Returns a malloc'd report,
 * or NULL with *out_err set. Exported for tests. */
char *tools_file_read(const char *filepath, long line_offset, long lines_length, char **out_err) {
    *out_err = NULL;
    if (filepath == NULL || filepath[0] == '\0') {
        *out_err = tool_prefixed_err("file_read", "a non-empty 'filepath' is required");
        return NULL;
    }
    char *rel = filetool_clean_path("file_read", filepath, out_err);
    if (rel == NULL) return NULL;

    bool binary = false;
    char *content = NULL;
    size_t len = 0;
    int rc = filetool_read_whole("file_read", rel, &content, &len, &binary, out_err);
    if (rc != 0) {
        free(rel);
        if (*out_err == NULL) *out_err = util_strdup("Out of memory");
        return NULL;
    }
    if (binary) {
        free(rel);
        return util_strdup("Read refused: binary file");
    }

    long total = filetool_count_lines(content, len);
    if (line_offset <= 0) line_offset = 1;

    char *result = NULL;
    if (total == 0) {
        /* An empty file: just the header, no content block. */
        util_growbuf out = {0};
        util_growbuf_append_str(&out, "File path: ");
        util_growbuf_append_str(&out, rel);
        char line[32];
        snprintf(line, sizeof(line), "\nTotal lines: %ld\n", total);
        util_growbuf_append_str(&out, line);
        result = util_growbuf_release(&out);
    } else if (line_offset > total) {
        util_growbuf msg = {0};
        char line[128];
        snprintf(line, sizeof(line),
                 "Read refused: line_offset %ld is beyond the end of the file (%ld lines)",
                 line_offset, total);
        util_growbuf_append_str(&msg, line);
        result = util_growbuf_release(&msg);
    } else {
        long max_lines = lines_length <= 0 ? TOOLS_READ_MAX_LINES : lines_length;
        if (max_lines > TOOLS_READ_MAX_LINES) max_lines = TOOLS_READ_MAX_LINES;
        long to = line_offset + max_lines - 1;
        if (to > total) to = total;

        /* First pass: collect the content block under the character cap
         * (the first requested line is always included, so the reported
         * range always advances). */
        util_growbuf body = {0};
        long shown_from = 0;
        long shown_to = 0;
        long appended = 0;
        size_t chars = 0;
        bool truncated = false;
        size_t pos = 0;
        long lineno = 1;
        while (pos < len && lineno <= to) {
            char *nl = memchr(content + pos, '\n', len - pos);
            size_t line_len = nl ? (size_t)(nl - (content + pos)) : len - pos;
            if (lineno >= line_offset) {
                size_t need = line_len + 1;
                if (appended > 0 && chars + need > (size_t)TOOLS_READ_MAX_CHARS) {
                    truncated = true;
                    break;
                }
                util_growbuf_append(&body, content + pos, line_len);
                util_growbuf_append_str(&body, "\n");
                chars += need;
                appended++;
                if (appended == 1) shown_from = lineno;
                shown_to = lineno;
            }
            lineno++;
            pos += line_len + (nl ? 1 : 0);
        }

        util_growbuf out = {0};
        char line[160];
        util_growbuf_append_str(&out, "File path: ");
        util_growbuf_append_str(&out, rel);
        snprintf(line, sizeof(line), "\nTotal lines: %ld\n", total);
        util_growbuf_append_str(&out, line);
        snprintf(line, sizeof(line), "\n----- lines from %ld to %ld -----\n", shown_from, shown_to);
        util_growbuf_append_str(&out, line);
        if (body.buf != NULL) util_growbuf_append(&out, body.buf, body.len);
        if (truncated) {
            snprintf(line, sizeof(line),
                     "[output truncated at %d characters; continue with line_offset %ld]",
                     TOOLS_READ_MAX_CHARS, shown_to + 1);
            util_growbuf_append_str(&out, line);
        }
        util_growbuf_free(&body);
        result = util_growbuf_release(&out);
    }
    free(rel);
    free(content);
    return result;
}

static int tool_file_read(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *fp_j = args ? cJSON_GetObjectItem(args, "filepath") : NULL;
    const char *filepath = (fp_j && cJSON_IsString(fp_j)) ? fp_j->valuestring : "";
    if (filepath[0] == '\0') {
        *out_resp = srv_build_error(id_node, "file_read requires a string 'filepath' argument");
        return EXIT_SUCCESS;
    }
    long offset = 0;
    long length = 0;
    if (!tool_int_arg(args, "line_offset", &offset) ||
        !tool_int_arg(args, "lines_length", &length)) {
        *out_resp = srv_build_error(id_node,
                                    "file_read: 'line_offset' and 'lines_length' must be integers");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_file_read(filepath, offset, length, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  file_create                                                        */
/* ------------------------------------------------------------------ */

/* Create or overwrite a text file with content written verbatim, under
 * the same path rules as tools_file_read. Returns a malloc'd "File
 * created/overwritten: ... (N bytes, M lines)" reply, or NULL with
 * *out_err set. Exported for tests. */
char *tools_file_create(const char *filepath, const char *content, char **out_err) {
    *out_err = NULL;
    if (filepath == NULL || filepath[0] == '\0') {
        *out_err = tool_prefixed_err("file_create", "a non-empty 'filepath' is required");
        return NULL;
    }
    if (content == NULL) content = "";
    char *rel = filetool_clean_path("file_create", filepath, out_err);
    if (rel == NULL) {
        if (*out_err == NULL) *out_err = util_strdup("Out of memory");
        return NULL;
    }

    bool existed = false;
    if (lstat(rel, &(struct stat){0}) == 0) existed = true;
    FILE *fp = fopen(rel, "wb");
    if (fp == NULL) {
        *out_err = tool_prefixed_err("file_create", "cannot write '%s': %s", rel, strerror(errno));
        free(rel);
        return NULL;
    }
    size_t len = strlen(content);
    bool failed = false;
    if (len > 0 && fwrite(content, 1, len, fp) != len) failed = true;
    if (fclose(fp) != 0) failed = true;
    if (failed) {
        *out_err = tool_prefixed_err("file_create", "write to '%s' failed", rel);
        free(rel);
        return NULL;
    }

    long lines = filetool_count_lines(content, len);
    util_growbuf out = {0};
    if (existed) {
        util_growbuf_append_str(&out, "File overwritten: ");
    } else {
        util_growbuf_append_str(&out, "File created: ");
    }
    util_growbuf_append_str(&out, rel);
    char tail[64];
    snprintf(tail, sizeof(tail), " (%zu bytes, %ld %s)", len, lines, lines == 1 ? "line" : "lines");
    util_growbuf_append_str(&out, tail);
    free(rel);
    return util_growbuf_release(&out);
}

static int tool_file_create(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *fp_j = args ? cJSON_GetObjectItem(args, "filepath") : NULL;
    const char *filepath = (fp_j && cJSON_IsString(fp_j)) ? fp_j->valuestring : "";
    cJSON *ct_j = args ? cJSON_GetObjectItem(args, "content") : NULL;
    if (filepath[0] == '\0') {
        *out_resp = srv_build_error(id_node, "file_create requires a string 'filepath' argument");
        return EXIT_SUCCESS;
    }
    if (ct_j == NULL || !cJSON_IsString(ct_j)) {
        *out_resp = srv_build_error(id_node, "file_create requires a string 'content' argument");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_file_create(filepath, ct_j->valuestring, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  file_edit                                                          */
/* ------------------------------------------------------------------ */

/* How far (in lines) old_string may sit from the linefrom hint. */
#define TOOLS_EDIT_TOLERANCE 3

/* Tab stop used when measuring and re-anchoring indentation. */
#define TOOLS_EDIT_TAB_WIDTH 4

/* Whitespace-tolerant projection of a text, used to match old_string
 * against a file without tripping over indentation: tabs, carriage
 * returns and whitespace runs collapse to a single space, and each
 * line's leading and trailing whitespace is dropped. Every byte of the
 * normalized text remembers the raw offset it came from, so a match
 * found here can be spliced back at raw precision. */
typedef struct {
    char *norm;  /* normalized text, NUL-terminated */
    size_t *off; /* raw offset of each normalized byte */
    size_t len;  /* normalized length in bytes */
} edit_norm;

static void edit_norm_free(edit_norm *en) {
    free(en->norm);
    free(en->off);
    en->norm = NULL;
    en->off = NULL;
    en->len = 0;
}

/* Bytes of one UTF-8 sequence from the lead byte. The file tools never
 * decode text; they only need raw offsets to line up, so invalid leads
 * and stray continuation bytes count as single bytes. */
static size_t edit_utf8_seq_len(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

static bool edit_norm_build(const char *raw, size_t raw_len, edit_norm *out) {
    out->norm = NULL;
    out->off = NULL;
    out->len = 0;

    char *norm = malloc(raw_len + 1);
    size_t *off = malloc((raw_len > 0 ? raw_len : 1) * sizeof(*off));
    if (norm == NULL || off == NULL) {
        free(norm);
        free(off);
        return false;
    }

    size_t n = 0;
    bool line_has_content = false;
    bool in_ws_run = false;
    size_t ws_run_start = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (c == '\n') {
            norm[n] = '\n';
            off[n] = i;
            n++;
            line_has_content = false;
            in_ws_run = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (line_has_content && !in_ws_run) {
                ws_run_start = i;
                in_ws_run = true;
            }
            continue;
        }
        if (in_ws_run) {
            norm[n] = ' ';
            off[n] = ws_run_start;
            n++;
            in_ws_run = false;
        }
        size_t seq = edit_utf8_seq_len((unsigned char)c);
        if (seq > raw_len - i) seq = raw_len - i;
        for (size_t k = 0; k < seq; k++) {
            norm[n] = raw[i + k];
            off[n] = i + k;
            n++;
        }
        line_has_content = true;
        i += seq - 1;
    }
    norm[n] = '\0';
    out->norm = norm;
    out->off = off;
    out->len = n;
    return true;
}

/* Display column reached after expanding s with tabs at
 * TOOLS_EDIT_TAB_WIDTH stops. */
static long edit_indent_cols(const char *s, size_t len) {
    long col = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\t') {
            col += TOOLS_EDIT_TAB_WIDTH - (col % TOOLS_EDIT_TAB_WIDTH);
        } else {
            col++;
        }
    }
    return col;
}

/* Length of the leading whitespace (spaces, tabs, CRs) of s's first
 * line. */
static size_t edit_leading_ws_len(const char *s, size_t max) {
    size_t i = 0;
    while (i < max && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return i;
}

/* True when only whitespace precedes pos on its line. */
static bool edit_starts_at_bol(const char *raw, size_t pos) {
    size_t ls = pos;
    while (ls > 0 && raw[ls - 1] != '\n') ls--;
    for (size_t i = ls; i < pos; i++) {
        if (raw[i] != ' ' && raw[i] != '\t' && raw[i] != '\r') return false;
    }
    return true;
}

/* True when only whitespace follows a match's end on its line. */
static bool edit_ends_at_eol(const char *raw, size_t raw_len, size_t end) {
    for (size_t i = end; i < raw_len && raw[i] != '\n'; i++) {
        if (raw[i] != ' ' && raw[i] != '\t' && raw[i] != '\r') return false;
    }
    return true;
}

/* One raw line of a split string. */
typedef struct {
    const char *p;
    size_t len;
} edit_span;

/* Split s into lines on '\n', dropping a trailing '\r' per line. "a\n"
 * splits into "a" and "" so a trailing newline survives a round trip
 * through the join. Returns a malloc'd array; NULL only on OOM. */
static edit_span *edit_split_lines(const char *s, size_t slen, int *out_count) {
    int count = 1;
    for (size_t i = 0; i < slen; i++) {
        if (s[i] == '\n') count++;
    }
    edit_span *spans = malloc((size_t)count * sizeof(*spans));
    if (spans == NULL) return NULL;

    int n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= slen; i++) {
        if (i == slen || s[i] == '\n') {
            size_t len = i - start;
            if (len > 0 && s[start + len - 1] == '\r') len--;
            spans[n].p = s + start;
            spans[n].len = len;
            n++;
            start = i + 1;
        }
    }
    *out_count = n;
    return spans;
}

/* Replace old_string with new_string in a text file, searching within
 * TOOLS_EDIT_TOLERANCE lines of the 1-based linefrom hint. Matching is
 * whitespace-tolerant (see edit_norm_build). The replacement is
 * re-indented by the difference between the file's and old_string's
 * first-line indentation, so an old_string written with sloppy
 * indentation still lands with the file's own indentation; tab-based
 * indentation is preserved when the replacement needs no shift.
 * Returns "Edit accepted" on success; refusals come back as text
 * ("Edit refused: old_string not found" / "Edit refused: old_string
 * found at line N"). Returns NULL with *out_err set for path guards
 * and I/O errors. Exported for tests. */
char *tools_file_edit(const char *filepath, long linefrom, const char *old_string,
                      const char *new_string, char **out_err) {
    *out_err = NULL;
    if (filepath == NULL || filepath[0] == '\0') {
        *out_err = tool_prefixed_err("file_edit", "a non-empty 'filepath' is required");
        return NULL;
    }
    if (old_string == NULL || old_string[0] == '\0') {
        *out_err = tool_prefixed_err("file_edit", "a non-empty 'old_string' is required");
        return NULL;
    }
    if (new_string == NULL) new_string = "";
    if (linefrom < 1) {
        *out_err = tool_prefixed_err("file_edit", "'linefrom' must be a 1-based line number");
        return NULL;
    }

    char *rel = filetool_clean_path("file_edit", filepath, out_err);
    if (rel == NULL) {
        if (*out_err == NULL) *out_err = util_strdup("Out of memory");
        return NULL;
    }

    bool binary = false;
    char *content = NULL;
    size_t content_len = 0;
    if (filetool_read_whole("file_edit", rel, &content, &content_len, &binary, out_err) != 0) {
        free(rel);
        if (*out_err == NULL) *out_err = util_strdup("Out of memory");
        return NULL;
    }
    if (binary) {
        free(rel);
        return util_strdup("Edit refused: binary file");
    }

    edit_norm nc;
    edit_norm no;
    if (!edit_norm_build(content, content_len, &nc) ||
        !edit_norm_build(old_string, strlen(old_string), &no)) {
        edit_norm_free(&nc);
        edit_norm_free(&no);
        free(content);
        free(rel);
        *out_err = util_strdup("Out of memory");
        return NULL;
    }
    if (no.len == 0) {
        edit_norm_free(&nc);
        edit_norm_free(&no);
        free(content);
        free(rel);
        *out_err = tool_prefixed_err("file_edit", "'old_string' contains no visible text");
        return NULL;
    }

    /* One pass over the normalized content: remember the first
     * occurrence anywhere (for the out-of-range refusal) and the
     * occurrence closest to linefrom within the tolerance window. */
    bool in_range = false;
    long best_dist = 0;
    long best_line = 0;
    size_t best_idx = 0;
    bool any = false;
    long first_line = 0;
    const char *hay = nc.norm;
    const char *q = hay;
    for (;;) {
        const char *hit = strstr(q, no.norm);
        if (hit == NULL) break;
        size_t idx = (size_t)(hit - hay);
        long line = 1;
        for (size_t i = 0; i < idx; i++) {
            if (hay[i] == '\n') line++;
        }
        if (!any) {
            any = true;
            first_line = line;
        }
        long dist = line > linefrom ? line - linefrom : linefrom - line;
        if (dist <= TOOLS_EDIT_TOLERANCE &&
            (!in_range || dist < best_dist || (dist == best_dist && line < best_line))) {
            in_range = true;
            best_dist = dist;
            best_line = line;
            best_idx = idx;
        }
        q = hit + 1;
    }

    char *result = NULL;
    if (!in_range) {
        if (any) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Edit refused: old_string found at line %ld", first_line);
            result = util_strdup(msg);
        } else {
            result = util_strdup("Edit refused: old_string not found");
        }
        edit_norm_free(&nc);
        edit_norm_free(&no);
        free(content);
        free(rel);
        return result;
    }

    /* Raw span of the match and its shape: whether it starts after only
     * indentation (a whole-line match, whose leading whitespace stays
     * in the file) and ends before only whitespace. A match whose
     * old_string ends with a newline consumes the line terminator
     * itself, which counts as ending the line. */
    size_t rs = nc.off[best_idx];
    size_t re = nc.off[best_idx + no.len - 1] + 1;
    bool bol = edit_starts_at_bol(content, rs);
    bool match_ends_nl = no.norm[no.len - 1] == '\n';
    bool eol = edit_ends_at_eol(content, content_len, re);
    if (match_ends_nl) eol = true;

    /* Re-anchoring delta: the display column where the match starts
     * minus old_string's own first-line indentation. A model that
     * quotes the file's indentation gets delta 0 (its new_string is
     * used as-is); a model that strips indentation gets the replacement
     * shifted onto the file's indentation. */
    size_t line_start = rs;
    while (line_start > 0 && content[line_start - 1] != '\n') line_start--;
    long ref_cols = edit_indent_cols(content + line_start, rs - line_start);
    size_t old_ws = edit_leading_ws_len(old_string, strlen(old_string));
    long delta = ref_cols - edit_indent_cols(old_string, old_ws);

    /* Only whole-line matches are re-indented: a match that starts
     * mid-line keeps new_string verbatim, and a match that ends
     * mid-line keeps its last replacement line verbatim so any text
     * after the match stays attached. */
    bool crlf = false;
    for (size_t i = 1; i < content_len; i++) {
        if (content[i] == '\n' && content[i - 1] == '\r') {
            crlf = true;
            break;
        }
    }

    int nlines = 0;
    edit_span *spans = edit_split_lines(new_string, strlen(new_string), &nlines);
    if (spans == NULL) {
        edit_norm_free(&nc);
        edit_norm_free(&no);
        free(content);
        free(rel);
        *out_err = util_strdup("Out of memory");
        return NULL;
    }

    const char *newline = "\n";
    if (crlf) newline = "\r\n";

    util_growbuf repl = {0};
    for (int i = 0; i < nlines; i++) {
        const char *lp = spans[i].p;
        size_t ll = spans[i].len;
        bool shift;
        if (!bol) {
            shift = false;
        } else if (nlines == 1) {
            shift = eol;
        } else if (i == 0) {
            shift = true;
        } else if (i == nlines - 1) {
            shift = eol;
        } else {
            shift = true;
        }

        if (i > 0) util_growbuf_append_str(&repl, newline);
        if (!shift) {
            util_growbuf_append(&repl, lp, ll);
            continue;
        }

        /* The first replacement line is spliced after the file line's
         * own indentation, so that much of its target indent is already
         * in place; later lines start fresh after a newline. */
        size_t ws = edit_leading_ws_len(lp, ll);
        long target = edit_indent_cols(lp, ws) + delta;
        if (target < 0) target = 0;
        long emit_cols = i == 0 ? target - ref_cols : target;
        if (emit_cols < 0) emit_cols = 0;
        for (long c = 0; c < emit_cols; c++) util_growbuf_append_str(&repl, " ");
        util_growbuf_append(&repl, lp + ws, ll - ws);
    }

    /* Whole-line matches splice at line boundaries, so no edit leaves a
     * half-consumed line behind: a deletion takes the line's indentation
     * and terminator with it, a replacement ending with a newline takes
     * over the line's own terminator, and a replacement replacing a
     * newline-terminated old_string without one of its own restores the
     * terminator. */
    bool repl_ends_nl = false;
    if (nlines >= 2 && spans[nlines - 1].len == 0) repl_ends_nl = true;
    if (bol && (eol || match_ends_nl)) {
        if (repl.len == 0) {
            rs = line_start;
            if (!match_ends_nl) {
                while (re < content_len &&
                       (content[re] == ' ' || content[re] == '\t' || content[re] == '\r')) {
                    re++;
                }
                if (re < content_len && content[re] == '\n') re++;
            }
        } else if (match_ends_nl && !repl_ends_nl) {
            util_growbuf_append_str(&repl, newline);
        } else if (!match_ends_nl && repl_ends_nl) {
            while (re < content_len &&
                   (content[re] == ' ' || content[re] == '\t' || content[re] == '\r')) {
                re++;
            }
            if (re < content_len && content[re] == '\n') re++;
        }
    }

    size_t repl_len = repl.len;
    size_t new_len = rs + repl_len + (content_len - re);
    char *updated = malloc(new_len + 1);
    if (updated == NULL) {
        free(spans);
        util_growbuf_free(&repl);
        edit_norm_free(&nc);
        edit_norm_free(&no);
        free(content);
        free(rel);
        *out_err = util_strdup("Out of memory");
        return NULL;
    }
    memcpy(updated, content, rs);
    if (repl_len > 0) memcpy(updated + rs, repl.buf, repl_len);
    memcpy(updated + rs + repl_len, content + re, content_len - re);
    updated[new_len] = '\0';

    bool failed = false;
    FILE *fp = fopen(rel, "wb");
    if (fp == NULL) {
        *out_err = tool_prefixed_err("file_edit", "cannot write '%s': %s", rel, strerror(errno));
        failed = true;
    } else {
        if (new_len > 0 && fwrite(updated, 1, new_len, fp) != new_len) failed = true;
        if (fclose(fp) != 0) failed = true;
        if (failed && *out_err == NULL) {
            *out_err = tool_prefixed_err("file_edit", "write to '%s' failed", rel);
        }
    }

    free(updated);
    free(spans);
    util_growbuf_free(&repl);
    edit_norm_free(&nc);
    edit_norm_free(&no);
    free(content);
    free(rel);

    if (failed) {
        free(result);
        return NULL;
    }
    result = util_strdup("Edit accepted");
    return result;
}

static int tool_file_edit(const cJSON *id_node, cJSON *args, char **out_resp) {
    *out_resp = NULL;

    cJSON *fp_j = args ? cJSON_GetObjectItem(args, "filepath") : NULL;
    const char *filepath = (fp_j && cJSON_IsString(fp_j)) ? fp_j->valuestring : "";
    cJSON *old_j = args ? cJSON_GetObjectItem(args, "old_string") : NULL;
    const char *old_string = (old_j && cJSON_IsString(old_j)) ? old_j->valuestring : "";
    cJSON *new_j = args ? cJSON_GetObjectItem(args, "new_string") : NULL;
    const char *new_string = (new_j && cJSON_IsString(new_j)) ? new_j->valuestring : "";
    if (filepath[0] == '\0') {
        *out_resp = srv_build_error(id_node, "file_edit requires a string 'filepath' argument");
        return EXIT_SUCCESS;
    }
    if (old_j == NULL || !cJSON_IsString(old_j)) {
        *out_resp = srv_build_error(id_node, "file_edit requires a string 'old_string' argument");
        return EXIT_SUCCESS;
    }
    if (new_j == NULL || !cJSON_IsString(new_j)) {
        *out_resp = srv_build_error(id_node, "file_edit requires a string 'new_string' argument");
        return EXIT_SUCCESS;
    }
    long linefrom = 0;
    if (!tool_int_arg(args, "linefrom", &linefrom) || linefrom < 1) {
        *out_resp =
            srv_build_error(id_node, "file_edit requires a positive integer 'linefrom' argument");
        return EXIT_SUCCESS;
    }

    char *err = NULL;
    char *text = tools_file_edit(filepath, linefrom, old_string, new_string, &err);
    if (text == NULL) {
        *out_resp = srv_build_error(id_node, err ? err : "Out of memory");
        free(err);
        return EXIT_SUCCESS;
    }
    cJSON *result = build_tool_text_result(text, false);
    free(text);
    if (result == NULL) return EXIT_INTERNAL_ERR;
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

typedef struct {
    const char *name;
    const char *description;
    char *(*schema)(void);
    int (*run)(const cJSON *id_node, cJSON *args, char **out_resp);
} builtin_tool;

static char *search_schema(void) {
    const char *props[] = {
        "query",
        "string",
        "search query string",
    };
    const char *required[] = {"query"};
    return build_tool_schema(props, 3, required, 1);
}

static char *fetch_schema(void) {
    const char *props[] = {
        "url",
        "string",
        "",
    };
    const char *required[] = {"url"};
    return build_tool_schema(props, 3, required, 1);
}

static char *file_scan_schema(void) {
    const char *props[] = {
        "filenames_glob",
        "string",
        "glob pattern resolved relative to the current working directory",
        "content_lines_regex",
        "string",
        "optional POSIX extended regex",
    };
    const char *required[] = {"filenames_glob"};
    return build_tool_schema(props, 6, required, 1);
}

static char *exec_schema(void) {
    const char *props[] = {
        "cmdline",
        "string",
        "shell command line",
    };
    const char *required[] = {"cmdline"};
    return build_tool_schema(props, 3, required, 1);
}

static char *exec_status_schema(void) {
    const char *props[] = {
        "pid",
        "integer",
        "",
    };
    const char *required[] = {"pid"};
    return build_tool_schema(props, 3, required, 1);
}

static char *sleep_schema(void) {
    const char *props[] = {
        "seconds",
        "number",
        "",
    };
    const char *required[] = {"seconds"};
    return build_tool_schema(props, 3, required, 1);
}

static char *file_read_schema(void) {
    const char *props[] = {
        "filepath",     "string",  "",
        "line_offset",  "integer", "1-based first line to read",
        "lines_length", "integer", "number of lines to read",
    };
    const char *required[] = {"filepath"};
    return build_tool_schema(props, 9, required, 1);
}

static char *file_create_schema(void) {
    const char *props[] = {
        "filepath", "string", "", "content", "string", "full content written to the file",
    };
    const char *required[] = {"filepath", "content"};
    return build_tool_schema(props, 6, required, 2);
}

static char *file_edit_schema(void) {
    const char *props[] = {
        "filepath",   "string",  "",
        "linefrom",   "integer", "1-based line where old_string is expected",
        "old_string", "string",  "text to replace",
        "new_string", "string",  "replacement text",
    };
    const char *required[] = {"filepath", "linefrom", "old_string", "new_string"};
    return build_tool_schema(props, 12, required, 4);
}

static const builtin_tool TOOLS[] = {
    {"online_search", SEARCH_DESC, search_schema, tool_online_search},
    {"online_fetch", FETCH_DESC, fetch_schema, tool_online_fetch},
    {"file_scan", SCAN_DESC, file_scan_schema, tool_file_scan},
    {"exec", EXEC_DESC, exec_schema, tool_exec},
    {"exec_status", EXEC_STATUS_DESC, exec_status_schema, tool_exec_status},
    {"sleep", SLEEP_DESC, sleep_schema, tool_sleep},
    {"file_read", FILE_READ_DESC, file_read_schema, tool_file_read},
    {"file_create", FILE_CREATE_DESC, file_create_schema, tool_file_create},
    {"file_edit", FILE_EDIT_DESC, file_edit_schema, tool_file_edit},
    {NULL, NULL, NULL, NULL},
};

/* Enabled set (parsed from the command-line list). Static because the
 * srv_handler_fn signature is process-wide, like the gateway's ctx. */
static char **g_enabled = NULL;
static int g_enabled_count = 0;

static const builtin_tool *find_tool(const char *name) {
    for (int i = 0; TOOLS[i].name != NULL; i++) {
        if (strcmp(TOOLS[i].name, name) == 0) return &TOOLS[i];
    }
    return NULL;
}

static int tool_enabled(const char *name) {
    for (int i = 0; i < g_enabled_count; i++) {
        if (strcmp(g_enabled[i], name) == 0) return 1;
    }
    return 0;
}

/* Add a tool to the enabled set unless it is already there (exec_status
 * is auto-added with exec, so an explicit listing must not trip the
 * duplicate check). Returns EXIT_ARGS_ERR on a user-typed duplicate. */
static int add_enabled_tool(const char *name) {
    if (tool_enabled(name)) return EXIT_ARGS_ERR;
    char **tmp = realloc(g_enabled, (size_t)(g_enabled_count + 1) * sizeof(*tmp));
    if (tmp == NULL) return EXIT_INTERNAL_ERR;
    g_enabled = tmp;
    char *copy = util_strdup(name);
    if (copy == NULL) return EXIT_INTERNAL_ERR;
    g_enabled[g_enabled_count++] = copy;
    return EXIT_SUCCESS;
}

static void print_available_tools(void) {
    fprintf(stderr, "available tools:");
    for (int i = 0; TOOLS_BUILTIN_NAMES[i] != NULL; i++) {
        fprintf(stderr, " %s", TOOLS_BUILTIN_NAMES[i]);
    }
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/*  MCP request handling (mirrors the gateway pattern)                 */
/* ------------------------------------------------------------------ */

static int tools_list(const cJSON *id_node, char **out_resp) {
    *out_resp = NULL;

    cJSON *tools = cJSON_CreateArray();
    if (tools == NULL) return EXIT_INTERNAL_ERR;

    int rc = EXIT_SUCCESS;
    for (int i = 0; TOOLS[i].name != NULL && rc == EXIT_SUCCESS; i++) {
        if (!tool_enabled(TOOLS[i].name)) continue;
        char *schema_str = TOOLS[i].schema();
        cJSON *schema = cJSON_Parse(schema_str ? schema_str : "{}");
        if (schema == NULL) schema = cJSON_CreateObject();
        cJSON *t = cJSON_CreateObject();
        if (t == NULL || schema == NULL) {
            cJSON_Delete(t);
            cJSON_Delete(schema);
            free(schema_str);
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].description);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
        free(schema_str);
    }
    if (rc != EXIT_SUCCESS) {
        cJSON_Delete(tools);
        return rc;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(tools);
        return EXIT_INTERNAL_ERR;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

static int tools_handle_request(runtime_ctx *ctx, const char *req_json, char **out_resp) {
    (void)ctx;
    *out_resp = NULL;

    cJSON *req = cJSON_Parse(req_json);
    if (req == NULL) {
        *out_resp = srv_build_error(NULL, "Parse error");
        return EXIT_SUCCESS;
    }

    cJSON *id_j = cJSON_GetObjectItem(req, "id");
    cJSON *method_j = cJSON_GetObjectItem(req, "method");
    if (method_j == NULL || !cJSON_IsString(method_j)) {
        *out_resp = srv_build_error(id_j, "Method not specified");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }
    const char *method = method_j->valuestring;

    if (strcmp(method, "initialize") == 0) {
        cJSON *result = cJSON_CreateObject();
        if (result == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
        cJSON *cap = cJSON_AddObjectToObject(result, "capabilities");
        if (cap != NULL) {
            cJSON_AddObjectToObject(cap, "tools");
        }
        cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
        if (info != NULL) {
            cJSON_AddStringToObject(info, "name", TOOLS_SERVER_NAME);
            cJSON_AddStringToObject(info, "version", LLMKIT_VERSION);
        }
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* Notifications are answered with nothing at all. */
    if (strncmp(method, "notifications/", 14) == 0) {
        *out_resp = util_strdup("");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    if (strcmp(method, "ping") == 0) {
        *out_resp = srv_build_success(id_j, "{}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    if (strcmp(method, "tools/list") == 0) {
        int rc = tools_list(id_j, out_resp);
        cJSON_Delete(req);
        return rc;
    }

    if (strcmp(method, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *name_j = params ? cJSON_GetObjectItem(params, "name") : NULL;
        cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        const char *tc_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";

        const builtin_tool *tool = find_tool(tc_name);
        if (tool == NULL || !tool_enabled(tc_name)) {
            *out_resp = srv_build_error(id_j, "Unknown tool");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }
        int rc = tool->run(id_j, args, out_resp);
        cJSON_Delete(req);
        return rc;
    }

    if (strcmp(method, "resources/list") == 0) {
        *out_resp = srv_build_success(id_j, "{\"resources\":[]}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }
    if (strcmp(method, "prompts/list") == 0) {
        *out_resp = srv_build_success(id_j, "{\"prompts\":[]}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    *out_resp = srv_build_error(id_j, "Method not supported");
    cJSON_Delete(req);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  tools_run                                                          */
/* ------------------------------------------------------------------ */

/* Free the enabled set. Called on re-entry and before returning, so a
 * failed parse never leaves entries behind for the next call. */
static void reset_enabled(void) {
    for (int i = 0; i < g_enabled_count; i++) free(g_enabled[i]);
    free(g_enabled);
    g_enabled = NULL;
    g_enabled_count = 0;
}

int tools_run(const char *tool_list, const char *listen_addr) {
    reset_enabled();

    if (tool_list == NULL || tool_list[0] == '\0') {
        fprintf(stderr, "error: mcp requires a comma-separated tool list\n");
        print_available_tools();
        return EXIT_ARGS_ERR;
    }

    /* Parse and validate the enabled set. */
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    const char *p = tool_list;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p != '\0' && *p != ',') p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
        if (len == 0) continue;

        char *name = malloc(len + 1);
        if (name == NULL) {
            fprintf(stderr, "error: out of memory\n");
            return EXIT_INTERNAL_ERR;
        }
        memcpy(name, start, len);
        name[len] = '\0';

        if (find_tool(name) == NULL) {
            fprintf(stderr, "error: unknown mcp tool '%s'\n", name);
            print_available_tools();
            free(name);
            return EXIT_ARGS_ERR;
        }
        int rc = add_enabled_tool(name);
        if (rc == EXIT_ARGS_ERR) {
            fprintf(stderr, "error: duplicate tool '%s' in list\n", name);
            free(name);
            return EXIT_ARGS_ERR;
        }
        if (rc != EXIT_SUCCESS) {
            fprintf(stderr, "error: out of memory\n");
            free(name);
            return EXIT_INTERNAL_ERR;
        }
        free(name);
    }

    /* exec_status is exec's companion: polling a background command needs
     * it, so enabling exec enables both. */
    if (tool_enabled("exec") && !tool_enabled("exec_status")) {
        if (add_enabled_tool("exec_status") != EXIT_SUCCESS) {
            fprintf(stderr, "error: out of memory\n");
            return EXIT_INTERNAL_ERR;
        }
    }

    if (g_enabled_count == 0) {
        fprintf(stderr, "error: mcp requires a comma-separated tool list\n");
        print_available_tools();
        return EXIT_ARGS_ERR;
    }

    {
        util_growbuf enabled_list = {0};
        for (int i = 0; i < g_enabled_count; i++) {
            util_growbuf_append_str(&enabled_list, g_enabled[i]);
            if (i + 1 < g_enabled_count) util_growbuf_append_str(&enabled_list, ", ");
        }
        char *joined = util_growbuf_release(&enabled_list);
        log_activity("[init] Built-in tools: %s", joined ? joined : "");
        free(joined);
    }

    int rc = srv_serve(&ctx, listen_addr, tools_handle_request, "Tools");

    reset_enabled();
    return rc;
}
