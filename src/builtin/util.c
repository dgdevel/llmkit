#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>


/* case-insensitive substring test */
bool bm_ci_contains(const char *hay, const char *needle) {
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

bool bm_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

size_t bm_lead_ws(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && bm_is_ws(s[i])) i++;
    return i;
}

size_t bm_trail_ws(const char *s, size_t n) {
    size_t i = n;
    while (i > 0 && bm_is_ws(s[i - 1])) i--;
    return n - i;
}

/* qsort comparator for a char *[] of names */
int bm_cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* collapse whitespace runs to single spaces, in place over a buf */
void bm_collapse_ws(buf_t *b) {
    if (!b->len) return;
    size_t w = 0;
    for (size_t r = 0; r < b->len;) {
        if (bm_is_ws(b->data[r])) {
            while (r < b->len && bm_is_ws(b->data[r])) r++;
            if (w) b->data[w++] = ' ';
        } else {
            b->data[w++] = b->data[r++];
        }
    }
    if (w && b->data[w - 1] == ' ') w--;
    b->len = w;
    b->data[w] = '\0';
}

void bm_utf8_put(buf_t *b, unsigned long cp) {
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

void bm_ent_decode(buf_t *out, const char *s, size_t n) {
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
            if (ok && cp <= 0x10FFFF) bm_utf8_put(out, cp);
            else buf_append_byte(out, '?');
        } else {
            bool found = false;
            for (size_t k = 0; k < ENTITIES_N; k++)
                if (strlen(ENTITIES[k].name) == elen &&
                    !strncasecmp(ENTITIES[k].name, e, elen)) {
                    bm_utf8_put(out, ENTITIES[k].cp);
                    found = true;
                    break;
                }
            if (!found) buf_append_byte(out, '?');
        }
        i = end + 1;
    }
}

/* percent-encoding for one query component (space -> %20) */
void bm_url_encode(buf_t *out, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            buf_append_byte(out, (char)*p);
        else
            buf_appendf(out, "%%%02X", *p);
    }
}

/* percent-decoding of a byte range ('+' becomes space: query component) */
void bm_pct_decode(buf_t *out, const char *s, size_t n) {
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
bool bm_rx_compile(regex_t *rx, const char *pattern, char *err,
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
bool bm_rx_match(const regex_t *rx, const char *s, size_t n) {
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
    if (!bm_rx_compile(&rx, pattern, err, errsz)) return false;
    bool m = bm_rx_match(&rx, text, strlen(text));
    regfree(&rx);
    return m;
}


/* ================= filesystem helpers ================= */

void bm_perms_of(const char *path, char out[4]) {
    out[0] = BM_ACCESS(path, R_OK) == 0 ? 'r' : '-';
    out[1] = BM_ACCESS(path, W_OK) == 0 ? 'w' : '-';
    out[2] = BM_ACCESS(path, X_OK) == 0 ? 'x' : '-';
    out[3] = '\0';
}

/* human readable byte size: 512b, 5Kb, 5.4Kb, 15Kb, 1Mb, 1.2Mb ... */
void bm_human_size(unsigned long long v, char *out, size_t sz) {
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
int bm_read_whole(const char *path, buf_t *out, unsigned long long cap) {
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
tscan_t bm_scan_text(const char *path, unsigned long long cap) {
    tscan_t r = { false, false, 0 };
    buf_t b;
    buf_init(&b);
    int rc = bm_read_whole(path, &b, cap);
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

void bm_sanitize_utf8(buf_t *b) {
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

bool bm_need_str(const cJSON *args, const char *name, const char **out,
                 buf_t *err) {
    const char *v = rec_str(args, name);
    if (!v) {
        buf_appendf(err, "missing required argument '%s'", name);
        return false;
    }
    *out = v;
    return true;
}

bool bm_need_int(const cJSON *args, const char *name, long long *out,
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
