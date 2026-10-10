/* sse.c - the one shared SSE line parser (design sec.5).
   event/data accumulation, blank-line dispatch, comments and id/ignored
   fields skipped. Chunk-safe: feed at any byte boundary. */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>

static void dispatch(sse_parser_t *p) {
    /* only data-carrying events dispatch; an event name without data is not
       an SSE event */
    if (p->have_data) {
        p->cb(p->ctx, p->event.len ? p->event.data : "message",
              p->data.data ? p->data.data : "", p->data.len);
    }
    p->event.len = 0;
    if (p->event.data) p->event.data[0] = '\0';
    p->data.len = 0;
    if (p->data.data) p->data.data[0] = '\0';
    p->have_data = false;
}

static void handle_line(sse_parser_t *p, const char *line, size_t n) {
    if (n == 0) { dispatch(p); return; } /* blank line */
    if (line[0] == ':') return;          /* comment */
    size_t i = 0;
    while (i < n && line[i] != ':') i++;
    /* field name = [0,i), value: skip one leading space after ':' */
    const char *val = "";
    size_t vlen = 0;
    if (i < n) { /* ':' present */
        val = line + i + 1;
        vlen = n - i - 1;
        if (vlen && val[0] == ' ') { val++; vlen--; }
    }
    if (i == 5 && memcmp(line, "event", 5) == 0) {
        buf_clear(&p->event);
        buf_append(&p->event, val, vlen);
    } else if (i == 4 && memcmp(line, "data", 4) == 0) {
        if (p->have_data) buf_append_byte(&p->data, '\n');
        buf_append(&p->data, val, vlen);
        p->have_data = true;
    }
    /* id, retry and unknown fields: ignored */
}

void sse_init(sse_parser_t *p, sse_cb cb, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ctx = ctx;
    buf_init(&p->line);
    buf_init(&p->event);
    buf_init(&p->data);
}

void sse_free(sse_parser_t *p) {
    buf_free(&p->line);
    buf_free(&p->event);
    buf_free(&p->data);
}

void sse_feed(sse_parser_t *p, const char *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] == '\n') {
            /* strip a trailing CR (be liberal: SSE allows CRLF) */
            size_t l = p->line.len;
            if (l && p->line.data[l - 1] == '\r') l--;
            handle_line(p, p->line.data ? p->line.data : "", l);
            buf_clear(&p->line);
        } else {
            buf_append_byte(&p->line, bytes[i]);
        }
    }
}

void sse_eof(sse_parser_t *p) {
    if (p->line.len) {
        size_t l = p->line.len;
        if (l && p->line.data[l - 1] == '\r') l--;
        handle_line(p, p->line.data, l);
        buf_clear(&p->line);
    }
    dispatch(p); /* a final unterminated event still dispatches */
}

/* ================= llm stream accumulation ================= */
/* the tool/block bookkeeping every llm sse consumer (both wires, the
   proxy) keeps while parsing a streamed turn */

sse_slot_t *sse_slot_at(sse_slot_t **arr, size_t *n, double idx) {
    if (!(idx >= 0) || idx > (double)MAX_SSE_SLOTS) return NULL;
    size_t i = (size_t)idx;
    if (i >= *n) {
        size_t nn = i + 1;
        sse_slot_t *grown = realloc(*arr, nn * sizeof **arr);
        if (!grown) return NULL;
        *arr = grown;
        for (size_t k = *n; k < nn; k++) {
            (*arr)[k].id = strdup("");
            (*arr)[k].name = strdup("");
            buf_init(&(*arr)[k].args);
        }
        *n = nn;
    }
    return &(*arr)[i];
}

void sse_slots_reset(sse_slot_t **arr, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        free((*arr)[i].id);
        free((*arr)[i].name);
        buf_free(&(*arr)[i].args);
    }
    free(*arr);
    *arr = NULL;
    *n = 0;
}

void sse_slot_set(sse_slot_t *s, const char *id, const char *name,
                  const char *args /* replaces when non-NULL */) {
    if (id) {
        free(s->id);
        s->id = strdup(id);
    }
    if (name) {
        free(s->name);
        s->name = strdup(name);
    }
    if (args) {
        buf_clear(&s->args);
        buf_append_str(&s->args, args);
    }
}

sse_block_t *sse_block_at(sse_block_t *blocks, size_t *n, int idx) {
    if (idx < 0 || (size_t)idx >= MAX_SSE_BLOCKS) return NULL;
    for (size_t i = *n; i <= (size_t)idx; i++)
        blocks[i].type = -1; /* calloc-zeroed fields stay cleared */
    if ((size_t)idx + 1 > *n) *n = (size_t)idx + 1;
    return &blocks[idx];
}

void sse_blocks_reset(sse_block_t *blocks, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        free(blocks[i].id);
        free(blocks[i].name);
        free(blocks[i].signature);
        buf_free(&blocks[i].args);
        blocks[i].id = blocks[i].name = blocks[i].signature = NULL;
    }
    *n = 0;
}
