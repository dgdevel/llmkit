/* selfcheck.c - plain asserts, no framework (design sec.14).
   Categories: prefix invariant, sse parser, stream parity, validation,
   state machine (flush/steering/drop rule/max rounds/sigint), mcp client
   (fake child servers), mcp proxy, call (argv compilation, scripted runs). */
#include "llmkit.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ================= harness ================= */

static int checks = 0, failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", name);
    }
}

static void check_str(const char *got, const char *want, const char *name) {
    checks++;
    if (!got || !want || strcmp(got, want)) {
        failures++;
        fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", name,
                got ? got : "(null)", want ? want : "(null)");
    }
}

/* capture sink */
typedef struct cap {
    char **v;
    size_t n, cap;
} cap_t;

static void cap_fn(void *ctx, cJSON *rec) {
    cap_t *c = ctx;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = realloc(c->v, c->cap * sizeof *c->v);
    }
    c->v[c->n++] = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
}

static void cap_reset(cap_t *c) {
    for (size_t i = 0; i < c->n; i++) free(c->v[i]);
    c->n = 0;
}

static void cap_destroy(cap_t *c) {
    cap_reset(c);
    free(c->v);
    c->v = NULL;
    c->cap = 0;
}

static const char *cap_field(cap_t *c, size_t i, const char *f) {
    cJSON *r = cJSON_Parse(c->v[i]);
    if (!r) return NULL;
    static char out[2048];
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(r, f);
    if (cJSON_IsString(x)) snprintf(out, sizeof out, "%s", x->valuestring);
    else if (cJSON_IsBool(x)) snprintf(out, sizeof out, "%s",
                                       cJSON_IsTrue(x) ? "true" : "false");
    else if (cJSON_IsNumber(x)) snprintf(out, sizeof out, "%.17g",
                                         x->valuedouble);
    else snprintf(out, sizeof out, "?");
    cJSON_Delete(r);
    return out;
}

/* line accumulator for pusher tests */
typedef struct line_acc {
    int n;
    char *lines[8];
} line_acc_t;

static void line_acc_on_line(void *ctx, char *line) {
    line_acc_t *b = ctx;
    if (b->n < 8) b->lines[b->n++] = line;
    else free(line);
}

/* ================= fake wire ================= */

typedef struct fturn {
    const char **recs;
    int nrecs;
    int kind; /* TURN_FINAL / TURN_TOOLS / TURN_FATAL / TURN_ABORTED */
    const char *fatal_json;
    int abort_after; /* >=0: emit recs[0..abort_after), set stop flag, ABORT */
} fturn_t;

typedef struct fwire {
    wire_t base;
    fturn_t *turns;
    int nturns, pos;
} fwire_t;

static int fwire_turn(wire_t *base, engine_t *e, turn_out_t *out) {
    fwire_t *f = (fwire_t *)base;
    memset(out, 0, sizeof *out);
    fturn_t *t = &f->turns[f->pos < f->nturns ? f->pos++ : f->nturns - 1];
    if (t->kind == TURN_FATAL) {
        out->error_rec = cJSON_Parse(t->fatal_json);
        return TURN_FATAL;
    }
    for (int i = 0; i < t->nrecs; i++) {
        if (t->abort_after >= 0 && i == t->abort_after) {
            g_stop_flag = 1; /* SIGINT mid-stream */
            return TURN_ABORTED;
        }
        cJSON *r = cJSON_Parse(t->recs[i]);
        bool last = (i == t->nrecs - 1);
        /* the last response record of a final turn is the held final */
        if (last && t->kind == TURN_FINAL && rec_classify(r) == R_RESPONSE &&
            !rec_bool(r, "partial", true)) {
            out->final_rec = r;
        } else {
            engine_emit_record(e, r);
        }
    }
    if (t->abort_after >= 0) { /* abort_after == nrecs: abort after the last */
        g_stop_flag = 1;
        return TURN_ABORTED;
    }
    if (t->kind == TURN_ABORTED) {
        g_stop_flag = 1;
        return TURN_ABORTED;
    }
    return t->kind;
}

static int fwire_build(wire_t *base, engine_t *e) {
    (void)base;
    (void)e;
    return 0;
}

static void fwire_destroy(wire_t *base) { free(base); }

static wire_t *fwire_new(fturn_t *turns, int n) {
    fwire_t *f = calloc(1, sizeof *f);
    f->base.turn = fwire_turn;
    f->base.build = fwire_build;
    f->base.destroy = fwire_destroy;
    buf_init(&f->base.last_body);
    f->turns = turns;
    f->nturns = n;
    return &f->base;
}

/* ================= fake tool exec ================= */

typedef struct ftool {
    const char *tool;
    const char *text;
    int rc; /* 0 ok, 1 failed, 2 timeout */
    bool stop_after; /* set the stop flag after this call (SIGINT mid tool) */
} ftool_t;

typedef struct ftool_script {
    ftool_t *v;
    int n, pos;
} ftool_script_t;

ftool_script_t g_ftools;

static int ftool_exec(engine_t *e, const char *tool, cJSON *args,
                      buf_t *text_out, bool *is_error, char *err, size_t errsz) {
    (void)e;
    (void)args;
    (void)is_error;
    ftool_t *t =
        &g_ftools.v[g_ftools.pos < g_ftools.n ? g_ftools.pos++ : g_ftools.n - 1];
    check(t->tool && !strcmp(t->tool, tool), "tool executed in order");
    if (t->text) buf_append_str(text_out, t->text);
    if (t->rc == 1) snprintf(err, errsz, "tool failed: %s", tool);
    if (t->rc == 2) snprintf(err, errsz, "tool timed out: %s", tool);
    int rc = t->rc;
    if (t->stop_after) g_stop_flag = 1;
    return rc;
}

/* ================= scripted engine driver ================= */

/* ================= ================= ================= */

/* helper: extract the message array ("messages":[...] / "input":[...]) */
static void body_messages(const char *body, buf_t *out) {
    const char *m = strstr(body, "\"messages\":[");
    const char *iname = strstr(body, "\"input\":[");
    const char *p = m ? m + strlen("\"messages\":[") - 1
                      : iname ? iname + strlen("\"input\":[") - 1 : NULL;
    check(p != NULL, "body has a message array");
    if (!p) return;
    /* bracket count from '[' */
    int depth = 0;
    const char *q = p;
    bool in_str = false, esc = false;
    for (; *q; q++) {
        if (esc) { esc = false; continue; }
        if (*q == '\\') { esc = true; continue; }
        if (*q == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (*q == '[' || *q == '{') depth++;
        else if (*q == ']' || *q == '}') {
            depth--;
            if (depth == 0) {
                /* contents only, without the closing bracket: turn N's array
                   grows by appending, so turn N-1's contents must be a
                    byte-prefix of turn N's */
                buf_append(out, p, (size_t)(q - p));
                return;
            }
        }
    }
}


/* ingest a transcript record: tlist_ingest duplicates, the caller owns the
   parsed tree - free it here */
static void tr_add(engine_t *e, const char *json) {
    cJSON *t = cJSON_Parse(json);
    tlist_ingest(&e->tr, t);
    cJSON_Delete(t);
}

static int feed_line(engine_t *e, const char *json) {
    cJSON *t = cJSON_Parse(json);
    return engine_input_record(e, t);
}

/* ================= 1. prefix invariant ================= */

static void add_tool(engine_t *e, const char *name, const char *desc) {
    mcp_mgr_t *m = (mcp_mgr_t *)e->mcp;
    tool_listing_t *tl = &m->listing;
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 8;
        tl->v = realloc(tl->v, tl->cap * sizeof *tl->v);
    }
    char tool[512];
    snprintf(tool, sizeof tool,
             "{\"name\":\"%s\",\"description\":\"%s\","
             "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
             "\"x\":{\"type\":\"string\"}},\"required\":[\"x\"]}}",
             name, desc);
    tl->v[tl->n].tool = cJSON_Parse(tool);
    tl->v[tl->n].srv = NULL;
    tl->v[tl->n].exposed_name = strdup(name);
    tl->n++;
}

