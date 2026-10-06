/* llmproxy.c - llmkit proxy: the llm wire viewer. A plain tcp http
   endpoint (no tls on the listening side - the upstream may still be
   https, curl carries that half) that forwards every POST to one fixed
   llm endpoint and renders the conversation it sees on stdout with
   prettyprint's display. The protocol flag names the language the
   upstream speaks, so both directions parse: a request maps to the
   transcript records its messages carry, a response (json or sse) to
   the records the wire would have emitted - partials included, rendered
   live as the bytes tee through. Transparent first: whatever parses or
   not, the client's bytes pass; a parse failure renders as a ! line,
   never as dropped traffic. */
#include "llmkit.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
static void sock_close(sock_t s) { closesocket(s); }
static int net_init(void) {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
static void sock_close(sock_t s) { close(s); }
static int net_init(void) { return 0; }
#endif

/* ================= command line ================= */

void llm_proxy_cfg_free(llm_proxy_cfg_t *c) {
    free(c->api_base);
    free(c->key);
    free(c->listen);
    memset(c, 0, sizeof *c);
}

static int perr(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int perr(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return 1;
}

int llm_proxy_parse(int argc, char **argv, llm_proxy_cfg_t *c, char *err,
                    size_t errsz) {
    memset(c, 0, sizeof *c);
    c->protocol = -1;
    bool have_key = false, have_listen = false;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--anthropic") || !strcmp(a, "--openai") ||
            !strcmp(a, "--openai-responses")) {
            if (c->protocol != -1) {
                perr(err, errsz, "protocol flag given twice");
                goto fail;
            }
            if (i + 1 >= argc) {
                perr(err, errsz, "missing <api_base> value for %s", a);
                goto fail;
            }
            c->protocol = !strcmp(a, "--anthropic")     ? PROTO_ANTHROPIC
                          : !strcmp(a, "--openai")      ? PROTO_OPENAI
                                                         : PROTO_RESPONSES;
            c->api_base = strdup(argv[++i]);
        } else if (!strcmp(a, "--key") || !strcmp(a, "--listen")) {
            if (i + 1 >= argc) {
                perr(err, errsz, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            if (!strcmp(a, "--key")) {
                if (have_key) {
                    perr(err, errsz, "--key given twice");
                    goto fail;
                }
                have_key = true;
                c->key = strdup(v);
            } else {
                if (have_listen) {
                    perr(err, errsz, "--listen given twice");
                    goto fail;
                }
                have_listen = true;
                c->listen = strdup(v);
            }
        } else if (a[0] == '-' && a[1] == '-') {
            perr(err, errsz, "unknown flag '%s'", a);
            goto fail;
        } else {
            perr(err, errsz,
                 "unexpected extra argument '%s' (the api base rides on the "
                 "protocol flag)",
                 a);
            goto fail;
        }
    }
    if (c->protocol == -1) {
        perr(err, errsz, "missing protocol flag "
                         "(--anthropic, --openai or --openai-responses)");
        goto fail;
    }
    if (!c->listen) c->listen = strdup("127.0.0.1:8080");
    return 0;
fail:
    llm_proxy_cfg_free(c);
    return 1;
}

/* ================= wire -> records ================= */
/* The interception half: json in the protocol's shapes mapped to the
   record catalogue, exactly the records whose jsonl prettyprint renders.
   Everything is read null-safely; a field no wire sends degrades to an
   empty block, never to garbage. */

/* one user record with a single text block (call.c's text_record shape) */
static cJSON *user_record(const char *text) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "user");
    cJSON *content = cJSON_AddArrayToObject(t, "content");
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", text);
    cJSON_AddItemToArray(content, blk);
    return t;
}

/* a json-encoded argument string -> an object ({} when unparseable) */
static cJSON *args_from_str(const char *s) {
    cJSON *a = s ? cJSON_Parse(s) : NULL;
    if (!a) a = cJSON_CreateObject();
    return a;
}

/* openai-style content value (string or block array) -> text, blocks
   joined with \n; image and unknown blocks contribute nothing */
static void oai_content_join(const cJSON *content, buf_t *out) {
    if (cJSON_IsString(content) && content->valuestring) {
        buf_append_str(out, content->valuestring);
        return;
    }
    if (!cJSON_IsArray(content)) return;
    bool any = false;
    for (const cJSON *b = content->child; b; b = b->next) {
        const char *ty = rec_str(b, "type");
        if (ty && strcmp(ty, "text") && strcmp(ty, "input_text") &&
            strcmp(ty, "output_text"))
            continue;
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
        if (!cJSON_IsString(tx) || !tx->valuestring) continue;
        if (any) buf_append_byte(out, '\n');
        buf_append_str(out, tx->valuestring);
        any = true;
    }
}

/* a responses reasoning item's summary blocks -> text */
static void reasoning_text(const cJSON *item, buf_t *out) {
    const cJSON *sum = cJSON_GetObjectItemCaseSensitive(item, "summary");
    if (!cJSON_IsArray(sum)) return;
    for (const cJSON *s = sum->child; s; s = s->next) {
        const char *st = rec_str(s, "text");
        if (!st) continue;
        if (out->len) buf_append_byte(out, '\n');
        buf_append_str(out, st);
    }
}

/* ---- requests ---- */

static void map_req_chat(const cJSON *body, emit_fn em, void *ctx) {
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(body, "messages");
    if (!cJSON_IsArray(ms)) return;
    for (const cJSON *m = ms->child; m; m = m->next) {
        const char *role = rec_str(m, "role");
        const cJSON *content =
            cJSON_GetObjectItemCaseSensitive(m, "content");
        if (role && !strcmp(role, "system")) continue; /* renders nothing */
        if (role && !strcmp(role, "tool")) {
            buf_t tx;
            buf_init(&tx);
            oai_content_join(content, &tx);
            em(ctx, rec_tool_response(rec_str(m, "tool_call_id"),
                                      tx.data ? tx.data : "", false));
            buf_free(&tx);
            continue;
        }
        if (role && !strcmp(role, "assistant")) {
            buf_t tx;
            buf_init(&tx);
            oai_content_join(content, &tx);
            if (tx.len) em(ctx, rec_text("response", tx.data, false));
            buf_free(&tx);
            const cJSON *tcs =
                cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
            if (cJSON_IsArray(tcs))
                for (const cJSON *tc = tcs->child; tc; tc = tc->next) {
                    const cJSON *fn =
                        cJSON_GetObjectItemCaseSensitive(tc, "function");
                    cJSON *args = args_from_str(rec_str(fn, "arguments"));
                    em(ctx, rec_tool_request(rec_str(fn, "name"), args,
                                             rec_str(tc, "id")));
                    cJSON_Delete(args);
                }
            continue;
        }
        /* user: the conversation's input side */
        buf_t tx;
        buf_init(&tx);
        oai_content_join(content, &tx);
        if (tx.len) em(ctx, user_record(tx.data));
        buf_free(&tx);
    }
}

