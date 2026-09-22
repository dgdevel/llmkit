/* wire_anthropic.c — anthropic messages wire (design §5, §7).
   Includes the append-only cache_control marker overlay: 4 slots max, the
   first at the end of the static prefix (last tool definition, else the last
   system block), the rest on the last content block of the last message of
   each completed turn. Markers are written once and never moved or removed. */
#include "llmkit.h"

#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CACHE_MARK ",\"cache_control\":{\"type\":\"ephemeral\"}"

typedef struct ablock {
    int type; /* 0 text, 1 thinking, 2 tool_use */
    const trec_t *rec;
} ablock_t;

typedef struct awire awire_t;

struct ant_ctx {
    struct awire *w;
    engine_t *e;
};

typedef struct ant_block {
    int type; /* -1 unset, 0 text, 1 thinking, 2 tool_use */
    char *id;
    char *name;
    buf_t args;
    char *signature;
    bool done;
} ant_block_t;

typedef struct awire {
    wire_t base;
    buf_t msgbuf; /* "[msg,msg,..." without the closing bracket */
    size_t done_upto;
    unsigned long epoch; /* cfg_epoch of the last rebuild */
    size_t llm_mark;
    int markers_used;     /* slots consumed: static + turn markers */
    bool static_charged;
    blkemit_t be;
    sse_parser_t sse;
    struct ant_ctx sctx;
    ant_block_t blocks[64];
    size_t nblocks;
    char finish[64];
    bool have_finish;
    double usage_in, usage_out;
    bool have_in, have_out;
    bool sse_done;
    bool failed;
    char fail_msg[512];
} awire_t;

/* ---------------- serialization ---------------- */

static void append_msg_sep(buf_t *b) {
    if (b->len > 1) buf_append_byte(b, ',');
}

static void ant_user_msg(awire_t *w, const trec_t *u) {
    append_msg_sep(&w->msgbuf);
    buf_append_str(&w->msgbuf, "{\"role\":\"user\",\"content\":[");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(u->tree, "content");
    bool first = true;
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            if (!first) buf_append_byte(&w->msgbuf, ',');
            first = false;
            buf_append_tree(&w->msgbuf, b); /* native text blocks, verbatim */
        }
    buf_append_str(&w->msgbuf, "]}");
}

static bool thinking_enabled(engine_t *e) {
    const cJSON *io =
        cJSON_GetObjectItemCaseSensitive(e->llm, "inference_options");
    const cJSON *tb =
        io ? cJSON_GetObjectItemCaseSensitive(io, "thinking_budget") : NULL;
    return cJSON_IsNumber(tb);
}

/* thinking is resolvable: from the current llm and signed */
static bool think_ok(const trec_t *r, size_t idx, size_t llm_mark) {
    if (idx < llm_mark) return false; /* dropped on llm change */
    return r->signature && r->signature[0];
}

