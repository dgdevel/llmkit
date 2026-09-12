#include "tools.h"
#include "htmlmd.h"
#include "jsonrpc.h"
#include "srv.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* A browser User-Agent: the DDG HTML endpoint (and many sites) reject
 * requests without one. */
#define TOOLS_USER_AGENT                                                      \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/124.0.0.0 Safari/537.36"

const char *const TOOLS_BUILTIN_NAMES[] = {"online_search", "online_fetch", NULL};

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
/*  Registry                                                           */
/* ------------------------------------------------------------------ */

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

static const builtin_tool TOOLS[] = {
    {"online_search", SEARCH_DESC, search_schema, tool_online_search},
    {"online_fetch", FETCH_DESC, fetch_schema, tool_online_fetch},
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

int tools_run(const char *tool_list, const char *listen_addr) {
    if (tool_list == NULL || tool_list[0] == '\0') {
        fprintf(stderr, "error: mcp requires a comma-separated tool list\n"
                        "available tools: online_search, online_fetch\n");
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
            fprintf(stderr, "error: unknown mcp tool '%s'\navailable tools:", name);
            for (int i = 0; TOOLS_BUILTIN_NAMES[i] != NULL; i++) {
                fprintf(stderr, " %s", TOOLS_BUILTIN_NAMES[i]);
            }
            fprintf(stderr, "\n");
            free(name);
            return EXIT_ARGS_ERR;
        }
        for (int i = 0; i < g_enabled_count; i++) {
            if (strcmp(g_enabled[i], name) == 0) {
                fprintf(stderr, "error: duplicate tool '%s' in list\n", name);
                free(name);
                return EXIT_ARGS_ERR;
            }
        }

        char **tmp = realloc(g_enabled, (size_t)(g_enabled_count + 1) * sizeof(*tmp));
        if (tmp == NULL) {
            fprintf(stderr, "error: out of memory\n");
            free(name);
            return EXIT_INTERNAL_ERR;
        }
        g_enabled = tmp;
        g_enabled[g_enabled_count++] = name;
    }

    if (g_enabled_count == 0) {
        fprintf(stderr, "error: mcp requires a comma-separated tool list\n"
                        "available tools: online_search, online_fetch\n");
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

    for (int i = 0; i < g_enabled_count; i++) free(g_enabled[i]);
    free(g_enabled);
    g_enabled = NULL;
    g_enabled_count = 0;
    return rc;
}
