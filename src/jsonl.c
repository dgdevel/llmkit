/* jsonl.c - input pipeline, validation, record builders, transcript (design sec.3). */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>

int exit_code_of(const char *code) {
    if (!code) return EXIT_OUT_OF_CHANNEL;
    if (!strcmp(code, EC_INVALID_RECORD)) return EXIT_INVALID_RECORD;
    if (!strcmp(code, EC_CONNECT_FAILED)) return EXIT_CONNECT_FAILED;
    if (!strcmp(code, EC_HTTP_ERROR)) return EXIT_HTTP_ERROR;
    if (!strcmp(code, EC_API_ERROR)) return EXIT_API_ERROR;
    if (!strcmp(code, EC_MAX_ROUNDS)) return EXIT_MAX_TOOL_ROUNDS;
    if (!strcmp(code, EC_IO_ERROR)) return EXIT_IO_ERROR;
    if (!strcmp(code, EC_INTERRUPTED)) return EXIT_INTERRUPTED;
    return EXIT_OUT_OF_CHANNEL;
}

int protocol_of(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "openai")) return PROTO_OPENAI;
    if (!strcmp(s, "openai_responses")) return PROTO_RESPONSES;
    if (!strcmp(s, "anthropic")) return PROTO_ANTHROPIC;
    return -1;
}

/* ---- classification ---- */

static const struct { const char *name; int kind; } rec_names[] = {
    {"header", R_HEADER},         {"llm", R_LLM},
    {"tools", R_TOOLS},           {"options", R_OPTIONS},
    {"system", R_SYSTEM},         {"user", R_USER},
    {"flush", R_FLUSH},           {"start", R_START},
    {"error", R_ERROR},           {"thinking", R_THINKING},
    {"response", R_RESPONSE},     {"tool_request", R_TOOL_REQUEST},
    {"tool_response", R_TOOL_RESPONSE},
    {"agent-as-tool", R_AGENT},   {"expose", R_EXPOSE},
    {"hide", R_HIDE},
};

int rec_classify(const cJSON *tree) {
    if (!cJSON_IsObject(tree)) return R_UNKNOWN;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(tree, "type");
    if (!cJSON_IsString(t) || !t->valuestring) return R_UNKNOWN;
    for (size_t i = 0; i < sizeof rec_names / sizeof rec_names[0]; i++)
        if (!strcmp(t->valuestring, rec_names[i].name)) return rec_names[i].kind;
    return R_UNKNOWN;
}

const char *rec_str(const cJSON *t, const char *field) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(t, field);
    if (cJSON_IsString(f) && f->valuestring) return f->valuestring;
    return NULL;
}

bool rec_bool(const cJSON *t, const char *field, bool dflt) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(t, field);
    if (cJSON_IsBool(f)) return cJSON_IsTrue(f);
    return dflt;
}

double rec_num(const cJSON *t, const char *field, double dflt) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(t, field);
    if (cJSON_IsNumber(f)) return f->valuedouble;
    return dflt;
}

/* ---- validators: return malloc'd reason or NULL ---- */

static char *vmsg(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *m = malloc((size_t)n + 1);
    vsnprintf(m, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return m;
}

/* header objects travel verbatim into http request lines: CR/LF in a
   name or value would inject additional headers */
static char *validate_headers(const cJSON *h) {
    for (const cJSON *it = h->child; it; it = it->next) {
        if (!it->string) continue;
        if (strchr(it->string, '\r') || strchr(it->string, '\n'))
            return vmsg("header name must not contain CR or LF");
        if (!cJSON_IsString(it))
            return vmsg("header '%s' must have a string value", it->string);
        if (strchr(it->valuestring, '\r') || strchr(it->valuestring, '\n'))
            return vmsg("header '%s' value must not contain CR or LF",
                        it->string);
    }
    return NULL;
}

char *validate_llm(const cJSON *t) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(t, "endpoint_protocol");
    if (!cJSON_IsString(p) || !p->valuestring)
        return vmsg("llm record missing endpoint_protocol");
    if (protocol_of(p->valuestring) < 0)
        return vmsg("unknown endpoint_protocol '%s'", p->valuestring);
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(t, "api_base");
    if (!cJSON_IsString(b) || !b->valuestring || !b->valuestring[0])
        return vmsg("llm record missing api_base");
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(t, "api_key");
    if (cJSON_IsString(k) && k->valuestring &&
        (strchr(k->valuestring, '\r') || strchr(k->valuestring, '\n')))
        return vmsg("api_key must not contain CR or LF");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(t, "headers");
    if (h && !cJSON_IsObject(h)) return vmsg("llm headers must be an object");
    if (h) {
        char *m = validate_headers(h);
        if (m) return m;
    }
    const cJSON *io = cJSON_GetObjectItemCaseSensitive(t, "inference_options");
    if (io) {
        char *m = validate_inference(io);
        if (m) return m;
    }
    return NULL;
}