static void map_req_responses(const cJSON *body, emit_fn em, void *ctx) {
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(body, "input");
    if (cJSON_IsString(input) && input->valuestring) {
        if (*input->valuestring) em(ctx, user_record(input->valuestring));
        return;
    }
    if (!cJSON_IsArray(input)) return;
    for (const cJSON *it = input->child; it; it = it->next) {
        const char *ty = rec_str(it, "type");
        if (ty && !strcmp(ty, "function_call")) {
            cJSON *args = args_from_str(rec_str(it, "arguments"));
            const char *id = rec_str(it, "call_id");
            if (!id) id = rec_str(it, "id");
            em(ctx, rec_tool_request(rec_str(it, "name"), args, id));
            cJSON_Delete(args);
        } else if (ty && !strcmp(ty, "function_call_output")) {
            em(ctx, rec_tool_response(rec_str(it, "call_id"),
                                      rec_str(it, "output"), false));
        } else if (ty && !strcmp(ty, "reasoning")) {
            buf_t tx;
            buf_init(&tx);
            reasoning_text(it, &tx);
            if (tx.len) em(ctx, rec_thinking_final(tx.data, NULL));
            buf_free(&tx);
        } else if (!ty || !strcmp(ty, "message")) {
            const char *role = rec_str(it, "role");
            buf_t tx;
            buf_init(&tx);
            oai_content_join(
                cJSON_GetObjectItemCaseSensitive(it, "content"), &tx);
            if (!tx.len) {
                buf_free(&tx);
                continue;
            }
            if (role && !strcmp(role, "user")) em(ctx, user_record(tx.data));
            else if (role && !strcmp(role, "assistant"))
                em(ctx, rec_text("response", tx.data, false));
            /* system/developer render nothing, like the wires */
            buf_free(&tx);
        }
        /* item_reference and unknown items: nothing to say */
    }
}

static void map_req_anthropic(const cJSON *body, emit_fn em, void *ctx) {
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(body, "messages");
    if (!cJSON_IsArray(ms)) return;
    for (const cJSON *m = ms->child; m; m = m->next) {
        const char *role = rec_str(m, "role");
        const cJSON *content =
            cJSON_GetObjectItemCaseSensitive(m, "content");
        if (role && !strcmp(role, "assistant")) {
            if (cJSON_IsArray(content))
                for (const cJSON *b = content->child; b; b = b->next) {
                    const char *ty = rec_str(b, "type");
                    if (ty && !strcmp(ty, "thinking"))
                        em(ctx, rec_thinking_final(
                                    rec_str(b, "thinking"),
                                    rec_str(b, "signature")));
                    else if (ty && !strcmp(ty, "text"))
                        em(ctx, rec_text("response", rec_str(b, "text"),
                                         false));
                    else if (ty && !strcmp(ty, "tool_use"))
                        em(ctx, rec_tool_request(
                                    rec_str(b, "name"),
                                    cJSON_GetObjectItemCaseSensitive(
                                        b, "input"),
                                    rec_str(b, "id")));
                }
            else if (cJSON_IsString(content) && content->valuestring)
                em(ctx, rec_text("response", content->valuestring, false));
            continue;
        }
        /* user: text blocks gather into one user record, tool_result
           blocks are the turn's tool traffic, in wire order */
        buf_t tx;
        buf_init(&tx);
        if (cJSON_IsString(content) && content->valuestring)
            buf_append_str(&tx, content->valuestring);
        else if (cJSON_IsArray(content))
            for (const cJSON *b = content->child; b; b = b->next) {
                const char *ty = rec_str(b, "type");
                if (ty && !strcmp(ty, "tool_result")) {
                    if (tx.len) {
                        em(ctx, user_record(tx.data));
                        buf_clear(&tx);
                    }
                    buf_t rtx;
                    buf_init(&rtx);
                    oai_content_join(
                        cJSON_GetObjectItemCaseSensitive(b, "content"),
                        &rtx);
                    em(ctx, rec_tool_response(rec_str(b, "tool_use_id"),
                                              rtx.data ? rtx.data : "",
                                              rec_bool(b, "is_error",
                                                       false)));
                    buf_free(&rtx);
                } else {
                    const cJSON *bt =
                        cJSON_GetObjectItemCaseSensitive(b, "text");
                    if (cJSON_IsString(bt) && bt->valuestring) {
                        if (tx.len) buf_append_byte(&tx, '\n');
                        buf_append_str(&tx, bt->valuestring);
                    }
                }
            }
        if (tx.len) em(ctx, user_record(tx.data));
        buf_free(&tx);
    }
}

void llm_proxy_map_request(int proto, const char *body, size_t n,
                           emit_fn em, void *ctx) {
    cJSON *t = cJSON_ParseWithLength(body ? body : "", n);
    if (!t) {
        em(ctx, rec_error(EC_INVALID_RECORD, "request body is not json",
                          true));
        return;
    }
    if (proto == PROTO_ANTHROPIC) map_req_anthropic(t, em, ctx);
    else if (proto == PROTO_RESPONSES) map_req_responses(t, em, ctx);
    else map_req_chat(t, em, ctx);
    cJSON_Delete(t);
}

/* ---- non-streamed responses ---- */

/* the turn's records collect, usage rides the last one (the wires' rule:
   the final record of the turn reports the token totals) */
typedef struct {
    cJSON **v;
    size_t n, cap;
} rlist_t;

static void rlist_push(rlist_t *l, cJSON *rec) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = rec;
}

static void rlist_emit(rlist_t *l, bool have_usage, double uin, double uout,
                       emit_fn em, void *ctx) {
    if (l->n && have_usage) rec_attach_usage(l->v[l->n - 1], uin, uout);
    for (size_t i = 0; i < l->n; i++) em(ctx, l->v[i]);
    free(l->v);
}

static void map_resp_chat(const cJSON *body, emit_fn em, void *ctx) {
    rlist_t l = {0};
    const cJSON *choices =
        cJSON_GetObjectItemCaseSensitive(body, "choices");
    if (cJSON_IsArray(choices))
        for (const cJSON *c = choices->child; c; c = c->next) {
            const cJSON *msg =
                cJSON_GetObjectItemCaseSensitive(c, "message");
            if (!msg) continue;
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(
                msg, "reasoning_content");
            if (!cJSON_IsString(reason))
                reason = cJSON_GetObjectItemCaseSensitive(msg, "reasoning");
            if (cJSON_IsString(reason) && reason->valuestring &&
                *reason->valuestring)
                rlist_push(&l, rec_thinking_final(reason->valuestring, ""));
            const cJSON *content =
                cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (cJSON_IsString(content) && content->valuestring &&
                *content->valuestring)
                rlist_push(&l,
                           rec_text("response", content->valuestring, false));
            const cJSON *tcs =
                cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
            if (cJSON_IsArray(tcs))
                for (const cJSON *tc = tcs->child; tc; tc = tc->next) {
                    const cJSON *fn =
                        cJSON_GetObjectItemCaseSensitive(tc, "function");
                    cJSON *args = args_from_str(rec_str(fn, "arguments"));
                    rlist_push(&l, rec_tool_request(rec_str(fn, "name"),
                                                    args, rec_str(tc, "id")));
                    cJSON_Delete(args);
                }
        }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(body, "usage");
    rlist_emit(&l, cJSON_IsObject(u),
               rec_num(u, "prompt_tokens", 0),
               rec_num(u, "completion_tokens", 0), em, ctx);
}

static void map_resp_responses(const cJSON *body, emit_fn em, void *ctx) {
    rlist_t l = {0};
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(body, "output");
    if (cJSON_IsArray(out))
        for (const cJSON *it = out->child; it; it = it->next) {
            const char *ty = rec_str(it, "type");
            if (ty && !strcmp(ty, "message")) {
                buf_t tx;
                buf_init(&tx);
                oai_content_join(
                    cJSON_GetObjectItemCaseSensitive(it, "content"), &tx);
                if (tx.len)
                    rlist_push(&l, rec_text("response", tx.data, false));
                buf_free(&tx);
            } else if (ty && !strcmp(ty, "reasoning")) {
                buf_t tx;
                buf_init(&tx);
                reasoning_text(it, &tx);
                if (tx.len)
                    rlist_push(&l, rec_thinking_final(tx.data, NULL));
                buf_free(&tx);
            } else if (ty && !strcmp(ty, "function_call")) {
                cJSON *args = args_from_str(rec_str(it, "arguments"));
                const char *id = rec_str(it, "call_id");
                if (!id) id = rec_str(it, "id");
                rlist_push(&l,
                           rec_tool_request(rec_str(it, "name"), args, id));
                cJSON_Delete(args);
            }
        }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(body, "usage");
    rlist_emit(&l, cJSON_IsObject(u),
               rec_num(u, "input_tokens", 0),
               rec_num(u, "output_tokens", 0), em, ctx);
}

