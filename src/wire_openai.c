/* wire_openai.c — chat completions + responses wires (design §5, §7).
   Serialization: per-conversation append-only message array buffer; only
   envelope fields are rebuilt per request, so sampling-only changes never
   touch the prefix bytes. */
#include "llmkit.h"

#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------- helpers ---------------- */

static const char *print_args(const cJSON *args) {
    static _Thread_local buf_t ab;
    static _Thread_local bool init = false;
    if (!init) { buf_init(&ab); init = true; }
    buf_clear(&ab);
    if (args) buf_append_tree(&ab, args);
    else buf_append_str(&ab, "{}");
    return ab.data ? ab.data : "{}";
}

static void append_msg_sep(buf_t *b) {
    if (b->len > 1) buf_append_byte(b, ','); /* "[" alone takes no comma */
}

/* ---------------- openai wire state ---------------- */

typedef struct {
    char *id;
    char *name;
    buf_t args;
} tslot_t;

typedef struct owire owire_t;

struct sse_ctx {
    struct owire *w;
    engine_t *e;
};

typedef struct owire {
    wire_t base;
    int proto; /* PROTO_OPENAI | PROTO_RESPONSES */
    /* prefix state: holds "[msg,msg,..." without the closing bracket */
    buf_t msgbuf;
    size_t done_upto; /* transcript records serialized */
    unsigned long epoch; /* cfg_epoch of the last rebuild */
    size_t llm_mark; /* thinking drop point (llm change) */
    /* per-turn state */
    blkemit_t be;
    sse_parser_t sse;
    struct sse_ctx sctx;
    bool sse_done; /* [DONE] / terminal event seen */
    bool failed;
    char fail_msg[512];
    tslot_t *slots;  /* chat tool accumulation, by index */
    size_t nslots;
    tslot_t *rslots; /* responses tool accumulation, by output_index */
    size_t nrslots;
    char finish[64];
    bool have_finish;
    double usage_in, usage_out;
    bool have_usage;
} owire_t;

/* ---------------- serialization: message array ---------------- */

static void oai_user_msg(owire_t *w, const trec_t *u) {
    buf_t txt;
    buf_init(&txt);
    user_text_join(u, &txt);
    append_msg_sep(&w->msgbuf);
    buf_append_str(&w->msgbuf, "{\"role\":\"user\",\"content\":");
    buf_append_jstr(&w->msgbuf, txt.data ? txt.data : "");
    buf_append_byte(&w->msgbuf, '}');
    buf_free(&txt);
}

static void oai_system_msg(owire_t *w, const cJSON *system) {
    /* only called on a fresh rebuild: becomes messages[0] */
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(system, "content");
    buf_t txt;
    buf_init(&txt);
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
            if (!cJSON_IsString(tx)) continue;
            if (txt.len) buf_append_byte(&txt, '\n');
            buf_append_str(&txt, tx->valuestring);
        }
    buf_append_str(&w->msgbuf, "{\"role\":\"system\",\"content\":");
    buf_append_jstr(&w->msgbuf, txt.data ? txt.data : "");
    buf_append_byte(&w->msgbuf, '}');
    buf_free(&txt);
}

static void oai_assistant_msg(owire_t *w, group_iter_t *g) {
    buf_t txt;
    buf_init(&txt);
    for (size_t i = g->begin; i < g->end; i++) {
        const trec_t *r = g->l->v[i];
        if (r->kind != T_TEXT) continue;
        if (txt.len) buf_append_byte(&txt, '\n');
        buf_append_str(&txt, r->text);
    }
    bool has_treq = false;
    for (size_t i = g->begin; i < g->end; i++)
        if (g->l->v[i]->kind == T_TREQ) has_treq = true;
    if (!txt.len && !has_treq) { buf_free(&txt); return; }
    append_msg_sep(&w->msgbuf);
    buf_append_str(&w->msgbuf, "{\"role\":\"assistant\",\"content\":");
    buf_append_jstr(&w->msgbuf, txt.data ? txt.data : "");
    if (has_treq) {
        buf_append_str(&w->msgbuf, ",\"tool_calls\":[");
        bool first = true;
        for (size_t i = g->begin; i < g->end; i++) {
            const trec_t *r = g->l->v[i];
            if (r->kind != T_TREQ) continue;
            if (!first) buf_append_byte(&w->msgbuf, ',');
            first = false;
            buf_append_str(&w->msgbuf, "{\"id\":");
            buf_append_jstr(&w->msgbuf, r->id);
            buf_append_str(&w->msgbuf, ",\"type\":\"function\",\"function\":{\"name\":");
            buf_append_jstr(&w->msgbuf, r->tool);
            buf_append_str(&w->msgbuf, ",\"arguments\":");
            buf_append_jstr(&w->msgbuf, print_args(r->args));
            buf_append_str(&w->msgbuf, "}}");
        }
        buf_append_byte(&w->msgbuf, ']');
    }
    buf_append_byte(&w->msgbuf, '}');
    buf_free(&txt);
}