/* serializes one assistant group; returns true when the marker was placed */
static bool ant_assistant_msg(awire_t *w, engine_t *e, group_iter_t *g,
                              bool want_marker) {
    bool has_treq = g->rend > g->rbegin;
    bool think_en = thinking_enabled(e);

    /* signed thinking is resent only for tool turns; with thinking enabled
       and no signed thinking the tool records of the turn are omitted */
    bool omit_tools = false;
    if (has_treq && think_en) {
        bool any_ok = false;
        for (size_t i = g->begin; i < g->end; i++) {
            const trec_t *r = g->l->v[i];
            if (r->kind == T_THINK && think_ok(r, i, w->llm_mark)) any_ok = true;
        }
        if (!any_ok) omit_tools = true;
    }

    bool placed = false;
    buf_t b;
    buf_init(&b);
    bool any_block = false;
    for (size_t i = g->begin; i < g->end; i++) {
        const trec_t *r = g->l->v[i];
        if (r->kind == T_THINK) {
            if (!has_treq) continue;      /* final turns never resend thinking */
            if (!think_ok(r, i, w->llm_mark)) continue;
            if (any_block) buf_append_byte(&b, ',');
            any_block = true;
            buf_append_str(&b, "{\"type\":\"thinking\",\"thinking\":");
            buf_append_jstr(&b, r->text);
            buf_append_str(&b, ",\"signature\":");
            buf_append_jstr(&b, r->signature);
            buf_append_byte(&b, '}');
        } else if (r->kind == T_TEXT) {
            if (any_block) buf_append_byte(&b, ',');
            any_block = true;
            buf_append_str(&b, "{\"type\":\"text\",\"text\":");
            buf_append_jstr(&b, r->text);
            /* marker on the LAST content block only — never more than one
               per group, the breakpoint budget is 4 in total */
            if (want_marker && !has_treq && i + 1 == g->end) {
                buf_append_str(&b, CACHE_MARK);
                placed = true;
            }
            buf_append_byte(&b, '}');
        } else if (r->kind == T_TREQ && !omit_tools) {
            if (any_block) buf_append_byte(&b, ',');
            any_block = true;
            buf_append_str(&b, "{\"type\":\"tool_use\",\"id\":");
            buf_append_jstr(&b, r->id);
            buf_append_str(&b, ",\"name\":");
            buf_append_jstr(&b, r->tool);
            buf_append_str(&b, ",\"input\":");
            buf_append_tree(&b, r->args);
            buf_append_byte(&b, '}');
        }
    }
    if (any_block) {
        append_msg_sep(&w->msgbuf);
        buf_append_str(&w->msgbuf, "{\"role\":\"assistant\",\"content\":[");
        buf_append(&w->msgbuf, b.data ? b.data : "", b.len);
        buf_append_str(&w->msgbuf, "]}");
    }
    buf_free(&b);

    if (!omit_tools && g->rend > g->rbegin) {
        append_msg_sep(&w->msgbuf);
        buf_append_str(&w->msgbuf, "{\"role\":\"user\",\"content\":[");
        for (size_t i = g->rbegin; i < g->rend; i++) {
            const trec_t *r = g->l->v[i];
            if (i > g->rbegin) buf_append_byte(&w->msgbuf, ',');
            buf_append_str(&w->msgbuf, "{\"type\":\"tool_result\",\"tool_use_id\":");
            buf_append_jstr(&w->msgbuf, r->id);
            buf_append_str(&w->msgbuf, ",\"content\":");
            buf_append_jstr(&w->msgbuf, r->text);
            if (r->is_error) buf_append_str(&w->msgbuf, ",\"is_error\":true");
            /* the turn marker rides the last block of the last message */
            if (want_marker && i + 1 == g->rend)
                buf_append_str(&w->msgbuf, CACHE_MARK);
            buf_append_byte(&w->msgbuf, '}');
        }
        buf_append_str(&w->msgbuf, "]}");
        if (want_marker) placed = true; /* last message of the turn */
    }
    return placed;
}

static void ant_serialize_new(awire_t *w, engine_t *e) {
    group_iter_t gi;
    group_begin(&gi, &e->tr);
    int kg;
    while ((kg = group_next(&gi)) != G_DONE) {
        if (gi.start < w->done_upto) continue;
        /* Markers go on the last message of each completed turn (design §7):
           an assistant group is turn-final iff a user group follows it. The
           group that ends the transcript is the current, incomplete turn and
           is serialized without a marker — appending one later would rewrite
           already-sent prefix bytes. */
        bool want_marker = false;
        if (kg == G_ASSIST && w->markers_used < 4) {
            group_iter_t peek = gi;
            if (group_next(&peek) == G_USER) want_marker = true;
        }
        if (kg == G_USER) ant_user_msg(w, gi.user);
        else if (ant_assistant_msg(w, e, &gi, want_marker))
            w->markers_used++;
        w->done_upto = gi.stop;
    }
}

/* ---------------- request build ---------------- */

static const cJSON *io_get_ant(engine_t *e, const char *field) {
    const cJSON *io =
        cJSON_GetObjectItemCaseSensitive(e->llm, "inference_options");
    if (!io) return NULL;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(io, field);
    return cJSON_IsNull(f) ? NULL : f;
}

static cJSON *ant_validate(engine_t *e) {
    const cJSON *mt = io_get_ant(e, "max_tokens");
    if (!cJSON_IsNumber(mt))
        return rec_error(EC_INVALID_RECORD,
                         "anthropic requires max_tokens in inference_options",
                         true);
    const cJSON *tb = io_get_ant(e, "thinking_budget");
    if (cJSON_IsNumber(tb) && tb->valuedouble >= mt->valuedouble)
        return rec_error(EC_INVALID_RECORD,
                         "thinking_budget must be lower than max_tokens", true);
    if (e->tr.n && e->tr.v[0]->kind != T_USER)
        return rec_error(
            EC_INVALID_RECORD,
            "anthropic requires the first message to have role user", true);
    return NULL;
}