static const char *const known_options[] = {
    "max_tool_rounds", "tool_call_timeout", "stream_interval",
    "llm_connect_timeout", "llm_read_timeout", "retain_context", NULL,
};

static const char *const known_inference[] = {
    "temperature", "top_p", "max_tokens", "stop", "top_k", "thinking_budget",
    "reasoning_effort", "presence_penalty", "frequency_penalty", "seed",
    "stream", NULL,
};

char *validate_inference(const cJSON *t) {
    if (!cJSON_IsObject(t)) return vmsg("inference_options must be an object");
    for (const cJSON *f = t->child; f; f = f->next) {
        bool known = false;
        for (int i = 0; known_inference[i]; i++)
            if (!strcmp(f->string, known_inference[i])) { known = true; break; }
        if (!known) return vmsg("unknown inference option '%s'", f->string);
    }
    const cJSON *stop = cJSON_GetObjectItemCaseSensitive(t, "stop");
    if (stop && !cJSON_IsArray(stop) && !cJSON_IsString(stop))
        return vmsg("stop must be a string or an array of strings");
    if (cJSON_IsArray(stop))
        for (const cJSON *s = stop->child; s; s = s->next)
            if (!cJSON_IsString(s))
                return vmsg("stop entries must be strings");
    return NULL;
}

char *validate_options(const cJSON *t) {
    if (!cJSON_IsObject(t)) return vmsg("options record must be an object");
    for (const cJSON *f = t->child; f; f = f->next) {
        if (f->string && !strcmp(f->string, "type")) continue; /* envelope */
        bool known = false;
        for (int i = 0; known_options[i]; i++)
            if (!strcmp(f->string, known_options[i])) { known = true; break; }
        if (!known) return vmsg("unknown option '%s'", f->string);
        if (!strcmp(f->string, "max_tool_rounds") && !cJSON_IsNumber(f))
            return vmsg("max_tool_rounds must be a number");
        if (!strcmp(f->string, "retain_context") && !cJSON_IsBool(f))
            return vmsg("retain_context must be a boolean");
        if (!strcmp(f->string, "tool_call_timeout") && !cJSON_IsNumber(f))
            return vmsg("tool_call_timeout must be a number");
        if (!strcmp(f->string, "stream_interval") && !cJSON_IsNumber(f))
            return vmsg("stream_interval must be a number");
        if (!strcmp(f->string, "llm_connect_timeout") && !cJSON_IsNumber(f))
            return vmsg("llm_connect_timeout must be a number");
        if (!strcmp(f->string, "llm_read_timeout") && !cJSON_IsNumber(f))
            return vmsg("llm_read_timeout must be a number");
    }
    return NULL;
}

char *validate_content(const cJSON *content) {
    if (!cJSON_IsArray(content)) return vmsg("content must be a list of blocks");
    int n = 0;
    for (const cJSON *b = content->child; b; b = b->next) {
        n++;
        if (!cJSON_IsObject(b)) return vmsg("content block must be an object");
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(b, "type");
        if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "text"))
            return vmsg("only text content blocks exist in this version");
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
        if (!cJSON_IsString(tx)) return vmsg("text block missing text field");
    }
    if (n == 0) return vmsg("content must have at least one block");
    return NULL;
}

