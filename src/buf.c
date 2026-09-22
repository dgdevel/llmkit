/* buf.c — byte buffers, deterministic JSON appenders, UTF-8 validation. */
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

void buf_append_jstr(buf_t *b, const char *s) {
    cJSON *it = cJSON_CreateString(s ? s : "");
    char *p = cJSON_PrintUnformatted(it);
    buf_append_str(b, p);
    cJSON_free(p);
    cJSON_Delete(it);
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

/* strict UTF-8: reject overlongs, surrogates, > U+10FFFF */
bool utf8_valid(const uint8_t *p, size_t n) {
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
        if (i + (size_t)len > n) return false;
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