static void append_system_param(buf_t *b, engine_t *e, bool marker) {
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(e->system, "content");
    buf_append_str(b, "\"system\":[");
    bool first = true;
    size_t count = 0;
    if (cJSON_IsArray(content))
        for (const cJSON *bl = content->child; bl; bl = bl->next) count++;
    size_t i = 0;
    if (cJSON_IsArray(content))
        for (const cJSON *bl = content->child; bl; bl = bl->next, i++) {
            if (!first) buf_append_byte(b, ',');
            first = false;
            if (marker && i + 1 == count) {
                /* embed verbatim with the marker injected before the last
                   block's closing brace */
                char *p = cJSON_PrintUnformatted(bl);
                size_t pl = strlen(p);
                if (pl) {
                    buf_append(b, p, pl - 1);
                    buf_append_str(b, CACHE_MARK);
                    buf_append_byte(b, '}');
                }
                cJSON_free(p);
            } else {
                buf_append_tree(b, bl);
            }
        }
    buf_append_str(b, "],");
}

/* returns stream flag; *err set on validation failure */
static bool ant_build_body(awire_t *w, engine_t *e, cJSON **err) {
    if ((*err = ant_validate(e))) return false;

    bool have_tools = e->tools && e->tools->n;
    bool static_present = have_tools || e->system;

    bool rebuild = w->epoch != e->cfg_epoch || e->tr.n < w->done_upto;
    if (rebuild) {
        buf_clear(&w->msgbuf);
        buf_append_byte(&w->msgbuf, '[');
        w->done_upto = 0;
        w->epoch = e->cfg_epoch;
        w->llm_mark = e->llm_mark;
        w->markers_used = 0;
        w->static_charged = false;
    }
    /* the static marker consumes one of the 4 slots, once */
    bool static_marker = false;
    if (static_present && !w->static_charged) {
        w->static_charged = true;
        if (w->markers_used < 4) {
            w->markers_used++;
            static_marker = true;
        }
    }
    ant_serialize_new(w, e);

    const cJSON *sf = io_get_ant(e, "stream");
    bool stream = sf ? cJSON_IsTrue(sf) : true;

    buf_t *b = &w->base.last_body;
    buf_clear(b);
    buf_append_byte(b, '{');
    const char *model = rec_str(e->llm, "model");
    if (model) {
        buf_append_str(b, "\"model\":");
        buf_append_jstr(b, model);
        buf_append_byte(b, ',');
    }
    if (e->system) append_system_param(b, e, static_marker && !have_tools);
    buf_append_str(b, "\"messages\":");
    buf_append(b, w->msgbuf.data ? w->msgbuf.data : "[", w->msgbuf.len);
    buf_append_byte(b, ']');

    if (have_tools) {
        buf_append_str(b, ",\"tools\":[");
        for (size_t i = 0; i < e->tools->n; i++) {
            const tool_entry_t *t = &e->tools->v[i];
            if (i) buf_append_byte(b, ',');
            buf_append_str(b, "{\"name\":");
            buf_append_jstr(b, t->exposed_name);
            const cJSON *desc =
                cJSON_GetObjectItemCaseSensitive(t->tool, "description");
            if (cJSON_IsString(desc) && desc->valuestring) {
                buf_append_str(b, ",\"description\":");
                buf_append_jstr(b, desc->valuestring);
            }
            const cJSON *sch =
                cJSON_GetObjectItemCaseSensitive(t->tool, "inputSchema");
            if (sch) {
                buf_append_str(b, ",\"input_schema\":");
                buf_append_tree(b, sch);
            }
            if (static_marker && i + 1 == e->tools->n)
                buf_append_str(b, CACHE_MARK);
            buf_append_byte(b, '}');
        }
        buf_append_byte(b, ']');
    }

    /* sampling fields in requirements §4 order; max_tokens is pulled to the
       end per design §5 */
    const cJSON *f;
    if ((f = io_get_ant(e, "temperature")) && cJSON_IsNumber(f)) {
        buf_append_str(b, ",\"temperature\":");
        buf_append_jnum(b, f->valuedouble);
    }
    if ((f = io_get_ant(e, "top_p")) && cJSON_IsNumber(f)) {
        buf_append_str(b, ",\"top_p\":");
        buf_append_jnum(b, f->valuedouble);
    }
    if ((f = io_get_ant(e, "stop"))) {
        buf_append_str(b, ",\"stop_sequences\":");
        buf_append_tree(b, f);
    }
    if ((f = io_get_ant(e, "top_k")) && cJSON_IsNumber(f)) {
        buf_append_str(b, ",\"top_k\":");
        buf_append_jnum(b, f->valuedouble);
    }
    if ((f = io_get_ant(e, "thinking_budget")) && cJSON_IsNumber(f)) {
        buf_append_str(b, ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":");
        buf_append_jnum(b, f->valuedouble);
        buf_append_byte(b, '}');
    }
    f = io_get_ant(e, "max_tokens");
    buf_append_str(b, ",\"max_tokens\":");
    buf_append_jnum(b, f ? f->valuedouble : 0);
    buf_appendf(b, ",\"stream\":%s}", stream ? "true" : "false");
    return stream;
}