static void test_prefix_invariant(void) {
    /* openai: continuation, sampling-only change, tools change, system
       change, llm switch */
    cap_t cap = { 0 };
    engine_t *e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"http://x/v1\",\"model\":\"m\","
                 "\"inference_options\":{\"stream\":false,\"temperature\":0.5}}");
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"be brief\"}]}");
    add_tool(e, "fs.list", "list files");

    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}");
    wire_t *w = wire_openai_new(PROTO_OPENAI);
    check(w->build(w, e) == 0, "openai build 1");
    buf_t p1;
    buf_init(&p1);
    body_messages(w->last_body.data, &p1);

    /* turn 2: response + steering user */
    tr_add(e, "{\"type\":\"response\",\"text\":\"hello\",\"partial\":false}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"more\"}]}");
    w->build(w, e);
    buf_t p2;
    buf_init(&p2);
    body_messages(w->last_body.data, &p2);
    check(p2.len > p1.len && memcmp(p1.data, p2.data, p1.len) == 0,
          "openai: prefix of turn 2 == turn 1 request");

    /* sampling-only change leaves the prefix untouched */
    cJSON *io = cJSON_GetObjectItemCaseSensitive(e->llm, "inference_options");
    cJSON_ReplaceItemInObjectCaseSensitive(io, "temperature",
                                           cJSON_CreateNumber(0.9));
    w->build(w, e);
    buf_t p3;
    buf_init(&p3);
    body_messages(w->last_body.data, &p3);
    check(p3.len == p2.len && memcmp(p3.data, p2.data, p3.len) == 0,
          "openai: sampling-only change keeps prefix bytes");

    /* tools change: envelope only */
    add_tool(e, "fs.read", "read a file");
    w->build(w, e);
    buf_t p4;
    buf_init(&p4);
    body_messages(w->last_body.data, &p4);
    check(p4.len == p3.len && memcmp(p4.data, p3.data, p4.len) == 0,
          "openai: tools change keeps message bytes");

    /* tool round: assistant tool_calls + tool response */
    tr_add(e, "{\"type\":\"tool_request\",\"tool\":\"fs.list\",\"arguments\":"
        "{\"x\":\"a\"},\"id\":\"c1\"}");
    tr_add(e, "{\"type\":\"tool_response\",\"id\":\"c1\",\"text\":\"r\"}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"go\"}]}");
    w->build(w, e);
    buf_t p5;
    buf_init(&p5);
    body_messages(w->last_body.data, &p5);
    check(p5.len > p4.len && memcmp(p4.data, p5.data, p4.len) == 0,
          "openai: tool turn appends to prefix");
    check(strstr(p5.data, "\"tool_calls\"") != NULL, "openai: tool_calls present");
    check(strstr(p5.data, "\"tool_call_id\":\"c1\"") != NULL,
          "openai: tool response message present");

    /* system change under openai rewrites messages[0] but keeps the tail */
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"new sys\"}]}");
    w->build(w, e);
    buf_t p6;
    buf_init(&p6);
    body_messages(w->last_body.data, &p6);
    check(strstr(p6.data, "new sys") != NULL && strstr(p6.data, "be brief") == NULL,
          "openai: system change rewrites messages[0]");
    const char *second_msg = strstr(p5.data, ",{\"role\":");
    check(second_msg != NULL, "openai: second message found");
    if (second_msg) {
        size_t tail = p5.len - (size_t)(second_msg - p5.data);
        check(p6.len - tail >= 1 &&
                  memcmp(p6.data + (p6.len - tail), second_msg, tail) == 0,
              "openai: system change keeps the tail after messages[0]");
    }

    /* llm switch, same protocol, no thinking: prefix identical */
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"http://y/v1\",\"model\":\"m2\","
                 "\"inference_options\":{\"stream\":false}}");
    w->build(w, e);
    buf_t p7;
    buf_init(&p7);
    body_messages(w->last_body.data, &p7);
    check(p7.len == p6.len && memcmp(p7.data, p6.data, p6.len) == 0,
          "openai: llm switch without thinking keeps prefix");

    w->destroy(w);
    buf_free(&p1); buf_free(&p2); buf_free(&p3); buf_free(&p4);
    buf_free(&p5); buf_free(&p6); buf_free(&p7);
    engine_free(e);
    cap_reset(&cap);

    /* ---- openai_responses ---- */
    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai_responses\","
                 "\"api_base\":\"http://x/v1\",\"inference_options\":"
                 "{\"stream\":false}}");
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"s\"}]}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q1\"}]}");
    w = wire_openai_new(PROTO_RESPONSES);
    w->build(w, e);
    buf_t r1;
    buf_init(&r1);
    body_messages(w->last_body.data, &r1);
    check(strstr(w->last_body.data, "\"instructions\":\"s\"") != NULL,
          "responses: system maps to instructions");
    check(strstr(w->last_body.data, "\"store\":false") != NULL,
          "responses: store:false constant");
    check(strstr(w->last_body.data,
                 "\"include\":[\"reasoning.encrypted_content\"]") != NULL,
          "responses: include constant");

    tr_add(e, "{\"type\":\"thinking\",\"text\":\"hmm\",\"partial\":false,"
        "\"signature\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"x\\\":1}\"}");
    tr_add(e, "{\"type\":\"tool_request\",\"tool\":\"fs.list\",\"arguments\":"
        "{\"x\":\"a\"},\"id\":\"c1\"}");
    tr_add(e, "{\"type\":\"tool_response\",\"id\":\"c1\",\"text\":\"ok\"}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q2\"}]}");
    w->build(w, e);
    buf_t r2;
    buf_init(&r2);
    body_messages(w->last_body.data, &r2);
    check(strstr(r2.data, "\"type\":\"function_call\",\"call_id\":\"c1\"") != NULL,
          "responses: function_call item present");
    check(strstr(r2.data, "\"function_call_output\",\"call_id\":\"c1\"") != NULL,
          "responses: function_call_output item present");
    check(strstr(r2.data, "\"type\":\"reasoning\",\"x\":1") != NULL,
          "responses: signed reasoning resent verbatim");
    check(r2.len > r1.len && memcmp(r1.data, r2.data, r1.len) == 0,
          "responses: prefix of turn 2 == turn 1");

    /* system change: instructions envelope only, input prefix untouched */
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"s2\"}]}");
    w->build(w, e);
    buf_t r3;
    buf_init(&r3);
    body_messages(w->last_body.data, &r3);
    check(r3.len == r2.len && memcmp(r3.data, r2.data, r3.len) == 0,
          "responses: system change keeps input prefix");
    check(strstr(w->last_body.data, "\"instructions\":\"s2\"") != NULL,
          "responses: system change reaches the instructions envelope");

    /* crafted signature: the raw-serialized reasoning item must be
       re-parsed, so a replayed transcript cannot inject json into the
       request body (the injected field is dropped, body stays valid) */
    tr_add(e, "{\"type\":\"thinking\",\"text\":\"evil\",\"partial\":false,"
        "\"signature\":\"{\\\"type\\\":\\\"reasoning\\\"},\\\"INJECTED\\\":"
        "true,\\\"x\\\":{\\\"y\\\":1\"}");
    tr_add(e, "{\"type\":\"tool_request\",\"tool\":\"fs.list\",\"arguments\":"
        "{\"x\":\"b\"},\"id\":\"c2\"}");
    tr_add(e, "{\"type\":\"tool_response\",\"id\":\"c2\",\"text\":\"ok\"}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q3\"}]}");
    w->build(w, e);
    {
        cJSON *whole = cJSON_Parse(w->last_body.data);
        check(whole != NULL,
              "responses: body stays valid json with a crafted signature");
        cJSON_Delete(whole);
    }
    check(strstr(w->last_body.data, "INJECTED") == NULL,
          "responses: crafted signature cannot inject raw json");

    w->destroy(w);
    buf_free(&r1); buf_free(&r2); buf_free(&r3);
    engine_free(e);
    cap_reset(&cap);

    /* ---- anthropic ---- */
    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"anthropic\","
                 "\"api_base\":\"http://x\",\"model\":\"claude\","
                 "\"inference_options\":{\"stream\":false,\"max_tokens\":100}}");
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"s\"}]}");
    add_tool(e, "fs.list", "list files");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q1\"},"
        "{\"type\":\"text\",\"text\":\"q1b\"}]}");
    wire_t *aw = wire_anthropic_new();
    check(aw->build(aw, e) == 0, "anthropic build ok");
    check(strstr(aw->last_body.data, "\"max_tokens\":100") != NULL,
          "anthropic: max_tokens present");
    check(strstr(aw->last_body.data, "\"type\":\"text\",\"text\":\"q1\"},{"
              "\"type\":\"text\",\"text\":\"q1b\"}") != NULL,
          "anthropic: native multi-block user content");
    /* static marker: last tool definition carries cache_control */
    const char *cm = strstr(aw->last_body.data, "cache_control");
    check(cm != NULL, "anthropic: static cache marker present");

    buf_t a1;
    buf_init(&a1);
    body_messages(aw->last_body.data, &a1);

    /* signed thinking + tool turn */
    tr_add(e, "{\"type\":\"thinking\",\"text\":\"th\",\"partial\":false,"
        "\"signature\":\"sig1\"}");
    tr_add(e, "{\"type\":\"response\",\"text\":\"txt\",\"partial\":false}");
    tr_add(e, "{\"type\":\"tool_request\",\"tool\":\"fs.list\",\"arguments\":"
        "{\"x\":\"a\"},\"id\":\"c1\"}");
    tr_add(e, "{\"type\":\"tool_response\",\"id\":\"c1\",\"text\":\"ok\"}");
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q2\"}]}");
    aw->build(aw, e);
    buf_t a2;
    buf_init(&a2);
    body_messages(aw->last_body.data, &a2);
    check(strstr(a2.data, "\"type\":\"thinking\",\"thinking\":\"th\","
              "\"signature\":\"sig1\"") != NULL,
          "anthropic: signed thinking resent for tool turn");
    check(strstr(a2.data, "\"type\":\"tool_result\",\"tool_use_id\":\"c1\"") !=
              NULL,
          "anthropic: tool_result message");
    check(a2.len > a1.len && memcmp(a1.data, a2.data, a1.len) == 0,
          "anthropic: prefix of turn 2 == turn 1");
    check(strstr(a2.data, "cache_control") != NULL,
          "anthropic: turn marker placed");

    /* system change: envelope only under anthropic */
    feed_line(e, "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                 "\"text\":\"s2\"}]}");
    aw->build(aw, e);
    buf_t a3;
    buf_init(&a3);
    body_messages(aw->last_body.data, &a3);
    check(a3.len == a2.len && memcmp(a3.data, a2.data, a3.len) == 0,
          "anthropic: system change keeps message prefix");

    /* unsigned thinking + tool turn: tool records omitted */
    engine_t *e2 = engine_new(cap_fn, &cap);
    e2->llm = cJSON_Duplicate(e->llm, 1);
    cJSON *io2 = cJSON_GetObjectItemCaseSensitive(e2->llm, "inference_options");
    cJSON_AddNumberToObject(io2, "thinking_budget", 50);
    e2->protocol = PROTO_ANTHROPIC;
    tr_add(e2, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    tr_add(e2, "{\"type\":\"thinking\",\"text\":\"th\",\"partial\":false}");
    tr_add(e2, "{\"type\":\"tool_request\",\"tool\":\"fs.list\",\"arguments\":"
        "{\"x\":\"a\"},\"id\":\"c1\"}");
    tr_add(e2, "{\"type\":\"tool_response\",\"id\":\"c1\",\"text\":\"ok\"}");
    tr_add(e2, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q2\"}]}");
    add_tool(e2, "fs.list", "list");
    wire_t *aw2 = wire_anthropic_new();
    check(aw2->build(aw2, e2) == 0, "anthropic: thinking_budget < max_tokens ok");
    check(strstr(aw2->last_body.data, "tool_use") == NULL,
          "anthropic: unsigned thinking omits tool records");
    check(strstr(aw2->last_body.data, "tool_result") == NULL,
          "anthropic: tool responses omitted with them");
    check(strstr(aw2->last_body.data, "\"thinking\":{") != NULL,
          "anthropic: thinking param sent");
    aw2->destroy(aw2);
    engine_free(e2);

    aw->destroy(aw);
    buf_free(&a1); buf_free(&a2); buf_free(&a3);
    engine_free(e);
    cap_destroy(&cap);
}