char *validate_tools(const cJSON *t) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(t, "tools");
    if (!cJSON_IsArray(list)) return vmsg("tools record missing tools list");
    for (const cJSON *s = list->child; s; s = s->next) {
        if (!cJSON_IsObject(s)) return vmsg("server entry must be an object");
        const char *ty = rec_str(s, "type");
        if (!ty) return vmsg("server entry missing type");
        int kind;
        if (!strcmp(ty, "stdio")) kind = MCP_STDIO;
        else if (!strcmp(ty, "http")) kind = MCP_HTTP;
        else if (!strcmp(ty, "sse")) kind = MCP_SSE;
        else return vmsg("unknown server type '%s'", ty);
        if (!rec_str(s, "name")) return vmsg("server entry missing name");
        if (kind == MCP_STDIO && !rec_str(s, "command_line"))
            return vmsg("stdio server missing command_line");
        if (kind != MCP_STDIO && !rec_str(s, "url"))
            return vmsg("http/sse server missing url");
        const char *pr = rec_str(s, "protocol");
        if (pr) {
            bool ok = !strcmp(pr, "2024-11-05") || !strcmp(pr, "2025-03-26") ||
                      !strcmp(pr, "2025-06-18") || !strcmp(pr, "2025-11-25") ||
                      !strcmp(pr, "2026-07-28");
            if (!ok) return vmsg("unsupported mcp protocol '%s'", pr);
        }
        const cJSON *rq = cJSON_GetObjectItemCaseSensitive(s, "required");
        if (rq && !cJSON_IsBool(rq)) return vmsg("required must be a boolean");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(s, "headers");
        if (h && !cJSON_IsObject(h)) return vmsg("server headers must be an object");
        if (h) {
            char *m = validate_headers(h);
            if (m) return m;
        }
    }
    /* duplicate server names are fatal */
    for (const cJSON *a = list->child; a; a = a->next) {
        const char *an = rec_str(a, "name");
        for (const cJSON *b = a->next; b; b = b->next) {
            const char *bn = rec_str(b, "name");
            if (!strcmp(an, bn))
                return vmsg("duplicate server name '%s'", an);
        }
    }
    return NULL;
}

/* ---- byte-level pusher ---- */

void jsonl_pusher_init(jsonl_pusher_t *p, jsonl_line_fn fn, void *ctx) {
    memset(p, 0, sizeof *p);
    p->on_line = fn;
    p->ctx = ctx;
    buf_init(&p->hold);
}

void jsonl_pusher_free(jsonl_pusher_t *p) { buf_free(&p->hold); }

cJSON *jsonl_parse_line(const char *line) {
    /* a leading BOM is not stripped (requirements: it makes the line
       malformed json) - cJSON would skip it, so reject it here */
    static const unsigned char bom[3] = { 0xef, 0xbb, 0xbf };
    if (!strncmp(line, (const char *)bom, 3)) return NULL;
    /* require_null_terminated: trailing garbage on the line is malformed.
       The length must include the terminating NUL - cJSON rejects a buffer
       that ends exactly at the value with no byte left to test for '\0'. */
    cJSON *t = cJSON_ParseWithLengthOpts(line, strlen(line) + 1, NULL, 1);
    if (!t) return NULL;
    if (!cJSON_IsObject(t)) { cJSON_Delete(t); return NULL; }
    return t;
}

int jsonl_feed(jsonl_pusher_t *p, const char *bytes, size_t n) {
    /* byte rule 1: drop every 0x0d byte (covers \r\n and raw CR) */
    buf_t raw;
    buf_init(&raw);
    for (size_t i = 0; i < n; i++)
        if (bytes[i] != '\x0d') buf_append_byte(&raw, bytes[i]);
    /* strict UTF-8 on held + new bytes */
    buf_t all;
    buf_init(&all);
    if (p->hold.len) buf_append(&all, p->hold.data, p->hold.len);
    buf_append(&all, raw.data ? raw.data : "", raw.len);
    buf_free(&raw);
    if (!utf8_valid((const uint8_t *)all.data, all.len)) {
        buf_free(&all);
        return -1;
    }
    size_t start = 0;
    for (size_t i = 0; i < all.len; i++) {
        if (all.data[i] == '\n') {
            size_t ln = i - start;
            char *line = malloc(ln + 1);
            memcpy(line, all.data + start, ln);
            line[ln] = '\0';
            if (memchr(line, '\0', ln)) { /* embedded NUL: malformed */
                free(line);
                buf_free(&all);
                return -1;
            }
            p->on_line(p->ctx, line);
            start = i + 1;
        }
    }
    buf_clear(&p->hold);
    buf_append(&p->hold, all.data + start, all.len - start);
    buf_free(&all);
    return 0;
}

