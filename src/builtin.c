/* builtin.c - llmkit builtin-mcp: a stdio mcp server offering generic-use
   tools (web search/fetch, file list/search/read/create/edit). No state,
   no config: `llmkit builtin-mcp` serves json-rpc on stdin/stdout. All
   tool and argument descriptions start empty and live in the DESCRIPTIONS
   block right below - edit the strings there. */
#include "llmkit.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif
#define BM_ACCESS _access
#define BM_MKDIR(p) _mkdir(p)
#define BM_LSTAT(p, s) stat((p), (s))
#else
#include <unistd.h>
#define BM_ACCESS access
#define BM_MKDIR(p) mkdir((p), 0777)
#define BM_LSTAT lstat
#endif

/* posix regcomp for the file tool patterns. mingw ships no <regex.h>:
   there the vendored openbsd implementation is compiled in (src/
   vendor/regex, added by the Makefile's mingw detection, its libc shims
   force-included from src/win_regex_glue.h) and this include reaches
   its header directly. */
#ifdef _WIN32
#include "vendor/regex/regex.h"
#else
#include <regex.h>
#endif

/* ===================================================================== */
/* ==== DESCRIPTIONS - the desc column of the tool tables below is ===== */
/* ==== the one place to edit them. Every description starts empty  ==== */
/* ==== (""): an empty string is sent as an empty description, a    ==== */
/* ==== nonempty one verbatim.                                      ==== */
/* ===================================================================== */

/* ================= tool table ================= */

typedef struct arg_def {
    const char *name;
    const char *type;   /* "string" | "integer" | "boolean" */
    const char *desc;
    bool required;
} arg_def_t;

typedef struct tool_def {
    const char *name;
    const char *desc;
    const arg_def_t *args;
    size_t nargs;
} tool_def_t;

static const arg_def_t ARGS_web_search[] = {
    {"keywords", "string", "", true},
};
static const arg_def_t ARGS_web_fetch[] = {
    {"url", "string", "", true},
};
static const arg_def_t ARGS_files_list[] = {
    {"path", "string", "", true},
    {"regex", "string", "", true},
};
static const arg_def_t ARGS_files_search[] = {
    {"path", "string", "", true},
    {"regex", "string", "", true},
};
static const arg_def_t ARGS_file_read[] = {
    {"path", "string", "", true},
    {"lines_offset", "integer", "", true},
    {"lines_length", "integer", "", true},
};
static const arg_def_t ARGS_file_create[] = {
    {"path", "string", "", true},
    {"content", "string", "", true},
    {"overwrite", "boolean", "", false},
};
static const arg_def_t ARGS_file_edit[] = {
    {"path", "string", "", true},
    {"oldString", "string", "", true},
    {"newString", "string", "", true},
    {"line_number", "integer", "", true},
};

static const tool_def_t TOOLS[] = {
    {"web_search", "", ARGS_web_search, 1},
    {"web_fetch", "", ARGS_web_fetch, 1},
    {"files_list", "", ARGS_files_list, 2},
    {"files_search", "", ARGS_files_search, 2},
    {"file_read", "", ARGS_file_read, 3},
    {"file_create", "", ARGS_file_create, 3},
    {"file_edit", "", ARGS_file_edit, 4},
};
enum { TOOLS_N = sizeof TOOLS / sizeof TOOLS[0] };

/* ================= small utilities ================= */

/* case-insensitive substring test */
static bool ci_contains(const char *hay, const char *needle) {
    if (!hay) return false;
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k] &&
               tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return true;
        if (!p[0]) break;
    }
    return false;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

static size_t lead_ws(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && is_ws(s[i])) i++;
    return i;
}

static size_t trail_ws(const char *s, size_t n) {
    size_t i = n;
    while (i > 0 && is_ws(s[i - 1])) i--;
    return n - i;
}

/* collapse whitespace runs to single spaces, in place over a buf */
static void collapse_ws(buf_t *b) {
    if (!b->len) return;
    size_t w = 0;
    for (size_t r = 0; r < b->len;) {
        if (is_ws(b->data[r])) {
            while (r < b->len && is_ws(b->data[r])) r++;
            if (w) b->data[w++] = ' ';
        } else {
            b->data[w++] = b->data[r++];
        }
    }
    if (w && b->data[w - 1] == ' ') w--;
    b->len = w;
    b->data[w] = '\0';
}