/* ================= 2. sse parser ================= */

static char sse_events[16][64];
static char sse_datas[16][512];
static int sse_count;

static void sse_test_cb(void *ctx, const char *ev, const char *data, size_t n) {
    (void)ctx;
    if (sse_count >= 16) return;
    snprintf(sse_events[sse_count], 64, "%s", ev);
    if (n >= 512) n = 511;
    memcpy(sse_datas[sse_count], data, n);
    sse_datas[sse_count][n] = '\0';
    sse_count++;
}

static void test_sse_parser(void) {
    /* basic + multi-line data + comment + split at every byte boundary */
    const char *doc =
        ": ping comment\r\n"
        "event: response.output_text.delta\r\n"
        "data: {\"delta\":\"he\n"
        "data: llo\"}\r\n"
        "\r\n"
        "event: ping\n"
        "data: {}\n"
        "\n\n"
        "data: [DONE]\n"
        "\n";
    sse_count = 0;
    sse_parser_t p;
    sse_init(&p, sse_test_cb, NULL);
    sse_feed(&p, doc, strlen(doc));
    sse_eof(&p);
    sse_free(&p);
    check(sse_count == 3, "sse: three events");
    check_str(sse_events[0], "response.output_text.delta", "sse: event name");
    check_str(sse_datas[0], "{\"delta\":\"he\nllo\"}", "sse: joined data lines");
    check_str(sse_events[1], "ping", "sse: ping event");
    check_str(sse_events[2], "message", "sse: default event name");
    check_str(sse_datas[2], "[DONE]", "sse: DONE data");

    /* chunk-split invariance: feed one byte at a time */
    sse_count = 0;
    sse_init(&p, sse_test_cb, NULL);
    for (const char *q = doc; *q; q++) sse_feed(&p, q, 1);
    sse_eof(&p);
    sse_free(&p);
    check(sse_count == 3, "sse: same events split per byte");
    check_str(sse_datas[0], "{\"delta\":\"he\nllo\"}", "sse: split data intact");

    /* awkward split: CR/LF split across chunks */
    sse_count = 0;
    sse_init(&p, sse_test_cb, NULL);
    sse_feed(&p, "event: x\r", 9);
    sse_feed(&p, "\ndata: 1\r", 9);
    sse_feed(&p, "\n\r", 2);
    sse_feed(&p, "\n", 1);
    sse_eof(&p);
    sse_free(&p);
    check(sse_count == 1, "sse: crlf split dispatches once");
    check_str(sse_datas[0], "1", "sse: crlf split data");

    /* unterminated trailing event dispatches at eof */
    sse_count = 0;
    sse_init(&p, sse_test_cb, NULL);
    sse_feed(&p, "data: tail", 10);
    sse_eof(&p);
    sse_free(&p);
    check(sse_count == 1 && !strcmp(sse_datas[0], "tail"),
          "sse: trailing event dispatched at eof");
}

/* ================= 3. validation ================= */

static void test_validation(void) {
    cap_t cap = { 0 };

    /* malformed json is rejected by the parse helper */
    engine_t *e = engine_new(cap_fn, &cap);
    check(jsonl_parse_line("{\"type\":\"user\"") == NULL, "malformed json rejected");
    check(jsonl_parse_line("[1,2]") == NULL, "non-object json rejected");
    check(jsonl_parse_line("{} {}") == NULL, "trailing garbage rejected");
    engine_free(e);
    cap_reset(&cap);

    /* BOM: first line malformed json (invalid_record via the pusher) */
    {
        jsonl_pusher_t pp;
        line_acc_t bx = { 0 };
        jsonl_pusher_init(&pp, line_acc_on_line, &bx);
        int rc = jsonl_feed(&pp,
                            "\xef\xbb\xbf{\"type\":\"header\",\"version\":1}\n",
                            strlen("\xef\xbb\xbf{\"type\":\"header\","
                                   "\"version\":1}\n"));
        check(rc == 0, "BOM passes byte rules");
        cJSON *bl = bx.n == 1 ? jsonl_parse_line(bx.lines[0]) : NULL;
        check(bx.n == 1 && bl == NULL,
              "BOM makes the first line malformed json");
        cJSON_Delete(bl);
        jsonl_pusher_free(&pp);
        for (int i = 0; i < bx.n; i++) free(bx.lines[i]);
    }

    /* raw CR dropping */
    {
        jsonl_pusher_t pp;
        line_acc_t bx = { 0 };
        jsonl_pusher_init(&pp, line_acc_on_line, &bx);
        jsonl_feed(&pp, "{\"type\":\"user\",\"a\":\"x\ry\"}\r\n",
                    strlen("{\"type\":\"user\",\"a\":\"x\ry\"}\r\n"));
        cJSON *pl = bx.n == 1 ? jsonl_parse_line(bx.lines[0]) : NULL;
        check(bx.n == 1 && pl != NULL,
              "raw CR dropped, line parses");
        cJSON_Delete(pl);
        jsonl_pusher_free(&pp);
        for (int i = 0; i < bx.n; i++) free(bx.lines[i]);
    }

    /* invalid UTF-8 */
    {
        jsonl_pusher_t pp;
        jsonl_pusher_init(&pp, (void (*)(void *, char *))0, NULL);
        check(jsonl_feed(&pp, "\"a\xc3(\"", 5) == -1, "invalid utf-8 rejected");
        jsonl_pusher_free(&pp);
    }

    /* missing llm at start; missing user; duplicate server name; unknown
       type; bare flush; bad options; bad inference */
    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}");
    cap_reset(&cap);
    e->emit_ctx = &cap;
    int rc = engine_start(e);
    check(rc == EXIT_INVALID_RECORD, "missing llm is fatal");
    check(cap.n == 1 && !strcmp(cap_field(&cap, 0, "code"), "invalid_record"),
          "missing llm emits invalid_record");
    cap_reset(&cap);
    engine_free(e);

    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    rc = engine_start(e);
    check(rc == EXIT_INVALID_RECORD, "missing user is fatal");
    cap_reset(&cap);
    engine_free(e);

    e = engine_new(cap_fn, &cap);
    rc = feed_line(e, "{\"type\":\"tools\",\"tools\":["
                      "{\"type\":\"stdio\",\"name\":\"a\",\"command_line\":\"x\"},"
                      "{\"type\":\"stdio\",\"name\":\"a\",\"command_line\":\"y\"}]}");
    check(rc == 2, "duplicate server name is fatal");
    check(cap.n == 1 && !strcmp(cap_field(&cap, 0, "fatal"), "true"),
          "duplicate server fatal record");
    cap_reset(&cap);
    engine_free(e);

    e = engine_new(cap_fn, &cap);
    rc = feed_line(e, "{\"type\":\"agent-as-tool\",\"tool_description\":\"x\"}");
    check(rc == 2, "agent-as-tool is an unknown type for the runner");
    cap_reset(&cap);
    rc = feed_line(e, "{\"type\":\"totally-new\"}");
    check(rc == 2, "unknown type fatal");
    cap_reset(&cap);
    rc = feed_line(e, "{\"type\":\"options\",\"weird\":1}");
    check(rc == 2, "unknown option name fatal");
    cap_reset(&cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\","
                 "\"inference_options\":{\"temp\":1}}");
    check(cap.n == 1, "unknown inference option fatal");
    cap_reset(&cap);
    engine_free(e);

    /* bare flush: first flush attempts the start (fatal: no llm); a second
       flush with no records is a non-fatal no-op */
    e = engine_new(cap_fn, &cap);
    rc = feed_line(e, "{\"type\":\"flush\"}");
    check(rc == 1, "first flush attempts the start");
    check(cap.n == 0, "no records emitted by the flush itself yet");
    rc = engine_start(e);
    check(rc == EXIT_INVALID_RECORD, "first flush with no records fails fatally");
    cap_reset(&cap);
    e->ever_flushed = true;
    rc = feed_line(e, "{\"type\":\"flush\"}");
    check(rc == 0, "bare flush is a no-op");
    check(cap.n == 1 && !strcmp(cap_field(&cap, 0, "code"), "invalid_record") &&
              !strcmp(cap_field(&cap, 0, "fatal"), "false"),
          "bare flush emits non-fatal invalid_record");
    cap_reset(&cap);
    engine_free(e);

    /* anthropic validations: missing max_tokens; thinking_budget >= max */
    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"anthropic\",\"api_base\":\"http://x\","
                 "\"inference_options\":{\"stream\":false}}");
    e->protocol = PROTO_ANTHROPIC;
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    wire_t *aw = wire_anthropic_new();
    check(aw->build(aw, e) == EXIT_INVALID_RECORD,
          "anthropic missing max_tokens is invalid_record");
    cap_reset(&cap);
    cJSON *io = cJSON_GetObjectItemCaseSensitive(e->llm, "inference_options");
    cJSON_AddNumberToObject(io, "max_tokens", 100);
    cJSON_AddNumberToObject(io, "thinking_budget", 100);
    check(aw->build(aw, e) == EXIT_INVALID_RECORD,
          "thinking_budget >= max_tokens is invalid_record");
    cJSON_ReplaceItemInObjectCaseSensitive(io, "thinking_budget",
                                           cJSON_CreateNumber(10));
    check(aw->build(aw, e) == 0, "thinking_budget < max_tokens ok");
    aw->destroy(aw);
    engine_free(e);
    cap_reset(&cap);

    /* anthropic first-message-user */
    e = engine_new(cap_fn, &cap);
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"anthropic\",\"api_base\":\"http://x\","
                 "\"inference_options\":{\"stream\":false,\"max_tokens\":10}}");
    e->protocol = PROTO_ANTHROPIC;
    tr_add(e, "{\"type\":\"response\",\"text\":\"first\",\"partial\":false}");
    aw = wire_anthropic_new();
    check(aw->build(aw, e) == EXIT_INVALID_RECORD,
          "anthropic rejects a non-user first message");
    aw->destroy(aw);
    engine_free(e);
    cap_reset(&cap);

    /* CR/LF in api_key or header values is header injection: fatal */
    e = engine_new(cap_fn, &cap);
    rc = feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"http://x\",\"api_key\":\"k\\r\\nX-Evil: 1\"}");
    check(rc == 2, "api_key with CRLF is fatal");
    cap_reset(&cap);
    rc = feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"http://x\",\"headers\":{\"X-A\":\"v\\nw\"}}");
    check(rc == 2, "header value with LF is fatal");
    cap_reset(&cap);
    rc = feed_line(e, "{\"type\":\"tools\",\"tools\":[{\"type\":\"http\","
                 "\"name\":\"h\",\"url\":\"http://x\",\"headers\":"
                 "{\"X-B\":\"a\\rb\"}}]}");
    check(rc == 2, "server header value with CR is fatal");
    cap_reset(&cap);
    engine_free(e);

    /* an over-long tool server prefix is rejected, not truncated */
    {
        mcp_mgr_t *mm = mcp_mgr_new();
        buf_t ltool;
        buf_init(&ltool);
        for (int i = 0; i < 300; i++) buf_append_byte(&ltool, 'a');
        buf_append_str(&ltool, ".t");
        char lerr[256] = "";
        buf_t lout;
        buf_init(&lout);
        bool liserr = false;
        int lrc = mcp_call(mm, ltool.data, NULL, &lout, &liserr, lerr,
                           sizeof lerr, -1);
        check(lrc == 1 && strstr(lerr, "too long") != NULL,
              "mcp_call rejects an over-long server prefix");
        buf_free(&ltool);
        buf_free(&lout);
        mcp_mgr_free(mm);
    }
    cap_destroy(&cap);
}

