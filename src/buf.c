/* buf.c - byte buffers, deterministic JSON appenders, UTF-8 validation. */
#include "llmkit.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void buf_init(buf_t *b) { b->data = NULL; b->len = b->cap = 0; }

void buf_free(buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

void buf_clear(buf_t *b) { b->len = 0; if (b->cap) b->data[0] = '\0'; }

void buf_reserve(buf_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd) { perror("llmkit: realloc"); exit(EXIT_OUT_OF_CHANNEL); }
    b->data = nd;
    b->cap = ncap;
}

void buf_append(buf_t *b, const void *p, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(buf_t *b, const char *s) {
    if (s) buf_append(b, s, strlen(s));
}

void buf_append_byte(buf_t *b, char c) { buf_append(b, &c, 1); }

void buf_appendf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

char *buf_steal(buf_t *b, size_t *len_out) {
    if (!b->data) buf_reserve(b, 1); /* ensure NUL */
    b->data[b->len] = '\0';
    char *p = b->data;
    if (len_out) *len_out = b->len;
    b->data = NULL;
    b->len = b->cap = 0;
    return p;
}

void ptr_push(void ***v, size_t *n, size_t *cap, void *item) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = realloc(*v, *cap * sizeof **v);
    }
    (*v)[(*n)++] = item;
}

void buf_append_jstr(buf_t *b, const char *s) {
    cJSON *it = cJSON_CreateString(s ? s : "");
    char *p = cJSON_PrintUnformatted(it);
    buf_append_str(b, p);
    cJSON_free(p);
    cJSON_Delete(it);
}

/* ---- the agent's trimmed tool arguments ----
   first head and last tail bytes around [...], cut on sequence
   bounds - strings longer than twenty bytes elide (head = tail = 10) */
static void trim_utf8_head(buf_t *out, const char *s, size_t want) {
    while (want && *s) {
        size_t k = utf8_seq_len(s);
        if (want < k) break; /* a wider char than budget left */
        buf_append(out, s, k);
        s += k;
        want -= k;
    }
}

static void trim_utf8_tail(buf_t *out, const char *s, size_t want) {
    /* collect the sequence lengths front to back (a backward walk would
       misread the continuations), then take whole sequences from the
       end while the budget lasts */
    size_t n = strlen(s);
    size_t *lens = malloc((n ? n : 1) * sizeof *lens);
    if (!lens) return;
    size_t cnt = 0, i = 0;
    while (i < n) {
        size_t k = utf8_seq_len(s + i);
        if (i + k > n) k = 1; /* never past the end */
        lens[cnt++] = k;
        i += k;
    }
    size_t take = 0, j = cnt;
    while (j > 0 && take + lens[j - 1] <= want) {
        j--;
        take += lens[j];
    }
    buf_append(out, s + n - take, take);
    free(lens);
}

void buf_append_jstr_headtail(buf_t *b, const char *s, size_t head,
                              size_t tail) {
    if (!s) s = "";
    buf_t t;
    buf_init(&t);
    trim_utf8_head(&t, s, head);
    buf_append_str(&t, "[...]");
    trim_utf8_tail(&t, s, tail);
    buf_append_jstr(b, t.data ? t.data : "");
    buf_free(&t);
}

/* one value: strings trim, arrays and objects recurse, the rest render
   as the untrimmed appenders write them */
static void trim_append_value(buf_t *b, const cJSON *v) {
    if (cJSON_IsString(v) && v->valuestring) {
        if (strlen(v->valuestring) > 20) {
            buf_append_jstr_headtail(b, v->valuestring, 10, 10);
            return;
        }
        buf_append_jstr(b, v->valuestring);
        return;
    }
    if (cJSON_IsArray(v) || cJSON_IsObject(v)) {
        buf_append_byte(b, cJSON_IsArray(v) ? '[' : '{');
        bool first = true;
        for (const cJSON *c = v->child; c; c = c->next) {
            if (!first) buf_append_byte(b, ',');
            first = false;
            if (cJSON_IsObject(v)) {
                buf_append_jstr(b, c->string ? c->string : "");
                buf_append_byte(b, ':');
            }
            trim_append_value(b, c);
        }
        buf_append_byte(b, cJSON_IsArray(v) ? ']' : '}');
        return;
    }
    buf_append_tree(b, v);
}

void buf_append_tree_trim(buf_t *b, const cJSON *t) {
    if (!t) { buf_append_str(b, "null"); return; }
    trim_append_value(b, t);
}

void buf_append_tree(buf_t *b, const cJSON *t) {
    if (!t) { buf_append_str(b, "null"); return; }
    char *p = cJSON_PrintUnformatted(t);
    buf_append_str(b, p);
    cJSON_free(p);
}

void buf_append_jnum(buf_t *b, double d) {
    cJSON *it = cJSON_CreateNumber(d);
    char *p = cJSON_PrintUnformatted(it);
    buf_append_str(b, p);
    cJSON_free(p);
    cJSON_Delete(it);
}

/* one UTF-8 sequence's length from its lead byte; 1 for a bare or
   malformed lead, never past the end of a valid string */
size_t utf8_seq_len(const char *p) {
    if (!p) return 1;
    unsigned char c = (unsigned char)*p;
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    /* the continuations of the length are presumed: the callers only
       land inside valid UTF-8 (utf8_valid gates the inputs) */
    return n;
}

/* strict UTF-8: reject overlongs, surrogates, > U+10FFFF */
static bool utf8_scan(const uint8_t *p, size_t n, bool allow_truncated) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        int len;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + (size_t)len > n) return allow_truncated;
        for (int k = 1; k < len; k++) {
            uint8_t cc = p[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (len == 2 && cp < 0x80) return false;
        if (len == 3 && cp < 0x800) return false;
        if (len == 4 && cp < 0x10000) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        i += (size_t)len;
    }
    return true;
}

bool utf8_valid(const uint8_t *p, size_t n) { return utf8_scan(p, n, false); }

/* the same, but the final sequence may be cut short by the buffer end
   (a stream split mid-character is a prefix, not yet an error) */
bool utf8_valid_prefix(const uint8_t *p, size_t n) {
    return utf8_scan(p, n, true);
}