static void utf8_put(buf_t *b, unsigned long cp) {
    if (cp < 0x80) {
        buf_append_byte(b, (char)cp);
    } else if (cp < 0x800) {
        buf_append_byte(b, (char)(0xC0 | (cp >> 6)));
        buf_append_byte(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_append_byte(b, (char)(0xE0 | (cp >> 12)));
        buf_append_byte(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_append_byte(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_append_byte(b, (char)(0xF0 | (cp >> 18)));
        buf_append_byte(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_append_byte(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_append_byte(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* html entity decoding for a byte range */
static const struct {
    const char *name;
    unsigned long cp;
} ENTITIES[] = {
    {"amp", 0x26},   {"lt", 0x3C},       {"gt", 0x3E},     {"quot", 0x22},
    {"apos", 0x27},  {"nbsp", 0xA0},     {"copy", 0xA9},   {"reg", 0xAE},
    {"trade", 0x2122},{"hellip", 0x2026},{"mdash", 0x2014},{"ndash", 0x2013},
    {"lsquo", 0x2018},{"rsquo", 0x2019}, {"ldquo", 0x201C},{"rdquo", 0x201D},
    {"laquo", 0xAB}, {"raquo", 0xBB},    {"deg", 0xB0},    {"plusmn", 0xB1},
    {"middot", 0xB7},{"bull", 0x2022},  {"dagger", 0x2020},{"euro", 0x20AC},
    {"pound", 0xA3}, {"yen", 0xA5},      {"cent", 0xA2},   {"sect", 0xA7},
    {"para", 0xB6},  {"times", 0xD7},    {"divide", 0xF7}, {"frac12", 0xBD},
    {"sup2", 0xB2},  {"sup3", 0xB3},     {"micro", 0xB5},
};
enum { ENTITIES_N = sizeof ENTITIES / sizeof ENTITIES[0] };

static void ent_decode(buf_t *out, const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        if (s[i] != '&') {
            buf_append_byte(out, s[i++]);
            continue;
        }
        /* find ';' within a sane window */
        size_t end = i + 1, lim = i + 12;
        if (lim > n) lim = n;
        while (end < lim && s[end] != ';') end++;
        if (end >= lim || end == i + 1) { /* no entity: literal '&' */
            buf_append_byte(out, s[i++]);
            continue;
        }
        size_t elen = end - i - 1; /* between & and ; */
        const char *e = s + i + 1;
        if (elen > 1 && e[0] == '#') {
            unsigned long cp = 0;
            bool ok = true;
            if (elen > 2 && (e[1] == 'x' || e[1] == 'X')) {
                for (size_t k = 2; k < elen; k++) {
                    int d = isdigit((unsigned char)e[k])
                                ? e[k] - '0'
                                : (tolower((unsigned char)e[k]) - 'a' + 10);
                    if (d < 0 || d > 15) { ok = false; break; }
                    cp = cp * 16 + (unsigned long)d;
                }
            } else {
                for (size_t k = 1; k < elen; k++) {
                    if (!isdigit((unsigned char)e[k])) { ok = false; break; }
                    cp = cp * 10 + (unsigned long)(e[k] - '0');
                }
            }
            if (ok && cp <= 0x10FFFF) utf8_put(out, cp);
            else buf_append_byte(out, '?');
        } else {
            bool found = false;
            for (size_t k = 0; k < ENTITIES_N; k++)
                if (strlen(ENTITIES[k].name) == elen &&
                    !strncasecmp(ENTITIES[k].name, e, elen)) {
                    utf8_put(out, ENTITIES[k].cp);
                    found = true;
                    break;
                }
            if (!found) buf_append_byte(out, '?');
        }
        i = end + 1;
    }
}

/* percent-encoding for one query component (space -> %20) */
static void url_encode(buf_t *out, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            buf_append_byte(out, (char)*p);
        else
            buf_appendf(out, "%%%02X", *p);
    }
}

/* percent-decoding of a byte range ('+' becomes space: query component) */
static void pct_decode(buf_t *out, const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        if (s[i] == '%' && i + 2 < n &&
            isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            int hi = isdigit((unsigned char)s[i + 1]) ? s[i + 1] - '0'
                                                      : tolower(s[i + 1]) - 'a' + 10;
            int lo = isdigit((unsigned char)s[i + 2]) ? s[i + 2] - '0'
                                                      : tolower(s[i + 2]) - 'a' + 10;
            buf_append_byte(out, (char)(hi * 16 + lo));
            i += 3;
        } else if (s[i] == '+') {
            buf_append_byte(out, ' ');
            i++;
        } else {
            buf_append_byte(out, s[i++]);
        }
    }
}

/* ================= regex (posix ere + \d\w\s sugar) ================= */
/* the file tools match posix ere via the libc regcomp (the windows build
   compiles openbsd's implementation, vendored in src/vendor/regex). the
   sugar \d \w \s, their negations and the escapes \n \t \r \f \v are
   translated below into plain ere so the semantics stay byte-oriented
   whatever the locale, and (?:...) becomes (...) because ere has no
   non-capturing groups. no step budget: a pathological pattern can be
   slow, accepted. */

static const char RX_WS[] = " \t\r\n\f\v";

/* translate the sugar into ere; returns a malloc'd pattern. class
   members are buffered so that ']' members can be re-emitted first:
   []...] is the only posix-portable spelling of a class containing ']'
   (glibc accepts \], posix does not). */
static char *rx_ere(const char *pat) {
    buf_t b;
    buf_init(&b);
    bool in_cls = false, first = false, neg = false;
    buf_t head, cls; /* ']' members / the rest, in order */
    buf_init(&head);
    buf_init(&cls);
    for (const char *p = pat; *p; p++) {
        char c = *p;
        if (!in_cls) {
            if (c == '[') {
                in_cls = true;
                first = true;
                neg = false;
                buf_clear(&head);
                buf_clear(&cls);
                continue; /* emitted when the class closes */
            } else if (c == '(' && p[1] == '?' && p[2] == ':') {
                buf_append_byte(&b, '(');
                p += 2;
                continue;
            } else if (c == '\\' && p[1]) {
                char e = p[1];
                if (e == 'd') buf_append_str(&b, "[0-9]");
                else if (e == 'D') buf_append_str(&b, "[^0-9]");
                else if (e == 'w') buf_append_str(&b, "[A-Za-z0-9_]");
                else if (e == 'W') buf_append_str(&b, "[^A-Za-z0-9_]");
                else if (e == 's') buf_appendf(&b, "[%s]", RX_WS);
                else if (e == 'S') buf_appendf(&b, "[^%s]", RX_WS);
                else if (e == 'n') buf_append_byte(&b, '\n');
                else if (e == 't') buf_append_byte(&b, '\t');
                else if (e == 'r') buf_append_byte(&b, '\r');
                else if (e == 'f') buf_append_byte(&b, '\f');
                else if (e == 'v') buf_append_byte(&b, '\v');
                else buf_appendf(&b, "\\%c", e); /* escaped metachar */
                p++;
                continue;
            }
            buf_append_byte(&b, c);
            continue;
        }
        /* inside [...]: posix [[:alpha:]] classes pass through untouched */
        if (c == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e) {
                buf_append(&cls, p, (size_t)(e - p) + 2);
                p = e + 1;
                first = false;
                continue;
            }
        }
        if (c == ']' && !first) { /* close the class */
            buf_append_byte(&b, '[');
            if (neg) buf_append_byte(&b, '^');
            buf_append(&b, head.data ? head.data : "", head.len);
            buf_append(&b, cls.data ? cls.data : "", cls.len);
            buf_append_byte(&b, ']');
            in_cls = false;
        } else if (c == '\\' && p[1]) {
            char e = *++p;
            if (e == 'd') buf_append_str(&cls, "0-9");
            else if (e == 'w') buf_append_str(&cls, "A-Za-z0-9_");
            else if (e == 's') buf_append_str(&cls, RX_WS);
            else if (e == ']') buf_append_byte(&head, ']');
            else if (e == '\\' || e == '^' || e == '-')
                buf_appendf(&cls, "\\%c", e);
            else buf_append_byte(&cls, (unsigned char)e); /* plain member */
            first = false;
        } else if (c == '^' && first && !neg) {
            neg = true; /* ']' right after stays a literal member */
        } else {
            if (c == ']') buf_append_byte(&head, ']'); /* []...] */
            else buf_append_byte(&cls, c);
            first = false;
        }
    }
    if (in_cls) { /* unterminated class: emit the parts, regcomp reports */
        buf_append_byte(&b, '[');
        if (neg) buf_append_byte(&b, '^');
        buf_append(&b, head.data ? head.data : "", head.len);
        buf_append(&b, cls.data ? cls.data : "", cls.len);
    }
    buf_free(&head);
    buf_free(&cls);
    if (!b.len) buf_append_str(&b, ".*"); /* empty pattern matches all */
    return buf_steal(&b, NULL);
}

/* compile a pattern for the file tools; false + err on invalid syntax */
static bool rx_compile(regex_t *rx, const char *pattern, char *err,
                       size_t errsz) {
    char *ere = rx_ere(pattern);
    int rc = regcomp(rx, ere, REG_EXTENDED | REG_NOSUB);
    if (rc) {
        char rxe[128];
        regerror(rc, rx, rxe, sizeof rxe);
        snprintf(err, errsz, "invalid regex: %s", rxe);
    }
    free(ere);
    return rc == 0;
}

/* regexec wants a nul-terminated subject: match a byte range */
static bool rx_match(const regex_t *rx, const char *s, size_t n) {
    char *z = malloc(n + 1);
    if (!z) return false;
    memcpy(z, s, n);
    z[n] = '\0';
    bool m = regexec(rx, z, 0, NULL, 0) == 0;
    free(z);
    return m;
}

bool builtin_regex_match(const char *pattern, const char *text, char *err,
                         size_t errsz) {
    if (err && errsz) err[0] = '\0';
    regex_t rx;
    if (!rx_compile(&rx, pattern, err, errsz)) return false;
    bool m = rx_match(&rx, text, strlen(text));
    regfree(&rx);
    return m;
}


/* ================= filesystem helpers ================= */

#define BM_PATH_MAX 4096
#define BM_FILE_CAP (64ull * 1024 * 1024)        /* per-file read cap */
#define BM_SEARCH_CAP (16ull * 1024 * 1024)      /* content-search cap */

static void perms_of(const char *path, char out[4]) {
    out[0] = BM_ACCESS(path, R_OK) == 0 ? 'r' : '-';
    out[1] = BM_ACCESS(path, W_OK) == 0 ? 'w' : '-';
    out[2] = BM_ACCESS(path, X_OK) == 0 ? 'x' : '-';
    out[3] = '\0';
}

/* human readable byte size: 512b, 5Kb, 5.4Kb, 15Kb, 1Mb, 1.2Mb ... */
static void human_size(unsigned long long v, char *out, size_t sz) {
    static const char *un[] = { "b", "Kb", "Mb", "Gb", "Tb" };
    if (v < 1024) {
        snprintf(out, sz, "%llub", v);
        return;
    }
    int u = 0;
    double x = (double)v;
    while (x >= 1024.0 && u < 4) {
        x /= 1024.0;
        u++;
    }
    if (x < 10.0) {
        unsigned long long d = (unsigned long long)(x * 10.0 + 0.5);
        if (d % 10 == 0)
            snprintf(out, sz, "%llu%s", d / 10, un[u]);
        else
            snprintf(out, sz, "%llu.%llu%s", d / 10, d % 10, un[u]);
    } else {
        snprintf(out, sz, "%llu%s", (unsigned long long)(x + 0.5), un[u]);
    }
}

/* whole-file read with a size cap. 0 ok, -1 io error, -2 too large */
static int read_whole(const char *path, buf_t *out, unsigned long long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char tmp[16384];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (out->len + (unsigned long long)n > cap) {
            fclose(f);
            return -2;
        }
        buf_append(out, tmp, n);
    }
    bool bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* textual + line count of a file: strict utf-8 without NUL bytes */
typedef struct tscan {
    bool ok;     /* file could be read */
    bool textual;
    unsigned long long lines;
} tscan_t;

static tscan_t scan_text(const char *path, unsigned long long cap) {
    tscan_t r = { false, false, 0 };
    buf_t b;
    buf_init(&b);
    int rc = read_whole(path, &b, cap);
    if (rc != 0) {
        buf_free(&b);
        r.ok = rc == -1 ? false : true; /* oversized: readable, not textual */
        return r;
    }
    r.ok = true;
    r.textual = !memchr(b.data, 0, b.len) &&
                utf8_valid((const uint8_t *)b.data, b.len);
    if (r.textual) {
        r.lines = 0;
        for (size_t i = 0; i < b.len; i++)
            if (b.data[i] == '\n') r.lines++;
        if (b.len && b.data[b.len - 1] != '\n') r.lines++;
    }
    buf_free(&b);
    return r;
}


static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---- listing walk ---- */

typedef struct walk_ctx {
    buf_t *out;
    regex_t rx;   /* compiled pattern */
    bool content; /* files_search: pattern matches content lines */
} walk_ctx_t;

static void entry_line(buf_t *out, const char *path, const struct stat *st,
                       unsigned long long lines, bool has_lines) {
    char perm[4], hs[48];
    perms_of(path, perm);
    human_size((unsigned long long)st->st_size, hs, sizeof hs);
    buf_appendf(out, "%s %s", perm, hs);
    if (has_lines) buf_appendf(out, ", %llu lines", lines);
    buf_appendf(out, " %s\n", path);
}

static void emit_entry(walk_ctx_t *w, const char *path,
                       const struct stat *st) {
    if (!w->content && !rx_match(&w->rx, path, strlen(path))) return;
    if (S_ISDIR(st->st_mode)) {
        if (!w->content) entry_line(w->out, path, st, 0, false);
        return;
    }
    if (w->content) {
        /* content search: read, validate, collect matching line numbers */
        buf_t b;
        buf_init(&b);
        int rc = read_whole(path, &b, BM_SEARCH_CAP);
        if (rc != 0 || memchr(b.data ? b.data : "", 0, b.len) ||
            !utf8_valid((const uint8_t *)(b.data ? b.data : ""), b.len)) {
            buf_free(&b);
            return;
        }
        /* count lines + test each */
        unsigned long long lno = 0, nlines = 0;
        buf_t nums;
        buf_init(&nums);
        bool any = false;
        size_t start = 0;
        for (size_t i = 0; i < b.len; i++) {
            if (b.data[i] == '\n') {
                size_t len = i - start;
                if (len && b.data[start + len - 1] == '\r') len--;
                lno++;
                if (rx_match(&w->rx, b.data + start, len)) {
                    if (any) buf_append_str(&nums, ", ");
                    buf_appendf(&nums, "%llu", lno);
                    any = true;
                }
                nlines++;
                start = i + 1;
            }
        }
        if (start < b.len) { /* final line without terminator */
            size_t len = b.len - start;
            if (len && b.data[start + len - 1] == '\r') len--;
            lno++;
            if (rx_match(&w->rx, b.data + start, len)) {
                if (any) buf_append_str(&nums, ", ");
                buf_appendf(&nums, "%llu", lno);
                any = true;
            }
            nlines++;
        }
        if (any) {
            entry_line(w->out, path, st, nlines, true);
            buf_appendf(w->out, "Matching lines: %s\n", nums.data ? nums.data : "");
        }
        buf_free(&nums);
        buf_free(&b);
        return;
    }
    tscan_t ts = scan_text(path, BM_FILE_CAP);
    entry_line(w->out, path, st, ts.lines, ts.ok && ts.textual);
}

static void walk_dir(walk_ctx_t *w, const char *dirpath, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dirpath);
    if (!d) return;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_names);
    for (size_t i = 0; i < n; i++) {
        char child[BM_PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", dirpath, names[i]) >=
            (int)sizeof child) {
            free(names[i]);
            continue;
        }
        struct stat st;
        if (!BM_LSTAT(child, &st)) {
            emit_entry(w, child, &st);
            if (S_ISDIR(st.st_mode)) walk_dir(w, child, depth + 1);
        }
        free(names[i]);
    }
    free(names);
}

/* top-level entry: root dir walks, root file lists once */
static bool walk_start(walk_ctx_t *w, const char *root, char *err,
                       size_t errsz) {
    struct stat st;
    if (stat(root, &st)) {
        snprintf(err, errsz, "cannot access '%s': %s", root, strerror(errno));
        return false;
    }
    if (S_ISDIR(st.st_mode)) walk_dir(w, root, 0);
    else emit_entry(w, root, &st);
    return true;
}

/* mkdir -p for the parent directory of a path */
static bool ensure_parent_dirs(const char *path) {
    char tmp[BM_PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return false;
    char *slash = strrchr(tmp, '/');
    if (!slash) return true; /* cwd */
    *slash = '\0';
    if (!tmp[0]) return true; /* "/file" root */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            BM_MKDIR(tmp);
            *p = '/';
        }
    }
    return BM_MKDIR(tmp) == 0 || errno == EEXIST;
}

/* ================= html dom (readability input) ================= */

typedef struct hnode hnode_t;
struct hnode {
    hnode_t *parent, *child, *tail, *next;
    char tag[12]; /* lowercase element name; "#r" for the root */
    char *text;   /* text nodes (tag "") own decoded text */
    char *cls, *id, *href, *src, *alt;
    double score;
};

typedef struct hdom {
    hnode_t *root;
    hnode_t **all; /* every node for iteration and teardown */
    size_t n, cap;
    bool oom;
} hdom_t;

static hnode_t *hnew(hdom_t *d, hnode_t *parent) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        d->all = realloc(d->all, d->cap * sizeof *d->all);
    }
    hnode_t *n = calloc(1, sizeof *n);
    if (!n) {
        d->oom = true;
        return NULL;
    }
    n->parent = parent;
    d->all[d->n++] = n;
    if (parent) {
        if (parent->tail) parent->tail->next = n;
        else parent->child = n;
        parent->tail = n;
    }
    return n;
}