/* ================= 4. state machine (fake endpoint) ================= */

static wire_t *g_factory_wire;

static wire_t *script_factory(engine_t *e) {
    (void)e;
    return g_factory_wire;
}

static void test_state_machine(void) {
    cap_t cap = { 0 };

    /* flush consumption + start marker order: response, start, next turn */
    fturn_t turns[] = {
        { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"a\",\"partial\":false}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"b\",\"partial\":false}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
    };
    g_factory_wire = fwire_new(turns, 2);
    engine_t *e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    feed_line(e, "{\"type\":\"flush\"}");
    check(engine_start(e) == 0, "start ok");
    cap_reset(&cap);
    /* steering: records + flush queued while the first turn runs */
    engine_test_push_record(e, cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q2\"}]}"));
    engine_test_push_record(e, cJSON_Parse("{\"type\":\"flush\"}"));
    int rc = engine_run(e);
    check(rc == EXIT_OK, "steered run exits 0");
    check(cap.n == 3, "steered run emits response, start, response");
    check_str(cap_field(&cap, 0, "type"), "response", "turn 1 final response");
    check_str(cap_field(&cap, 1, "type"), "start", "start marker at boundary");
    check_str(cap_field(&cap, 2, "text"), "b", "turn 2 answers the steering");
    engine_free(e);
    cap_reset(&cap);

    /* drop rule: records without flush at the final response */
    g_factory_wire = fwire_new(turns, 1);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (drop rule case)");
    cap_reset(&cap);
    engine_test_push_record(e, cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"never\"}]}"));
    /* no flush, stdin open */
    rc = engine_run(e);
    check(rc == EXIT_OK, "drop rule exits 0");
    check(cap.n == 2, "drop rule: io_error then final response");
    check_str(cap_field(&cap, 0, "code"), "io_error", "drop rule io_error");
    check_str(cap_field(&cap, 0, "fatal"), "false", "drop rule non-fatal");
    check(strstr(cap.v[0], "1 record") != NULL, "drop rule names the count");
    check_str(cap_field(&cap, 1, "type"), "response", "final response after io_error");
    engine_free(e);
    cap_reset(&cap);

    /* EOF applies pending records like a flush (no marker) */
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (eof case)");
    cap_reset(&cap);
    engine_test_push_record(e, cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q2\"}]}"));
    engine_test_push_eof(e);
    rc = engine_run(e);
    check(rc == EXIT_OK, "eof steering exits 0");
    check(cap.n == 2, "eof steering: response, response (no marker)");
    check_str(cap_field(&cap, 1, "text"), "b", "eof steering ran turn 2");
    engine_free(e);
    cap_reset(&cap);

    /* steering mid tool batch: running tool completes, rest suspended */
    fturn_t tool_turns[] = {
        { .recs = (const char *[]){
             "{\"type\":\"response\",\"text\":\"t\",\"partial\":false}",
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c1\"}",
             "{\"type\":\"tool_request\",\"tool\":\"a.t2\",\"arguments\":{},\"id\":\"c2\"}"},
          .nrecs = 3, .kind = TURN_TOOLS, .abort_after = -1 },
        { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"after\",\"partial\":false}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
    };
    ftool_t tools[] = {
        { .tool = "a.t1", .text = "done", .rc = 0 },
    };
    g_ftools = (ftool_script_t){ tools, 1, 0 };
    g_factory_wire = fwire_new(tool_turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (tool steering)");
    cap_reset(&cap);
    /* t1 runs; while it executes the steering flush arrives (queued before
       run: the engine drains after each tool; the first tool already ran) */
    engine_test_push_record(e, cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"steer\"}]}"));
    engine_test_push_record(e, cJSON_Parse("{\"type\":\"flush\"}"));
    rc = engine_run(e);
    check(rc == EXIT_OK, "tool steering exits 0");
    bool have_t1 = false, have_t2_susp = false, have_start = false;
    for (size_t i = 0; i < cap.n; i++) {
        cJSON *r = cJSON_Parse(cap.v[i]);
        if (rec_classify(r) == R_TOOL_RESPONSE) {
            const char *id = rec_str(r, "id");
            if (!strcmp(id, "c1")) have_t1 = true;
            if (!strcmp(id, "c2") && rec_bool(r, "is_error", false))
                have_t2_susp = strstr(cap.v[i], "steered") != NULL;
        }
        if (rec_classify(r) == R_START) have_start = true;
        cJSON_Delete(r);
    }
    check(have_t1, "running tool completed with its tool_response");
    check(have_t2_susp, "not yet started tool suspended: user steered");
    check(have_start, "consumed flush emitted the start marker");
    engine_free(e);
    cap_reset(&cap);

    /* max_tool_rounds: the would-exceed turn is not started */
    fturn_t loop_turns[] = {
        { .recs = (const char *[]){
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c1\"}"},
          .nrecs = 1, .kind = TURN_TOOLS, .abort_after = -1 },
        { .recs = (const char *[]){
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c2\"}"},
          .nrecs = 1, .kind = TURN_TOOLS, .abort_after = -1 },
    };
    ftool_t tools2[] = { { .tool = "a.t1", .text = "ok", .rc = 0 } };
    g_ftools = (ftool_script_t){ tools2, 1, 0 };
    g_factory_wire = fwire_new(loop_turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"options\",\"max_tool_rounds\":1}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (max rounds)");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_MAX_TOOL_ROUNDS, "max_tool_rounds exit code 6");
    check(cap.n >= 3, "max rounds: tool executed then fatal error");
    {
        cJSON *last = cJSON_Parse(cap.v[cap.n - 1]);
        check_str(rec_str(last, "code"), "max_tool_rounds_exceeded",
                  "max_tool_rounds record");
        cJSON_Delete(last);
    }
    engine_free(e);
    cap_reset(&cap);

    /* SIGINT mid stream: trailing partial, no final, io_error, interrupted */
    fturn_t abort_turns[] = {
        { .recs = (const char *[]){
             "{\"type\":\"response\",\"text\":\"par\",\"partial\":true}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = 1 },
    };
    g_factory_wire = fwire_new(abort_turns, 1);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    engine_test_push_record(e, cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"unflushed\"}]}"));
    check(engine_start(e) == 0, "start ok (sigint mid stream)");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_INTERRUPTED, "sigint mid stream exits 8");
    check_str(cap_field(&cap, 0, "type"), "response", "partial emitted");
    check_str(cap_field(&cap, 0, "partial"), "true", "partial stays partial");
    check_str(cap_field(&cap, 1, "code"), "io_error", "drop rule before interrupted");
    check_str(cap_field(&cap, 2, "code"), "interrupted", "interrupted fatal");
    g_stop_flag = 0;
    engine_free(e);
    cap_reset(&cap);

    /* SIGINT mid tool: running tool completes, rest answered, interrupted */
    fturn_t tool_abort[] = {
        { .recs = (const char *[]){
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c1\"}",
             "{\"type\":\"tool_request\",\"tool\":\"a.t2\",\"arguments\":{},\"id\":\"c2\"}"},
          .nrecs = 2, .kind = TURN_TOOLS, .abort_after = -1 },
    };
    ftool_t tools3[] = { { .tool = "a.t1", .text = "slow", .rc = 0,
                           .stop_after = true } };
    g_ftools = (ftool_script_t){ tools3, 1, 0 };
    g_factory_wire = fwire_new(tool_abort, 1);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (sigint mid tool)");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_INTERRUPTED, "sigint mid tool exits 8");
    bool t1_ok = false, t2_susp = false;
    for (size_t i = 0; i < cap.n; i++) {
        cJSON *r = cJSON_Parse(cap.v[i]);
        if (rec_classify(r) == R_TOOL_RESPONSE) {
            const char *id = rec_str(r, "id");
            if (!strcmp(id, "c1")) t1_ok = true;
            if (!strcmp(id, "c2") && rec_bool(r, "is_error", false))
                t2_susp = strstr(cap.v[i], "interrupted") != NULL;
        }
        cJSON_Delete(r);
    }
    check(t1_ok, "mid-tool: the running tool completed");
    check(t2_susp, "mid-tool: the not yet started tool answered interrupted");
    check_str(cap_field(&cap, cap.n - 1, "code"), "interrupted",
              "mid-tool: interrupted record");
    g_stop_flag = 0;
    engine_free(e);
    cap_reset(&cap);

    /* SIGINT before start: io_error only, nonzero, no interrupted record */
    e = engine_new(cap_fn, &cap);
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    g_stop_flag = 1;
    rc = engine_pre_start_stop(e);
    check(rc == EXIT_INTERRUPTED, "sigint before start exits nonzero");
    check(cap.n == 1 && !strcmp(cap_field(&cap, 0, "code"), "io_error") &&
              !strcmp(cap_field(&cap, 0, "fatal"), "false"),
          "before start: only the non-fatal io_error");
    g_stop_flag = 0;
    engine_free(e);
    cap_reset(&cap);

    /* tool failure and timeout still answer */
    fturn_t fail_turns[] = {
        { .recs = (const char *[]){
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c1\"}"},
          .nrecs = 1, .kind = TURN_TOOLS, .abort_after = -1 },
        { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"done\",\"partial\":false}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
    };
    ftool_t tools4[] = { { .tool = "a.t1", .text = NULL, .rc = 2 } };
    g_ftools = (ftool_script_t){ tools4, 1, 0 };
    g_factory_wire = fwire_new(fail_turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "start ok (tool timeout)");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_OK, "tool timeout: loop continues to exit 0");
    bool tr = false, tt = false, answered = false;
    for (size_t i = 0; i < cap.n; i++) {
        cJSON *r = cJSON_Parse(cap.v[i]);
        int k = rec_classify(r);
        if (k == R_ERROR && !strcmp(rec_str(r, "code"), "tool_timeout") &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "fatal")))
            tt = true;
        if (k == R_TOOL_RESPONSE && rec_bool(r, "is_error", false)) answered = true;
        if (k == R_RESPONSE) tr = true;
        cJSON_Delete(r);
    }
    check(tr && tt && answered, "tool timeout: answered + non-fatal + final");
    engine_free(e);
    cap_destroy(&cap);
}