/* ---------------- streaming ---------------- */

static const char *finish_norm_ant(const char *sr) {
    if (!sr) return "stop";
    if (!strcmp(sr, "end_turn") || !strcmp(sr, "stop_sequence")) return "stop";
    if (!strcmp(sr, "max_tokens")) return "length";
    if (!strcmp(sr, "refusal")) return "content_filter";
    if (!strcmp(sr, "tool_use")) return "tool_use";
    return sr;
}

static ant_block_t *ant_block_at(awire_t *w, int idx) {
    if (idx < 0 || (size_t)idx >= 64) return NULL;
    for (size_t i = w->nblocks; i <= (size_t)idx; i++) {
        w->blocks[i].type = -1;
        w->blocks[i].id = NULL;
        w->blocks[i].name = NULL;
        buf_init(&w->blocks[i].args);
        w->blocks[i].signature = NULL;
        w->blocks[i].done = false;
    }
    if ((size_t)idx + 1 > w->nblocks) w->nblocks = (size_t)idx + 1;
    return &w->blocks[idx];
}

static void ant_blocks_reset(awire_t *w) {
    for (size_t i = 0; i < w->nblocks; i++) {
        free(w->blocks[i].id);
        free(w->blocks[i].name);
        free(w->blocks[i].signature);
        buf_free(&w->blocks[i].args);
    }
    w->nblocks = 0;
}

static void ant_emit_tool(awire_t *w, engine_t *e, ant_block_t *bl, bool last) {
    cJSON *args = cJSON_Parse(bl->args.data ? bl->args.data : "{}");
    if (!args) args = cJSON_CreateObject();
    cJSON *rec = rec_tool_request(bl->name, args, bl->id);
    cJSON_Delete(args);
    if (last) {
        if (w->have_in || w->have_out)
            rec_attach_usage(rec, w->usage_in, w->have_out ? w->usage_out : 0);
        if (w->have_finish)
            rec_attach_finish(rec, finish_norm_ant(w->finish));
    }
    engine_emit_record(e, rec);
}