static hnode_t *hadd_text(hdom_t *d, hnode_t *parent, const char *s,
                          size_t len) {
    hnode_t *n = hnew(d, parent);
    if (!n) return NULL;
    buf_t b;
    buf_init(&b);
    ent_decode(&b, s, len);
    n->text = buf_steal(&b, NULL);
    return n;
}

static bool tag_in(const char *tag, const char *const *set, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(tag, set[i])) return true;
    return false;
}

static const char *const VOID_TAGS[] = { "area", "base", "br",  "col",
                                         "embed", "hr",   "img", "input",
                                         "link",  "meta", "param", "source",
                                         "track", "wbr" };
static const char *const RAW_TAGS[] = { "script", "style", "noscript",
                                        "template" };
static const char *const JUNK_TAGS[] = {
    "script", "style",   "noscript", "template", "svg",   "iframe", "form",
    "button", "input",   "select",   "textarea", "option", "object", "embed",
    "link",   "meta",    "base",     "canvas",   "video", "audio",  "track",
    "source"
};
static const char *const BLOCK_TAGS[] = {
    "p",      "div",   "ul",        "ol",   "li",   "table", "thead", "tbody",
    "tfoot",  "tr",    "td",        "th",   "h1",   "h2",    "h3",    "h4",
    "h5",     "h6",    "blockquote", "pre", "section", "article", "header",
    "footer", "nav",   "aside",     "main", "form", "fieldset", "hr", "figure",
    "figcaption", "dl", "dt",       "dd",   "address", "details", "summary"
};

enum {
    VOID_N = sizeof VOID_TAGS / sizeof VOID_TAGS[0],
    RAW_N = sizeof RAW_TAGS / sizeof RAW_TAGS[0],
    JUNK_N = sizeof JUNK_TAGS / sizeof JUNK_TAGS[0],
    BLOCK_N = sizeof BLOCK_TAGS / sizeof BLOCK_TAGS[0]
};

/* should an open 'cur' close when 'new' opens? */
static bool closes_on(const char *cur, const char *nw) {
    if (!strcmp(cur, "p") && tag_in(nw, BLOCK_TAGS, BLOCK_N)) return true;
    if (!strcmp(cur, "li") && !strcmp(nw, "li")) return true;
    if ((!strcmp(cur, "dt") || !strcmp(cur, "dd")) &&
        (!strcmp(nw, "dt") || !strcmp(nw, "dd")))
        return true;
    if ((!strcmp(cur, "td") || !strcmp(cur, "th")) &&
        (!strcmp(nw, "td") || !strcmp(nw, "th") || !strcmp(nw, "tr")))
        return true;
    if (!strcmp(cur, "tr") && !strcmp(nw, "tr")) return true;
    if (!strcmp(cur, "option") && !strcmp(nw, "option")) return true;
    if ((!strcmp(cur, "thead") || !strcmp(cur, "tbody") || !strcmp(cur, "tfoot")) &&
        (!strcmp(nw, "thead") || !strcmp(nw, "tbody") || !strcmp(nw, "tfoot")))
        return true;
    return false;
}

static size_t ci_find_close(const char *s, size_t i, size_t n,
                            const char *tag) {
    /* find "</tag" case-insensitively at or after i; s is NUL-terminated */
    size_t tl = strlen(tag);
    for (size_t k = i; k + tl + 2 <= n && s[k]; k++) {
        if (s[k] == '<' && s[k + 1] == '/' &&
            !strncasecmp(s + k + 2, tag, tl))
            return k;
    }
    return (size_t)-1;
}