static void oai_toolresp_msgs(owire_t *w, group_iter_t *g) {
    for (size_t i = g->rbegin; i < g->rend; i++) {
        const trec_t *r = g->l->v[i];
        append_msg_sep(&w->msgbuf);
        buf_append_str(&w->msgbuf, "{\"role\":\"tool\",\"tool_call_id\":");
        buf_append_jstr(&w->msgbuf, r->id);
        buf_append_str(&w->msgbuf, ",\"content\":");
        buf_append_jstr(&w->msgbuf, r->text);
        buf_append_byte(&w->msgbuf, '}');
    }
}

static void oai_serialize_new(owire_t *w, engine_t *e) {
    group_iter_t gi;
    group_begin(&gi, &e->tr);
    int kg;
    while ((kg = group_next(&gi)) != G_DONE) {
        if (gi.start < w->done_upto) continue;
        if (kg == G_USER) oai_user_msg(w, gi.user);
        else {
            oai_assistant_msg(w, &gi);
            oai_toolresp_msgs(w, &gi);
        }
        w->done_upto = gi.stop;
    }
}

/* responses api input items */

static void rsp_user_item(owire_t *w, const trec_t *u) {
    buf_t txt;
    buf_init(&txt);
    user_text_join(u, &txt);
    append_msg_sep(&w->msgbuf);
    buf_append_str(&w->msgbuf, "{\"type\":\"message\",\"role\":\"user\",\"content\":");
    buf_append_jstr(&w->msgbuf, txt.data ? txt.data : "");
    buf_append_byte(&w->msgbuf, '}');
    buf_free(&txt);
}