static void ant_event(void *ctx, const char *event, const char *data,
                      size_t n) {
    struct ant_ctx *c = ctx;
    awire_t *w = c->w;
    if (!strcmp(event, "ping")) return;
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    if (!strcmp(event, "message_start")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(d, "message");
        const cJSON *usage =
            msg ? cJSON_GetObjectItemCaseSensitive(msg, "usage") : NULL;
        if (usage) {
            w->usage_in = rec_num(usage, "input_tokens", 0);
            w->have_in = true;
        }
    } else if (!strcmp(event, "content_block_start")) {
        int idx = (int)rec_num(d, "index", 0);
        const cJSON *cb = cJSON_GetObjectItemCaseSensitive(d, "content_block");
        const char *ty = rec_str(cb, "type");
        ant_block_t *bl = ant_block_at(w, idx);
        if (bl) {
            if (ty && !strcmp(ty, "thinking")) {
                bl->type = 1;
                blk_stop(&w->be); /* a new block closes any open one */
            } else if (ty && !strcmp(ty, "text")) {
                bl->type = 0;
                blk_stop(&w->be);
            } else if (ty && !strcmp(ty, "tool_use")) {
                bl->type = 2;
                blk_stop(&w->be);
                const char *id = rec_str(cb, "id");
                const char *nm = rec_str(cb, "name");
                bl->id = strdup(id ? id : "");
                bl->name = strdup(nm ? nm : "");
            }
        }
    } else if (!strcmp(event, "content_block_delta")) {
        int idx = (int)rec_num(d, "index", 0);
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(d, "delta");
        const char *ty = rec_str(delta, "type");
        ant_block_t *bl = ant_block_at(w, idx);
        if (!bl) goto out;
        if (ty && !strcmp(ty, "text_delta")) {
            const char *t = rec_str(delta, "text");
            if (t) blk_delta(&w->be, 0, t, strlen(t));
        } else if (ty && !strcmp(ty, "thinking_delta")) {
            const char *t = rec_str(delta, "thinking");
            if (t) blk_delta(&w->be, 1, t, strlen(t));
        } else if (ty && !strcmp(ty, "signature_delta")) {
            const char *s = rec_str(delta, "signature");
            if (s) {
                free(bl->signature);
                bl->signature = strdup(s);
            }
        } else if (ty && !strcmp(ty, "input_json_delta")) {
            const char *pj = rec_str(delta, "partial_json");
            if (pj) buf_append_str(&bl->args, pj);
        }
    } else if (!strcmp(event, "content_block_stop")) {
        ant_block_t *bl = ant_block_at(w, (int)rec_num(d, "index", 0));
        if (!bl) goto out;
        bl->done = true;
        if (bl->type == 0) blk_stop(&w->be);
        else if (bl->type == 1) blk_stop_thinking(&w->be, bl->signature);
        /* tool_use blocks are emitted at message_stop */
    } else if (!strcmp(event, "message_delta")) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(d, "delta");
        const char *sr = rec_str(delta, "stop_reason");
        if (sr) {
            snprintf(w->finish, sizeof w->finish, "%s", sr);
            w->have_finish = true;
        }
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(d, "usage");
        if (usage) {
            w->usage_out = rec_num(usage, "output_tokens", 0);
            w->have_out = true;
        }
    } else if (!strcmp(event, "message_stop")) {
        w->sse_done = true;
    } else if (!strcmp(event, "error")) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(d, "error");
        const char *m = rec_str(err, "message");
        snprintf(w->fail_msg, sizeof w->fail_msg, "%s", m ? m : "unknown error");
        w->failed = true;
        w->sse_done = true;
    }
out:
    cJSON_Delete(d);
}

static void ant_http_data(void *ctx, const char *bytes, size_t n) {
    struct ant_ctx *c = ctx;
    sse_feed(&c->w->sse, bytes, n);
}

static int ant_finish_stream(awire_t *w, engine_t *e, turn_out_t *out) {
    blk_stop(&w->be);
    size_t ntools = 0;
    for (size_t i = 0; i < w->nblocks; i++)
        if (w->blocks[i].type == 2) ntools++;
    if (ntools) {
        blk_emit_pending(&w->be);
        size_t seen = 0;
        for (size_t i = 0; i < w->nblocks; i++) {
            if (w->blocks[i].type != 2) continue;
            seen++;
            ant_emit_tool(w, e, &w->blocks[i], seen == ntools);
        }
        return TURN_TOOLS;
    }
    out->final_rec = blk_take_pending(&w->be);
    if (out->final_rec) {
        if (w->have_in || w->have_out)
            rec_attach_usage(out->final_rec, w->usage_in,
                             w->have_out ? w->usage_out : 0);
        if (w->have_finish)
            rec_attach_finish(out->final_rec, finish_norm_ant(w->finish));
    }
    return TURN_FINAL;
}

/* ---------------- non-streaming ---------------- */