int jsonl_eof(jsonl_pusher_t *p) {
    if (p->done) return 0;
    p->done = true;
    if (!p->hold.len) return 0;
    if (!utf8_valid((const uint8_t *)p->hold.data, p->hold.len)) return -1;
    if (memchr(p->hold.data, '\0', p->hold.len)) return -1;
    char *line = buf_steal(&p->hold, NULL);
    p->on_line(p->ctx, line);
    return 0;
}

/* ---- output record builders (fixed field order) ---- */

cJSON *rec_error(const char *code, const char *message, bool fatal) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "error");
    cJSON_AddStringToObject(r, "code", code);
    cJSON_AddStringToObject(r, "message", message);
    cJSON_AddBoolToObject(r, "fatal", fatal);
    return r;
}

cJSON *rec_start(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "start");
    return r;
}

cJSON *rec_text(const char *type, const char *text, bool partial) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", type);
    cJSON_AddStringToObject(r, "text", text ? text : "");
    cJSON_AddBoolToObject(r, "partial", partial);
    return r;
}

cJSON *rec_thinking_final(const char *text, const char *signature) {
    cJSON *r = rec_text("thinking", text, false);
    cJSON_AddStringToObject(r, "signature", signature ? signature : "");
    return r;
}

cJSON *rec_response_final(const char *text) {
    return rec_text("response", text, false);
}

cJSON *rec_tool_request(const char *tool, const cJSON *arguments, const char *id) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "tool_request");
    cJSON_AddStringToObject(r, "tool", tool);
    cJSON_AddItemToObject(r, "arguments",
                          arguments ? cJSON_Duplicate(arguments, 1)
                                    : cJSON_CreateObject());
    cJSON_AddStringToObject(r, "id", id);
    return r;
}

cJSON *rec_tool_response(const char *id, const char *text, bool is_error) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "tool_response");
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddStringToObject(r, "text", text ? text : "");
    if (is_error) cJSON_AddBoolToObject(r, "is_error", true);
    return r;
}

void rec_attach_usage(cJSON *rec, double in, double out) {
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "input_tokens", in);
    cJSON_AddNumberToObject(u, "output_tokens", out);
    cJSON_AddItemToObject(rec, "usage", u);
}

void rec_attach_finish(cJSON *rec, const char *finish) {
    cJSON_AddStringToObject(rec, "finish_reason", finish);
}

/* ---- transcript ---- */

void trec_free(trec_t *r) {
    if (!r) return;
    cJSON_Delete(r->tree);
    free(r->text);
    free(r->signature);
    free(r->tool);
    free(r->id);
    cJSON_Delete(r->args);
    free(r);
}

void tlist_clear(tlist_t *l) {
    for (size_t i = 0; i < l->n; i++) trec_free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->cap = 0;
    l->n = 0;
}

static trec_t *tlist_push(tlist_t *l, uint8_t kind) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
        if (!l->v) { perror("realloc"); exit(1); }
    }
    trec_t *r = calloc(1, sizeof *r);
    r->kind = kind;
    l->v[l->n++] = r;
    return r;
}

/* open block tail of a given kind, or NULL */
static trec_t *tlist_open_block(tlist_t *l, uint8_t kind) {
    if (!l->n) return NULL;
    trec_t *t = l->v[l->n - 1];
    if ((t->kind == T_THINK || t->kind == T_TEXT) && t->kind == kind &&
        !t->complete)
        return t;
    return NULL;
}

static bool ingest_textlike(tlist_t *l, uint8_t kind, const cJSON *tree,
                            const char *text, bool partial, const char *signature) {
    trec_t *t = tlist_open_block(l, kind);
    if (!t) {
        t = tlist_push(l, kind);
        t->text = strdup("");
    }
    if (text) {
        size_t ol = strlen(t->text), nl = strlen(text);
        t->text = realloc(t->text, ol + nl + 1);
        memcpy(t->text + ol, text, nl);
        t->text[ol + nl] = '\0';
    }
    if (!partial) {
        t->complete = true;
        if (kind == T_THINK) {
            free(t->signature);
            t->signature = strdup(signature ? signature : "");
        }
    }
    (void)tree;
    return true;
}