/* ================= 5. mcp stdio client (fake child server) ================= */

static const char *FAKE_MCP_PY =
    "import sys, json\n"
    "for line in sys.stdin:\n"
    "    line = line.strip()\n"
    "    if not line: continue\n"
    "    m = json.loads(line)\n"
    "    method = m.get('method')\n"
    "    rid = m.get('id')\n"
    "    if method == 'initialize':\n"
    "        print(json.dumps({'jsonrpc':'2.0','id':rid,'result':"
    "{'protocolVersion':m['params']['protocolVersion'],'capabilities':{},"
    "'serverInfo':{'name':'fake','version':'1'}}}), flush=True)\n"
    "    elif method == 'tools/list':\n"
    "        print(json.dumps({'jsonrpc':'2.0','id':rid,'result':{'tools':"
    "[{'name':'echo','description':'echo text','inputSchema':{'type':"
    "'object','properties':{'text':{'type':'string'}},'required':"
    "['text']}},{'name':'boom','inputSchema':{'type':'object'}}]}}), "
    "flush=True)\n"
    "    elif method == 'tools/call':\n"
    "        a = m['params'].get('arguments',{})\n"
    "        if m['params']['name'] == 'echo':\n"
    "            print(json.dumps({'jsonrpc':'2.0','id':rid,'result':"
    "{'content':[{'type':'text','text':'echo: '+a.get('text','')}]}}), "
    "flush=True)\n"
    "        else:\n"
    "            print(json.dumps({'jsonrpc':'2.0','id':rid,'result':"
    "{'content':[{'type':'text','text':'boom'}],'isError':True}}), "
    "flush=True)\n"
    "    elif rid is not None:\n"
    "        print(json.dumps({'jsonrpc':'2.0','id':rid,'error':{'code':"
    "-32601,'message':'nope'}}), flush=True)\n";

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void test_mcp_stdio(void) {
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);
    cap_t cap = { 0 };
    engine_t *e = engine_new(cap_fn, &cap);
    char tools[512];
    snprintf(tools, sizeof tools,
             "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"srv\","
             "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}");
    cJSON *trec = cJSON_Parse(tools);
    check(mcp_reconcile((mcp_mgr_t *)e->mcp, e, trec) == 0, "mcp connect ok");
    const tool_listing_t *tl = mcp_listing((mcp_mgr_t *)e->mcp);
    check(tl->n == 2, "mcp listing has two tools");
    check_str(tl->v[0].exposed_name, "srv.echo", "exposed name is prefixed");
    char *snap = cJSON_PrintUnformatted(tl->v[0].tool);
    check(snap && strstr(snap, "echo text") != NULL,
          "listing snapshot carries the description");
    cJSON_free(snap);

    buf_t out;
    buf_init(&out);
    bool is_error = false;
    char err[256] = "";
    cJSON *args = cJSON_Parse("{\"text\":\"hi\"}");
    int rc = mcp_call((mcp_mgr_t *)e->mcp, "srv.echo", args, &out, &is_error,
                      err, sizeof err, 5);
    check(rc == 0 && !is_error, "echo call ok");
    check_str(out.data ? out.data : "", "echo: hi", "echo text round trip");
    buf_clear(&out);
    cJSON_Delete(args);

    args = cJSON_Parse("{}");
    rc = mcp_call((mcp_mgr_t *)e->mcp, "srv.boom", args, &out, &is_error, err,
                  sizeof err, 5);
    check(rc == 0 && is_error, "isError maps to is_error");
    cJSON_Delete(args);
    buf_free(&out);

    mcp_kill_all((mcp_mgr_t *)e->mcp);
    cJSON_Delete(trec);
    engine_free(e);
    cap_destroy(&cap);
}

/* ================= 6. mcp proxy ================= */

static void test_mcp_proxy(void) {
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);
    proxy_state_t p;

    /* whitelist with rename + description override */
    proxy_state_init(&p);
    check(proxy_config_line(&p, strdup(
        "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\","
        "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}")), "cfg tools ok");
    check(proxy_config_line(&p, strdup(
        "{\"type\":\"expose\",\"tool\":\"up.echo\",\"name\":\"say\","
        "\"description\":\"say it\",\"arguments\":{\"text\":{\"name\":"
        "\"words\"}}}")), "cfg expose ok");
    check(proxy_resolve_and_build(&p) == 0, "resolve ok");
    check(p.nex == 1, "one exposed tool");
    check_str(rec_str(p.ex[0].presentation, "name"), "say", "exposed name");
    check_str(rec_str(p.ex[0].presentation, "description"), "say it",
              "description override");
    const cJSON *sch =
        cJSON_GetObjectItemCaseSensitive(p.ex[0].presentation, "inputSchema");
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(sch, "properties");
    const cJSON *words = cJSON_GetObjectItemCaseSensitive(props, "words");
    check(words != NULL, "argument renamed in schema");
    check(cJSON_GetObjectItemCaseSensitive(props, "text") == NULL,
          "upstream argument name replaced");
    const cJSON *req = cJSON_GetObjectItemCaseSensitive(sch, "required");
    check(req && cJSON_IsArray(req) && req->child &&
              !strcmp(req->child->valuestring, "words"),
          "required follows the rename");
    check(p.ex[0].nmap == 1 && !strcmp(p.ex[0].from[0], "words") &&
              !strcmp(p.ex[0].to[0], "text"),
          "inverse map recorded");

    /* rewrite determinism: serialize twice */
    char *s1 = cJSON_PrintUnformatted(p.ex[0].presentation);
    char *s2 = cJSON_PrintUnformatted(p.ex[0].presentation);
    check(!strcmp(s1, s2), "rewrite serialization deterministic");
    cJSON_free(s1);
    cJSON_free(s2);

    /* inverse mapping on forwarding: exposed call reaches upstream name */
    cJSON *id = cJSON_CreateNumber(1);
    cJSON *call = cJSON_Parse(
        "{\"name\":\"say\",\"arguments\":{\"words\":\"proxy\"}}");
    cJSON *raw = NULL;
    char *emsg = NULL;
    check(proxy_handle(&p, "tools/call", call, id, &raw, &emsg) == 0,
          "exposed tools/call through the proxy handler");
    if (raw) {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(raw, "content");
        const cJSON *first = content ? content->child : NULL;
        check(first && !strcmp(rec_str(first, "text"), "echo: proxy"),
              "inverse argument mapping reached the upstream tool");
        cJSON_Delete(raw);
    }
    free(emsg);
    cJSON_Delete(call);
    cJSON_Delete(id);
    proxy_state_free(&p);

    /* blacklist mode: hide one tool */
    proxy_state_init(&p);
    proxy_config_line(&p, strdup(
        "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\","
        "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}"));
    proxy_config_line(&p, strdup("{\"type\":\"hide\",\"tool\":\"up.boom\"}"));
    check(proxy_resolve_and_build(&p) == 0, "blacklist resolve ok");
    check(p.nex == 1 && !strcmp(rec_str(p.ex[0].presentation, "name"), "up.echo"),
          "blacklist keeps the other tool");
    proxy_state_free(&p);

    /* default: every tool exposed, selector names */
    proxy_state_init(&p);
    proxy_config_line(&p, strdup(
        "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\","
        "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}"));
    check(proxy_resolve_and_build(&p) == 0, "default resolve ok");
    check(p.nex == 2 && !strcmp(rec_str(p.ex[0].presentation, "name"), "up.echo"),
          "default mode exposes everything with selector names");
    proxy_state_free(&p);

    /* fatal cases: unknown server, unknown tool, duplicate exposure, mix */
    struct {
        const char *lines[4];
        const char *name;
    } bad[] = {
        { { "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\",\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}",
            "{\"type\":\"expose\",\"tool\":\"nope.echo\"}" }, "unknown server" },
        { { "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\",\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}",
            "{\"type\":\"expose\",\"tool\":\"up.missing\"}" }, "unknown tool" },
        { { "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\",\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}",
            "{\"type\":\"expose\",\"tool\":\"up.echo\"}",
            "{\"type\":\"expose\",\"tool\":\"up.echo\",\"name\":\"x\"}" }, "duplicate expose" },
        { { "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\",\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}",
            "{\"type\":\"expose\",\"tool\":\"up.echo\"}",
            "{\"type\":\"hide\",\"tool\":\"up.boom\"}" }, "mixing expose and hide" },
        { { "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"up\",\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\"}]}",
            "{\"type\":\"expose\",\"tool\":\"up.echo\",\"name\":\"dup\"}",
            "{\"type\":\"expose\",\"tool\":\"up.boom\",\"name\":\"dup\"}" }, "duplicate exposed names" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        proxy_state_init(&p);
        for (int j = 0; bad[i].lines[j]; j++)
            proxy_config_line(&p, strdup(bad[i].lines[j]));
        check(proxy_resolve_and_build(&p) != 0, bad[i].name);
        proxy_state_free(&p);
    }

    /* invalid config record types */
    proxy_state_init(&p);
    check(!proxy_config_line(&p, strdup("{\"type\":\"llm\"}")),
          "llm record invalid in a config");
    proxy_state_free(&p);
    proxy_state_init(&p);
    check(!proxy_config_line(&p, strdup("{\"type\":\"flush\"}")),
          "flush invalid in a config");
    proxy_state_free(&p);
    proxy_state_init(&p);
    check(!proxy_config_line(&p, strdup("{\"type\":\"user\",\"content\":"
                                        "[{\"type\":\"text\",\"text\":\"x\"}]}")),
          "user invalid in a config");
    proxy_state_free(&p);
}