static void map_resp_anthropic(const cJSON *body, emit_fn em, void *ctx) {
    rlist_t l = {0};
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(body, "content");
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const char *ty = rec_str(b, "type");
            if (ty && !strcmp(ty, "thinking"))
                rlist_push(&l, rec_thinking_final(rec_str(b, "thinking"),
                                                  rec_str(b, "signature")));
            else if (ty && !strcmp(ty, "text"))
                rlist_push(&l,
                           rec_text("response", rec_str(b, "text"), false));
            else if (ty && !strcmp(ty, "tool_use"))
                rlist_push(&l,
                           rec_tool_request(
                               rec_str(b, "name"),
                               cJSON_GetObjectItemCaseSensitive(b, "input"),
                               rec_str(b, "id")));
        }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(body, "usage");
    rlist_emit(&l, cJSON_IsObject(u),
               rec_num(u, "input_tokens", 0),
               rec_num(u, "output_tokens", 0), em, ctx);
}

void llm_proxy_map_response(int proto, long status, const char *body,
                            size_t n, emit_fn em, void *ctx) {
    if (status < 200 || status >= 300) {
        /* the wires' error shaping, verbatim through their own helper */
        http_req_t r;
        memset(&r, 0, sizeof r);
        buf_init(&r.resp);
        r.status = status;
        if (n) buf_append(&r.resp, body, n);
        em(ctx, http_status_error(&r, proto != PROTO_ANTHROPIC));
        buf_free(&r.resp);
        return;
    }
    cJSON *t = cJSON_ParseWithLength(body ? body : "", n);
    if (!t) {
        em(ctx, rec_error(EC_HTTP_ERROR,
                          "endpoint returned a non-json body", true));
        return;
    }
    if (proto == PROTO_ANTHROPIC) map_resp_anthropic(t, em, ctx);
    else if (proto == PROTO_RESPONSES) map_resp_responses(t, em, ctx);
    else map_resp_chat(t, em, ctx);
    cJSON_Delete(t);
}

/* ---- streamed responses: the same records, partial, live ---- */

enum { MAX_TOOL_SLOTS = 1024, MAX_ANT_BLOCKS = 64 };

typedef struct {
    char *id, *name;
    buf_t args;
} pslot_t;

typedef struct {
    int type; /* -1 unset, 0 text, 1 thinking, 2 tool_use */
    char *id, *name;
    buf_t args;
} pblock_t;

struct llm_proxy_sse {
    int proto;
    emit_fn em;
    void *ctx;
    sse_parser_t sp;
    int open;          /* open streamed block: 0 response, 1 thinking, -1 */
    char *pend_sig;    /* signature of the open thinking block */
    cJSON *held_final; /* the turn's last block-final: usage rides it */
    pslot_t *slots;    /* chat tool_calls / responses function_call */
    size_t nslots;
    pblock_t blk[MAX_ANT_BLOCKS]; /* anthropic content blocks */
    size_t nblk;
    double u_in, u_out;
    bool have_in, have_out;
    bool done; /* [DONE] / message_stop / response.completed seen */
    bool failed;
    char fail_msg[512];
};

/* close the open block; the closer is held (not emitted) - it is the
   candidate final record of the turn, the one usage would attach to */
static void ps_close(llm_proxy_sse_t *s) {
    if (s->open < 0) return;
    if (s->held_final) {
        s->em(s->ctx, s->held_final);
        s->held_final = NULL;
    }
    if (s->open == 1)
        s->held_final =
            rec_thinking_final("", s->pend_sig ? s->pend_sig : "");
    else
        s->held_final = rec_text("response", "", false);
    s->open = -1;
    free(s->pend_sig);
    s->pend_sig = NULL;
}

static void ps_delta(llm_proxy_sse_t *s, int kind, const char *text) {
    if (!text || !*text) return;
    if (s->open != kind) {
        ps_close(s);
        s->open = kind;
    }
    s->em(s->ctx, rec_text(kind == 1 ? "thinking" : "response", text, true));
}

/* endpoint-controlled slot index, bounded and allocation-checked - a
   hostile endpoint must not be able to size an allocation */