static void dom_parse(hdom_t *d, const char *html, size_t n) {
    memset(d, 0, sizeof *d);
    char *s = malloc(n + 1);
    if (!s) {
        d->oom = true;
        return;
    }
    memcpy(s, html, n);
    s[n] = '\0';
    size_t len = 0;
    while (len < n && s[len]) len++; /* stop at any embedded NUL */
    d->root = hnew(d, NULL);
    if (!d->root) {
        free(s);
        return;
    }
    strcpy(d->root->tag, "#r");
    hnode_t *cur = d->root;
    size_t i = 0;
    while (i < len && !d->oom) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < len && s[j] != '<') j++;
            hadd_text(d, cur, s + i, j - i);
            i = j;
            continue;
        }
        if (len - i >= 4 && !strncmp(s + i, "<!--", 4)) {
            const char *e = strstr(s + i + 4, "-->");
            i = e ? (size_t)(e - s) + 3 : len;
            continue;
        }
        if (s[i + 1] == '!' || s[i + 1] == '?') {
            const char *e = strchr(s + i, '>');
            i = e ? (size_t)(e - s) + 1 : len;
            continue;
        }
        if (s[i + 1] == '/') {
            /* closing tag: unwind to the matching open element */
            size_t j = i + 2;
            char name[12];
            size_t nl = 0;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-') &&
                   nl + 1 < sizeof name)
                name[nl++] = (char)tolower((unsigned char)s[j++]);
            name[nl] = '\0';
            const char *e = strchr(s + j, '>');
            i = e ? (size_t)(e - s) + 1 : len;
            if (!nl || !strcmp(name, "#r")) continue;
            for (hnode_t *p = cur; p && p != d->root; p = p->parent)
                if (!strcmp(p->tag, name)) {
                    cur = p->parent;
                    break;
                }
            continue;
        }
        if (!isalpha((unsigned char)s[i + 1])) {
            /* stray '<': text */
            hadd_text(d, cur, s + i, 1);
            i++;
            continue;
        }
        /* opening tag */
        size_t j = i + 1;
        char name[12];
        size_t nl = 0;
        while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-') &&
               nl + 1 < sizeof name)
            name[nl++] = (char)tolower((unsigned char)s[j++]);
        name[nl] = '\0';
        /* attributes until '>' */
        char *cls = NULL, *id = NULL, *href = NULL, *src = NULL, *alt = NULL;
        bool selfclose = false;
        for (;;) {
            while (j < len && is_ws(s[j])) j++;
            if (j >= len) break;
            if (s[j] == '>') {
                j++;
                break;
            }
            if (s[j] == '/') {
                selfclose = true;
                j++;
                continue;
            }
            char an[16];
            size_t al = 0;
            while (j < len && !is_ws(s[j]) && s[j] != '=' && s[j] != '>' &&
                   s[j] != '/' && al + 1 < sizeof an)
                an[al++] = (char)tolower((unsigned char)s[j++]);
            an[al] = '\0';
            if (al && s[j] == '=') {
                j++;
                while (j < len && is_ws(s[j])) j++;
                char q = 0;
                if (j < len && (s[j] == '"' || s[j] == '\'')) {
                    q = s[j++];
                }
                size_t vs = j;
                while (j < len && (q ? s[j] != q
                                     : (!is_ws(s[j]) && s[j] != '>')))
                    j++;
                buf_t vb;
                buf_init(&vb);
                ent_decode(&vb, s + vs, j - vs);
                if (q && j < len) j++; /* closing quote */
                char *val = buf_steal(&vb, NULL);
                if (!strcmp(an, "class") && !cls) cls = val;
                else if (!strcmp(an, "id") && !id) id = val;
                else if (!strcmp(an, "href") && !href) href = val;
                else if (!strcmp(an, "src") && !src) src = val;
                else if (!strcmp(an, "alt") && !alt) alt = val;
                else free(val);
            }
        }
        i = j;
        if (tag_in(name, RAW_TAGS, RAW_N)) {
            /* raw text element: skip to its close, keep nothing */
            size_t cl = ci_find_close(s, i, len, name);
            if (cl == (size_t)-1) i = len;
            else {
                const char *e = strchr(s + cl, '>');
                i = e ? (size_t)(e - s) + 1 : len;
            }
            free(cls); free(id); free(href); free(src); free(alt);
            continue;
        }
        while (cur != d->root && closes_on(cur->tag, name)) cur = cur->parent;
        bool voidt = tag_in(name, VOID_TAGS, VOID_N);
        hnode_t *el = hnew(d, cur);
        if (!el) break;
        strcpy(el->tag, name);
        el->cls = cls;
        el->id = id;
        el->href = href;
        el->src = src;
        el->alt = alt;
        if (!selfclose && !voidt) cur = el;
    }
    free(s);
}

static void dom_free(hdom_t *d) {
    for (size_t i = 0; i < d->n; i++) {
        free(d->all[i]->text);
        free(d->all[i]->cls);
        free(d->all[i]->id);
        free(d->all[i]->href);
        free(d->all[i]->src);
        free(d->all[i]->alt);
        free(d->all[i]);
    }
    free(d->all);
    memset(d, 0, sizeof *d);
}

/* unlink children whose tag is in the junk set, recursively */
static void strip_tags(hnode_t *n, const char *const *set, size_t setn) {
    hnode_t **link = &n->child;
    n->tail = NULL;
    hnode_t *last = NULL;
    for (hnode_t *c = n->child; c;) {
        hnode_t *next = c->next;
        if (c->text ? false : tag_in(c->tag, set, setn)) {
            c->parent = NULL; /* subtree detached; freed at dom_free */
            c->next = NULL;
        } else {
            strip_tags(c, set, setn);
            *link = c;
            last = c;
            link = &c->next;
        }
        c = next;
    }
    *link = NULL;
    n->tail = last;
}

static hnode_t *find_tag(hdom_t *d, const char *tag) {
    for (size_t i = 0; i < d->n; i++)
        if (d->all[i]->text == NULL && !strcmp(d->all[i]->tag, tag))
            return d->all[i];
    return NULL;
}

/* concatenated text of a subtree (with a work budget); <br> counts as \n */
static void node_text(const hnode_t *n, buf_t *out, long *budget) {
    if (*budget <= 0) return;
    if (n->text) {
        buf_append(out, n->text, strlen(n->text));
        *budget -= (long)strlen(n->text);
    } else if (!strcmp(n->tag, "br")) {
        buf_append_byte(out, '\n');
    }
    for (hnode_t *c = n->child; c; c = c->next) {
        if (*budget <= 0) return;
        node_text(c, out, budget);
    }
}

/* ---- readability-lite scoring ---- */

static double pattern_bonus(const hnode_t *e) {
    static const char *const pos[] = { "article", "body",    "main",
                                       "content", "entry",   "post",
                                       "text",    "blog",    "story" };
    static const char *const neg[] = { "nav",     "footer",  "header",
                                       "sidebar", "comment", "menu",
                                       "advert",  "promo",   "social",
                                       "share",   "widget",  "meta",
                                       "hidden",  "related", "popup",
                                       "banner",  "sponsor" };
    double s = 0;
    for (size_t i = 0; i < sizeof pos / sizeof pos[0]; i++) {
        if (ci_contains(e->cls, pos[i]) || ci_contains(e->id, pos[i])) s += 25;
    }
    for (size_t i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        if (ci_contains(e->cls, neg[i]) || ci_contains(e->id, neg[i])) s -= 25;
    }
    if (s > 50) s = 50;
    if (s < -25) s = -25;
    return s;
}

static bool scoreable_tag(const char *t) {
    return !strcmp(t, "p") || !strcmp(t, "pre") || !strcmp(t, "blockquote") ||
           !strcmp(t, "td") || !strcmp(t, "div") || !strcmp(t, "section") ||
           !strcmp(t, "article") || !strcmp(t, "main") || !strcmp(t, "figure");
}

static hnode_t *best_candidate(hdom_t *d) {
    long budget = 4L * 1024 * 1024;
    /* paragraphs feed their ancestors */
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag)) continue;
        if (strcmp(e->tag, "p") && strcmp(e->tag, "pre") &&
            strcmp(e->tag, "blockquote") && strcmp(e->tag, "td"))
            continue;
        buf_t t;
        buf_init(&t);
        node_text(e, &t, &budget);
        long commas = 0;
        for (size_t k = 0; k < t.len; k++)
            if (t.data[k] == ',') commas++;
        long pts = 1 + commas + (t.len > 2000 ? 20 : (long)(t.len / 100));
        if (e->parent) e->parent->score += pts;
        if (e->parent && e->parent->parent) e->parent->parent->score += pts / 2.0;
        buf_free(&t);
        if (budget <= 0) break;
    }
    /* direct-text candidates */
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag)) continue;
        size_t dl = 0;
        long dc = 0;
        for (hnode_t *c = e->child; c; c = c->next) {
            if (c->text) {
                dl += strlen(c->text);
                for (const char *p = c->text; *p; p++)
                    if (*p == ',') dc++;
            }
        }
        if (dl > 20) e->score += 1 + dc + (long)(dl / 100);
        e->score += pattern_bonus(e);
    }
    hnode_t *best = NULL;
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag) || !strcmp(e->tag, "#r")) continue;
        if (e->score > 0 && (!best || e->score > best->score)) best = e;
    }
    if (!best) return d->root;
    /* a single paragraph is not a page: take its block parent */
    if (!strcmp(best->tag, "p") || !strcmp(best->tag, "pre") ||
        !strcmp(best->tag, "blockquote") || !strcmp(best->tag, "td"))
        best = (best->parent && strcmp(best->parent->tag, "#r"))
                   ? best->parent
                   : best;
    return best;
}

/* ---- markdown rendering ---- */

static void md_inline(const hnode_t *n, buf_t *out);
static void md_one(const hnode_t *n, buf_t *out);
static void md_li(const hnode_t *li, buf_t *out);
static void md_trim(buf_t *b);

/* append one rendered block: a single blank line between blocks */
static void md_join_block(buf_t *out, const buf_t *sub) {
    if (!sub->len) return;
    if (out->len) {
        size_t nl = 0;
        for (size_t i = out->len; i > 0 && out->data[i - 1] == '\n'; i--) nl++;
        if (nl < 2) buf_append_str(out, "\n\n");
    }
    buf_append(out, sub->data, sub->len);
}

static void md_inline_children(const hnode_t *n, buf_t *out) {
    for (const hnode_t *c = n->child; c; c = c->next) md_inline(c, out);
}