/* ================= 7. exit code table (engine level) ================= */

static void test_exit_codes(void) {
    cap_t cap = { 0 };
    struct {
        const char *fatal_json;
        int want;
    } cases[] = {
        { "{\"type\":\"error\",\"code\":\"invalid_record\",\"message\":\"x\",\"fatal\":true}", 2 },
        { "{\"type\":\"error\",\"code\":\"connect_failed\",\"message\":\"x\",\"fatal\":true}", 3 },
        { "{\"type\":\"error\",\"code\":\"http_error\",\"message\":\"x\",\"fatal\":true}", 4 },
        { "{\"type\":\"error\",\"code\":\"api_error\",\"message\":\"x\",\"fatal\":true}", 5 },
        { "{\"type\":\"error\",\"code\":\"io_error\",\"message\":\"x\",\"fatal\":true}", 7 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fturn_t t = { .recs = NULL, .nrecs = 0, .kind = TURN_FATAL,
                      .fatal_json = cases[i].fatal_json, .abort_after = -1 };
        g_factory_wire = fwire_new(&t, 1);
        engine_t *e = engine_new(cap_fn, &cap);
        e->wire_factory = script_factory;
        e->inq = queue_new();
        feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
        feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
        check(engine_start(e) == 0, "start ok for exit code case");
        cap_reset(&cap);
        int rc = engine_run(e);
        check(rc == cases[i].want, "exit code mapping");
        engine_free(e); /* also destroys the shared factory wire */
        cap_reset(&cap);
    }
    cap_destroy(&cap);
}

/* ================= 8. jsonrpc server loop (in-process) ================= */

typedef struct loop_io {
    const char *in;
    size_t pos;
    buf_t out;
} loop_io_t;

static int loop_read(void *ctx, char *buf, size_t bufsz) {
    loop_io_t *io = ctx;
    if (io->pos >= strlen(io->in)) return 0;
    size_t n = strlen(io->in) - io->pos;
    if (n > bufsz) n = bufsz;
    memcpy(buf, io->in + io->pos, n);
    io->pos += n;
    return (int)n;
}

static void loop_write(void *ctx, const char *line, size_t n) {
    loop_io_t *io = ctx;
    /* serve_line writes the line and its '\n' terminator separately */
    buf_append(&io->out, line, n);
}

static int loop_handle(void *ctx, const char *method, cJSON *params, cJSON *id,
                       cJSON **result_out, char **errmsg_out) {
    (void)ctx;
    (void)params;
    if (id && !strcmp(method, "ping")) {
        *result_out = cJSON_Parse("{\"ok\":true}");
        return 0;
    }
    if (id && !strcmp(method, "tools/list")) {
        *result_out = cJSON_Parse("{\"tools\":[{\"name\":\"t\"}]}");
        return 0;
    }
    if (!strcmp(method, "notifications/initialized")) return 2;
    if (id) {
        *errmsg_out = strdup("nope");
        return 1;
    }
    return 2;
}

static void test_rpc_loop(void) {
    loop_io_t io = { 0 };
    buf_init(&io.out);
    io.in =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n"
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"bogus\"}\n"
        "garbage\n";
    rpc_handler_t h = { loop_handle, NULL };
    rpc_serve(&h, loop_read, loop_write, &io);
    /* two replies + one error, in request order; no reply for the
       notification or the garbage line */
    size_t lines = 0;
    for (size_t i = 0; i < io.out.len; i++)
        if (io.out.data[i] == '\n') lines++;
    check(lines == 3, "rpc loop: 3 replies, notifications silent");
    check(strstr(io.out.data, "\"id\":1") != NULL &&
              strstr(io.out.data, "\"id\":2") != NULL &&
              strstr(io.out.data, "\"id\":3") != NULL,
          "rpc loop: replies carry ids");
    check(strstr(io.out.data, "\"error\"") != NULL,
          "rpc loop: unknown method is an error reply");
    check(strstr(io.out.data, "\"id\":1") < strstr(io.out.data, "\"id\":2"),
          "rpc loop: replies in request order");
    buf_free(&io.out);
}


/* ================= 9. stream parity + agent (localhost fake endpoint) ===== */

/* one scripted endpoint: odd requests answer a tool-call turn, even requests
   a final turn; SSE when the request body asks to stream, json otherwise.
   argv[1] = request log path ('-' = no logging). Prints the port. */
static const char *FAKE_ENDPOINT_PY =
    "import sys, json\n"
    "from http.server import BaseHTTPRequestHandler, HTTPServer\n"
    "LOG = open(sys.argv[1], 'a') if sys.argv[1] != '-' else None\n"
    "EVIL = len(sys.argv) > 2 and sys.argv[2] == 'evil'\n"
    "STATE = {'n': 0}\n"
    "def sse(self, chunks):\n"
    "    self.send_response(200)\n"
    "    self.send_header('Content-Type', 'text/event-stream')\n"
    "    self.end_headers()\n"
    "    for c in chunks:\n"
    "        self.wfile.write(b'data: ' + json.dumps(c).encode() + b'\\n\\n')\n"
    "    self.wfile.write(b'data: [DONE]\\n\\n')\n"
    "def js(self, obj):\n"
    "    self.send_response(200)\n"
    "    self.send_header('Content-Type', 'application/json')\n"
    "    self.end_headers()\n"
    "    self.wfile.write(json.dumps(obj).encode())\n"
    "class H(BaseHTTPRequestHandler):\n"
    "    def log_message(self, *a): pass\n"
    "    def do_POST(self):\n"
    "        n = int(self.headers.get('Content-Length', 0))\n"
    "        body = self.rfile.read(n)\n"
    "        if LOG:\n"
    "            LOG.write(body.decode() + '\\n'); LOG.flush()\n"
    "        req = json.loads(body or b'{}')\n"
    "        stream = req.get('stream', False)\n"
    "        STATE['n'] += 1\n"
    "        if EVIL:\n"
    "            sse(self, [\n"
    "              {'choices':[{'delta':{'tool_calls':[{'index':1000000000,"
    "'id':'cx','function':{'name':'fs.echo','arguments':'{}'}}]}}]},\n"
    "              {'choices':[{'delta':{},'finish_reason':'stop'}]},\n"
    "            ])\n"
    "            return\n"
    "        if STATE['n'] % 2 == 1:\n"
    "            if stream:\n"
    "                sse(self, [\n"
    "                  {'choices':[{'delta':{'reasoning_content':'thinking hard'}}]},\n"
    "                  {'choices':[{'delta':{'content':'calling now'}}]},\n"
    "                  {'choices':[{'delta':{'tool_calls':[{'index':0,'id':'c1',"
    "'function':{'name':'fs.echo','arguments':'{\"x\":'}}]}}]},\n"
    "                  {'choices':[{'delta':{'tool_calls':[{'index':0,"
    "'function':{'arguments':'\"hi\"}'}}]}}]},\n"
    "                  {'choices':[{'delta':{},'finish_reason':'tool_calls'}]},\n"
    "                  {'usage':{'prompt_tokens':10,'completion_tokens':5}},\n"
    "                ])\n"
    "            else:\n"
    "                js(self, {'choices':[{'message':{'reasoning_content':"
    "'thinking hard','content':'calling now','tool_calls':[{'id':'c1',"
    "'type':'function','function':{'name':'fs.echo','arguments':"
    "'{\"x\":\"hi\"}'}}]},'finish_reason':'tool_calls'}],"
    "'usage':{'prompt_tokens':10,'completion_tokens':5}})\n"
    "        else:\n"
    "            if stream:\n"
    "                sse(self, [\n"
    "                  {'choices':[{'delta':{'content':'all '}}]},\n"
    "                  {'choices':[{'delta':{'content':'done'}}]},\n"
    "                  {'choices':[{'delta':{},'finish_reason':'stop'}]},\n"
    "                  {'usage':{'prompt_tokens':20,'completion_tokens':7}},\n"
    "                ])\n"
    "            else:\n"
    "                js(self, {'choices':[{'message':{'content':'all done'},"
    "'finish_reason':'stop'}],'usage':{'prompt_tokens':20,"
    "'completion_tokens':7}})\n"
    "srv = HTTPServer(('127.0.0.1', 0), H)\n"
    "print(srv.server_address[1], flush=True)\n"
    "srv.serve_forever()\n";

static int read_line_fd(int fd, char *buf, size_t sz) {
    size_t n = 0;
    while (n + 1 < sz) {
        ssize_t r = read(fd, buf + n, 1);
        if (r <= 0) break;
        if (buf[n] == '\n') {
            buf[n] = '\0';
            return (int)n;
        }
        n++;
    }
    buf[n] = '\0';
    return (int)n;
}

/* spawn the scripted endpoint; false on failure. mode "evil" makes every
   response carry a hostile oversized tool_calls index */
static bool endpoint_spawn_mode(spawn_t *sp, const char *log_path,
                                const char *mode, char *url_out, size_t urlsz) {
    write_file("/tmp/llmkit-test-endpoint.py", FAKE_ENDPOINT_PY);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "python3 /tmp/llmkit-test-endpoint.py %s %s",
             log_path, mode ? mode : "");
    if (spawn_shell(cmd, sp)) return false;
    char port[64] = "";
    read_line_fd(sp->from_fd, port, sizeof port);
    int p = atoi(port);
    if (p <= 0) {
        spawn_kill(sp);
        return false;
    }
    snprintf(url_out, urlsz, "http://127.0.0.1:%d", p);
    return true;
}