static int ant_body_map(awire_t *w, engine_t *e, const cJSON *body,
                        turn_out_t *out) {
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(body, "content");
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(body, "usage");
    if (usage) {
        w->usage_in = rec_num(usage, "input_tokens", 0);
        w->usage_out = rec_num(usage, "output_tokens", 0);
        w->have_in = w->have_out = true;
    }
    const char *sr = rec_str(body, "stop_reason");
    if (sr) {
        snprintf(w->finish, sizeof w->finish, "%s", sr);
        w->have_finish = true;
    }
    size_t ntools = 0;
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const char *ty = rec_str(b, "type");
            if (ty && !strcmp(ty, "tool_use")) ntools++;
        }
    size_t seen = 0;
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const char *ty = rec_str(b, "type");
            if (ty && !strcmp(ty, "thinking")) {
                const char *t = rec_str(b, "thinking");
                if (t) blk_delta(&w->be, 1, t, strlen(t));
                blk_stop_thinking(&w->be, rec_str(b, "signature"));
            } else if (ty && !strcmp(ty, "text")) {
                const char *t = rec_str(b, "text");
                if (t) blk_delta(&w->be, 0, t, strlen(t));
                blk_stop(&w->be);
            } else if (ty && !strcmp(ty, "tool_use")) {
                seen++;
                const cJSON *input = cJSON_GetObjectItemCaseSensitive(b, "input");
                cJSON *rec = rec_tool_request(rec_str(b, "name"), input,
                                              rec_str(b, "id"));
                if (seen == ntools) {
                    if (usage)
                        rec_attach_usage(rec, w->usage_in, w->usage_out);
                    if (sr) rec_attach_finish(rec, finish_norm_ant(sr));
                }
                engine_emit_record(e, rec);
            }
        }
    if (ntools) {
        blk_emit_pending(&w->be);
        return TURN_TOOLS;
    }
    out->final_rec = blk_take_pending(&w->be);
    if (out->final_rec) {
        if (w->have_in || w->have_out)
            rec_attach_usage(out->final_rec, w->usage_in, w->usage_out);
        if (w->have_finish)
            rec_attach_finish(out->final_rec, finish_norm_ant(w->finish));
    }
    return TURN_FINAL;
}

/* ---------------- errors (same shapes as openai) ---------------- */

static cJSON *ant_transport_error(http_req_t *req) {
    char msg[512];
    if (req->curl_res == CURLE_OPERATION_TIMEDOUT && !req->got_data)
        return rec_error(EC_CONNECT_FAILED, "connect to llm endpoint timed out",
                         true);
    if (req->curl_res == CURLE_COULDNT_RESOLVE_HOST ||
        req->curl_res == CURLE_COULDNT_CONNECT ||
        req->curl_res == CURLE_SSL_CONNECT_ERROR) {
        snprintf(msg, sizeof msg, "cannot reach llm endpoint: %s",
                 curl_easy_strerror(req->curl_res));
        return rec_error(EC_CONNECT_FAILED, msg, true);
    }
    snprintf(msg, sizeof msg, "llm endpoint transport error: %s",
             curl_easy_strerror(req->curl_res));
    return rec_error(EC_HTTP_ERROR, msg, true);
}

static cJSON *ant_status_error(http_req_t *req) {
    cJSON *body = cJSON_ParseWithLength(req->resp.data ? req->resp.data : "",
                                        req->resp.len);
    char msg[768];
    const cJSON *err =
        body ? cJSON_GetObjectItemCaseSensitive(body, "error") : NULL;
    if (err) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(m) && m->valuestring) {
            snprintf(msg, sizeof msg, "HTTP %ld: %s", req->status,
                     m->valuestring);
            cJSON_Delete(body);
            return rec_error(EC_API_ERROR, msg, true);
        }
    }
    cJSON_Delete(body);
    size_t n = req->resp.len > 200 ? 200 : req->resp.len;
    char cut[256] = "";
    if (n) {
        memcpy(cut, req->resp.data, n);
        cut[n] = '\0';
    }
    snprintf(msg, sizeof msg, "HTTP %ld: %s", req->status, cut);
    return rec_error(EC_HTTP_ERROR, msg, true);
}

/* ---------------- turn driver ---------------- */