static void md_inline(const hnode_t *n, buf_t *out) {
    if (!n) return;
    if (n->text) {
        /* collapse internal runs, keep edge spaces: the block assembly
           trims the finished run once */
        size_t len = strlen(n->text), i = 0;
        while (i < len) {
            if (is_ws(n->text[i])) {
                while (i < len && is_ws(n->text[i])) i++;
                buf_append_byte(out, ' ');
            } else {
                buf_append_byte(out, n->text[i++]);
            }
        }
        return;
    }
    const char *t = n->tag;
    if (!strcmp(t, "br")) {
        buf_append_byte(out, '\n');
    } else if (!strcmp(t, "img")) {
        buf_appendf(out, "![%s](%s)", n->alt ? n->alt : "",
                    n->src ? n->src : "");
    } else if (!strcmp(t, "a")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        if (n->href && n->href[0] && inner.len)
            buf_appendf(out, "[%s](%s)", inner.data, n->href);
        else
            buf_append(out, inner.data, inner.len);
        buf_free(&inner);
    } else if (!strcmp(t, "strong") || !strcmp(t, "b")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "**%s**", inner.data);
        buf_free(&inner);
    } else if (!strcmp(t, "em") || !strcmp(t, "i")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "*%s*", inner.data);
        buf_free(&inner);
    } else if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") ||
               !strcmp(t, "tt") || !strcmp(t, "var")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "`%s`", inner.data);
        buf_free(&inner);
    } else {
        md_inline_children(n, out);
    }
}

/* trim whitespace at both ends of a buf */
static void md_trim(buf_t *b) {
    size_t r = 0;
    while (r < b->len && is_ws(b->data[r])) r++; /* leading */
    size_t w = 0;
    while (r < b->len) b->data[w++] = b->data[r++];
    while (w && is_ws(b->data[w - 1])) w--;
    b->len = w;
    if (b->cap) b->data[w] = '\0';
}

/* prefix every nonempty line (all lines when all=true) with prefix */
static void prefix_lines(buf_t *b, const char *prefix, bool all) {
    buf_t o;
    buf_init(&o);
    size_t start = 0;
    for (size_t i = 0; i <= b->len; i++) {
        if (i == b->len || b->data[i] == '\n') {
            bool emit_nl = i < b->len;
            bool empty = i == start;
            if (all || !empty) buf_append_str(&o, prefix);
            buf_append(&o, b->data + start, i - start);
            if (emit_nl) buf_append_byte(&o, '\n');
            start = i + 1;
        }
    }
    buf_clear(b);
    buf_append(b, o.data ? o.data : "", o.len);
    buf_free(&o);
}

/* indent every continuation line (after the first) by n spaces */
static void indent_continuation(buf_t *b, size_t n) {
    buf_t o;
    buf_init(&o);
    for (size_t i = 0; i < b->len; i++) {
        buf_append_byte(&o, b->data[i]);
        if (b->data[i] == '\n')
            for (size_t k = 0; k < n; k++) buf_append_byte(&o, ' ');
    }
    buf_clear(b);
    buf_append(b, o.data ? o.data : "", o.len);
    buf_free(&o);
}

static bool is_list(const hnode_t *n) {
    return !n->text && (!strcmp(n->tag, "ul") || !strcmp(n->tag, "ol"));
}

static bool is_heading(const hnode_t *n) {
    return !n->text && n->tag[0] == 'h' && isdigit((unsigned char)n->tag[1]) &&
           !n->tag[2];
}

/* one block element rendered into out (no leading/trailing newlines) */
static void md_one(const hnode_t *n, buf_t *out) {
    if (!n) return;
    if (n->text) {
        buf_t t;
        buf_init(&t);
        buf_append_str(&t, n->text);
        collapse_ws(&t);
        if (t.len) buf_append(out, t.data, t.len);
        buf_free(&t);
        return;
    }
    const char *t = n->tag;
    if (is_heading(n)) {
        int lvl = t[1] - '0';
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) {
            for (int i = 0; i < lvl; i++) buf_append_byte(out, '#');
            buf_appendf(out, " %s", inner.data);
        }
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "p") || !strcmp(t, "figcaption")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        collapse_ws(&inner);
        md_trim(&inner);
        buf_append(out, inner.data, inner.len);
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "ul") || !strcmp(t, "ol")) {
        bool ordered = t[0] == 'o';
        long idx = 1;
        bool first = true;
        for (const hnode_t *li = n->child; li; li = li->next) {
            if (li->text) continue; /* stray text between items */
            if (strcmp(li->tag, "li")) {
                /* unexpected child: render as its own block */
                buf_t sub;
                buf_init(&sub);
                md_one(li, &sub);
                if (sub.len) {
                    if (!first) buf_append_byte(out, '\n');
                    buf_append(out, sub.data, sub.len);
                    first = false;
                }
                buf_free(&sub);
                continue;
            }
            buf_t lb;
            buf_init(&lb);
            md_li(li, &lb);
            char marker[24];
            long shown = idx > 999999 ? 999999 : idx;
            if (ordered) snprintf(marker, sizeof marker, "%ld. ", shown);
            else snprintf(marker, sizeof marker, "- ");
            idx++;
            indent_continuation(&lb, strlen(marker));
            if (!first) buf_append_byte(out, '\n');
            buf_append_str(out, marker);
            buf_append(out, lb.data ? lb.data : "", lb.len);
            buf_free(&lb);
            first = false;
        }
        return;
    }
    if (!strcmp(t, "blockquote")) {
        buf_t inner;
        buf_init(&inner);
        for (const hnode_t *c = n->child; c; c = c->next) {
            buf_t sub;
            buf_init(&sub);
            md_one(c, &sub);
            if (sub.len) md_join_block(&inner, &sub);
            buf_free(&sub);
        }
        prefix_lines(&inner, "> ", true);
        buf_append(out, inner.data ? inner.data : "", inner.len);
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "pre")) {
        long budget = 1L << 20;
        buf_t raw;
        buf_init(&raw);
        node_text(n, &raw, &budget);
        while (raw.len && (raw.data[raw.len - 1] == '\n' ||
                           is_ws(raw.data[raw.len - 1])))
            raw.len--;
        if (raw.cap) raw.data[raw.len] = '\0';
        buf_append_str(out, "```\n");
        buf_append(out, raw.data ? raw.data : "", raw.len);
        buf_append_str(out, "\n```");
        buf_free(&raw);
        return;
    }
    if (!strcmp(t, "hr")) {
        buf_append_str(out, "---");
        return;
    }
    if (!strcmp(t, "table")) {
        bool first = true;
        for (const hnode_t *sec = n->child; sec; sec = sec->next) {
            bool is_sec =
                !sec->text && (!strcmp(sec->tag, "thead") ||
                               !strcmp(sec->tag, "tbody") ||
                               !strcmp(sec->tag, "tfoot"));
            if (!is_sec && (sec->text || strcmp(sec->tag, "tr"))) continue;
            /* thead/tbody wrap their rows; a bare tr is its own row */
            const hnode_t *rows = is_sec ? sec->child : sec;
            for (const hnode_t *tr = rows; tr; tr = tr->next) {
                if (tr->text || strcmp(tr->tag, "tr")) continue;
                buf_t row;
                buf_init(&row);
                size_t c = 0;
                for (const hnode_t *cell = tr->child; cell; cell = cell->next) {
                    if (cell->text || (strcmp(cell->tag, "td") &&
                                       strcmp(cell->tag, "th")))
                        continue;
                    buf_t ct;
                    buf_init(&ct);
                    md_inline_children(cell, &ct);
                    collapse_ws(&ct);
                    md_trim(&ct);
                    if (c) buf_append_str(&row, " | ");
                    buf_append_str(&row, ct.data ? ct.data : "");
                    buf_free(&ct);
                    c++;
                }
                if (row.len || c) {
                    if (!first) buf_append_byte(out, '\n');
                    buf_appendf(out, "| %s |", row.data ? row.data : "");
                    if (first) { /* separator under the header row */
                        buf_append_byte(out, '\n');
                        buf_append_byte(out, '|');
                        for (size_t k = 0; k < c; k++)
                            buf_append_str(out, " --- |");
                    }
                    first = false;
                }
                buf_free(&row);
                if (!is_sec) break; /* the single tr was sec itself */
            }
        }
        return;
    }
    if (!strcmp(t, "img")) {
        buf_appendf(out, "![%s](%s)", n->alt ? n->alt : "",
                    n->src ? n->src : "");
        return;
    }
    /* structural containers and unknown tags: children as blocks */
    for (const hnode_t *c = n->child; c; c = c->next) {
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        md_join_block(out, &sub);
        buf_free(&sub);
    }
}