static pslot_t *ps_slot_at(pslot_t **arr, size_t *n, double idx) {
    if (!(idx >= 0) || idx > (double)MAX_TOOL_SLOTS) return NULL;
    size_t i = (size_t)idx;
    if (i >= *n) {
        size_t nn = i + 1;
        pslot_t *grown = realloc(*arr, nn * sizeof **arr);
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

static void ps_slots_reset(pslot_t **arr, size_t *n) {
    for (size_t i = 0; i < *n; i++) {
        free((*arr)[i].id);
        free((*arr)[i].name);
        buf_free(&(*arr)[i].args);
    }
    free(*arr);
    *arr = NULL;
    *n = 0;
}

static pblock_t *ps_block_at(llm_proxy_sse_t *s, int idx) {
    if (idx < 0 || (size_t)idx >= MAX_ANT_BLOCKS) return NULL;
    for (size_t i = s->nblk; i <= (size_t)idx; i++) {
        s->blk[i].type = -1;
        s->blk[i].id = NULL;
        s->blk[i].name = NULL;
        buf_init(&s->blk[i].args);
    }
    if ((size_t)idx + 1 > s->nblk) s->nblk = (size_t)idx + 1;
    return &s->blk[idx];
}

static void ps_event_chat(llm_proxy_sse_t *s, const char *event,
                          const char *data, size_t n) {
    if (strcmp(event, "message")) return;
    if (n == 6 && !memcmp(data, "[DONE]", 6)) {
        s->done = true;
        return;
    }
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(d, "choices");
    if (cJSON_IsArray(choices))
        for (const cJSON *c = choices->child; c; c = c->next) {
            const cJSON *delta =
                cJSON_GetObjectItemCaseSensitive(c, "delta");
            if (!delta) continue;
            const cJSON *content =
                cJSON_GetObjectItemCaseSensitive(delta, "content");
            if (cJSON_IsString(content) && content->valuestring)
                ps_delta(s, 0, content->valuestring);
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(
                delta, "reasoning_content");
            if (!cJSON_IsString(reason))
                reason =
                    cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
            if (cJSON_IsString(reason) && reason->valuestring)
                ps_delta(s, 1, reason->valuestring);
            const cJSON *tcs =
                cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
            if (cJSON_IsArray(tcs))
                for (const cJSON *tc = tcs->child; tc; tc = tc->next) {
                    pslot_t *sl = ps_slot_at(&s->slots, &s->nslots,
                                             rec_num(tc, "index", 0));
                    if (!sl) continue;
                    const char *id = rec_str(tc, "id");
                    if (id) {
                        free(sl->id);
                        sl->id = strdup(id);
                    }
                    const cJSON *fn =
                        cJSON_GetObjectItemCaseSensitive(tc, "function");
                    if (fn) {
                        const char *nm = rec_str(fn, "name");
                        if (nm) {
                            free(sl->name);
                            sl->name = strdup(nm);
                        }
                        const cJSON *ar =
                            cJSON_GetObjectItemCaseSensitive(fn,
                                                             "arguments");
                        if (cJSON_IsString(ar) && ar->valuestring)
                            buf_append_str(&sl->args, ar->valuestring);
                    }
                }
        }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(d, "usage");
    if (cJSON_IsObject(u)) {
        s->u_in = rec_num(u, "prompt_tokens", 0);
        s->u_out = rec_num(u, "completion_tokens", 0);
        s->have_in = s->have_out = true;
    }
    cJSON_Delete(d);
}

static void ps_event_responses(llm_proxy_sse_t *s, const char *ev,
                               const char *data, size_t n) {
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    if (!strcmp(ev, "response.output_text.delta")) {
        ps_delta(s, 0, rec_str(d, "delta"));
    } else if (!strcmp(ev, "response.reasoning_summary_text.delta")) {
        ps_delta(s, 1, rec_str(d, "delta"));
    } else if (!strcmp(ev, "response.function_call_arguments.delta")) {
        pslot_t *sl = ps_slot_at(&s->slots, &s->nslots,
                                 rec_num(d, "output_index", 0));
        if (sl) {
            const char *a = rec_str(d, "delta");
            if (a) buf_append_str(&sl->args, a);
        }
    } else if (!strcmp(ev, "response.output_item.done")) {
        /* message/reasoning text already streamed as deltas; only the
           complete function_call item lands in a slot */
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(d, "item");
        const char *ty = rec_str(item, "type");
        if (item && ty && !strcmp(ty, "function_call")) {
            pslot_t *sl = ps_slot_at(
                &s->slots, &s->nslots,
                rec_num(item, "output_index", (double)s->nslots));
            if (sl) {
                const char *id = rec_str(item, "call_id");
                if (!id) id = rec_str(item, "id");
                if (id) {
                    free(sl->id);
                    sl->id = strdup(id);
                }
                const char *nm = rec_str(item, "name");
                if (nm) {
                    free(sl->name);
                    sl->name = strdup(nm);
                }
                buf_clear(&sl->args);
                const char *ar = rec_str(item, "arguments");
                if (ar) buf_append_str(&sl->args, ar);
            }
        }
    } else if (!strcmp(ev, "response.output_text.done")) {
        ps_close(s);
    } else if (!strcmp(ev, "response.completed") ||
               !strcmp(ev, "response.incomplete")) {
        const cJSON *resp =
            cJSON_GetObjectItemCaseSensitive(d, "response");
        const cJSON *u = resp
                             ? cJSON_GetObjectItemCaseSensitive(resp,
                                                                "usage")
                             : NULL;
        if (cJSON_IsObject(u)) {
            s->u_in = rec_num(u, "input_tokens", 0);
            s->u_out = rec_num(u, "output_tokens", 0);
            s->have_in = s->have_out = true;
        }
        s->done = true;
    } else if (!strcmp(ev, "response.failed") || !strcmp(ev, "error")) {
        const cJSON *resp =
            cJSON_GetObjectItemCaseSensitive(d, "response");
        const cJSON *err =
            resp ? cJSON_GetObjectItemCaseSensitive(resp, "error")
                 : cJSON_GetObjectItemCaseSensitive(d, "error");
        const char *m = rec_str(err, "message");
        if (!m) m = rec_str(d, "message");
        snprintf(s->fail_msg, sizeof s->fail_msg, "%s",
                 m ? m : "unknown error");
        s->failed = true;
        s->done = true;
    }
    cJSON_Delete(d);
}

static void ps_event_anthropic(llm_proxy_sse_t *s, const char *event,
                               const char *data, size_t n) {
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    if (!strcmp(event, "message_start")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(d, "message");
        const cJSON *u =
            msg ? cJSON_GetObjectItemCaseSensitive(msg, "usage") : NULL;
        if (cJSON_IsObject(u)) {
            s->u_in = rec_num(u, "input_tokens", 0);
            s->have_in = true;
        }
    } else if (!strcmp(event, "content_block_start")) {
        const cJSON *cb = cJSON_GetObjectItemCaseSensitive(d,
                                                           "content_block");
        const char *ty = rec_str(cb, "type");
        pblock_t *bl = ps_block_at(s, (int)rec_num(d, "index", 0));
        if (!bl) goto out;
        if (ty && !strcmp(ty, "thinking")) {
            ps_close(s);
            s->open = 1;
            bl->type = 1;
        } else if (ty && !strcmp(ty, "text")) {
            ps_close(s);
            s->open = 0;
            bl->type = 0;
        } else if (ty && !strcmp(ty, "tool_use")) {
            ps_close(s);
            bl->type = 2;
            const char *id = rec_str(cb, "id");
            const char *nm = rec_str(cb, "name");
            bl->id = strdup(id ? id : "");
            bl->name = strdup(nm ? nm : "");
        }
    } else if (!strcmp(event, "content_block_delta")) {
        const cJSON *delta =
            cJSON_GetObjectItemCaseSensitive(d, "delta");
        const char *ty = rec_str(delta, "type");
        if (ty && !strcmp(ty, "text_delta"))
            ps_delta(s, 0, rec_str(delta, "text"));
        else if (ty && !strcmp(ty, "thinking_delta"))
            ps_delta(s, 1, rec_str(delta, "thinking"));
        else if (ty && !strcmp(ty, "signature_delta")) {
            const char *sig = rec_str(delta, "signature");
            if (sig) {
                free(s->pend_sig);
                s->pend_sig = strdup(sig);
            }
        } else if (ty && !strcmp(ty, "input_json_delta")) {
            pblock_t *bl =
                ps_block_at(s, (int)rec_num(d, "index", 0));
            const char *pj = rec_str(delta, "partial_json");
            if (bl && pj) buf_append_str(&bl->args, pj);
        }
    } else if (!strcmp(event, "content_block_stop")) {
        int idx = (int)rec_num(d, "index", 0);
        if (s->open >= 0 && idx >= 0 && (size_t)idx < s->nblk &&
            (s->blk[idx].type == 0 || s->blk[idx].type == 1))
            ps_close(s);
        /* tool_use blocks are emitted at finish */
    } else if (!strcmp(event, "message_delta")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(d, "usage");
        if (cJSON_IsObject(u)) {
            s->u_out = rec_num(u, "output_tokens", 0);
            s->have_out = true;
        }
    } else if (!strcmp(event, "message_stop")) {
        s->done = true;
    } else if (!strcmp(event, "error")) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(d, "error");
        const char *m = rec_str(err, "message");
        snprintf(s->fail_msg, sizeof s->fail_msg, "%s",
                 m ? m : "unknown error");
        s->failed = true;
        s->done = true;
    }
out:
    cJSON_Delete(d);
}

static void ps_event(void *v, const char *event, const char *data,
                     size_t n) {
    llm_proxy_sse_t *s = v;
    if (!strcmp(event, "ping")) return;
    if (s->proto == PROTO_ANTHROPIC)
        ps_event_anthropic(s, event, data, n);
    else if (s->proto == PROTO_RESPONSES)
        ps_event_responses(s, event, data, n);
    else
        ps_event_chat(s, event, data, n);
}

static size_t ps_ntools(llm_proxy_sse_t *s) {
    if (s->proto == PROTO_ANTHROPIC) {
        size_t n = 0;
        for (size_t i = 0; i < s->nblk; i++) n += s->blk[i].type == 2;
        return n;
    }
    return s->nslots;
}

static void ps_emit_tools(llm_proxy_sse_t *s, size_t ntools) {
    size_t seen = 0;
    if (s->proto == PROTO_ANTHROPIC) {
        for (size_t i = 0; i < s->nblk; i++) {
            if (s->blk[i].type != 2) continue;
            seen++;
            cJSON *args = cJSON_Parse(s->blk[i].args.data
                                          ? s->blk[i].args.data
                                          : "{}");
            if (!args) args = cJSON_CreateObject();
            cJSON *rec =
                rec_tool_request(s->blk[i].name, args, s->blk[i].id);
            cJSON_Delete(args);
            if (seen == ntools && (s->have_in || s->have_out))
                rec_attach_usage(rec, s->u_in,
                                 s->have_out ? s->u_out : 0);
            s->em(s->ctx, rec);
        }
        return;
    }
    for (size_t i = 0; i < s->nslots; i++) {
        cJSON *args =
            cJSON_Parse(s->slots[i].args.data ? s->slots[i].args.data
                                              : "{}");
        if (!args) args = cJSON_CreateObject();
        cJSON *rec = rec_tool_request(s->slots[i].name, args, s->slots[i].id);
        cJSON_Delete(args);
        if (i + 1 == s->nslots && (s->have_in || s->have_out))
            rec_attach_usage(rec, s->u_in, s->have_out ? s->u_out : 0);
        s->em(s->ctx, rec);
    }
}

llm_proxy_sse_t *llm_proxy_sse_new(int proto, emit_fn em, void *ctx) {
    llm_proxy_sse_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->proto = proto;
    s->em = em;
    s->ctx = ctx;
    s->open = -1;
    sse_init(&s->sp, ps_event, s);
    return s;
}

void llm_proxy_sse_feed(llm_proxy_sse_t *s, const char *bytes, size_t n) {
    sse_feed(&s->sp, bytes, n);
}

void llm_proxy_sse_finish(llm_proxy_sse_t *s) {
    sse_eof(&s->sp);
    if (s->failed) {
        s->em(s->ctx, rec_error(EC_API_ERROR, s->fail_msg, true));
        return;
    }
    if (s->proto == PROTO_ANTHROPIC && !s->done) {
        /* the wire's own rule: a stream that never reached message_stop */
        s->em(s->ctx, rec_error(EC_API_ERROR,
                                "stream ended without message_stop", true));
        return;
    }
    ps_close(s); /* the wires' blk_stop: the open block's final is held */
    size_t ntools = ps_ntools(s);
    if (ntools) {
        if (s->held_final) {
            s->em(s->ctx, s->held_final);
            s->held_final = NULL;
        }
        ps_emit_tools(s, ntools);
        return;
    }
    if (s->held_final) {
        if (s->have_in || s->have_out)
            rec_attach_usage(s->held_final, s->u_in,
                             s->have_out ? s->u_out : 0);
        s->em(s->ctx, s->held_final);
        s->held_final = NULL;
    }
}

void llm_proxy_sse_free(llm_proxy_sse_t *s) {
    if (!s) return;
    sse_free(&s->sp);
    ps_slots_reset(&s->slots, &s->nslots);
    for (size_t i = 0; i < s->nblk; i++) {
        free(s->blk[i].id);
        free(s->blk[i].name);
        buf_free(&s->blk[i].args);
    }
    free(s->pend_sig);
    cJSON_Delete(s->held_final);
    free(s);
}

/* ================= client side: plain http ================= */

/* caps keep a hostile local client from sizing memory; generous enough
   for image-bearing llm requests */
enum { HEAD_MAX = 64 * 1024 };
#define BODY_MAX (1u << 30)

typedef struct {
    char *name, *value;
} cheader_t;

typedef struct {
    char method[8], target[2048];
    int minor; /* HTTP/1.<minor> */
    cheader_t *h;
    size_t nh, caph;
    buf_t body;
    bool keep, expect100;
} creq_t;

static void creq_free(creq_t *r) {
    for (size_t i = 0; i < r->nh; i++) {
        free(r->h[i].name);
        free(r->h[i].value);
    }
    free(r->h);
    buf_free(&r->body);
    memset(r, 0, sizeof *r);
}

static const char *creq_header(const creq_t *r, const char *name) {
    for (size_t i = 0; i < r->nh; i++)
        if (!strcasecmp(r->h[i].name, name)) return r->h[i].value;
    return NULL;
}

static bool sock_write_all(sock_t s, const char *p, size_t n) {
    while (n) {
        size_t chunk = n > (1u << 20) ? (1u << 20) : n;
        ssize_t w = send(s, p, (int)chunk, 0);
        if (w < 0) {
#ifdef _WIN32
            return false;
#else
            if (errno == EINTR) {
                if (g_stop_flag) return false;
                continue;
            }
            return false;
#endif
        }
        if (w == 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

typedef struct {
    sock_t fd;
    buf_t in;   /* unconsumed request bytes */
    size_t pos; /* consumed upto */
} creader_t;

static void cr_init(creader_t *r, sock_t fd) {
    r->fd = fd;
    buf_init(&r->in);
    r->pos = 0;
}

static void cr_free(creader_t *r) { buf_free(&r->in); }

/* one more chunk into the buffer; 1 got, 0 eof (or stop), -1 error */
static int cr_fill(creader_t *r) {
    char tmp[16384];
    for (;;) {
        ssize_t n = recv(r->fd, tmp, (int)sizeof tmp, 0);
        if (n > 0) {
            buf_append(&r->in, tmp, (size_t)n);
            return 1;
        }
        if (n == 0) return 0;
#ifdef _WIN32
        return -1;
#else
        if (errno == EINTR) {
            if (g_stop_flag) return 0;
            continue;
        }
        return -1;
#endif
    }
}

static const char *find_sub(const char *hay, size_t n, const char *needle,
                            size_t nlen) {
    if (n < nlen) return NULL;
    for (size_t i = 0; i + nlen <= n; i++)
        if (!memcmp(hay + i, needle, nlen)) return hay + i;
    return NULL;
}

static int parse_head(const char *head, size_t n, creq_t *req) {
    /* request line */
    size_t i = 0;
    while (i < n && head[i] != '\n')
        i++;
    size_t rl = i < n ? i : n;
    if (rl && head[rl - 1] == '\r') rl--;
    const char *sp1 = memchr(head, ' ', rl);
    const char *sp2 =
        sp1 ? memchr(sp1 + 1, ' ', rl - (size_t)(sp1 - head) - 1) : NULL;
    if (!sp1 || !sp2 || sp1 == head) return -1;
    size_t ml = (size_t)(sp1 - head);
    if (!ml || ml >= sizeof req->method) return -1;
    memcpy(req->method, head, ml);
    req->method[ml] = '\0';
    size_t tl = (size_t)(sp2 - sp1 - 1);
    if (!tl || tl >= sizeof req->target) return -1;
    memcpy(req->target, sp1 + 1, tl);
    req->target[tl] = '\0';
    size_t vl = rl - (size_t)(sp2 + 1 - head);
    if (vl != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) ||
        (sp2[8] != '0' && sp2[8] != '1'))
        return -1;
    req->minor = sp2[8] - '0';

    /* header fields */
    size_t pos = i < n ? i + 1 : n;
    while (pos < n) {
        size_t e = pos;
        while (e < n && head[e] != '\n')
            e++;
        size_t ll = e - pos;
        if (ll && head[pos + ll - 1] == '\r') ll--;
        if (!ll) break; /* stray blank line: end of fields */
        const char *colon = memchr(head + pos, ':', ll);
        if (!colon) return -1;
        size_t nl = (size_t)(colon - (head + pos));
        size_t vs = nl + 1;
        while (vs < ll &&
               (head[pos + vs] == ' ' || head[pos + vs] == '\t'))
            vs++;
        size_t ve = ll;
        while (ve > vs &&
               (head[pos + ve - 1] == ' ' || head[pos + ve - 1] == '\t'))
            ve--;
        for (size_t k = 0; k < nl; k++) {
            unsigned char ch = (unsigned char)head[pos + k];
            if (ch <= 0x20 || ch >= 0x7f) return -1;
        }
        for (size_t k = vs; k < ve; k++) {
            unsigned char ch = (unsigned char)head[pos + k];
            if (ch < 0x20 || ch == 0x7f) return -1;
        }
        if (req->nh == req->caph) {
            req->caph = req->caph ? req->caph * 2 : 16;
            req->h = realloc(req->h, req->caph * sizeof *req->h);
        }
        req->h[req->nh].name = strndup(head + pos, nl);
        req->h[req->nh].value = strndup(head + pos + vs, ve - vs);
        req->nh++;
        if (e >= n) break;
        pos = e + 1;
    }
    return 0;
}

/* read one request. 0 ok, 1 clean eof (no pending bytes), -1 io error,
   -2 malformed (400), -3 chunked body (411), -4 head too large (431),
   -5 body too large (413) */
static int read_request(creader_t *r, creq_t *req, sock_t c) {
    memset(req, 0, sizeof *req);
    buf_init(&req->body);
    size_t head_len = 0, body_off = 0;

    for (;;) {
        size_t avail = r->in.len - r->pos;
        const char *base = r->in.data ? r->in.data : "";
        const char *crlf =
            find_sub(base + r->pos, avail, "\r\n\r\n", 4);
        const char *lflf = find_sub(base + r->pos, avail, "\n\n", 2);
        const char *hit = NULL;
        size_t tlen = 0;
        if (crlf && (!lflf || crlf <= lflf)) {
            hit = crlf;
            tlen = 4;
        } else if (lflf) {
            hit = lflf;
            tlen = 2;
        }
        if (hit) {
            head_len = (size_t)(hit - (base + r->pos));
            body_off = r->pos + head_len + tlen;
            break;
        }
        if (avail > HEAD_MAX) return -4;
        int rc = cr_fill(r);
        if (rc == 0)
            return (r->in.len == r->pos) ? 1 : -1;
        if (rc < 0) return -1;
    }

    if (parse_head(r->in.data + r->pos, head_len, req) != 0) return -2;
    const char *te = creq_header(req, "transfer-encoding");
    if (te && *te) return -3;
    const char *cn = creq_header(req, "connection");
    if (cn) {
        buf_t low;
        buf_init(&low);
        for (const char *p = cn; *p; p++)
            buf_append_byte(&low, (char)tolower((unsigned char)*p));
        bool close_hdr = strstr(low.data ? low.data : "", "close") != NULL;
        bool ka_hdr =
            strstr(low.data ? low.data : "", "keep-alive") != NULL;
        buf_free(&low);
        req->keep = close_hdr ? false : (ka_hdr ? true : req->minor == 1);
    } else {
        req->keep = req->minor == 1;
    }
    const char *ex = creq_header(req, "expect");
    if (ex && !strcasecmp(ex, "100-continue")) req->expect100 = true;

    size_t blen = 0;
    const char *cl = creq_header(req, "content-length");
    if (cl && *cl) {
        char *end = NULL;
        errno = 0;
        unsigned long long v = strtoull(cl, &end, 10);
        if (errno || !cl[0] || !end || *end) return -2;
        if (v > BODY_MAX) return -5;
        blen = (size_t)v;
    }

    if (req->expect100) { /* unblock the client before the body wait */
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!sock_write_all(c, cont, sizeof cont - 1)) return -1;
    }
    while (r->in.len - body_off < blen) {
        int rc = cr_fill(r);
        if (rc <= 0) return -1;
    }
    if (blen) buf_append(&req->body, r->in.data + body_off, blen);
    r->pos = body_off + blen;
    return 0;
}

/* ================= upstream side ================= */

static const char *proto_path(int proto) {
    return proto == PROTO_ANTHROPIC     ? "/messages"
           : proto == PROTO_RESPONSES   ? "/responses"
                                        : "/chat/completions";
}

static const char *proto_name(int proto) {
    return proto == PROTO_ANTHROPIC     ? "anthropic"
           : proto == PROTO_RESPONSES   ? "openai-responses"
                                        : "openai";
}

static bool req_hdr_skip(const char *name) {
    static const char *skip[] = {
        "host",           "content-length",   "transfer-encoding",
        "connection",     "keep-alive",       "expect",
        "accept-encoding", "proxy-authenticate", "proxy-authorization",
        "te",             "trailer",          "upgrade",
    };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (!strcasecmp(name, skip[i])) return true;
    return false;
}

static bool resp_hdr_skip(const char *name) {
    static const char *skip[] = {
        "content-length", "content-encoding", "transfer-encoding",
        "connection",     "keep-alive",       "proxy-authenticate",
        "proxy-authorization", "te",          "trailer",
        "upgrade",
    };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (!strcasecmp(name, skip[i])) return true;
    return false;
}

static void live_emit(void *ctx, cJSON *rec) {
    pretty_live_record(ctx, rec);
    cJSON_Delete(rec);
}

typedef struct {
    sock_t c;
    pretty_live_t *pr;
    llm_proxy_sse_t *sse;
    int proto;
    bool head_sent;
    bool is_sse;
    bool client_ok;
    bool got_body;
    long status;
    char reason[64];
    buf_t ctype;
    char **hlines; /* response headers to forward, "Name: value" */
    size_t nh, caph;
    buf_t body; /* buffered (non-sse) response body */
} fx_t;

static bool fx_is_sse(const fx_t *f) {
    static const char ct[] = "text/event-stream";
    return f->ctype.len >= sizeof ct - 1 &&
           !strncasecmp(f->ctype.data ? f->ctype.data : "", ct,
                        sizeof ct - 1);
}

static void fx_send_head(fx_t *f, long content_len /* -1: none */) {
    if (f->head_sent) return;
    buf_t h;
    buf_init(&h);
    buf_appendf(&h, "HTTP/1.1 %ld %s\r\n", f->status,
                f->reason[0] ? f->reason : "OK");
    for (size_t i = 0; i < f->nh; i++) {
        buf_append_str(&h, f->hlines[i]);
        buf_append_str(&h, "\r\n");
    }
    if (content_len >= 0)
        buf_appendf(&h, "Content-Length: %ld\r\n", content_len);
    buf_append_str(&h,
                   f->is_sse ? "Connection: close\r\n"
                             : "Connection: keep-alive\r\n");
    buf_append_str(&h, "\r\n");
    f->client_ok =
        sock_write_all(f->c, h.data ? h.data : "", h.len) && f->client_ok;
    f->head_sent = true;
    buf_free(&h);
}

static void fx_add_hline(fx_t *f, const char *line) {
    if (f->nh == f->caph) {
        f->caph = f->caph ? f->caph * 2 : 16;
        f->hlines = realloc(f->hlines, f->caph * sizeof *f->hlines);
    }
    char *d = strdup(line);
    if (!d) return;
    f->hlines[f->nh++] = d;
}

static size_t fx_hdr_cb(char *buf, size_t sz, size_t nm, void *ud) {
    fx_t *f = ud;
    size_t n = sz * nm;
    if (n > 5 && !memcmp(buf, "HTTP/", 5)) {
        const char *sp = memchr(buf, ' ', n);
        if (sp) {
            f->status = strtol(sp + 1, NULL, 10);
            size_t vs = (size_t)(sp + 1 - buf);
            while (vs < n && buf[vs] != ' ')
                vs++;
            size_t ve = n;
            while (ve > vs && (buf[ve - 1] == '\r' || buf[ve - 1] == '\n' ||
                               buf[ve - 1] == ' '))
                ve--;
            size_t cp = ve - vs < sizeof f->reason - 1
                            ? ve - vs
                            : sizeof f->reason - 1;
            memcpy(f->reason, buf + vs, cp);
            f->reason[cp] = '\0';
        }
        return n;
    }
    size_t e = n;
    while (e && (buf[e - 1] == '\r' || buf[e - 1] == '\n'))
        e--;
    if (e > 13 && !strncasecmp(buf, "Content-Type:", 13)) {
        size_t vs = 13;
        while (vs < e && (buf[vs] == ' ' || buf[vs] == '\t'))
            vs++;
        buf_clear(&f->ctype);
        buf_append(&f->ctype, buf + vs, e - vs);
    }
    const char *colon = memchr(buf, ':', e);
    if (colon) {
        size_t nl = (size_t)(colon - buf);
        char name[64];
        size_t cp = nl < sizeof name - 1 ? nl : sizeof name - 1;
        memcpy(name, buf, cp);
        name[cp] = '\0';
        if (!resp_hdr_skip(name)) {
            char *line = malloc(e + 1);
            if (line) {
                memcpy(line, buf, e);
                line[e] = '\0';
                fx_add_hline(f, line); /* strdups; frees line below */
                free(line);
            }
        }
    }
    return n;
}

static size_t fx_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    fx_t *f = ud;
    size_t n = sz * nm;
    f->got_body = true;
    if (!f->head_sent) {
        f->is_sse = fx_is_sse(f);
        if (f->is_sse) fx_send_head(f, -1); /* live: close-delimited */
    }
    if (f->is_sse && f->head_sent) {
        /* tee to the client and parse live, byte for byte */
        f->client_ok =
            sock_write_all(f->c, ptr, n) && f->client_ok;
        if (f->sse) llm_proxy_sse_feed(f->sse, ptr, n);
    } else {
        buf_append(&f->body, ptr, n);
    }
    if (g_stop_flag) return 0; /* abort the transfer */
    return n;
}

static void send_local_error(sock_t c, int code, const char *reason,
                             const char *msg) {
    buf_t body;
    buf_init(&body);
    buf_append_str(&body, "{\"error\":{\"message\":");
    buf_append_jstr(&body, msg);
    buf_append_str(&body, "}}");
    buf_t h;
    buf_init(&h);
    buf_appendf(&h,
                "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                code, reason, (long)body.len);
    buf_append(&h, body.data ? body.data : "", body.len);
    sock_write_all(c, h.data ? h.data : "", h.len);
    buf_free(&h);
    buf_free(&body);
}

/* forward one request upstream, answer the client, render both
   directions. returns false when the connection must close. */
static bool forward(creq_t *req, sock_t c, const llm_proxy_cfg_t *cfg,
                    pretty_live_t *pr) {
    /* the request renders before it leaves - the response will follow */
    llm_proxy_map_request(cfg->protocol,
                          req->body.data ? req->body.data : "",
                          req->body.len, live_emit, pr);

    buf_t url;
    buf_init(&url);
    buf_append_str(&url, cfg->api_base);
    {
        const char *pp = proto_path(cfg->protocol);
        if (url.len && url.data[url.len - 1] == '/' && pp[0] == '/') pp++;
        buf_append_str(&url, pp);
    }

    struct curl_slist *hdrs = NULL;
    bool have_ctype = false, have_ua = false, have_auth = false;
    for (size_t i = 0; i < req->nh; i++) {
        if (req_hdr_skip(req->h[i].name)) continue;
        if (!strcasecmp(req->h[i].name, "content-type")) have_ctype = true;
        if (!strcasecmp(req->h[i].name, "user-agent")) have_ua = true;
        if (!strcasecmp(req->h[i].name, "authorization") ||
            !strcasecmp(req->h[i].name, "x-api-key"))
            have_auth = true;
        http_hdr_add(&hdrs, req->h[i].name, req->h[i].value);
    }
    if (!have_ctype) http_hdr_add(&hdrs, "Content-Type", "application/json");
    if (!have_ua) http_hdr_add(&hdrs, "User-Agent", "llmkit/" LLMKIT_VERSION);
    if (cfg->key && cfg->key[0] && !have_auth) {
        if (cfg->protocol == PROTO_ANTHROPIC) {
            http_hdr_add(&hdrs, "x-api-key", cfg->key);
        } else {
            buf_t auth;
            buf_init(&auth);
            buf_appendf(&auth, "Bearer %s", cfg->key);
            http_hdr_add(&hdrs, "Authorization", auth.data ? auth.data
                                                           : "");
            buf_free(&auth);
        }
    }

    fx_t f;
    memset(&f, 0, sizeof f);
    f.c = c;
    f.pr = pr;
    f.proto = cfg->protocol;
    f.client_ok = true;
    buf_init(&f.ctype);
    buf_init(&f.body);
    f.sse = llm_proxy_sse_new(cfg->protocol, live_emit, pr);

    bool keep = false;
    CURL *cu = curl_easy_init();
    if (!cu) {
        send_local_error(c, 502, "Bad Gateway", "cannot start the transfer");
        goto done;
    }
    curl_easy_setopt(cu, CURLOPT_URL, url.data);
    curl_easy_setopt(cu, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(cu, CURLOPT_POST, 1L);
    curl_easy_setopt(cu, CURLOPT_POSTFIELDS,
                     req->body.data ? req->body.data : "");
    curl_easy_setopt(cu, CURLOPT_POSTFIELDSIZE, (long)req->body.len);
    curl_easy_setopt(cu, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(cu, CURLOPT_HEADERFUNCTION, fx_hdr_cb);
    curl_easy_setopt(cu, CURLOPT_HEADERDATA, &f);
    curl_easy_setopt(cu, CURLOPT_WRITEFUNCTION, fx_write_cb);
    curl_easy_setopt(cu, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(cu, CURLOPT_CONNECTTIMEOUT, 30L);
    /* identity only: the interception side reads these bytes */
    curl_easy_setopt(cu, CURLOPT_ACCEPT_ENCODING, "identity");

    CURLcode res = curl_easy_perform(cu);
    curl_easy_cleanup(cu);

    if (res != CURLE_OK) {
        /* transport failure: the wires' error shaping on stdout, a 502
           for the client when nothing of the response left yet */
        http_req_t hr;
        memset(&hr, 0, sizeof hr);
        buf_init(&hr.resp);
        hr.curl_res = res;
        hr.got_data = f.got_body;
        live_emit(pr, http_transport_error(&hr));
        buf_free(&hr.resp);
        if (!f.head_sent)
            send_local_error(c, 502, "Bad Gateway",
                             curl_easy_strerror(res));
        goto done;
    }

    if (!f.head_sent) { /* empty body: the head never went out live */
        f.is_sse = fx_is_sse(&f);
        fx_send_head(&f, f.is_sse ? -1 : (long)f.body.len);
    }
    if (!f.is_sse) {
        if (f.body.len)
            f.client_ok = sock_write_all(c, f.body.data, f.body.len) &&
                          f.client_ok;
        llm_proxy_map_response(cfg->protocol, f.status,
                               f.body.data ? f.body.data : "", f.body.len,
                               live_emit, pr);
        keep = req->keep && f.client_ok && !g_stop_flag;
    } else {
        llm_proxy_sse_finish(f.sse);
        keep = false; /* the streamed response is close-delimited */
    }

done:
    llm_proxy_sse_free(f.sse);
    for (size_t i = 0; i < f.nh; i++) free(f.hlines[i]);
    free(f.hlines);
    buf_free(&f.ctype);
    buf_free(&f.body);
    curl_slist_free_all(hdrs);
    buf_free(&url);
    return keep;
}

/* ================= serve ================= */

static void serve_connection(sock_t c, const llm_proxy_cfg_t *cfg,
                             pretty_live_t *pr) {
    creader_t r;
    cr_init(&r, c);
    for (;;) {
        creq_t req;
        int rc = read_request(&r, &req, c);
        if (rc == 1) break;  /* clean eof */
        if (rc == -1) break; /* io error: just close */
        if (rc == -2) {
            send_local_error(c, 400, "Bad Request",
                             "malformed http request");
            creq_free(&req);
            break;
        }
        if (rc == -3) {
            send_local_error(c, 411, "Length Required",
                             "chunked request bodies are not supported");
            creq_free(&req);
            break;
        }
        if (rc == -4) {
            send_local_error(c, 431, "Request Header Fields Too Large",
                             "the request head is too large");
            creq_free(&req);
            break;
        }
        if (rc == -5) {
            send_local_error(c, 413, "Content Too Large",
                             "the request body is too large");
            creq_free(&req);
            break;
        }
        if (strcmp(req.method, "POST")) {
            send_local_error(c, 405, "Method Not Allowed",
                             "llmkit proxy forwards POST requests only");
            creq_free(&req);
            break;
        }
        bool keep = forward(&req, c, cfg, pr);
        creq_free(&req);
        if (!keep || g_stop_flag) break;
    }
    cr_free(&r);
    sock_close(c);
}

static int listen_tcp(const char *host, const char *port,
                      char bound[64]) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* v4 listen: no ipv6 socket, doc'd */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host && *host ? host : NULL, port, &hints, &res);
    if (rc != 0 || !res) {
        fprintf(stderr, "llmkit proxy: cannot resolve the listen address: "
                        "%s\n",
                gai_strerror(rc));
        return (int)SOCK_INVALID;
    }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID) {
        fprintf(stderr, "llmkit proxy: cannot create the listen socket\n");
        freeaddrinfo(res);
        return (int)SOCK_INVALID;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
               sizeof one);
    if (bind(s, res->ai_addr, res->ai_addrlen) != 0) {
        fprintf(stderr,
                "llmkit proxy: cannot bind %s:%s: %s\n",
                host && *host ? host : "0.0.0.0", port, strerror(errno));
        freeaddrinfo(res);
        sock_close(s);
        return (int)SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (listen(s, 16) != 0) {
        fprintf(stderr, "llmkit proxy: cannot listen: %s\n",
                strerror(errno));
        sock_close(s);
        return (int)SOCK_INVALID;
    }
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getsockname(s, (struct sockaddr *)&sa, &sl) == 0) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
        snprintf(bound, 64, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    } else {
        snprintf(bound, 64, "%s", host && *host ? host : "0.0.0.0");
    }
    return (int)s;
}

/* ================= command ================= */

static void proxy_usage(FILE *out) {
    fputs("usage: llmkit proxy (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "               [--key <token>] [--listen <host:port>]\n"
          "\n"
          "       a plain http endpoint (no tls) that forwards POSTs to\n"
          "       the api_base in the named protocol and renders the\n"
          "       conversation on stdout, prettyprint-style; default\n"
          "       listen 127.0.0.1:8080\n",
          out);
}

int cmd_llmproxy(int argc, char **argv) {
    signals_init();
    llm_proxy_cfg_t c;
    char err[256] = "";
    if (llm_proxy_parse(argc - 2, argv + 2, &c, err, sizeof err) != 0) {
        fprintf(stderr, "llmkit proxy: %s\n", err);
        proxy_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    if (!utf8_valid((const uint8_t *)c.api_base, strlen(c.api_base)) ||
        (c.key && !utf8_valid((const uint8_t *)c.key, strlen(c.key))) ||
        !utf8_valid((const uint8_t *)c.listen, strlen(c.listen))) {
        fprintf(stderr, "llmkit proxy: invalid UTF-8 in a flag value\n");
        llm_proxy_cfg_free(&c);
        return EXIT_OUT_OF_CHANNEL;
    }
    if (strncasecmp(c.api_base, "http://", 7) != 0 &&
        strncasecmp(c.api_base, "https://", 8) != 0) {
        fprintf(stderr,
                "llmkit proxy: <api_base> must start with http:// or "
                "https://\n");
        llm_proxy_cfg_free(&c);
        return EXIT_OUT_OF_CHANNEL;
    }

    /* the listen spec: [host]:port, ipv4 names or numbers, no ipv6 */
    char host[128] = "", port[8] = "";
    {
        const char *cl = strrchr(c.listen, ':');
        if (!cl) {
            fprintf(stderr,
                    "llmkit proxy: --listen wants <host:port> (host may "
                    "be empty), got '%s'\n",
                    c.listen);
            llm_proxy_cfg_free(&c);
            proxy_usage(stderr);
            return EXIT_OUT_OF_CHANNEL;
        }
        size_t hl = (size_t)(cl - c.listen);
        if (hl >= sizeof host) {
            fprintf(stderr, "llmkit proxy: --listen host is too long\n");
            llm_proxy_cfg_free(&c);
            return EXIT_OUT_OF_CHANNEL;
        }
        memcpy(host, c.listen, hl);
        host[hl] = '\0';
        snprintf(port, sizeof port, "%s", cl + 1);
        char *end = NULL;
        errno = 0;
        long p = strtol(port, &end, 10);
        if (errno || !port[0] || !end || *end || p < 0 || p > 65535) {
            fprintf(stderr,
                    "llmkit proxy: --listen wants a numeric port, got "
                    "'%s'\n",
                    port);
            llm_proxy_cfg_free(&c);
            return EXIT_OUT_OF_CHANNEL;
        }
    }

    if (net_init() != 0) {
        fprintf(stderr, "llmkit proxy: cannot start the network stack\n");
        llm_proxy_cfg_free(&c);
        return EXIT_OUT_OF_CHANNEL;
    }
    char bound[64];
    int ls = listen_tcp(host, port, bound);
    if (ls == (int)SOCK_INVALID) {
        llm_proxy_cfg_free(&c);
        return EXIT_OUT_OF_CHANNEL;
    }
    fprintf(stderr,
            "llmkit proxy: listening on http://%s (plain tcp, no tls)\n",
            bound);
    fprintf(stderr, "llmkit proxy: forwarding to %s %s%s\n",
            proto_name(c.protocol), c.api_base, proto_path(c.protocol));

    pretty_live_t *pr = pretty_live_new(stdout);
    while (!g_stop_flag) {
        sock_t cs = accept((sock_t)ls, NULL, NULL);
        if (cs == SOCK_INVALID) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            if (g_stop_flag) break;
            msleep(50); /* transient (emfile &c): stay up */
            continue;
        }
        serve_connection(cs, &c, pr);
    }
    sock_close((sock_t)ls);
    int rc = pretty_live_io_failed(pr) ? EXIT_OUT_OF_CHANNEL : EXIT_OK;
    pretty_live_free(pr);
    llm_proxy_cfg_free(&c);
    return rc;
}