static int awire_turn(wire_t *base, engine_t *e, turn_out_t *out) {
    awire_t *w = (awire_t *)base;
    memset(out, 0, sizeof *out);
    cJSON *verr = NULL;
    bool stream = ant_build_body(w, e, &verr);
    if (verr) {
        out->error_rec = verr;
        return TURN_FATAL;
    }

    blk_begin_turn(&w->be, e);
    w->be.streaming = stream;
    ant_blocks_reset(w);
    w->have_finish = w->have_in = w->have_out = false;
    w->sse_done = false;
    w->failed = false;
    w->finish[0] = '\0';
    w->fail_msg[0] = '\0';
    sse_free(&w->sse);
    w->sctx.w = w;
    w->sctx.e = e;
    sse_init(&w->sse, ant_event, &w->sctx);

    buf_t url;
    buf_init(&url);
    buf_append_str(&url, rec_str(e->llm, "api_base"));
    buf_append_str(&url, "/messages");

    struct curl_slist *hdrs = NULL;
    http_hdr_add_json(&hdrs);
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(e->llm, "headers");
    bool auth_override = false;
    if (cJSON_IsObject(custom))
        for (const cJSON *it = custom->child; it; it = it->next)
            if (it->string && !strcasecmp(it->string, "Authorization"))
                auth_override = true;
    const char *key = rec_str(e->llm, "api_key");
    if (key && key[0] && !auth_override) {
        buf_t auth;
        buf_init(&auth);
        buf_appendf(&auth, "Authorization: Bearer %s", key);
        hdrs = curl_slist_append(hdrs, auth.data);
        buf_free(&auth);
    }
    char errh[256] = "";
    http_hdrs_from_json(&hdrs, custom, errh, sizeof errh);
    if (stream) http_hdr_add(&hdrs, "Accept", "text/event-stream");

    http_req_t req;
    memset(&req, 0, sizeof req);
    buf_init(&req.resp);
    buf_init(&req.content_type);
    req.url = url.data;
    req.hdrs = hdrs;
    req.body = w->base.last_body.data;
    req.body_len = w->base.last_body.len;
    req.connect_to = e->llm_connect_timeout;
    req.read_to = e->llm_read_timeout;
    if (stream) {
        req.on_data = ant_http_data;
        req.cb_ctx = &w->sctx;
    }

    int rc = http_perform(&req);
    int result;
    if (rc == -1) {
        blk_abort(&w->be);
        result = g_stop_flag ? TURN_ABORTED : TURN_FATAL;
        if (result == TURN_FATAL) out->error_rec = ant_transport_error(&req);
    } else if (rc == 1) {
        out->error_rec = ant_status_error(&req);
        result = TURN_FATAL;
    } else if (w->failed) {
        out->error_rec = rec_error(EC_API_ERROR, w->fail_msg, true);
        result = TURN_FATAL;
    } else if (stream && !(req.content_type.data &&
                          strncasecmp(req.content_type.data, "application/json",
                                      strlen("application/json")) == 0)) {
        sse_eof(&w->sse);
        if (!w->sse_done) {
            out->error_rec =
                rec_error(EC_API_ERROR, "stream ended without message_stop", true);
            result = TURN_FATAL;
        } else {
            result = ant_finish_stream(w, e, out);
        }
    } else {
        cJSON *body = cJSON_ParseWithLength(req.resp.data ? req.resp.data : "",
                                            req.resp.len);
        if (!body) {
            out->error_rec = rec_error(EC_HTTP_ERROR,
                                       "endpoint returned a non-json body", true);
            result = TURN_FATAL;
        } else {
            result = ant_body_map(w, e, body, out);
            cJSON_Delete(body);
        }
    }

    buf_free(&req.resp);
    buf_free(&req.content_type);
    curl_slist_free_all(hdrs);
    buf_free(&url);
    sse_free(&w->sse);
    return result;
}

static int awire_build(wire_t *base, engine_t *e) {
    awire_t *w = (awire_t *)base;
    cJSON *verr = NULL;
    ant_build_body(w, e, &verr);
    if (verr) {
        e->emit(e->emit_ctx, verr);
        return EXIT_INVALID_RECORD;
    }
    return 0;
}

static void awire_destroy(wire_t *base) {
    awire_t *w = (awire_t *)base;
    buf_free(&w->msgbuf);
    blk_free(&w->be);
    sse_free(&w->sse);
    ant_blocks_reset(w);
    buf_free(&base->last_body);
    free(w);
}

wire_t *wire_anthropic_new(void) {
    awire_t *w = calloc(1, sizeof *w);
    w->epoch = ULONG_MAX; /* force the first build to serialize everything */
    w->base.turn = awire_turn;
    w->base.build = awire_build;
    w->base.destroy = awire_destroy;
    buf_init(&w->msgbuf);
    buf_init(&w->base.last_body);
    blk_init(&w->be);
    return &w->base;
}