/* render an li: an inline run plus any nested blocks, joined into one buf */
static void md_li(const hnode_t *li, buf_t *out) {
    buf_t inline_acc;
    buf_init(&inline_acc);
    for (const hnode_t *c = li->child; c; c = c->next) {
        bool block_child =
            !c->text && (is_list(c) || !strcmp(c->tag, "p") ||
                         !strcmp(c->tag, "pre") || !strcmp(c->tag, "table") ||
                         !strcmp(c->tag, "blockquote") || is_heading(c));
        if (!block_child) {
            md_inline(c, &inline_acc);
            continue;
        }
        /* block child: flush the inline run accumulated so far */
        collapse_ws(&inline_acc);
        md_trim(&inline_acc);
        if (inline_acc.len) {
            md_join_block(out, &inline_acc);
            buf_clear(&inline_acc);
        }
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        md_join_block(out, &sub);
        buf_free(&sub);
    }
    collapse_ws(&inline_acc);
    md_trim(&inline_acc);
    if (inline_acc.len) md_join_block(out, &inline_acc);
    buf_free(&inline_acc);
}

char *builtin_html_to_markdown(const char *html, size_t n) {
    hdom_t d;
    dom_parse(&d, html, n);
    if (d.oom || !d.root) {
        dom_free(&d);
        return strdup("");
    }
    strip_tags(d.root, JUNK_TAGS, JUNK_N);

    buf_t out;
    buf_init(&out);

    /* title */
    hnode_t *title = find_tag(&d, "title");
    if (title) {
        long budget = 4096;
        buf_t tb;
        buf_init(&tb);
        node_text(title, &tb, &budget);
        collapse_ws(&tb);
        md_trim(&tb);
        if (tb.len) buf_appendf(&out, "# %s\n\n", tb.data);
        buf_free(&tb);
    }

    hnode_t *best = best_candidate(&d);
    {
        static const char *const chrome[] = { "nav", "aside", "footer" };
        strip_tags(best, chrome, sizeof chrome / sizeof chrome[0]);
    }

    for (const hnode_t *c = best->child; c; c = c->next) {
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        if (sub.len) md_join_block(&out, &sub);
        buf_free(&sub);
    }

    while (out.len && is_ws(out.data[out.len - 1])) out.len--;
    if (out.cap) out.data[out.len] = '\0';

    char *res = strdup(out.data ? out.data : "");
    buf_free(&out);
    dom_free(&d);
    return res;
}


/* ================= duckduckgo results parsing ================= */

/* normalize a result href: unwrap the duckduckgo redirect, absolutize */
static char *ddg_url(const char *href) {
    if (!href) return NULL;
    buf_t out;
    buf_init(&out);
    const char *u = strstr(href, "uddg=");
    if (u) {
        u += 5;
        const char *e = strchr(u, '&');
        pct_decode(&out, u, e ? (size_t)(e - u) : strlen(u));
    } else if (!strncmp(href, "//", 2)) {
        buf_appendf(&out, "https:%s", href);
    } else if (href[0] == '/') {
        buf_appendf(&out, "https://duckduckgo.com%s", href);
    } else {
        buf_append_str(&out, href);
    }
    return buf_steal(&out, NULL);
}

/* walk the parsed page in document order: an element whose class holds
   result__a opens a pending result (its text is the title), the next
   result__snippet element completes it (its text is the description);
   a new result__a flushes the pending one with the title alone. the
   d.all array is in document order, which is the pairing order. */
char *builtin_ddg_results(const char *html, size_t n) {
    hdom_t d;
    dom_parse(&d, html, n);
    buf_t out;
    buf_init(&out);
    size_t results = 0;
    char *pending_url = NULL, *pending_title = NULL;
    for (size_t i = 0; i < d.n && results < 25; i++) {
        hnode_t *e = d.all[i];
        if (e->text) continue;
        bool is_link = ci_contains(e->cls, "result__a");
        bool is_snip = ci_contains(e->cls, "result__snippet");
        if (!is_link && !is_snip) continue;
        buf_t t;
        buf_init(&t);
        long budget = 1 << 20;
        node_text(e, &t, &budget);
        collapse_ws(&t);
        char *text = buf_steal(&t, NULL);
        if (is_link) {
            if (pending_url) { /* flush the pending result without snippet */
                if (results) buf_append_byte(&out, '\n');
                buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                            pending_title ? pending_title : "");
                results++;
                free(pending_url);
                free(pending_title);
            }
            pending_url = ddg_url(e->href);
            pending_title = text;
        } else if (pending_url) {
            if (results) buf_append_byte(&out, '\n');
            buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                        text && text[0] ? text
                                        : (pending_title ? pending_title
                                                         : ""));
            results++;
            free(pending_url);
            free(pending_title);
            pending_url = NULL;
            pending_title = NULL;
            free(text);
        } else {
            free(text); /* snippet without a preceding result */
        }
    }
    if (pending_url && results < 25) {
        if (results) buf_append_byte(&out, '\n');
        buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                    pending_title ? pending_title : "");
        free(pending_url);
        free(pending_title);
    }
    dom_free(&d);
    return buf_steal(&out, NULL);
}


/* ================= web fetch helpers ================= */

static const char *BROWSER_UA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

static bool valid_http_url(const char *u) {
    size_t n = strlen(u);
    if (!n || n > 2048) return false;
    for (const char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c == 0x7f) return false;
    }
    /* structure (http/https scheme, nonempty host) via libcurl's parser */
    CURLU *h = curl_url();
    bool ok = h && curl_url_set(h, CURLUPART_URL, u, 0) == CURLUE_OK;
    if (ok) {
        char *scheme = NULL, *host = NULL;
        ok = curl_url_get(h, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
             scheme && (!strcasecmp(scheme, "http") ||
                        !strcasecmp(scheme, "https"));
        if (ok)
            ok = curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 host && host[0];
        curl_free(scheme);
        curl_free(host);
    }
    curl_url_cleanup(h);
    return ok;
}

/* GET a url; returns the body (malloc'd, *len_out set) or NULL with err */
static char *http_get(const char *url, size_t *len_out, char *ct_out,
                      size_t ctsz, long *status_out, char *err, size_t errsz) {
    http_req_t r;
    memset(&r, 0, sizeof r);
    buf_init(&r.resp);
    buf_init(&r.content_type);
    r.url = url;
    r.is_get = true;
    r.ua = BROWSER_UA;
    r.connect_to = 10;
    r.read_to = 30;
    r.total_to = 60;
    struct curl_slist *hdrs = NULL;
    http_hdr_add(&hdrs, "Accept", "text/html,application/xhtml+xml");
    r.hdrs = hdrs;
    int rc = http_perform(&r);
    char *body = NULL;
    if (rc == 0) {
        size_t l = r.resp.len;
        body = buf_steal(&r.resp, &l);
        *len_out = l;
        snprintf(ct_out, ctsz, "%s", r.content_type.data ? r.content_type.data
                                                         : "");
        *status_out = r.status;
    } else if (rc == 1) {
        snprintf(err, errsz, "http status %ld returned", r.status);
        *status_out = r.status;
    } else {
        snprintf(err, errsz, "transport error: %s",
                 curl_easy_strerror(r.curl_res));
    }
    buf_free(&r.resp);
    buf_free(&r.content_type);
    curl_slist_free_all(hdrs);
    return body;
}

/* replace invalid utf-8 bytes with '?' so the json reply stays valid */
static void sanitize_utf8(buf_t *b) {
    if (utf8_valid((const uint8_t *)(b->data ? b->data : ""), b->len)) return;
    if (!b->data) return;
    size_t r = 0, w = 0;
    while (r < b->len) {
        unsigned char c = (unsigned char)b->data[r];
        size_t need = c < 0x80 ? 1
                     : (c & 0xE0) == 0xC0 ? 2
                     : (c & 0xF0) == 0xE0 ? 3
                     : (c & 0xF8) == 0xF0 ? 4
                                          : 0;
        bool ok = need > 0;
        if (ok && r + need <= b->len) {
            for (size_t k = 1; k < need; k++)
                if ((b->data[r + k] & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
        } else {
            ok = false;
        }
        if (ok) {
            memmove(b->data + w, b->data + r, need);
            w += need;
            r += need;
        } else {
            b->data[w++] = '?';
            r++;
        }
    }
    b->len = w;
    b->data[w] = '\0';
}

/* ================= tool implementations ================= */

typedef void (*tool_fn)(const cJSON *args, buf_t *out, bool *is_error);

static bool need_str(const cJSON *args, const char *name, const char **out,
                     buf_t *err) {
    const char *v = rec_str(args, name);
    if (!v) {
        buf_appendf(err, "missing required argument '%s'", name);
        return false;
    }
    *out = v;
    return true;
}

static bool need_int(const cJSON *args, const char *name, long long *out,
                     buf_t *err) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!cJSON_IsNumber(v)) {
        buf_appendf(err, "missing required argument '%s'", name);
        return false;
    }
    double d = v->valuedouble;
    if (d != (double)(long long)d) {
        buf_appendf(err, "argument '%s' must be an integer", name);
        return false;
    }
    *out = (long long)d;
    return true;
}