static bool endpoint_spawn(spawn_t *sp, const char *log_path, char *url_out,
                           size_t urlsz) {
    return endpoint_spawn_mode(sp, log_path, NULL, url_out, urlsz);
}

/* the same scripted response with stream on and off: identical final
   records (design sec.5, sec.13) */
static void test_stream_parity(void) {
    spawn_t sp;
    char url[128];
    if (!endpoint_spawn(&sp, "-", url, sizeof url)) {
        check(false, "parity endpoint spawned");
        return;
    }
    check(true, "parity endpoint spawned");

    ftool_t tools[] = { { .tool = "fs.echo", .text = "tool says hi", .rc = 0 } };
    g_ftools = (ftool_script_t){ tools, 1, 0 };

    const char *runs[2] = { NULL, NULL };
    cap_t caps[2] = { { 0 }, { 0 } };
    for (int i = 0; i < 2; i++) {
        engine_t *e = engine_new(cap_fn, &caps[i]);
        char llm[512];
        snprintf(llm, sizeof llm,
                 "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"%s\",\"model\":\"m\",\"inference_options\":"
                 "{\"stream\":%s}}",
                 url, i == 0 ? "true" : "false");
        e->llm = cJSON_Parse(llm);
        e->protocol = PROTO_OPENAI;
        e->tool_exec = ftool_exec;
        tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hello\"}]}");
        check(engine_start(e) == 0, "parity: start ok");
        int rc = engine_run(e);
        check(rc == EXIT_OK, "parity: run exits 0");
        engine_free(e);
        /* keep only final records (partial:false / tool / error records) */
        buf_t keep;
        buf_init(&keep);
        for (size_t k = 0; k < caps[i].n; k++) {
            cJSON *r = cJSON_Parse(caps[i].v[k]);
            bool partial = rec_classify(r) == R_RESPONSE &&
                           rec_bool(r, "partial", true);
            bool tpart = rec_classify(r) == R_THINKING &&
                         rec_bool(r, "partial", true);
            cJSON_Delete(r);
            if (partial || tpart) continue;
            buf_append_str(&keep, caps[i].v[k]);
            buf_append_byte(&keep, '\n');
        }
        runs[i] = keep.data ? keep.data : "";
    }
    check_str(runs[1], runs[0], "stream parity: identical final records");
    check(strstr(runs[0], "\"thinking\"") != NULL &&
              strstr(runs[0], "\"signature\":\"\"") != NULL,
          "parity: thinking final carries an empty signature");
    for (int i = 0; i < 2; i++) {
        cap_destroy(&caps[i]);
        free((void *)runs[i]);
    }
    spawn_kill(&sp);
}

/* a hostile endpoint must not be able to size (or overflow) an allocation
   through tool_calls[].index - the slot table is bounded and checked */
static void test_hostile_endpoint(void) {
    spawn_t sp;
    char url[128];
    if (!endpoint_spawn_mode(&sp, "-", "evil", url, sizeof url)) {
        check(false, "hostile endpoint spawned");
        return;
    }
    check(true, "hostile endpoint spawned");

    cap_t cap = { 0 };
    engine_t *e = engine_new(cap_fn, &cap);
    char llm[512];
    snprintf(llm, sizeof llm,
             "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
             "\"api_base\":\"%s\",\"model\":\"m\"}",
             url);
    e->llm = cJSON_Parse(llm);
    e->protocol = PROTO_OPENAI;
    tr_add(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
              "\"text\":\"hello\"}]}");
    check(engine_start(e) == 0, "hostile: start ok");
    int rc = engine_run(e); /* stream defaults on */
    check(rc == EXIT_OK, "hostile: oversized tool index is dropped, run ok");
    engine_free(e);
    cap_destroy(&cap);
    spawn_kill(&sp);
}

/* agent-as-tool over pipes: seed bootstrap, invoke reply, retain_context
   accumulation (the seed history must reach the endpoint) */
static void test_agent_tool(void) {
    const char *log = "/tmp/llmkit-test-agent-requests.log";
    unlink(log);
    spawn_t sp;
    char url[128];
    if (!endpoint_spawn(&sp, log, url, sizeof url)) {
        check(false, "agent endpoint spawned");
        return;
    }
    check(true, "agent endpoint spawned");
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);

    char seed[1024];
    snprintf(seed, sizeof seed,
             "{\"type\":\"header\",\"version\":1}\n"
             "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
             "\"api_base\":\"%s\",\"model\":\"m\",\"inference_options\":"
             "{\"stream\":false}}\n"
             "{\"type\":\"options\",\"retain_context\":true}\n"
             "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":"
             "\"fs\",\"command_line\":\"python3 "
             "/tmp/llmkit-test-fake-mcp.py\"}]}\n"
             "{\"type\":\"agent-as-tool\",\"tool_description\":\"the agent\","
             "\"input_description\":\"the input\"}\n"
             "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
             "\"seed-history-marker\"}]}\n"
             "{\"type\":\"response\",\"text\":\"seed-answer\",\"partial\":"
             "false}\n",
             url);
    write_file("/tmp/llmkit-test-agent-seed.jsonl", seed);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) {
        check(false, "agent pipes");
        spawn_kill(&sp);
        return;
    }
    pid_t pid = fork();
    check(pid >= 0, "agent fork");
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        int rc = cmd_agent("/tmp/llmkit-test-agent-seed.jsonl");
        _exit(rc == 0 ? 0 : 1);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    FILE *to = fdopen(in_pipe[1], "w");
    FILE *from = fdopen(out_pipe[0], "r");
    char line[4096];

    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "llmkit-agent-as-tool") != NULL,
          "agent: initialize reply");

    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "\"name\":\"invoke\"") != NULL &&
              strstr(line, "the agent") != NULL &&
              strstr(line, "the input") != NULL,
          "agent: tools/list exposes invoke with the seed descriptions");

    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"invoke\",\"arguments\":"
                "{\"input\":\"go\"}}}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "all done") != NULL &&
              strstr(line, "isError") == NULL,
          "agent: invoke replies with the final response text");

    /* retain_context: a second invoke accumulates on the seeded transcript */
    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"invoke\",\"arguments\":"
                "{\"input\":\"again\"}}}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "all done") != NULL,
          "agent: second invoke over the retained transcript");

    fclose(to);
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent: exits 0");
    fclose(from);

    /* the seeded history must have been sent to the endpoint (retain mode
       keeps one accumulated transcript starting as the bare seed), and the
       retained first invoke must appear in the second invoke's requests */
    FILE *lg = fopen(log, "r");
    bool hist = false, first = false;
    if (lg) {
        char lbuf[8192];
        while (fgets(lbuf, sizeof lbuf, lg)) {
            if (strstr(lbuf, "seed-history-marker")) hist = true;
            if (strstr(lbuf, "seed-answer") && strstr(lbuf, "\"go\""))
                first = true;
        }
        fclose(lg);
    }
    check(hist, "agent retain: the seed history reaches the endpoint");
    check(first, "agent retain: the first invoke is retained in the next");
    spawn_kill(&sp);
}

/* ================= llmkit call ================= */

