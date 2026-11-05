/* jstream.c - incremental JSON tool-arguments decoder for the live file
   preview (repl/agent display).

   A tool call's arguments arrive over the wire as a byte stream of JSON
   text; the display wants the decoded bytes of one string field -
   file_create's `content` - the moment they parse, so the user watches
   the file generate before the request completes and the tool runs.

   jstream_feed hands the raw stream bytes; the wanted field's decoded
   bytes leave through the callback as they become known: literal bytes
   one by one, an escape only once its whole sequence landed (a \uXXXX
   may split across feeds - the partial hex is held). The decoder keeps
   no copy of the stream, so the preview costs O(1) memory beyond the
   bytes already rendered.

   The shape it walks is a tool-arguments object: {"key":"value",...}
   with flat string values - the built-in tools' arguments are exactly
   that. A non-string value (number, literal, nested container) is
   skipped whole; nesting inside a skipped value is balanced with its
   own depth count. A syntax break kills the stream (feed returns
   false): the caller drops the preview and the final tool_request
   record renders as always. */
#include "llmkit.h"

#include <string.h>

enum {
    JS_TOP,      /* before the opening '{' */
    JS_KEY,      /* inside a key string */
    JS_KEY_ESC,  /* an escape within a key */
    JS_SEP,      /* after '{' or ',': key string or '}' */
    JS_COLON,    /* after a key, expecting ':' */
    JS_VAL,      /* after ':', the value's first byte decides */
    JS_STR,      /* inside the wanted string value */
    JS_STR_ESC,  /* an escape within the wanted string */
    JS_STR_U,    /* \u: collecting the four hex digits */
    JS_SKIP,     /* a non-string value: number or literal */
    JS_SKIP_STR, /* a string value being skipped */
    JS_SKIP_ESC, /* an escape within a skipped string */
    JS_SKIP_CONT, /* a container value being skipped: brace-balanced */
    JS_SKIP_CONT_STR, /* a string inside a skipped container */
    JS_SKIP_CONT_ESC, /* an escape within a skipped container string */
    JS_SEP_END   /* a value closed: ',' or '}' (or whitespace) next */
};

static bool js_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* UTF-8 of one codepoint; the escape is complete, so the full sequence
   emits at once. lone surrogates land as U+FFFD (replacement), pairs
   combine */