static void tool_web_search(const cJSON *args, buf_t *out, bool *is_error) {
    const char *keywords = NULL;
    if (!need_str(args, "keywords", &keywords, out)) {
        *is_error = true;
        return;
    }
    buf_t url;
    buf_init(&url);
    buf_append_str(&url, "https://html.duckduckgo.com/html/?q=");
    url_encode(&url, keywords);
    size_t blen = 0;
    char ct[128] = "", err[256] = "";
    long status = 0;
    char *body = http_get(url.data, &blen, ct, sizeof ct, &status, err,
                          sizeof err);
    buf_free(&url);
    if (!body) {
        buf_clear(out);
        buf_appendf(out, "web_search failed: %s", err);
        *is_error = true;
        return;
    }
    char *res = builtin_ddg_results(body, blen);
    free(body);
    buf_clear(out);
    if (res) {
        buf_t t;
        buf_init(&t);
        buf_append_str(&t, res);
        sanitize_utf8(&t);
        if (t.len) buf_append(out, t.data, t.len);
        else buf_append_str(out, "no results");
        buf_free(&t);
    } else {
        buf_append_str(out, "no results");
    }
    free(res);
}

static void tool_web_fetch(const cJSON *args, buf_t *out, bool *is_error) {
    const char *url = NULL;
    if (!need_str(args, "url", &url, out)) {
        *is_error = true;
        return;
    }
    if (!valid_http_url(url)) {
        buf_clear(out);
        buf_appendf(out, "invalid url: %s", url);
        *is_error = true;
        return;
    }
    size_t blen = 0;
    char ct[128] = "", err[256] = "";
    long status = 0;
    char *body = http_get(url, &blen, ct, sizeof ct, &status, err, sizeof err);
    if (!body) {
        buf_clear(out);
        buf_appendf(out, "web_fetch failed: %s", err);
        *is_error = true;
        return;
    }
    if (blen > 20ull * 1024 * 1024) {
        free(body);
        buf_clear(out);
        buf_append_str(out, "web_fetch failed: page too large (over 20Mb)");
        *is_error = true;
        return;
    }
    if (ct[0] && strncasecmp(ct, "text/html", 9) &&
        !strstr(ct, "application/xhtml")) {
        free(body);
        buf_clear(out);
        buf_appendf(out, "web_fetch failed: not an html page (content type %s)",
                    ct);
        *is_error = true;
        return;
    }
    char *md = builtin_html_to_markdown(body, blen);
    free(body);
    buf_clear(out);
    if (md && md[0]) {
        buf_t t = { md, strlen(md), 0 };
        sanitize_utf8(&t);
        buf_append_str(out, md);
        free(md);
    } else {
        free(md);
        buf_append_str(out, "no readable content found");
        *is_error = true;
    }
}

/* shared body of files_list (pattern matches paths) and files_search
   (pattern matches line content) */
static void tool_files(const cJSON *args, buf_t *out, bool *is_error,
                       bool content) {
    const char *path = NULL, *regex = NULL;
    if (!need_str(args, "path", &path, out) ||
        !need_str(args, "regex", &regex, out)) {
        *is_error = true;
        return;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        return;
    }
    char err[192] = "";
    regex_t rx;
    if (!rx_compile(&rx, regex, err, sizeof err)) {
        buf_clear(out);
        buf_appendf(out, "%s", err);
        *is_error = true;
        return;
    }
    walk_ctx_t w = { out, rx, content };
    char werr[256] = "";
    bool ok = walk_start(&w, path, werr, sizeof werr);
    regfree(&rx);
    if (!ok) {
        buf_clear(out);
        buf_appendf(out, "%s", werr);
        *is_error = true;
    }
}

static void tool_files_list(const cJSON *a, buf_t *o, bool *e) {
    tool_files(a, o, e, false);
}

static void tool_files_search(const cJSON *a, buf_t *o, bool *e) {
    tool_files(a, o, e, true);
}

static void tool_file_read(const cJSON *args, buf_t *out, bool *is_error) {
    const char *path = NULL;
    long long off = 0, count = -1;
    if (!need_str(args, "path", &path, out) ||
        !need_int(args, "lines_offset", &off, out) ||
        !need_int(args, "lines_length", &count, out)) {
        *is_error = true;
        return;
    }
    if (off < 0 || count < 0) {
        buf_clear(out);
        buf_append_str(out, "lines_offset and lines_length must not be negative");
        *is_error = true;
        return;
    }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is not a regular file", path);
        *is_error = true;
        return;
    }
    buf_t b;
    buf_init(&b);
    int rc = read_whole(path, &b, BM_FILE_CAP);
    if (rc == -2) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "file too large (over %llu bytes)", BM_FILE_CAP);
        *is_error = true;
        return;
    }
    if (rc == -1) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "cannot read '%s'", path);
        *is_error = true;
        return;
    }
    if (memchr(b.data, 0, b.len) || !utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "'%s' is not a textual file", path);
        *is_error = true;
        return;
    }
    /* split lines */
    size_t start = 0;
    unsigned long long lno = 0;
    bool first = true;
    for (size_t i = 0; i < b.len; i++) {
        if (b.data[i] == '\n') {
            size_t len = i - start;
            if (len && b.data[start + len - 1] == '\r') len--;
            if (lno >= (unsigned long long)off &&
                lno < (unsigned long long)off + (unsigned long long)count) {
                if (!first) buf_append_byte(out, '\n');
                buf_append(out, b.data + start, len);
                first = false;
            }
            lno++;
            start = i + 1;
        }
    }
    if (start < b.len) { /* final line without terminator */
        size_t len = b.len - start;
        if (len && b.data[start + len - 1] == '\r') len--;
        if (lno >= (unsigned long long)off &&
            lno < (unsigned long long)off + (unsigned long long)count) {
            if (!first) buf_append_byte(out, '\n');
            buf_append(out, b.data + start, len);
        }
        lno++;
    }
    buf_free(&b);
    if ((unsigned long long)off > lno) {
        buf_clear(out);
        buf_appendf(out, "lines_offset %lld beyond end of file (%llu lines)",
                    off, lno);
        *is_error = true;
        return;
    }
}

static void tool_file_create(const cJSON *args, buf_t *out, bool *is_error) {
    const char *path = NULL, *content = NULL;
    if (!need_str(args, "path", &path, out) ||
        !need_str(args, "content", &content, out)) {
        *is_error = true;
        return;
    }
    bool overwrite = rec_bool(args, "overwrite", false);
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        return;
    }
    struct stat st;
    if (!stat(path, &st)) {
        if (!overwrite) {
            buf_clear(out);
            buf_appendf(out, "'%s' already exists (pass overwrite=true)", path);
            *is_error = true;
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            buf_clear(out);
            buf_appendf(out, "'%s' is a directory", path);
            *is_error = true;
            return;
        }
    }
    if (!ensure_parent_dirs(path)) {
        buf_clear(out);
        buf_appendf(out, "cannot create parent directories of '%s'", path);
        *is_error = true;
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        buf_clear(out);
        buf_appendf(out, "cannot write '%s': %s", path, strerror(errno));
        *is_error = true;
        return;
    }
    size_t n = strlen(content);
    bool ok = fwrite(content, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        buf_clear(out);
        buf_appendf(out, "write error on '%s'", path);
        *is_error = true;
        return;
    }
    buf_clear(out);
    buf_appendf(out, "wrote %lu bytes to %s", (unsigned long)n, path);
}

/* ---- file_edit ---- */

typedef struct fline {
    const char *s;
    size_t len; /* without terminator */
    const char *end;
    size_t endlen;
} fline_t;

/* split buffer into lines; a trailing '\n' terminates its line and does
   not open a phantom empty last line. the recorded terminator includes a
   preceding '\r' so crlf endings survive a rewrite */
static size_t split_lines(const char *buf, size_t len, fline_t **out) {
    size_t cap = 64, n = 0;
    fline_t *v = malloc(cap * sizeof *v);
    size_t start = 0;
    for (size_t i = 0; i < len;) {
        if (buf[i] == '\n') {
            if (n == cap) {
                cap *= 2;
                v = realloc(v, cap * sizeof *v);
            }
            v[n].s = buf + start;
            v[n].len = i - start;
            v[n].end = buf + i;
            v[n].endlen = 1;
            if (v[n].len && v[n].s[v[n].len - 1] == '\r') {
                v[n].len--;
                v[n].end--;
                v[n].endlen++;
            }
            n++;
            start = i + 1;
        }
        i++;
    }
    if (start < len) { /* final line without terminator */
        if (n == cap) {
            cap++;
            v = realloc(v, cap * sizeof *v);
        }
        v[n].s = buf + start;
        v[n].len = len - start;
        v[n].end = buf + len;
        v[n].endlen = 0;
        if (v[n].len && v[n].s[v[n].len - 1] == '\r') {
            v[n].len--;
            v[n].end--;
            v[n].endlen = 1;
        }
        n++;
    }
    *out = v;
    return n;
}

/* split a string argument into lines ('\n' separated) */
static size_t split_arg_lines(const char *s, fline_t **out) {
    return split_lines(s, strlen(s), out);
}

static bool line_eq_trim(const fline_t *a, const char *bs, size_t blen) {
    size_t al = lead_ws(a->s, a->len);
    size_t at = trail_ws(a->s + al, a->len - al);
    size_t a_len = a->len - al - at;
    size_t bl = lead_ws(bs, blen);
    size_t bt = trail_ws(bs + bl, blen - bl);
    size_t b_len = blen - bl - bt;
    return a_len == b_len && !memcmp(a->s + al, bs + bl, a_len);
}