static void test_call(void) {
    call_cfg_t c;
    char err[256];
    buf_t b;

    /* parse: the full vector, exact llm serialization */
    {
        char *av[] = {"--anthropic", "http://x/v1", "--key", "k",
                      "--model", "m", "--max-tokens", "1024",
                      "--system-prompt", "sys",
                      "--header", "a=1", "--header", "b=2",
                      "--prompt", "hi"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: full vector parses");
        check(c.protocol == PROTO_ANTHROPIC, "call: anthropic mapping");
        check(c.max_tokens == 1024, "call: max-tokens captured");
        cJSON *llm = call_build_llm(&c);
        buf_init(&b);
        buf_append_tree(&b, llm);
        cJSON_Delete(llm);
        check_str(b.data,
                  "{\"type\":\"llm\",\"endpoint_protocol\":\"anthropic\","
                  "\"api_base\":\"http://x/v1\",\"api_key\":\"k\","
                  "\"model\":\"m\",\"inference_options\":"
                  "{\"max_tokens\":1024},\"headers\":{\"a\":\"1\","
                  "\"b\":\"2\"}}",
                  "call: llm serialization");
        buf_free(&b);
        cJSON *sys = call_build_system(&c);
        buf_init(&b);
        buf_append_tree(&b, sys);
        cJSON_Delete(sys);
        check_str(b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"sys\"}]}",
                  "call: system serialization");
        buf_free(&b);
        cJSON *user = call_build_user(c.prompt);
        buf_init(&b);
        buf_append_tree(&b, user);
        cJSON_Delete(user);
        check_str(b.data,
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}",
                  "call: user serialization");
        buf_free(&b);
        call_cfg_free(&c);
    }

    /* minimal vector: absent fields are not sent at all */
    {
        char *av[] = {"--openai", "http://x", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: minimal vector parses");
        check(c.protocol == PROTO_OPENAI, "call: openai mapping");
        check(call_build_system(&c) == NULL,
              "call: absent system -> no record");
        check(call_build_tools(&c, "/x") == NULL,
              "call: no proxies -> no record");
        cJSON *llm = call_build_llm(&c);
        buf_init(&b);
        buf_append_tree(&b, llm);
        cJSON_Delete(llm);
        check_str(b.data,
                  "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                  "\"api_base\":\"http://x\"}",
                  "call: minimal llm serialization");
        buf_free(&b);
        call_cfg_free(&c);
    }

    /* responses mapping + header replace */
    {
        char *av[] = {"--openai-responses", "http://x", "--header", "a=1",
                      "--header", "a=2", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: responses vector parses");
        check(c.protocol == PROTO_RESPONSES, "call: responses mapping");
        check(c.nhdrs == 1 && !strcmp(c.hdr_values[0], "2"),
              "call: later --header replaces the earlier");
        call_cfg_free(&c);
    }

    /* shell quoting */
    buf_init(&b);
    call_shell_quote(&b, "/opt/llm kit's/bin");
    check_str(b.data, "'/opt/llm kit'\\''s/bin'", "call: shell quote");
    buf_free(&b);

    /* tools build: argv-order names, quoted command_line */
    {
        char *av[] = {"--openai", "http://x", "--mcp-proxy", "/tmp/fs.jsonl",
                      "--mcp-proxy", "dir/sub/other.cfg", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: proxy vector parses");
        cJSON *t = call_build_tools(&c, "/opt/llm kit's/bin");
        buf_init(&b);
        buf_append_tree(&b, t);
        cJSON_Delete(t);
        check_str(b.data,
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                  "\"name\":\"fs\",\"command_line\":\"'/opt/llm kit'\\\\''s/"
                  "bin' mcp-proxy '/tmp/fs.jsonl'\"},{\"type\":\"stdio\","
                  "\"name\":\"other\",\"command_line\":\"'/opt/llm kit'\\\\''"
                  "s/bin' mcp-proxy 'dir/sub/other.cfg'\"}]}",
                  "call: tools serialization");
        buf_free(&b);
        call_cfg_free(&c);
    }

    /* usage errors */
    {
        char *e0[] = {NULL};
        check(call_parse(0, e0, &c, err, sizeof err) == 1,
              "call: no flags is a usage error");
        check(err[0] != '\0', "call: usage error fills the message");
        char *e1[] = {"--openai"};
        check(call_parse(1, e1, &c, err, sizeof err) == 1,
              "call: missing api_base");
        char *e2[] = {"--openai", "http://x"};
        check(call_parse(2, e2, &c, err, sizeof err) == 1,
              "call: missing --prompt");
        char *e3[] = {"--openai", "--anthropic", "http://x", "--prompt", "p"};
        check(call_parse(5, e3, &c, err, sizeof err) == 1,
              "call: protocol flag twice");
        char *e4[] = {"--openai", "http://x", "--prompt"};
        check(call_parse(3, e4, &c, err, sizeof err) == 1,
              "call: missing flag value");
        char *e5[] = {"--openai", "http://x", "--prompt", "p", "extra"};
        check(call_parse(5, e5, &c, err, sizeof err) == 1,
              "call: extra positional");
        char *e6[] = {"--openai", "http://x", "--wat", "--prompt", "p"};
        check(call_parse(5, e6, &c, err, sizeof err) == 1,
              "call: unknown flag");
        char *e7[] = {"--openai", "http://x", "--header", "novalue",
                      "--prompt", "p"};
        check(call_parse(6, e7, &c, err, sizeof err) == 1,
              "call: malformed --header");
        char *e8[] = {"--openai", "http://x", "--header", "=v",
                      "--prompt", "p"};
        check(call_parse(6, e8, &c, err, sizeof err) == 1,
              "call: empty --header name");
        char *e9[] = {"--openai", "http://x", "--mcp-proxy", "/a/.jsonl",
                      "--prompt", "p"};
        check(call_parse(6, e9, &c, err, sizeof err) == 1,
              "call: empty proxy server name");
        char *e10[] = {"--openai", "http://x", "--mcp-proxy", "/a/x.jsonl",
                       "--mcp-proxy", "/b/x.jsonl", "--prompt", "p"};
        check(call_parse(8, e10, &c, err, sizeof err) == 1,
              "call: repeated proxy server name");
        char *e11[] = {"--openai", "http://x", "--key", "a", "--key", "b",
                       "--prompt", "p"};
        check(call_parse(7, e11, &c, err, sizeof err) == 1,
              "call: --key twice");
        char *e12[] = {"--openai", "http://x", "--max-tokens", "0",
                       "--prompt", "p"};
        check(call_parse(5, e12, &c, err, sizeof err) == 1,
              "call: --max-tokens wants a positive integer");
    }

    /* record tier: validation fires before anything runs */
    {
        char *av[] = {"--openai", "http://x", "--key", "a\r", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: CR-in-key vector parses (CLI shape is fine)");
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FILE *out = open_memstream(&ob, &on);
        FILE *er = open_memstream(&eb, &en);
        check(call_run(&c, out, er, "/x", NULL) == EXIT_INVALID_RECORD,
              "call: CR in key is invalid_record, exit 2");
        fclose(out);
        fclose(er);
        check(strstr(eb, "llmkit call: invalid_record:") == eb,
              "call: stderr line format");
        check(on == 0, "call: nothing on stdout before validation passes");
        free(ob);
        free(eb);
        call_cfg_free(&c);
    }
    {
        char *av[] = {"--openai", "http://x", "--prompt", "\xff"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: bad-utf8 vector parses");
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FILE *out = open_memstream(&ob, &on);
        FILE *er = open_memstream(&eb, &en);
        check(call_run(&c, out, er, "/x", NULL) == EXIT_INVALID_RECORD,
              "call: invalid UTF-8 in a flag is invalid_record");
        fclose(out);
        fclose(er);
        free(ob);
        free(eb);
        call_cfg_free(&c);
    }

    /* end-to-end per protocol: text out, thinking dropped, newline */
    {
        int protos[] = {PROTO_OPENAI, PROTO_RESPONSES, PROTO_ANTHROPIC};
        for (int i = 0; i < 3; i++) {
            fturn_t turns[] = {
                { .recs = (const char *[]){
                      "{\"type\":\"thinking\",\"text\":\"secret\","
                      "\"partial\":false}",
                      "{\"type\":\"response\",\"text\":\"Hel\","
                      "\"partial\":true}",
                      "{\"type\":\"response\",\"text\":\"lo\","
                      "\"partial\":false}"},
                  .nrecs = 3, .kind = TURN_FINAL, .abort_after = -1 },
            };
            g_factory_wire = fwire_new(turns, 1);
            memset(&c, 0, sizeof c);
            c.protocol = protos[i];
            c.api_base = strdup("http://x");
            c.prompt = strdup("q");
            c.max_tokens = i == 2 ? 512 : -1; /* anthropic requires it */
            char *ob = NULL, *eb = NULL;
            size_t on = 0, en = 0;
            FILE *out = open_memstream(&ob, &on);
            FILE *er = open_memstream(&eb, &en);
            int rc = call_run(&c, out, er, "/x", script_factory);
            fclose(out);
            fclose(er);
            check(rc == EXIT_OK, "call: scripted run exits 0");
            check_str(ob, "Hello\n", "call: thinking dropped, text + newline");
            check(en == 0, "call: no stderr on a clean run");
            free(ob);
            free(eb);
            call_cfg_free(&c);
        }
    }

    /* fatal endpoint error: exit code + one stderr line */
    {
        fturn_t turns[] = {
            { .recs = NULL, .nrecs = 0, .kind = TURN_FATAL,
              .fatal_json = "{\"type\":\"error\",\"code\":\"api_error\","
                            "\"message\":\"boom\",\"fatal\":true}",
              .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 1);
        memset(&c, 0, sizeof c);
        c.protocol = PROTO_OPENAI;
        c.api_base = strdup("http://x");
        c.prompt = strdup("q");
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FILE *out = open_memstream(&ob, &on);
        FILE *er = open_memstream(&eb, &en);
        int rc = call_run(&c, out, er, "/x", script_factory);
        fclose(out);
        fclose(er);
        check(rc == EXIT_API_ERROR, "call: api_error exit code");
        check(strstr(eb, "llmkit call: api_error: boom") == eb,
              "call: fatal error stderr line");
        check(on == 0, "call: no stdout on a fatal run");
        free(ob);
        free(eb);
        call_cfg_free(&c);
    }

    /* empty answer writes nothing at all */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 1);
        memset(&c, 0, sizeof c);
        c.protocol = PROTO_OPENAI;
        c.api_base = strdup("http://x");
        c.prompt = strdup("q");
        char *ob = NULL;
        size_t on = 0;
        FILE *out = open_memstream(&ob, &on);
        int rc = call_run(&c, out, stderr, "/x", script_factory);
        fclose(out);
        check(rc == EXIT_OK, "call: empty answer exits 0");
        check(on == 0, "call: empty answer writes nothing");
        free(ob);
        call_cfg_free(&c);
    }
}

int main(void) {
    signals_init();
    http_global_init();
    fprintf(stderr, "selfcheck: prefix invariant\n");
    test_prefix_invariant();
    fprintf(stderr, "selfcheck: sse parser\n");
    test_sse_parser();
    fprintf(stderr, "selfcheck: validation\n");
    test_validation();
    fprintf(stderr, "selfcheck: state machine\n");
    test_state_machine();
    fprintf(stderr, "selfcheck: exit codes\n");
    test_exit_codes();
    fprintf(stderr, "selfcheck: rpc loop\n");
    test_rpc_loop();
    fprintf(stderr, "selfcheck: mcp stdio client\n");
    test_mcp_stdio();
    fprintf(stderr, "selfcheck: mcp proxy\n");
    test_mcp_proxy();
    fprintf(stderr, "selfcheck: stream parity\n");
    test_stream_parity();
    fprintf(stderr, "selfcheck: hostile endpoint\n");
    test_hostile_endpoint();
    fprintf(stderr, "selfcheck: agent-as-tool\n");
    test_agent_tool();
    fprintf(stderr, "selfcheck: call\n");
    test_call();
    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