static void rsp_assistant_items(owire_t *w, group_iter_t *g) {
    bool has_treq = g->rend > g->rbegin;
    buf_t txt;
    buf_init(&txt);
    for (size_t i = g->begin; i < g->end; i++) {
        const trec_t *r = g->l->v[i];
        if (r->kind != T_TEXT) continue;
        if (txt.len) buf_append_byte(&txt, '\n');
        buf_append_str(&txt, r->text);
    }
    /* reasoning items are resent verbatim, but only for tool turns whose
       thinking came from the current llm (requirements §3, §5) */
    bool any = txt.len || has_treq;
    for (size_t i = g->begin; i < g->end && !any; i++)
        if (g->l->v[i]->kind == T_THINK) any = true;
    if (!any) { buf_free(&txt); return; }
    if (has_treq) {
        for (size_t i = g->begin; i < g->end; i++) {
            const trec_t *r = g->l->v[i];
            if (r->kind != T_THINK) continue;
            if (i < w->llm_mark) continue; /* dropped on llm change */
            if (!r->signature || !r->signature[0]) continue;
            append_msg_sep(&w->msgbuf);
            buf_append_str(&w->msgbuf, r->signature); /* raw serialized item */
        }
    }
    if (txt.len) {
        append_msg_sep(&w->msgbuf);
        buf_append_str(
            &w->msgbuf,
            "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":");
        buf_append_jstr(&w->msgbuf, txt.data);
        buf_append_str(&w->msgbuf, "}]}");
    }
    for (size_t i = g->begin; i < g->end; i++) {
        const trec_t *r = g->l->v[i];
        if (r->kind != T_TREQ) continue;
        append_msg_sep(&w->msgbuf);
        buf_append_str(&w->msgbuf, "{\"type\":\"function_call\",\"call_id\":");
        buf_append_jstr(&w->msgbuf, r->id);
        buf_append_str(&w->msgbuf, ",\"name\":");
        buf_append_jstr(&w->msgbuf, r->tool);
        buf_append_str(&w->msgbuf, ",\"arguments\":");
        buf_append_jstr(&w->msgbuf, print_args(r->args));
        buf_append_byte(&w->msgbuf, '}');
    }
    buf_free(&txt);
}

static void rsp_toolresp_items(owire_t *w, group_iter_t *g) {
    for (size_t i = g->rbegin; i < g->rend; i++) {
        const trec_t *r = g->l->v[i];
        append_msg_sep(&w->msgbuf);
        buf_append_str(&w->msgbuf, "{\"type\":\"function_call_output\",\"call_id\":");
        buf_append_jstr(&w->msgbuf, r->id);
        buf_append_str(&w->msgbuf, ",\"output\":");
        buf_append_jstr(&w->msgbuf, r->text);
        buf_append_byte(&w->msgbuf, '}');
    }
}

static void rsp_serialize_new(owire_t *w, engine_t *e) {
    group_iter_t gi;
    group_begin(&gi, &e->tr);
    int kg;
    while ((kg = group_next(&gi)) != G_DONE) {
        if (gi.start < w->done_upto) continue;
        if (kg == G_USER) rsp_user_item(w, gi.user);
        else {
            rsp_assistant_items(w, &gi);
            rsp_toolresp_items(w, &gi);
        }
        w->done_upto = gi.stop;
    }
}

/* ---------------- request build ---------------- */

static const cJSON *inference(engine_t *e) {
    return cJSON_GetObjectItemCaseSensitive(e->llm, "inference_options");
}

static const cJSON *io_get(engine_t *e, const char *field) {
    const cJSON *io = inference(e);
    if (!io) return NULL;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(io, field);
    return cJSON_IsNull(f) ? NULL : f;
}

static void append_opt_num(buf_t *b, engine_t *e, const char *name,
                           const char *wire_name) {
    const cJSON *f = io_get(e, name);
    if (!cJSON_IsNumber(f)) return;
    buf_appendf(b, ",\"%s\":", wire_name);
    buf_append_jnum(b, f->valuedouble);
}

static void append_opt_str(buf_t *b, engine_t *e, const char *name,
                           const char *wire_name) {
    const cJSON *f = io_get(e, name);
    if (!cJSON_IsString(f) || !f->valuestring) return;
    buf_appendf(b, ",\"%s\":", wire_name);
    buf_append_jstr(b, f->valuestring);
}

static void append_tool_entry(buf_t *b, const tool_entry_t *t, bool nested) {
    if (nested) buf_append_str(b, "{\"type\":\"function\",\"function\":{\"name\":");
    else buf_append_str(b, "{\"type\":\"function\",\"name\":");
    buf_append_jstr(b, t->exposed_name);
    const cJSON *desc = cJSON_GetObjectItemCaseSensitive(t->tool, "description");
    if (cJSON_IsString(desc) && desc->valuestring) {
        buf_append_str(b, ",\"description\":");
        buf_append_jstr(b, desc->valuestring);
    }
    const cJSON *sch = cJSON_GetObjectItemCaseSensitive(t->tool, "inputSchema");
    if (sch) {
        buf_append_str(b, nested ? ",\"parameters\":" : ",\"parameters\":");
        buf_append_tree(b, sch);
    }
    buf_append_str(b, nested ? "}}" : "}");
}

static void append_tools(buf_t *b, engine_t *e, bool nested) {
    const tool_listing_t *tl = e->tools;
    if (!tl || !tl->n) return;
    buf_append_str(b, ",\"tools\":[");
    for (size_t i = 0; i < tl->n; i++) {
        if (i) buf_append_byte(b, ',');
        append_tool_entry(b, &tl->v[i], nested);
    }
    buf_append_byte(b, ']');
}

/* returns the stream flag */
static bool build_body(owire_t *w, engine_t *e) {
    bool rebuild = w->epoch != e->cfg_epoch || e->tr.n < w->done_upto;
    if (rebuild) {
        buf_clear(&w->msgbuf);
        buf_append_byte(&w->msgbuf, '[');
        w->done_upto = 0;
        w->epoch = e->cfg_epoch;
        w->llm_mark = e->llm_mark;
        if (w->proto == PROTO_OPENAI && e->system) oai_system_msg(w, e->system);
    }
    if (w->proto == PROTO_OPENAI) oai_serialize_new(w, e);
    else rsp_serialize_new(w, e);

    const cJSON *sf = io_get(e, "stream");
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
    if (w->proto == PROTO_RESPONSES && e->system) {
        const cJSON *content =
            cJSON_GetObjectItemCaseSensitive(e->system, "content");
        buf_t txt;
        buf_init(&txt);
        if (cJSON_IsArray(content))
            for (const cJSON *bl = content->child; bl; bl = bl->next) {
                const cJSON *tx = cJSON_GetObjectItemCaseSensitive(bl, "text");
                if (!cJSON_IsString(tx)) continue;
                if (txt.len) buf_append_byte(&txt, '\n');
                buf_append_str(&txt, tx->valuestring);
            }
        buf_append_str(b, "\"instructions\":");
        buf_append_jstr(b, txt.data ? txt.data : "");
        buf_append_byte(b, ',');
        buf_free(&txt);
    }
    buf_append_str(b, w->proto == PROTO_OPENAI ? "\"messages\":" : "\"input\":");
    buf_append(b, w->msgbuf.data ? w->msgbuf.data : "[", w->msgbuf.len);
    buf_append_byte(b, ']');
    if (w->proto == PROTO_OPENAI) {
        append_tools(b, e, true);
        /* sampling fields in requirements §4 order */
        append_opt_num(b, e, "temperature", "temperature");
        append_opt_num(b, e, "top_p", "top_p");
        append_opt_num(b, e, "max_tokens", "max_tokens");
        {
            const cJSON *f = io_get(e, "stop");
            if (f) {
                buf_append_str(b, ",\"stop\":");
                buf_append_tree(b, f);
            }
        }
        append_opt_str(b, e, "reasoning_effort", "reasoning_effort");
        append_opt_num(b, e, "presence_penalty", "presence_penalty");
        append_opt_num(b, e, "frequency_penalty", "frequency_penalty");
        append_opt_num(b, e, "seed", "seed");
        buf_appendf(b, ",\"stream\":%s", stream ? "true" : "false");
        if (stream)
            buf_append_str(b, ",\"stream_options\":{\"include_usage\":true}");
        buf_append_byte(b, '}');
    } else {
        append_tools(b, e, false);
        append_opt_num(b, e, "temperature", "temperature");
        append_opt_num(b, e, "top_p", "top_p");
        {
            const cJSON *f = io_get(e, "reasoning_effort");
            if (cJSON_IsString(f) && f->valuestring) {
                buf_append_str(b, ",\"reasoning\":{\"effort\":");
                buf_append_jstr(b, f->valuestring);
                buf_append_byte(b, '}');
            }
        }
        append_opt_num(b, e, "max_tokens", "max_output_tokens");
        buf_appendf(b, ",\"stream\":%s", stream ? "true" : "false");
        buf_append_str(b, ",\"store\":false");
        buf_append_str(b, ",\"include\":[\"reasoning.encrypted_content\"]");
        buf_append_byte(b, '}');
    }
    return stream;
}

/* ---------------- error records ---------------- */

static cJSON *transport_error(http_req_t *req) {
    char msg[512];
    if (req->curl_res == CURLE_OPERATION_TIMEDOUT && !req->got_data) {
        snprintf(msg, sizeof msg, "connect to llm endpoint timed out");
        return rec_error(EC_CONNECT_FAILED, msg, true);
    }
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

static cJSON *status_error(http_req_t *req) {
    cJSON *body =
        cJSON_ParseWithLength(req->resp.data ? req->resp.data : "", req->resp.len);
    char msg[768];
    const cJSON *err = body ? cJSON_GetObjectItemCaseSensitive(body, "error") : NULL;
    if (err) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(err, "type");
        if (cJSON_IsString(m) && m->valuestring) {
            if (cJSON_IsString(t) && t->valuestring)
                snprintf(msg, sizeof msg, "HTTP %ld: %s (%s)", req->status,
                         m->valuestring, t->valuestring);
            else
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

/* ---------------- finish normalization ---------------- */

static const char *finish_norm_openai(const char *fr) {
    if (!fr) return "stop";
    if (!strcmp(fr, "stop")) return "stop";
    if (!strcmp(fr, "length")) return "length";
    if (!strcmp(fr, "content_filter")) return "content_filter";
    if (!strcmp(fr, "tool_calls") || !strcmp(fr, "function_call"))
        return "tool_use";
    return fr;
}

static const char *finish_norm_responses(const char *status,
                                         const char *incomplete_reason,
                                         bool have_fc) {
    if (!status || !strcmp(status, "completed")) return have_fc ? "tool_use" : "stop";
    if (!strcmp(status, "incomplete")) {
        if (incomplete_reason && !strcmp(incomplete_reason, "max_output_tokens"))
            return "length";
        if (incomplete_reason && !strcmp(incomplete_reason, "content_filter"))
            return "content_filter";
        return incomplete_reason ? incomplete_reason : "incomplete";
    }
    return status;
}

/* ---------------- tool slots ---------------- */

static tslot_t *slot_at(tslot_t **arr, size_t *n, size_t idx) {
    if (idx >= *n) {
        size_t nn = idx + 1;
        *arr = realloc(*arr, nn * sizeof **arr);
        for (size_t i = *n; i < nn; i++) {
            (*arr)[i].id = strdup("");
            (*arr)[i].name = strdup("");
            buf_init(&(*arr)[i].args);
        }
        *n = nn;
    }
    return &(*arr)[idx];
}

static void slots_reset(tslot_t **arr, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        free((*arr)[i].id);
        free((*arr)[i].name);
        buf_free(&(*arr)[i].args);
    }
    free(*arr);
    *arr = NULL;
    *n = 0;
}

/* ---------------- streaming: chat completions ---------------- */

static void chat_chunk(owire_t *w, const cJSON *ch) {
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(ch, "choices");
    if (cJSON_IsArray(choices))
        for (const cJSON *c = choices->child; c; c = c->next) {
            const cJSON *delta = cJSON_GetObjectItemCaseSensitive(c, "delta");
            if (delta) {
                const cJSON *content =
                    cJSON_GetObjectItemCaseSensitive(delta, "content");
                if (cJSON_IsString(content) && content->valuestring)
                    blk_delta(&w->be, 0, content->valuestring,
                              strlen(content->valuestring));
                const cJSON *reason =
                    cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
                if (!cJSON_IsString(reason))
                    reason = cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
                if (cJSON_IsString(reason) && reason->valuestring)
                    blk_delta(&w->be, 1, reason->valuestring,
                              strlen(reason->valuestring));
                const cJSON *tcs =
                    cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
                if (cJSON_IsArray(tcs))
                    for (const cJSON *tc = tcs->child; tc; tc = tc->next) {
                        double di = rec_num(tc, "index", 0);
                        tslot_t *s = slot_at(&w->slots, &w->nslots,
                                             (size_t)(di < 0 ? 0 : di));
                        const char *id = rec_str(tc, "id");
                        if (id) {
                            free(s->id);
                            s->id = strdup(id);
                        }
                        const cJSON *fn =
                            cJSON_GetObjectItemCaseSensitive(tc, "function");
                        if (fn) {
                            const char *nm = rec_str(fn, "name");
                            if (nm) {
                                free(s->name);
                                s->name = strdup(nm);
                            }
                            const cJSON *ar =
                                cJSON_GetObjectItemCaseSensitive(fn, "arguments");
                            if (cJSON_IsString(ar) && ar->valuestring)
                                buf_append_str(&s->args, ar->valuestring);
                        }
                    }
            }
            const char *fr = rec_str(c, "finish_reason");
            if (fr) {
                snprintf(w->finish, sizeof w->finish, "%s", fr);
                w->have_finish = true;
            }
        }
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(ch, "usage");
    if (usage) {
        w->usage_in = rec_num(usage, "prompt_tokens", 0);
        w->usage_out = rec_num(usage, "completion_tokens", 0);
        w->have_usage = true;
    }
}

/* stop the open block, flush the block-final, emit tool requests when any */
static int chat_finish_turn(owire_t *w, engine_t *e, turn_out_t *out) {
    blk_stop(&w->be);
    if (!w->have_finish) {
        snprintf(w->finish, sizeof w->finish,
                 w->nslots ? "tool_calls" : "stop");
        w->have_finish = true;
    }
    if (w->nslots) {
        blk_emit_pending(&w->be);
        const char *norm = finish_norm_openai(w->finish);
        for (size_t i = 0; i < w->nslots; i++) {
            cJSON *args =
                cJSON_Parse(w->slots[i].args.data ? w->slots[i].args.data : "{}");
            if (!args) args = cJSON_CreateObject();
            cJSON *rec = rec_tool_request(w->slots[i].name, args, w->slots[i].id);
            cJSON_Delete(args);
            if (i + 1 == w->nslots) {
                if (w->have_usage)
                    rec_attach_usage(rec, w->usage_in, w->usage_out);
                rec_attach_finish(rec, norm);
            }
            engine_emit_record(e, rec);
        }
        return TURN_TOOLS;
    }
    out->final_rec = blk_take_pending(&w->be);
    if (out->final_rec) {
        if (w->have_usage)
            rec_attach_usage(out->final_rec, w->usage_in, w->usage_out);
        rec_attach_finish(out->final_rec, finish_norm_openai(w->finish));
    }
    return TURN_FINAL;
}

/* ---------------- streaming: responses api ---------------- */

static void rsp_item_done(owire_t *w, engine_t *e, const cJSON *item) {
    (void)e;
    const char *ty = rec_str(item, "type");
    if (ty && !strcmp(ty, "message")) {
        /* in streaming the deltas already fed the block; in non-streaming
           the item carries the whole text */
        if (!w->be.streaming) {
            const cJSON *content =
                cJSON_GetObjectItemCaseSensitive(item, "content");
            if (cJSON_IsArray(content))
                for (const cJSON *b = content->child; b; b = b->next) {
                    const char *bty = rec_str(b, "type");
                    if (bty && !strcmp(bty, "output_text")) {
                        const char *txt = rec_str(b, "text");
                        if (txt) blk_delta(&w->be, 0, txt, strlen(txt));
                    }
                }
        }
        blk_stop(&w->be);
        return;
    }
    if (ty && !strcmp(ty, "reasoning")) {
        char *sig = cJSON_PrintUnformatted(item);
        if (!w->be.streaming) {
            buf_t txt;
            buf_init(&txt);
            const cJSON *sum = cJSON_GetObjectItemCaseSensitive(item, "summary");
            if (cJSON_IsArray(sum))
                for (const cJSON *s = sum->child; s; s = s->next) {
                    const char *st = rec_str(s, "text");
                    if (st) {
                        if (txt.len) buf_append_byte(&txt, '\n');
                        buf_append_str(&txt, st);
                    }
                }
            if (txt.len) blk_delta(&w->be, 1, txt.data, txt.len);
            buf_free(&txt);
        }
        blk_stop_thinking(&w->be, sig);
        cJSON_free(sig);
        return;
    }
    if (ty && !strcmp(ty, "function_call")) {
        double di = rec_num(item, "output_index", (double)w->nrslots);
        if (di < 0) di = 0;
        tslot_t *s = slot_at(&w->rslots, &w->nrslots, (size_t)di);
        const char *id = rec_str(item, "call_id");
        if (!id) id = rec_str(item, "id");
        if (id) {
            free(s->id);
            s->id = strdup(id);
        }
        const char *nm = rec_str(item, "name");
        if (nm) {
            free(s->name);
            s->name = strdup(nm);
        }
        const char *ar = rec_str(item, "arguments");
        buf_clear(&s->args);
        if (ar) buf_append_str(&s->args, ar);
    }
}

static void rsp_event(owire_t *w, engine_t *e, const char *ev, const char *data,
                      size_t n) {
    if (!strcmp(ev, "ping")) return;
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    if (!strcmp(ev, "response.output_text.delta")) {
        const char *s = rec_str(d, "delta");
        if (s) blk_delta(&w->be, 0, s, strlen(s));
    } else if (!strcmp(ev, "response.reasoning_summary_text.delta")) {
        const char *s = rec_str(d, "delta");
        if (s) blk_delta(&w->be, 1, s, strlen(s));
    } else if (!strcmp(ev, "response.output_item.done")) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(d, "item");
        if (item) rsp_item_done(w, e, item);
    } else if (!strcmp(ev, "response.function_call_arguments.delta")) {
        double di = rec_num(d, "output_index", 0);
        if (di < 0) di = 0;
        tslot_t *s = slot_at(&w->rslots, &w->nrslots, (size_t)di);
        const char *s2 = rec_str(d, "delta");
        if (s2) buf_append_str(&s->args, s2);
    } else if (!strcmp(ev, "response.output_text.done")) {
        blk_stop(&w->be);
    } else if (!strcmp(ev, "response.completed") ||
               !strcmp(ev, "response.incomplete")) {
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(d, "response");
        const char *status = rec_str(resp, "status");
        const cJSON *inc =
            resp ? cJSON_GetObjectItemCaseSensitive(resp, "incomplete_details")
                 : NULL;
        const char *reason = rec_str(inc, "reason");
        const cJSON *usage =
            resp ? cJSON_GetObjectItemCaseSensitive(resp, "usage") : NULL;
        if (usage) {
            w->usage_in = rec_num(usage, "input_tokens", 0);
            w->usage_out = rec_num(usage, "output_tokens", 0);
            w->have_usage = true;
        }
        snprintf(w->finish, sizeof w->finish, "%s",
                 finish_norm_responses(status, reason, w->nrslots > 0));
        w->have_finish = true;
        w->sse_done = true;
    } else if (!strcmp(ev, "response.failed") || !strcmp(ev, "error")) {
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(d, "response");
        const cJSON *err =
            resp ? cJSON_GetObjectItemCaseSensitive(resp, "error")
                 : cJSON_GetObjectItemCaseSensitive(d, "error");
        const char *m = rec_str(err, "message");
        if (!m) m = rec_str(d, "message");
        snprintf(w->fail_msg, sizeof w->fail_msg, "%s", m ? m : "unknown error");
        w->failed = true;
        w->sse_done = true;
    }
    cJSON_Delete(d);
}

static int rsp_finish_turn(owire_t *w, engine_t *e, turn_out_t *out) {
    blk_stop(&w->be);
    if (w->nrslots) {
        blk_emit_pending(&w->be);
        for (size_t i = 0; i < w->nrslots; i++) {
            cJSON *args = cJSON_Parse(w->rslots[i].args.data
                                          ? w->rslots[i].args.data
                                          : "{}");
            if (!args) args = cJSON_CreateObject();
            cJSON *rec = rec_tool_request(w->rslots[i].name, args, w->rslots[i].id);
            cJSON_Delete(args);
            if (i + 1 == w->nrslots) {
                if (w->have_usage)
                    rec_attach_usage(rec, w->usage_in, w->usage_out);
                if (w->have_finish) rec_attach_finish(rec, w->finish);
            }
            engine_emit_record(e, rec);
        }
        return TURN_TOOLS;
    }
    out->final_rec = blk_take_pending(&w->be);
    if (out->final_rec) {
        if (w->have_usage)
            rec_attach_usage(out->final_rec, w->usage_in, w->usage_out);
        if (w->have_finish) rec_attach_finish(out->final_rec, w->finish);
    }
    return TURN_FINAL;
}

/* ---------------- sse glue ---------------- */

static void sse_event_cb(void *ctx, const char *event, const char *data,
                         size_t n) {
    struct sse_ctx *c = ctx;
    if (!strcmp(event, "ping")) return;
    if (c->w->proto == PROTO_RESPONSES) {
        rsp_event(c->w, c->e, event, data, n);
        return;
    }
    /* chat completions: bare data events */
    if (strcmp(event, "message")) return;
    if (n == 6 && !memcmp(data, "[DONE]", 6)) {
        c->w->sse_done = true;
        return;
    }
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    chat_chunk(c->w, d);
    cJSON_Delete(d);
}

static void http_data_cb(void *ctx, const char *bytes, size_t n) {
    struct sse_ctx *c = ctx;
    sse_feed(&c->w->sse, bytes, n);
}

/* ---------------- non-streaming body mapping ---------------- */

static int chat_body_map(owire_t *w, engine_t *e, const cJSON *body,
                         turn_out_t *out) {
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(body, "choices");
    const cJSON *ch = choices ? choices->child : NULL;
    const cJSON *msg =
        ch ? cJSON_GetObjectItemCaseSensitive(ch, "message") : NULL;
    if (msg) {
        const cJSON *reason =
            cJSON_GetObjectItemCaseSensitive(msg, "reasoning_content");
        if (!cJSON_IsString(reason))
            reason = cJSON_GetObjectItemCaseSensitive(msg, "reasoning");
        if (cJSON_IsString(reason) && reason->valuestring) {
            blk_delta(&w->be, 1, reason->valuestring,
                      strlen(reason->valuestring));
            blk_stop_thinking(&w->be, "");
        }
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (cJSON_IsString(content) && content->valuestring)
            blk_delta(&w->be, 0, content->valuestring,
                      strlen(content->valuestring));
        const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        if (cJSON_IsArray(tcs))
            for (const cJSON *tc = tcs->child; tc; tc = tc->next) {
                double di = rec_num(tc, "index", (double)w->nslots);
                if (di < 0) di = 0;
                tslot_t *s = slot_at(&w->slots, &w->nslots, (size_t)di);
                const char *id = rec_str(tc, "id");
                if (id) {
                    free(s->id);
                    s->id = strdup(id);
                }
                const cJSON *fn =
                    cJSON_GetObjectItemCaseSensitive(tc, "function");
                if (fn) {
                    const char *nm = rec_str(fn, "name");
                    if (nm) {
                        free(s->name);
                        s->name = strdup(nm);
                    }
                    const char *ar = rec_str(fn, "arguments");
                    buf_clear(&s->args);
                    if (ar) buf_append_str(&s->args, ar);
                }
            }
    }
    const char *fr = rec_str(ch, "finish_reason");
    if (fr) {
        snprintf(w->finish, sizeof w->finish, "%s", fr);
        w->have_finish = true;
    }
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(body, "usage");
    if (usage) {
        w->usage_in = rec_num(usage, "prompt_tokens", 0);
        w->usage_out = rec_num(usage, "completion_tokens", 0);
        w->have_usage = true;
    }
    return chat_finish_turn(w, e, out);
}

static int rsp_body_map(owire_t *w, engine_t *e, const cJSON *body,
                        turn_out_t *out) {
    const cJSON *out_arr = cJSON_GetObjectItemCaseSensitive(body, "output");
    if (cJSON_IsArray(out_arr))
        for (const cJSON *item = out_arr->child; item; item = item->next)
            rsp_item_done(w, e, item);
    const char *status = rec_str(body, "status");
    const cJSON *inc =
        cJSON_GetObjectItemCaseSensitive(body, "incomplete_details");
    const char *reason = rec_str(inc, "reason");
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(body, "usage");
    if (usage) {
        w->usage_in = rec_num(usage, "input_tokens", 0);
        w->usage_out = rec_num(usage, "output_tokens", 0);
        w->have_usage = true;
    }
    snprintf(w->finish, sizeof w->finish, "%s",
             finish_norm_responses(status, reason, w->nrslots > 0));
    w->have_finish = true;
    if (status && !strcmp(status, "failed")) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(body, "error");
        const char *m = rec_str(err, "message");
        snprintf(w->fail_msg, sizeof w->fail_msg, "%s", m ? m : "response failed");
        return -1;
    }
    return rsp_finish_turn(w, e, out);
}

/* ---------------- the turn driver ---------------- */

static void owire_reset_turn(owire_t *w, engine_t *e, bool stream) {
    blk_begin_turn(&w->be, e);
    w->be.streaming = stream;
    w->sse_done = false;
    w->failed = false;
    w->fail_msg[0] = '\0';
    w->have_finish = false;
    w->have_usage = false;
    w->finish[0] = '\0';
    slots_reset(&w->slots, &w->nslots);
    slots_reset(&w->rslots, &w->nrslots);
    sse_free(&w->sse);
    w->sctx.w = w;
    w->sctx.e = e;
    sse_init(&w->sse, sse_event_cb, &w->sctx);
}

/* post-transfer: turn records from the completed stream or body */
static int map_response(owire_t *w, engine_t *e, bool stream, http_req_t *req,
                        turn_out_t *out) {
    /* a json body answers even a streaming request (liberal in what we
       accept; the final records are identical either way) */
    bool ct_json =
        req->content_type.data &&
        strncasecmp(req->content_type.data, "application/json",
                    strlen("application/json")) == 0;
    if (stream && !ct_json) {
        sse_eof(&w->sse);
        if (!w->sse_done) {
            out->error_rec = rec_error(
                EC_API_ERROR, "stream ended without a terminal event", true);
            return TURN_FATAL;
        }
        return w->proto == PROTO_OPENAI ? chat_finish_turn(w, e, out)
                                        : rsp_finish_turn(w, e, out);
    }
    cJSON *body =
        cJSON_ParseWithLength(req->resp.data ? req->resp.data : "", req->resp.len);
    if (!body) {
        out->error_rec =
            rec_error(EC_HTTP_ERROR, "endpoint returned a non-json body", true);
        return TURN_FATAL;
    }
    int r = w->proto == PROTO_OPENAI ? chat_body_map(w, e, body, out)
                                     : rsp_body_map(w, e, body, out);
    cJSON_Delete(body);
    if (r == -1) {
        out->error_rec = rec_error(EC_API_ERROR, w->fail_msg, true);
        return TURN_FATAL;
    }
    return r;
}

static int owire_turn(wire_t *base, engine_t *e, turn_out_t *out) {
    owire_t *w = (owire_t *)base;
    memset(out, 0, sizeof *out);
    bool stream = build_body(w, e);
    owire_reset_turn(w, e, stream);

    const char *base_url = rec_str(e->llm, "api_base");
    buf_t url;
    buf_init(&url);
    buf_append_str(&url, base_url);
    buf_append_str(&url,
                   w->proto == PROTO_OPENAI ? "/chat/completions" : "/responses");

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
        req.on_data = http_data_cb;
        req.cb_ctx = &w->sctx;
    }

    int rc = http_perform(&req);
    int result;
    if (rc == -1) {
        blk_abort(&w->be);
        result = g_stop_flag ? TURN_ABORTED : TURN_FATAL;
        if (result == TURN_FATAL) out->error_rec = transport_error(&req);
    } else if (rc == 1) {
        out->error_rec = status_error(&req);
        result = TURN_FATAL;
    } else if (w->failed) {
        out->error_rec = rec_error(EC_API_ERROR, w->fail_msg, true);
        result = TURN_FATAL;
    } else {
        result = map_response(w, e, stream, &req, out);
    }

    buf_free(&req.resp);
    buf_free(&req.content_type);
    curl_slist_free_all(hdrs);
    buf_free(&url);
    sse_free(&w->sse);
    return result;
}

static int owire_build(wire_t *base, engine_t *e) {
    owire_t *w = (owire_t *)base;
    build_body(w, e);
    return 0;
}

static void owire_destroy(wire_t *base) {
    owire_t *w = (owire_t *)base;
    buf_free(&w->msgbuf);
    blk_free(&w->be);
    sse_free(&w->sse);
    slots_reset(&w->slots, &w->nslots);
    slots_reset(&w->rslots, &w->nrslots);
    buf_free(&base->last_body);
    free(w);
}

wire_t *wire_openai_new(int proto) {
    owire_t *w = calloc(1, sizeof *w);
    w->epoch = ULONG_MAX; /* force the first build to serialize everything */
    w->base.turn = owire_turn;
    w->base.build = owire_build;
    w->base.destroy = owire_destroy;
    buf_init(&w->msgbuf);
    buf_init(&w->base.last_body);
    blk_init(&w->be);
    w->proto = proto;
    return &w->base;
}