/* tlist_ingest duplicates what the transcript needs; the caller keeps
   ownership of record. false when the record is not a transcript record. */
bool tlist_ingest(tlist_t *l, cJSON *record) {
    int k = rec_classify(record);
    switch (k) {
    case R_USER: {
        trec_t *t = tlist_push(l, T_USER);
        t->tree = cJSON_Duplicate(record, 1);
        t->complete = true;
        return true;
    }
    case R_THINKING:
        return ingest_textlike(l, T_THINK, record, rec_str(record, "text"),
                               rec_bool(record, "partial", true),
                               rec_str(record, "signature"));
    case R_RESPONSE:
        return ingest_textlike(l, T_TEXT, record, rec_str(record, "text"),
                               rec_bool(record, "partial", true), NULL);
    case R_TOOL_REQUEST: {
        trec_t *t = tlist_push(l, T_TREQ);
        t->tree = cJSON_Duplicate(record, 1);
        t->tool = strdup(rec_str(record, "tool") ? rec_str(record, "tool") : "");
        t->id = strdup(rec_str(record, "id") ? rec_str(record, "id") : "");
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(record, "arguments");
        t->args = cJSON_IsObject(a) ? cJSON_Duplicate(a, 1)
                                    : cJSON_CreateObject();
        t->complete = true;
        return true;
    }
    case R_TOOL_RESPONSE: {
        trec_t *t = tlist_push(l, T_TRESP);
        t->tree = cJSON_Duplicate(record, 1);
        t->id = strdup(rec_str(record, "id") ? rec_str(record, "id") : "");
        t->text = strdup(rec_str(record, "text") ? rec_str(record, "text") : "");
        t->is_error = rec_bool(record, "is_error", false);
        t->complete = true;
        return true;
    }
    default:
        return false;
    }
}

/* ---- grouping ---- */

void group_begin(group_iter_t *g, const tlist_t *l) {
    memset(g, 0, sizeof *g);
    g->l = l;
}

int group_next(group_iter_t *g) {
    const tlist_t *l = g->l;
    if (g->i >= l->n) return G_DONE;
    size_t i = g->i;
    if (l->v[i]->kind == T_USER) {
        g->user = l->v[i];
        g->start = i;
        g->stop = i + 1;
        g->i = i + 1;
        return G_USER;
    }
    /* assistant run */
    size_t end = i;
    while (end < l->n && l->v[end]->kind != T_USER &&
           l->v[end]->kind != T_TRESP)
        end++;
    size_t rend = end;
    while (rend < l->n && l->v[rend]->kind == T_TRESP) rend++;
    g->begin = i;
    g->end = end;
    g->rbegin = end;
    g->rend = rend;
    g->start = i;
    g->stop = rend;
    g->i = rend;
    /* complete: a tool turn (answered) or a run whose last block closed */
    bool has_req = false;
    for (size_t k = i; k < end; k++)
        if (l->v[k]->kind == T_TREQ) has_req = true;
    if (has_req) g->complete = (rend > end);
    else {
        g->complete = false;
        if (end > i) {
            const trec_t *last = l->v[end - 1];
            if ((last->kind == T_TEXT || last->kind == T_THINK) && last->complete)
                g->complete = true;
        }
    }
    return G_ASSIST;
}

bool user_text_join(const trec_t *u, buf_t *out) {
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(u->tree, "content");
    if (!cJSON_IsArray(content)) return false;
    bool any = false;
    for (const cJSON *b = content->child; b; b = b->next) {
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
        if (!cJSON_IsString(tx)) continue;
        if (any) buf_append_byte(out, '\n');
        buf_append_str(out, tx->valuestring);
        any = true;
    }
    return any;
}

/* ---- stdout sink ---- */

void jsonl_stdout_sink(void *ctx, cJSON *rec) {
    (void)ctx;
    char *p = cJSON_PrintUnformatted(rec);
    if (!p) return;
    fwrite(p, 1, strlen(p), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (ferror(stdout)) {
        /* out of channel: exit without emitting records */
        cJSON_free(p);
        cJSON_Delete(rec);
        exit(EXIT_OUT_OF_CHANNEL);
    }
    cJSON_free(p);
    cJSON_Delete(rec);
}