static void tool_file_edit(const cJSON *args, buf_t *out, bool *is_error) {
    const char *path = NULL, *olds = NULL, *news = NULL;
    long long line_number = 0;
    if (!need_str(args, "path", &path, out) ||
        !need_str(args, "oldString", &olds, out) ||
        !need_str(args, "newString", &news, out) ||
        !need_int(args, "line_number", &line_number, out)) {
        *is_error = true;
        return;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        return;
    }
    if (!strlen(olds)) {
        buf_clear(out);
        buf_append_str(out, "oldString must not be empty");
        *is_error = true;
        return;
    }
    if (line_number < 0) {
        buf_clear(out);
        buf_append_str(out, "line_number must not be negative");
        *is_error = true;
        return;
    }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is not a regular file", path);
        *is_error = true;
        return;
    }
    buf_t b;
    buf_init(&b);
    int rc = read_whole(path, &b, BM_FILE_CAP);
    if (rc != 0) {
        buf_free(&b);
        buf_clear(out);
        if (rc == -2)
            buf_appendf(out, "file too large (over %llu bytes)", BM_FILE_CAP);
        else
            buf_appendf(out, "cannot read '%s'", path);
        *is_error = true;
        return;
    }
    if (memchr(b.data, 0, b.len) || !utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "'%s' is not a textual file", path);
        *is_error = true;
        return;
    }
    fline_t *fl = NULL;
    size_t nfl = split_lines(b.data, b.len, &fl);
    fline_t *ol = NULL;
    size_t nol = split_arg_lines(olds, &ol);
    fline_t *nl = NULL;
    size_t nnl = split_arg_lines(news, &nl);

    /* candidate start lines: line_number with +/-3 tolerance, closest first;
       line_number 0 (absent) searches the whole file */
    long long order[7] = { 0, -1, 1, -2, 2, -3, 3 };
    size_t ncand = line_number > 0 ? 7 : (nfl ? nfl : 1);
    long long found = -1;
    for (size_t ci = 0; ci < ncand && found < 0; ci++) {
        long long idx;
        if (line_number > 0) {
            long long d = order[ci];
            idx = line_number - 1 + d;
        } else {
            idx = (long long)ci;
        }
        if (idx < 0 || (size_t)idx + nol > nfl) continue;
        bool ok = true;
        for (size_t k = 0; k < nol; k++)
            if (!line_eq_trim(&fl[idx + k], ol[k].s, ol[k].len)) {
                ok = false;
                break;
            }
        if (ok) found = idx;
    }
    if (found < 0) {
        free(fl);
        free(ol);
        free(nl);
        buf_free(&b);
        buf_clear(out);
        if (line_number > 0)
            buf_appendf(out,
                        "oldString not found at line %lld (tolerance 3) in %s",
                        line_number, path);
        else
            buf_appendf(out, "oldString not found in %s", path);
        *is_error = true;
        return;
    }

    /* indentation: base whitespace of the first replaced line, new block
       shifted so its first line lands on that column */
    buf_t basews;
    buf_init(&basews);
    size_t base_len = lead_ws(fl[found].s, fl[found].len);
    buf_append(&basews, fl[found].s, base_len);
    size_t first_len = nnl ? lead_ws(nl[0].s, nl[0].len) : 0;

    buf_t nb;
    buf_init(&nb);
    /* the replaced block runs to end of file when it consumed the last
       original line; then (and only then) may the last new line go
       without a terminator */
    bool block_at_eof = (size_t)found + nol == nfl;
    for (size_t i = 0; i < nfl; i++) {
        if (i == (size_t)found) {
            for (size_t j = 0; j < nnl; j++) {
                bool blank = nl[j].len == 0 ||
                             lead_ws(nl[j].s, nl[j].len) == nl[j].len;
                if (!blank) {
                    size_t lj = lead_ws(nl[j].s, nl[j].len);
                    if (lj >= first_len) {
                        buf_append(&nb, basews.data, basews.len);
                        for (size_t k = 0; k < lj - first_len; k++)
                            buf_append_byte(&nb, ' ');
                    } else {
                        long long t = (long long)basews.len +
                                      (long long)lj - (long long)first_len;
                        if (t <= 0) {
                            /* dedented past the margin: no indent */
                        } else if ((size_t)t <= basews.len) {
                            buf_append(&nb, basews.data, (size_t)t);
                        } else {
                            buf_append(&nb, basews.data, basews.len);
                        }
                    }
                    buf_append(&nb, nl[j].s + lj, nl[j].len - lj);
                }
                /* terminator: inherit the corresponding original line's,
                   lines past the replaced block inherit its last line's;
                   every new line except a final one needs one */
                const fline_t *src = (j < nol) ? &fl[found + j]
                                               : &fl[found + nol - 1];
                if (block_at_eof && j + 1 == nnl)
                    buf_append(&nb, src->end, src->endlen);
                else if (src->endlen == 0)
                    buf_append_byte(&nb, '\n');
                else
                    buf_append(&nb, src->end, src->endlen);
            }
            i += nol - 1; /* skip the replaced originals */
            continue;
        }
        buf_append(&nb, fl[i].s, fl[i].len);
        buf_append(&nb, fl[i].end, fl[i].endlen);
    }

    FILE *f = fopen(path, "wb");
    if (!f || fwrite(nb.data, 1, nb.len, f) != nb.len || fclose(f) != 0) {
        free(fl);
        free(ol);
        free(nl);
        buf_free(&b);
        buf_free(&nb);
        buf_free(&basews);
        buf_clear(out);
        buf_appendf(out, "cannot write '%s'", path);
        *is_error = true;
        return;
    }
    buf_clear(out);
    if (nol == 1)
        buf_appendf(out, "replaced line %llu in %s",
                    (unsigned long long)(found + 1), path);
    else
        buf_appendf(out, "replaced lines %llu-%llu in %s",
                    (unsigned long long)(found + 1),
                    (unsigned long long)(found + nol), path);
    free(fl);
    free(ol);
    free(nl);
    buf_free(&b);
    buf_free(&nb);
    buf_free(&basews);
}

static const tool_fn TOOL_FNS[TOOLS_N] = {
    tool_web_search, tool_web_fetch,  tool_files_list, tool_files_search,
    tool_file_read,  tool_file_create, tool_file_edit,
};

/* ================= json-rpc handler ================= */

static cJSON *tools_list_result(void) {
    cJSON *tools = cJSON_CreateArray();
    for (size_t i = 0; i < TOOLS_N; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON *req = cJSON_CreateArray();
        for (size_t a = 0; a < TOOLS[i].nargs; a++) {
            const arg_def_t *d = &TOOLS[i].args[a];
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", d->type);
            cJSON_AddStringToObject(p, "description", d->desc);
            cJSON_AddItemToObject(props, d->name, p);
            if (d->required) cJSON_AddItemToArray(req, cJSON_CreateString(d->name));
        }
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(tools, t);
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "tools", tools);
    return res;
}

int builtin_handle(void *ctx, const char *method, cJSON *params, cJSON *id,
                   cJSON **result_out, char **errmsg_out) {
    (void)ctx;
    (void)id;
    if (!strcmp(method, "initialize")) {
        *result_out = rpc_initialize_result(params, "llmkit-builtin-mcp");
        return 0;
    }
    if (!strcmp(method, "notifications/initialized") || !strcmp(method, "ping"))
        return 2;
    if (!strcmp(method, "tools/list")) {
        *result_out = tools_list_result();
        return 0;
    }
    if (!strcmp(method, "tools/call")) {
        const char *name = rec_str(params, "name");
        size_t ti = TOOLS_N;
        if (name)
            for (size_t i = 0; i < TOOLS_N; i++)
                if (!strcmp(TOOLS[i].name, name)) ti = i;
        if (ti == TOOLS_N) {
            *errmsg_out = strdup(name ? "unknown tool" : "missing tool name");
            return 1;
        }
        const cJSON *args =
            cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (args && !cJSON_IsObject(args)) {
            *errmsg_out = strdup("arguments must be an object");
            return 1;
        }
        cJSON *empty = NULL;
        if (!args) {
            empty = cJSON_CreateObject();
            args = empty;
        }
        buf_t out;
        buf_init(&out);
        bool is_error = false;
        TOOL_FNS[ti](args, &out, &is_error);
        cJSON_Delete(empty);
        cJSON *res = cJSON_CreateObject();
        cJSON *content = cJSON_CreateArray();
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        cJSON_AddStringToObject(blk, "text", out.data ? out.data : "");
        cJSON_AddItemToArray(content, blk);
        cJSON_AddItemToObject(res, "content", content);
        if (is_error) cJSON_AddBoolToObject(res, "isError", true);
        buf_free(&out);
        *result_out = res;
        return 0;
    }
    *errmsg_out = strdup("method not found");
    return 1;
}

int cmd_builtin(void) {
    signals_init();
    rpc_handler_t h = { builtin_handle, NULL };
    rpc_serve_stdio(&h);
    return 0;
}