static void js_emit_cp(jstream_t *js, unsigned cp) {
    char u[4];
    size_t n = 0;
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        u[n++] = (char)(0xF0 | (cp >> 18));
        u[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp >= 0x800) {
        u[n++] = (char)(0xE0 | (cp >> 12));
        u[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp >= 0x80) {
        u[n++] = (char)(0xC0 | (cp >> 6));
        u[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        u[n++] = (char)cp;
    }
    if (js->cb) js->cb(js->ctx, u, n);
}

void jstream_init(jstream_t *js, jstream_cb cb, void *ctx,
                  const char *field) {
    memset(js, 0, sizeof *js);
    js->cb = cb;
    js->ctx = ctx;
    js->field = field;
    js->state = JS_TOP;
    buf_init(&js->key);
    buf_init(&js->hex);
}

void jstream_free(jstream_t *js) {
    buf_free(&js->key);
    buf_free(&js->hex);
}

bool jstream_feed(jstream_t *js, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        switch (js->state) {
        case JS_TOP:
            if (c == '{') js->state = JS_SEP;
            else if (!js_space(c)) return false;
            break;
        case JS_KEY:
            if (c == '"') {
                js->state = JS_COLON;
            } else if (c == '\\') {
                js->state = JS_KEY_ESC;
            } else {
                buf_append_byte(&js->key, c);
            }
            break;
        case JS_KEY_ESC:
            /* keys compare literally: the built-in tools' argument names
               are plain ascii words, an escaped key byte lands raw and
               the field match simply fails */
            buf_append_byte(&js->key, c);
            js->state = JS_KEY;
            break;
        case JS_SEP:
            if (c == '"') {
                buf_clear(&js->key);
                js->state = JS_KEY;
            } else if (c == '}') {
                js->state = JS_TOP; /* object closed */
                js->done = true;
            } else if (!js_space(c)) {
                return false;
            }
            break;
        case JS_COLON:
            if (c == ':') js->state = JS_VAL;
            else if (!js_space(c)) return false;
            break;
        case JS_VAL:
            if (c == '"') {
                /* the wanted field opens exactly here */
                js->wanting = js->key.data && js->field &&
                              !strcmp(js->key.data, js->field);
                buf_clear(&js->hex);
                js->surrogate = 0;
                js->state = js->wanting ? JS_STR : JS_SKIP_STR;
            } else if (c == '{' || c == '[') {
                js->skip_depth = 1;
                js->state = JS_SKIP_CONT;
            } else if (js_space(c)) {
                /* value still to start */
            } else {
                js->state = JS_SKIP; /* number or literal */
                if (c != '-' && (c < '0' || c > '9') && c != 't' &&
                    c != 'f' && c != 'n')
                    return false;
            }
            break;
        case JS_STR:
            if (c == '"') {
                js->wanting = false;
                js->state = JS_SEP_END;
            } else if (c == '\\') {
                js->state = JS_STR_ESC;
            } else {
                if (js->cb) js->cb(js->ctx, &c, 1);
            }
            break;
        case JS_STR_ESC:
            switch (c) {
            case 'u':
                buf_clear(&js->hex);
                js->state = JS_STR_U;
                continue;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: return false;
            }
            if (js->cb) js->cb(js->ctx, &c, 1);
            js->state = JS_STR;
            break;
        case JS_STR_U: {
            unsigned v;
            if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (unsigned)(c - 'A' + 10);
            else return false;
            buf_append_byte(&js->hex, c);
            (void)v;
            if (js->hex.len == 4) {
                unsigned cp = 0;
                for (size_t k = 0; k < 4; k++) {
                    char h = js->hex.data[k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (unsigned)(h - 'a' + 10);
                    else cp |= (unsigned)(h - 'A' + 10);
                }
                buf_clear(&js->hex);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    js->surrogate = cp; /* lead: hold for the pair */
                } else if (cp >= 0xDC00 && cp <= 0xDFFF && js->surrogate) {
                    unsigned pair = 0x10000 +
                                    ((js->surrogate - 0xD800) << 10) +
                                    (cp - 0xDC00);
                    js->surrogate = 0;
                    js_emit_cp(js, pair);
                } else {
                    if (js->surrogate) { /* lone lead: replacement */
                        js->surrogate = 0;
                        js_emit_cp(js, 0xFFFD);
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    js_emit_cp(js, cp);
                }
                js->state = JS_STR;
            }
            break;
        }
        case JS_SKIP:
            /* number or literal: bytes until the separator */
            if (c == ',') {
                js->state = JS_SEP;
            } else if (c == '}') {
                js->state = JS_TOP;
                js->done = true;
            } else if (c == ']' || c == '[' || c == '{' || c == '"') {
                return false;
            } else if (js_space(c)) {
                js->state = JS_SEP_END;
            }
            break;
        case JS_SKIP_STR:
            if (c == '"') js->state = JS_SEP_END;
            else if (c == '\\') js->state = JS_SKIP_ESC;
            break;
        case JS_SKIP_ESC:
            js->state = JS_SKIP_STR; /* any escape byte: skip it */
            break;
        case JS_SKIP_CONT:
            if (c == '{' || c == '[') js->skip_depth++;
            else if (c == '}') {
                if (--js->skip_depth == 0) {
                    js->state = JS_SEP_END;
                    break;
                }
            } else if (c == ']') {
                if (--js->skip_depth == 0) js->state = JS_SEP_END;
            } else if (c == '"') {
                /* strings inside a skipped container still need escape
                   awareness - a brace inside one must not count */
                js->state = JS_SKIP_CONT_STR;
            }
            break;
        case JS_SKIP_CONT_STR:
            if (c == '\\') js->state = JS_SKIP_CONT_ESC;
            else if (c == '"') js->state = JS_SKIP_CONT;
            break;
        case JS_SKIP_CONT_ESC:
            js->state = JS_SKIP_CONT_STR;
            break;
        case JS_SEP_END:
            if (c == ',') {
                js->state = JS_SEP;
            } else if (c == '}') {
                js->state = JS_TOP;
                js->done = true;
            } else if (!js_space(c)) {
                return false;
            }
            break;
        }
        }
        return true;
    }
