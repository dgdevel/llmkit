/* selfcheck.c - plain asserts, no framework (design sec.14).
   Categories: prefix invariant, sse parser, stream parity, validation,
   state machine (flush/steering/drop rule/max rounds/sigint), mcp client
   (fake child servers), mcp proxy, call (argv compilation, scripted runs). */
#include "llmkit.h"
#include "prompts.gen.h"

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

/* the system text the engine carries at its first turn, captured by
   fwire_turn (one buffer, read right after the session under test);
   multi-block system records join with \n - the openai wire's join */
static char g_seen_system[4096];

static void capture_system(const engine_t *e) {
    g_seen_system[0] = '\0';
    if (!e || !e->system) return;
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(e->system, "content");
    if (!cJSON_IsArray(content)) return;
    buf_t b;
    buf_init(&b);
    for (const cJSON *c = content->child; c; c = c->next) {
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(c, "text");
        if (!cJSON_IsString(tx) || !tx->valuestring) continue;
        if (b.len) buf_append_byte(&b, '\n');
        buf_append_str(&b, tx->valuestring);
    }
    snprintf(g_seen_system, sizeof g_seen_system, "%s",
             b.data ? b.data : "");
    buf_free(&b);
}

static int fwire_turn(wire_t *base, engine_t *e, turn_out_t *out) {
    fwire_t *f = (fwire_t *)base;
    if (f->pos == 0) capture_system(e);
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
    const char *mid_rec; /* user record queued while the tool executes */
    bool mid_flush;     /* a flush queued while the tool executes */
} ftool_t;

typedef struct ftool_script {
    ftool_t *v;
    int n, pos;
} ftool_script_t;

ftool_script_t g_ftools;

static int ftool_exec(engine_t *e, const char *tool, cJSON *args,
                      buf_t *text_out, bool *is_error, char *err, size_t errsz) {
    (void)args;
    (void)is_error;
    ftool_t *t =
        &g_ftools.v[g_ftools.pos < g_ftools.n ? g_ftools.pos++ : g_ftools.n - 1];
    check(t->tool && !strcmp(t->tool, tool), "tool executed in order");
    if (t->text) buf_append_str(text_out, t->text);
    if (t->rc == 1) snprintf(err, errsz, "tool failed: %s", tool);
    if (t->rc == 2) snprintf(err, errsz, "tool timed out: %s", tool);
    int rc = t->rc;
    /* stdin traffic arriving while the tool runs */
    if (t->mid_rec)
        engine_test_push_record(e, cJSON_Parse(t->mid_rec));
    if (t->mid_flush)
        engine_test_push_record(e, cJSON_Parse("{\"type\":\"flush\"}"));
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
    tl->v[tl->n].terminal = false;
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
    "'object','properties':{'text':{'type':'string','description':"
    "'the text to echo back'}},'required':['text']}},{'name':'boom',"
    "'inputSchema':{'type':'object'}}]}}), flush=True)\n"
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

/* the file's bytes, NUL terminated: "" when it cannot be read */
static void slurp_file(const char *path, char *got, size_t cap) {
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        n = fread(got, 1, cap - 1, f);
        fclose(f);
    }
    got[n] = '\0';
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

/* ================= 8b. builtin-mcp server ================= */

/* one tools/call round trip through the stdio loop; returns the text
   content of the reply (malloc'd) */
static char *bc_call(const char *tool, const char *args_json, bool *is_err) {
    char req[2048];
    snprintf(req, sizeof req,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
             tool, args_json);
    loop_io_t io = { 0 };
    buf_init(&io.out);
    io.in = req;
    rpc_handler_t h = { builtin_handle, NULL };
    rpc_serve(&h, loop_read, loop_write, &io);
    cJSON *r = cJSON_Parse(io.out.data);
    char *out = NULL;
    if (r) {
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "result");
        if (!res) {
            out = strdup("(rpc error)");
        } else {
            const cJSON *content =
                cJSON_GetObjectItemCaseSensitive(res, "content");
            const cJSON *blk = content ? content->child : NULL;
            const cJSON *txt = blk
                ? cJSON_GetObjectItemCaseSensitive(blk, "text") : NULL;
            out = strdup(cJSON_IsString(txt) ? txt->valuestring : "");
            if (is_err)
                *is_err = cJSON_IsTrue(
                    cJSON_GetObjectItemCaseSensitive(res, "isError"));
        }
        cJSON_Delete(r);
    }
    buf_free(&io.out);
    return out ? out : strdup("(no reply)");
}

static void test_builtin(void) {
    /* regex engine */
    char err[128] = "";
    check(builtin_regex_match("a", "./abc.txt", NULL, 0),
          "builtin: unanchored match");
    check(!builtin_regex_match("^abc", "./abc.txt", NULL, 0),
          "builtin: ^ anchors at start");
    check(builtin_regex_match("txt$", "./abc.txt", NULL, 0),
          "builtin: $ anchors at end");
    check(builtin_regex_match("b[0-9]+x", "ab123x", NULL, 0),
          "builtin: class with range");
    check(builtin_regex_match("foo|bar", "zzbarzz", NULL, 0),
          "builtin: alternation");
    check(builtin_regex_match("(ab)+c", "xxababcxx", NULL, 0),
          "builtin: group quantifier");
    check(!builtin_regex_match("a{3}", "aa", NULL, 0) &&
              builtin_regex_match("a{2,3}", "aa", NULL, 0),
          "builtin: counted repetition");
    check(builtin_regex_match("\\d+\\.\\d+", "v1.5", NULL, 0),
          "builtin: escaped digit class");
    check(builtin_regex_match("\\w+=\\w+", "k=v", NULL, 0),
          "builtin: escaped word class");
    check(builtin_regex_match("(?:ab)+c", "xxababcxx", NULL, 0),
          "builtin: non-capturing group");
    check(builtin_regex_match("[[:alpha:]]{3}", "x abc y", NULL, 0),
          "builtin: posix bracket class");
    check(!builtin_regex_match("^[^0-9]*$", "a1", NULL, 0) &&
              builtin_regex_match("^[^0-9]*$", "ab", NULL, 0),
          "builtin: negated class");
    check(builtin_regex_match("", "anything", NULL, 0),
          "builtin: empty pattern matches all");
    snprintf(err, sizeof err, "%s", "");
    check(!builtin_regex_match("[a-", "x", err, sizeof err) && err[0],
          "builtin: invalid pattern reports error");
    snprintf(err, sizeof err, "%s", "");
    check(!builtin_regex_match("a(", "x", err, sizeof err) && err[0],
          "builtin: unbalanced parenthesis rejected");
    check(builtin_regex_match("plain", "a plain match", NULL, 0),
          "builtin: literal substring");

    /* tools/list: every tool present, descriptions bundled (src/prompts) */
    {
        loop_io_t io = { 0 };
        buf_init(&io.out);
        io.in = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
        rpc_handler_t h = { builtin_handle, NULL };
        rpc_serve(&h, loop_read, loop_write, &io);
        cJSON *r = cJSON_Parse(io.out.data);
        check(r != NULL, "builtin: tools/list replies");
        const cJSON *tools = r
            ? cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetObjectItemCaseSensitive(r, "result"), "tools")
            : NULL;
        check(cJSON_IsArray(tools) && cJSON_GetArraySize(tools) == 13,
              "builtin: thirteen tools listed");
        const char *want[13] = { "web_search",   "web_fetch",   "files_list",
                                 "files_search", "file_read",   "file_create",
                                 "file_edit",    "file_analyze",
                                 "process_exec", "process_status",
                                 "process_wait", "skills_search",
                                 "skills_read" };
        bool names_ok = true, descs_set = true, args_set = true;
        if (cJSON_IsArray(tools))
            for (int i = 0; i < 13; i++) {
                const cJSON *t = cJSON_GetArrayItem(tools, i);
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(t, "name");
                if (!cJSON_IsString(nm) || strcmp(nm->valuestring, want[i]))
                    names_ok = false;
                const cJSON *d =
                    cJSON_GetObjectItemCaseSensitive(t, "description");
                if (!cJSON_IsString(d)) descs_set = false;
                const cJSON *schema =
                    cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
                const cJSON *props = cJSON_GetObjectItemCaseSensitive(
                    schema, "properties");
                if (!cJSON_IsObject(props)) args_set = false;
            }
        check(names_ok, "builtin: tool names in order");
        check(descs_set, "builtin: tool descriptions ride along");
        check(args_set, "builtin: tools declare input schemas");
        { /* the file walkers declare their optional hidden switch */
            bool hidden_ok = true;
            for (int i = 2; i <= 3; i++) {
                const cJSON *t = cJSON_GetArrayItem(tools, i);
                const cJSON *schema =
                    cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
                const cJSON *props = cJSON_GetObjectItemCaseSensitive(
                    schema, "properties");
                const cJSON *sh = cJSON_GetObjectItemCaseSensitive(
                    props, "show_hidden_files");
                const cJSON *ty = cJSON_GetObjectItemCaseSensitive(sh, "type");
                const cJSON *req =
                    cJSON_GetObjectItemCaseSensitive(schema, "required");
                if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "boolean"))
                    hidden_ok = false;
                cJSON *it = NULL;
                cJSON_ArrayForEach(it, req)
                    if (cJSON_IsString(it) &&
                        !strcmp(it->valuestring, "show_hidden_files"))
                        hidden_ok = false;
            }
            check(hidden_ok,
                  "builtin: show_hidden_files is an optional boolean");
        }
        cJSON_Delete(r);
        buf_free(&io.out);
    }

    /* file tool lifecycle */
    const char *notes = "/tmp/llmkit-test-builtin/notes.txt";
    system("rm -rf /tmp/llmkit-test-builtin");
    bool ie = false;
    char *t = bc_call("file_create",
                      "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                      "\"content\":\"alpha\\nbeta\\ngamma\\n\"}",
                      &ie);
    check(!ie && t && strstr(t, "wrote 17 bytes"), "builtin: file_create");
    free(t);

    t = bc_call("file_create",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"content\":\"x\"}", &ie);
    check(ie && t && strstr(t, "already exists"),
          "builtin: file_create refuses to overwrite");
    free(t);

    t = bc_call("file_read",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"lines_offset\":1,\"lines_length\":2}", &ie);
    check_str(t, "beta\ngamma", "builtin: file_read slice");
    free(t);

    t = bc_call("file_read",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"lines_offset\":9,\"lines_length\":1}", &ie);
    check(ie && t && strstr(t, "beyond end of file"),
          "builtin: file_read offset past end");
    free(t);

    /* edit: stated line 5, real line 2 (within tolerance 5), multi line
       replacement keeps the new block's relative indentation */
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"oldString\":\"beta\\n  gamma\",\"newString\":\"b:\\n  x\","
                "\"line_number\":5}", &ie);
    check(!ie && t && strstr(t, "replaced lines 2-3"),
          "builtin: file_edit tolerance + span");
    free(t);
    t = bc_call("file_read",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"lines_offset\":1,\"lines_length\":2}", &ie);
    check_str(t, "b:\n  x", "builtin: file_edit kept relative indent");
    free(t);

    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\","
                "\"oldString\":\"nowhere to be found\",\"newString\":\"x\","
                "\"line_number\":1}", &ie);
    check(ie && t && strstr(t, "not found"),
          "builtin: file_edit no match is an error");
    free(t);

    /* crlf endings survive an edit */
    write_file("/tmp/llmkit-test-builtin/crlf.txt",
               "crlf one\r\ncrlf two\r\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/crlf.txt\","
                "\"oldString\":\"crlf two\",\"newString\":\"second\","
                "\"line_number\":2}", &ie);
    check(!ie && t && strstr(t, "replaced line 2"),
          "builtin: file_edit in a crlf file");
    free(t);
    {
        char got[128];
        slurp_file("/tmp/llmkit-test-builtin/crlf.txt", got, sizeof got);
        check(!strcmp(got, "crlf one\r\nsecond\r\n"),
              "builtin: crlf terminators preserved");
    }

    /* the tolerance reaches five lines and stops there */
    write_file("/tmp/llmkit-test-builtin/tol.txt",
               "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/tol.txt\","
                "\"oldString\":\"one\",\"newString\":\"ONE\","
                "\"line_number\":6}", &ie);
    check(!ie && t && strstr(t, "replaced line 1"),
          "builtin: file_edit matches five lines away");
    free(t);
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/tol.txt\","
                "\"oldString\":\"one\",\"newString\":\"ONE\","
                "\"line_number\":7}", &ie);
    check(ie && t && strstr(t, "not found"),
          "builtin: file_edit stops at five lines");
    free(t);

    /* whitespace drift in an oldString still lands: the file indents with
       a tab where the argument used spaces, and doubles a space inside
       the line */
    write_file("/tmp/llmkit-test-builtin/ws.txt",
               "def f():\n\tif x:\treturn 1\n\treturn 2\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/ws.txt\","
                "\"oldString\":\"    if x: return 1\","
                "\"newString\":\"    if y: return 9\","
                "\"line_number\":2}", &ie);
    check(!ie && t && strstr(t, "replaced line 2"),
          "builtin: file_edit tolerates tab/space drift");
    free(t);
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/ws.txt\","
                "\"oldString\":\"def  f():\",\"newString\":\"def g():\","
                "\"line_number\":1}", &ie);
    check(!ie && t && strstr(t, "replaced line 1"),
          "builtin: file_edit tolerates doubled spaces");
    free(t);
    {   /* the new block lands on the file's own tab indent, its text kept
           verbatim */
        char got[128];
        slurp_file("/tmp/llmkit-test-builtin/ws.txt", got, sizeof got);
        check(!strcmp(got, "def g():\n\tif y: return 9\n\treturn 2\n"),
              "builtin: file_edit drift keeps the file's indent");
    }
    /* whitespace only: tokens that differ in shape are still no match */
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/ws.txt\","
                "\"oldString\":\"def g ():\",\"newString\":\"x\","
                "\"line_number\":1}", &ie);
    check(ie && t && strstr(t, "not found"),
          "builtin: file_edit does not glue tokens apart");
    free(t);

    /* a literal match wins over a relaxed one even when the relaxed
       candidate sits closer to line_number */
    write_file("/tmp/llmkit-test-builtin/lit.txt", "foo  bar\nfoo bar\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/lit.txt\","
                "\"oldString\":\"foo bar\",\"newString\":\"FOO\","
                "\"line_number\":1}", &ie);
    check(!ie && t && strstr(t, "replaced line 2"),
          "builtin: file_edit prefers the literal match");
    free(t);
    {
        char got[64];
        slurp_file("/tmp/llmkit-test-builtin/lit.txt", got, sizeof got);
        check(!strcmp(got, "foo  bar\nFOO\n"),
              "builtin: file_edit left the relaxed line alone");
    }

    /* blank lines padded around the block are dropped, and the replaced
       span counts the real lines only */
    write_file("/tmp/llmkit-test-builtin/pad.txt", "a\nb\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/pad.txt\","
                "\"oldString\":\" \\na\\nb\\n\\n\",\"newString\":\"A\\nB\","
                "\"line_number\":1}", &ie);
    check(!ie && t && strstr(t, "replaced lines 1-2"),
          "builtin: file_edit drops padded blank lines");
    free(t);
    {
        char got[64];
        slurp_file("/tmp/llmkit-test-builtin/pad.txt", got, sizeof got);
        check(!strcmp(got, "A\nB\n"),
              "builtin: file_edit padded block replaced whole");
    }

    /* a blank line that really is in the file stays in the replaced span */
    write_file("/tmp/llmkit-test-builtin/keep.txt", "a\n\nb\n");
    t = bc_call("file_edit",
                "{\"path\":\"/tmp/llmkit-test-builtin/keep.txt\","
                "\"oldString\":\"a\\n\\nb\",\"newString\":\"A\\n\\nB\","
                "\"line_number\":1}", &ie);
    check(!ie && t && strstr(t, "replaced lines 1-3"),
          "builtin: file_edit keeps an inner blank line");
    free(t);

    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"notes\"}", &ie);
    {   /* [permissions] [size] [path] [line count] */
        char perm[8] = "", sz[40] = "", pgot[512] = "";
        int lgot = 0;
        int nf = t ? sscanf(t, "%7s %39s %511[^,], %d lines", perm, sz, pgot,
                            &lgot) : 0;
        check(!ie && nf == 4 && !strcmp(perm, "rw-") && !strcmp(pgot, notes) &&
                  lgot == 3 && strstr(t, ", 3 lines\n"),
              "builtin: files_list line format");
    }
    free(t);

    /* files_list holds at the directory's own entries unless asked to
       descend */
    t = bc_call("file_create",
                "{\"path\":\"/tmp/llmkit-test-builtin/sub/deep.txt\","
                "\"content\":\"deep\\n\"}", &ie);
    check(!ie && t, "builtin: file_create nested parent dirs");
    free(t);
    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"deep\"}", &ie);
    check(!ie && t && !strstr(t, "deep.txt"),
          "builtin: files_list stays on the top level by default");
    free(t);
    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"deep\","
                              "\"recurse_subdirectories\":true}", &ie);
    check(!ie && t && strstr(t, "sub/deep.txt"),
          "builtin: files_list descends when asked");
    free(t);
    /* the line count sits at the end of the line, and a directory (no
       line count to give) ends at its path */
    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"sub$|notes\"}", &ie);
    check(!ie && t && strstr(t, "/sub\n") && !strstr(t, "sub,") &&
              strstr(t, "notes.txt, 3 lines\n"),
          "builtin: files_list names the line count last");
    free(t);

    t = bc_call("files_search", "{\"path\":\"/tmp/llmkit-test-builtin\","
                                "\"regex\":\"alpha|x$\"}", &ie);
    check(!ie && t && strstr(t, "notes.txt") &&
              strstr(t, "Matching lines: 1, 3"),
          "builtin: files_search reports matching line numbers");
    free(t);

    /* dot-prefixed entries: out of the walk by default, in when asked */
    write_file("/tmp/llmkit-test-builtin/.hidden.txt", "secret\n");
    t = bc_call("file_create",
                "{\"path\":\"/tmp/llmkit-test-builtin/.hiddendir/deep.txt\","
                "\"content\":\"secret\\n\"}", &ie);
    check(!ie && t, "builtin: file_create tucks a file in a dot dir");
    free(t);
    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"hidden\","
                              "\"recurse_subdirectories\":true}", &ie);
    check(!ie && t && !strstr(t, "hidden"),
          "builtin: files_list leaves dot entries out by default");
    free(t);
    t = bc_call("files_list",
                "{\"path\":\"/tmp/llmkit-test-builtin\","
                "\"regex\":\"hidden\",\"show_hidden_files\":true,"
                "\"recurse_subdirectories\":true}", &ie);
    check(!ie && t && strstr(t, ".hidden.txt") && strstr(t, "/.hiddendir\n") &&
              strstr(t, ".hiddendir/deep.txt"),
          "builtin: files_list lists dot entries when asked");
    free(t);
    /* the root the caller names is never filtered, only what it holds */
    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin/.hiddendir\","
                              "\"regex\":\"deep\"}", &ie);
    check(!ie && t && strstr(t, "deep.txt"),
          "builtin: files_list honors an explicit dot root");
    free(t);
    t = bc_call("files_search", "{\"path\":\"/tmp/llmkit-test-builtin\","
                                "\"regex\":\"^secret$\"}", &ie);
    check(!ie && t && !strstr(t, "hidden"),
          "builtin: files_search skips dot files by default");
    free(t);
    t = bc_call("files_search",
                "{\"path\":\"/tmp/llmkit-test-builtin\","
                "\"regex\":\"^secret$\",\"show_hidden_files\":true}", &ie);
    check(!ie && t && strstr(t, ".hidden.txt") &&
              strstr(t, ".hiddendir/deep.txt"),
          "builtin: files_search reads dot files when asked");
    free(t);

    t = bc_call("files_list", "{\"path\":\"/tmp/llmkit-test-builtin\","
                              "\"regex\":\"[a-\"}", &ie);
    check(ie && t && strstr(t, "invalid regex"),
          "builtin: bad regex is an error");
    free(t);

    /* file_analyze: a file's structure, one line per element - comments
       and string literals must not look like structure */
    write_file("/tmp/llmkit-test-builtin/shape.c",
               "/* not a struct: class Point { */\n"
               "#include <stdio.h>\n"
               "\n"
               "typedef struct point {\n"
               "    int x;\n"
               "    char *name;\n"
               "} point_t;\n"
               "\n"
               "static int helper(int a) {\n"
               "    char *s = \"struct not_a_struct {\";\n"
               "    return a + 1;\n"
               "}\n");
    t = bc_call("file_analyze",
                "{\"path\":\"/tmp/llmkit-test-builtin/shape.c\"}", &ie);
    check(!ie && t && strstr(t, "  [c]  12 lines\n") &&
              strstr(t, "struct point\n") && strstr(t, "int x\n") &&
              strstr(t, "char *name\n") &&
              strstr(t, "static int helper(int a)\n") &&
              !strstr(t, "class Point") && !strstr(t, "not_a_struct"),
          "builtin: file_analyze c aggregates, members, signatures");
    free(t);

    write_file("/tmp/llmkit-test-builtin/doc.md",
               "# Title\n"
               "\n"
               "text\n"
               "\n"
               "## Section ###\n"
               "\n"
               "```\n"
               "# not a heading\n"
               "```\n"
               "\n"
               "### Sub\n");
    t = bc_call("file_analyze",
                "{\"path\":\"/tmp/llmkit-test-builtin/doc.md\"}", &ie);
    check(!ie && t && strstr(t, "  [markdown]  11 lines\n") &&
              strstr(t, "Title\n") && strstr(t, "Section\n") &&
              strstr(t, "Sub\n") && !strstr(t, "not a heading"),
          "builtin: file_analyze markdown heading tree");
    free(t);

    write_file("/tmp/llmkit-test-builtin/mod.py",
               "class Foo(Base):\n"
               "    def __init__(self, a):\n"
               "        pass\n"
               "\n"
               "async def main():\n"
               "    pass\n");
    t = bc_call("file_analyze",
                "{\"path\":\"/tmp/llmkit-test-builtin/mod.py\"}", &ie);
    check(!ie && t && strstr(t, "  [python]  6 lines\n") &&
              strstr(t, "class Foo(Base)\n") &&
              strstr(t, "def __init__(self, a)\n") &&
              strstr(t, "async def main()\n"),
          "builtin: file_analyze python classes and defs");
    free(t);

    write_file("/tmp/llmkit-test-builtin/plain.md", "just text\n");
    t = bc_call("file_analyze",
                "{\"path\":\"/tmp/llmkit-test-builtin/plain.md\"}", &ie);
    check(!ie && t && strstr(t, "(nothing found)"),
          "builtin: file_analyze empty structure");
    free(t);

    t = bc_call("file_analyze",
                "{\"path\":\"/tmp/llmkit-test-builtin/notes.txt\"}", &ie);
    check(ie && t && strstr(t, "unknown file type") &&
              strstr(t, "markdown") && strstr(t, "python"),
          "builtin: file_analyze refuses an unknown type");
    free(t);

    t = bc_call("file_analyze", "{\"path\":\"/tmp/llmkit-test-builtin\"}",
                &ie);
    check(ie && t && strstr(t, "is a directory"),
          "builtin: file_analyze refuses a directory");
    free(t);

    t = bc_call("file_analyze", "{}", &ie);
    check(ie && t && strstr(t, "missing required argument 'path'"),
          "builtin: file_analyze needs a path");
    free(t);

    /* process tools: completed reply, timestamps, tail, temp file */
    t = bc_call("process_exec", "{\"cmdline\":\"echo one; echo two\"}", &ie);
    check(!ie && t && strstr(t, "Exit code: 0\n") &&
              strstr(t, "Last three output lines:\n") &&
              strstr(t, ": one\n") && strstr(t, ": two\n") &&
              !strstr(t, "still running") &&
              !strstr(t, "Process output available"),
          "builtin: process_exec completed reply with stamped lines");
    free(t);

    t = bc_call("process_exec",
                "{\"cmdline\":\"echo a; echo b; echo c; echo d; echo e\"}",
                &ie);
    check(!ie && t && strstr(t, "Exit code: 0") &&
              strstr(t, "Process output available in") &&
              strstr(t, "(currently 5 lines)") &&
              !strstr(t, ": a\n") && strstr(t, ": d\n") && strstr(t, ": e\n"),
          "builtin: process_exec quotes the tail, names the temp file");
    {   /* the temp file holds all five stamped lines */
        char path[512] = "";
        char *pin = t ? strstr(t, "Process output available in ") : NULL;
        if (pin)
            sscanf(pin + 27, "%511s", path);
        FILE *pf = path[0] ? fopen(path, "r") : NULL;
        unsigned lines = 0;
        bool stamped = true;
        if (pf) {
            char lb[128];
            while (fgets(lb, sizeof lb, pf)) {
                lines++;
                if (!(lb[2] == ':' && lb[5] == ':' && lb[8] == ':' &&
                      lb[0] >= '0'))
                    stamped = false;
            }
            fclose(pf);
        }
        check(pf && lines == 5 && stamped,
              "builtin: process temp file has all stamped lines");
    }
    free(t);

    t = bc_call("process_exec", "{\"cmdline\":\"exit 7\"}", &ie);
    check(!ie && t && strstr(t, "Exit code: 7"),
          "builtin: process_exec exit code");
    free(t);

    t = bc_call("process_exec", "{\"cmdline\":\"\"}", &ie);
    check(ie && t && strstr(t, "must not be empty"),
          "builtin: process_exec empty cmdline is an error");
    free(t);

    /* foreign pids: state only, no output. the test process itself is
       running, 999999999 is gone, 0 is not a process id */
    char fown[64];
    snprintf(fown, sizeof fown, "{\"pid\":%ld}", (long)getpid());
    t = bc_call("process_status", fown, &ie);
    check(!ie && t && strstr(t, "is still running") &&
              strstr(t, "not spawned by this server") &&
              !strstr(t, "Last three output lines"),
          "builtin: process_status of a foreign running pid");
    free(t);

    t = bc_call("process_status", "{\"pid\":999999999}", &ie);
    check(!ie && t && strstr(t, "is not running") &&
              strstr(t, "not spawned by this server"),
          "builtin: process_status of a foreign dead pid");
    free(t);

    t = bc_call("process_status", "{\"pid\":0}", &ie);
    check(ie && t && strstr(t, "positive process id"),
          "builtin: process_status of pid 0 is an error");
    free(t);

    /* the 10s wait ends with the running reply; process_status then
       follows the same pid to completion */
    t = bc_call("process_exec", "{\"cmdline\":\"echo started; sleep 11\"}",
                &ie);
    check(!ie && t && strstr(t, "is still running") &&
              strstr(t, "Use process_status to monitor it.") &&
              strstr(t, ": started\n"),
          "builtin: process_exec 10s wait reports still running");
    long pid = -1;
    if (t) {
        char *pp = strstr(t, "PID ");
        if (pp) pid = strtol(pp + 4, NULL, 10);
    }
    free(t);
    check(pid > 0, "builtin: running reply carries the pid");
    if (pid > 0) {
        char pargs[64];
        snprintf(pargs, sizeof pargs, "{\"pid\":%ld}", pid);
        t = bc_call("process_status", pargs, &ie);
        check(!ie && t && strstr(t, "is still running"),
              "builtin: process_status of a running pid");
        free(t);
        bool done = false;
        char *fin = NULL;
        for (int i = 0; i < 10 && !done; i++) {
            msleep(500);
            fin = bc_call("process_status", pargs, &ie);
            if (!ie && fin && strstr(fin, "Exit code: 0")) done = true;
            else free(fin), fin = NULL;
        }
        check(done && fin && strstr(fin, "Exit code: 0") &&
                  strstr(fin, ": started\n"),
              "builtin: process_status sees the completion");
        free(fin);
    }

    /* process_wait: a bad timeout is an error, then a live pid shows
       both ends - the timeout expiring while it runs, and the
       termination beating a longer one */
    t = bc_call("process_wait", "{\"pid\":999999999,\"timeout\":-1}", &ie);
    check(ie && t && strstr(t, "timeout must be between"),
          "builtin: process_wait out-of-range timeout is an error");
    free(t);

    t = bc_call("process_wait", "{\"pid\":0,\"timeout\":1}", &ie);
    check(ie && t && strstr(t, "positive process id"),
          "builtin: process_wait of pid 0 is an error");
    free(t);

    /* a foreign pid this time: forked here, never through process_exec
       - the wait sees it running, then gone, with no output */
    pid_t fpid = fork();
    if (fpid == 0) { /* quiet for longer than the first timeout */
        sleep(3);
        _exit(0);
    }
    check(fpid > 0, "builtin: process_wait foreign pid setup");
    if (fpid > 0) {
        char fargs[64];
        snprintf(fargs, sizeof fargs, "{\"pid\":%d,\"timeout\":1}", (int)fpid);
        t = bc_call("process_wait", fargs, &ie);
        check(!ie && t && strstr(t, "is still running") &&
                  strstr(t, "not spawned by this server") &&
                  !strstr(t, "Last three output lines"),
              "builtin: process_wait foreign pid timeout expiry");
        free(t);
        kill(fpid, SIGKILL);
        waitpid(fpid, NULL, 0); /* reaped, so no zombie masks the exit */
        snprintf(fargs, sizeof fargs, "{\"pid\":%d,\"timeout\":30}", (int)fpid);
        t = bc_call("process_wait", fargs, &ie);
        check(!ie && t && strstr(t, "is not running") &&
                  strstr(t, "not spawned by this server"),
              "builtin: process_wait foreign pid termination");
        free(t);
    }

    t = bc_call("process_exec", "{\"cmdline\":\"echo waiting; sleep 12\"}",
                &ie);
    check(!ie && t && strstr(t, "is still running"),
          "builtin: process_wait setup: a pid left running");
    pid = -1;
    if (t) {
        char *pp = strstr(t, "PID ");
        if (pp) pid = strtol(pp + 4, NULL, 10);
    }
    free(t);
    if (pid > 0) {
        char pargs[64];
        snprintf(pargs, sizeof pargs, "{\"pid\":%ld,\"timeout\":1}", pid);
        t = bc_call("process_wait", pargs, &ie);
        check(!ie && t && strstr(t, "is still running") &&
                  strstr(t, ": waiting\n"),
              "builtin: process_wait timeout expiry reports still running");
        free(t);
        snprintf(pargs, sizeof pargs, "{\"pid\":%ld,\"timeout\":30}", pid);
        t = bc_call("process_wait", pargs, &ie);
        check(!ie && t && strstr(t, "Exit code: 0") &&
                  strstr(t, ": waiting\n") && !strstr(t, "still running"),
              "builtin: process_wait sees the termination");
        free(t);
    }

    /* skills tools: fixed roots (<cwd>/.agents/skills, $HOME/.agents/
       skills), so the test chdirs into a scratch tree and overrides HOME */
    {
        system("rm -rf /tmp/llmkit-test-skills");
        system("mkdir -p /tmp/llmkit-test-skills/cwd/.agents/skills/beta "
               "/tmp/llmkit-test-skills/home/.agents/skills/alpha");
        write_file("/tmp/llmkit-test-skills/home/.agents/skills/alpha/SKILL.md",
                   "---\nname: alpha\ndescription: the alpha skill\n---\n\n"
                   "body mentions zebra once\n");
        write_file("/tmp/llmkit-test-skills/cwd/.agents/skills/beta/SKILL.md",
                   "---\nname: beta\ndescription: >-\n  the beta skill\n"
                   "---\n\nzebra zebra zebra\n");
        char cwdbuf[4096];
        const char *oldcwd =
            getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : "/tmp";
        const char *oldhome = getenv("HOME");
        chdir("/tmp/llmkit-test-skills/cwd");
        setenv("HOME", "/tmp/llmkit-test-skills/home", 1);

        t = bc_call("skills_search", "{\"keywords\":\"zebra\"}", &ie);
        check(!ie && t && strstr(t, "name: beta") && strstr(t, "name: alpha") &&
                  strstr(t, "description: the beta skill") &&
                  strstr(t, "description: the alpha skill") &&
                  strstr(t, "name: beta") < strstr(t, "name: alpha"),
              "builtin: skills_search orders by match count");
        free(t);

        t = bc_call("skills_search", "{\"keywords\":\"  \"}", &ie);
        check(ie && t && strstr(t, "keywords must not be empty"),
              "builtin: skills_search empty keywords is an error");
        free(t);

        t = bc_call("skills_search", "{\"keywords\":\"nomatch\"}", &ie);
        check(!ie && t && !strcmp(t, "no matching skills"),
              "builtin: skills_search without matches");
        free(t);

        t = bc_call("skills_read", "{\"name\":\"alpha\"}", &ie);
        check(!ie && t && strstr(t, "zebra once") &&
                  strstr(t, "description: the alpha skill"),
              "builtin: skills_read by frontmatter name");
        free(t);

        /* the local root shadows the global one on a name clash */
        system("mkdir -p /tmp/llmkit-test-skills/cwd/.agents/skills/alpha");
        write_file("/tmp/llmkit-test-skills/cwd/.agents/skills/alpha/SKILL.md",
                   "local alpha body\n");
        t = bc_call("skills_read", "{\"name\":\"alpha\"}", &ie);
        check(!ie && t && !strcmp(t, "local alpha body\n"),
              "builtin: skills_read prefers the local root");
        free(t);

        t = bc_call("skills_read", "{\"name\":\"missing\"}", &ie);
        check(ie && t && strstr(t, "no skill named"),
              "builtin: skills_read unknown name is an error");
        free(t);

        if (oldhome)
            setenv("HOME", oldhome, 1);
        else
            unsetenv("HOME");
        chdir(oldcwd);
        system("rm -rf /tmp/llmkit-test-skills");
    }

    /* duckduckgo parser on a canned results page */
    {
        const char *ddg =
            "<html><body><div class=\"result results_links\">"
            "<h2><a rel=\"nofollow\" class=\"result__a\" "
            "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fone"
            "&amp;rut=abc\">Example One</a></h2>"
            "<a class=\"result__snippet\" href=\"//r\">The <b>first</b> "
            "snippet</a></div>"
            "<div class=\"result\"><h2><a class=\"result__a\" "
            "href=\"https://example.com/two\">Example Two</a></h2></div>"
            "</body></html>";
        char *out = builtin_ddg_results(ddg, strlen(ddg));
        check_str(out,
                  "URL: https://example.com/one\n"
                  "Description: The first snippet\n"
                  "\n"
                  "URL: https://example.com/two\n"
                  "Description: Example Two\n",
                  "builtin: ddg parse, redirect unwrap, snippet pairing");
        free(out);
    }

    /* readability + markdown conversion on a canned page */
    {
        const char *page =
            "<html><head><title>Demo Page</title></head><body>"
            "<nav>nav junk</nav>"
            "<div id=\"content\" class=\"article\">"
            "<h1>Head One</h1>"
            "<p>Intro with a <a href=\"https://e.com/d\">dest</a> and "
            "<strong>bold</strong> text.</p>"
            "<ul><li>one</li><li>two</li></ul>"
            "<pre><code>line1\n  line2</code></pre>"
            "<table><tr><th>a</th><th>b</th></tr>"
            "<tr><td>1</td><td>2</td></tr></table>"
            "</div><div class=\"footer\">footer junk</div></body></html>";
        char *md = builtin_html_to_markdown(page, strlen(page));
        check(md && strstr(md, "# Demo Page") &&
              strstr(md, "# Head One") &&
              strstr(md, "[dest](https://e.com/d)") &&
              strstr(md, "**bold**") &&
              strstr(md, "- one") && strstr(md, "- two") &&
              strstr(md, "```\nline1\n  line2\n```") &&
              strstr(md, "| a | b |") && strstr(md, "| 1 | 2 |"),
              "builtin: html to markdown structure");
        check(md && !strstr(md, "nav junk") && !strstr(md, "footer junk"),
              "builtin: chrome stripped");
        free(md);
    }
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
    "MID = len(sys.argv) > 2 and sys.argv[2] == 'interleave'\n"
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
    "                chunks = [{'choices':[{'delta':{'content':'all '}}]}]\n"
    "                if MID:\n"
    "                    chunks.append({'choices':[{'delta':"
    "{'reasoning_content':'mid-thought'}}]})\n"
    "                chunks += [\n"
    "                  {'choices':[{'delta':{'content':'done'}}]},\n"
    "                  {'choices':[{'delta':{},'finish_reason':'stop'}]},\n"
    "                  {'usage':{'prompt_tokens':20,'completion_tokens':7}},\n"
    "                ]\n"
    "                sse(self, chunks)\n"
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

/* agent-as-tool with streaming on and a zero flush interval: the answer
   arrives as chunked partials plus a block-final that carries only the
   remainder since the last flush - the invoke reply must be the whole
   response block, and text streamed before the tool round ("calling
   now") must not leak into it. The "interleave" mode additionally
   slips a reasoning delta between the content chunks: thinking in the
   middle must not end the collected answer either */
static void agent_stream_case(const char *mode, const char *label,
                            const char *leak_label) {
    spawn_t sp;
    char url[128];
    if (!endpoint_spawn_mode(&sp, "-", mode, url, sizeof url)) {
        check(false, "agent endpoint spawned (mode)");
        return;
    }
    check(true, "agent endpoint spawned (mode)");
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);

    char seed[1024];
    snprintf(seed, sizeof seed,
             "{\"type\":\"header\",\"version\":1}\n"
             "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
             "\"api_base\":\"%s\",\"model\":\"m\",\"inference_options\":"
             "{\"stream\":true}}\n"
             "{\"type\":\"options\",\"stream_interval\":0}\n"
             "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":"
             "\"fs\",\"command_line\":\"python3 "
             "/tmp/llmkit-test-fake-mcp.py\"}]}\n"
             "{\"type\":\"agent-as-tool\",\"tool_description\":\"the agent\","
             "\"input_description\":\"the input\"}\n",
             url);
    write_file("/tmp/llmkit-test-agent-stream-seed.jsonl", seed);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) {
        check(false, "streamed agent pipes");
        spawn_kill(&sp);
        return;
    }
    pid_t pid = fork();
    check(pid >= 0, "streamed agent fork");
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        int rc = cmd_agent("/tmp/llmkit-test-agent-stream-seed.jsonl");
        _exit(rc == 0 ? 0 : 1);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    FILE *to = fdopen(in_pipe[1], "w");
    FILE *from = fdopen(out_pipe[0], "r");
    char line[4096];

    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{}}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "llmkit-agent-as-tool") != NULL,
          "agent-as-tool: initialize reply (streamed)");

    fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                "\"params\":{\"name\":\"invoke\",\"arguments\":"
                "{\"input\":\"go\"}}}\n");
    fflush(to);
    check(fgets(line, sizeof line, from) != NULL &&
              strstr(line, "all done") != NULL &&
              strstr(line, "isError") == NULL,
          label);
    check(strstr(line, "calling now") == NULL &&
              strstr(line, "thinking hard") == NULL &&
              strstr(line, "mid-thought") == NULL,
          leak_label);

    fclose(to);
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "agent-as-tool: exits 0 (streamed)");
    fclose(from);
    spawn_kill(&sp);
}

static void test_agent_tool_streamed(void) {
    agent_stream_case(NULL,
        "streamed agent: invoke replies with the whole block",
        "streamed agent: non-answer text does not leak");
    agent_stream_case("interleave",
        "interleaved agent: invoke replies with the whole block",
        "interleaved agent: non-answer text does not leak");
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

    /* --reasoning-effort: beside --max-tokens, and alone */
    {
        char *av[] = {"--openai", "http://x", "--max-tokens", "512",
                      "--reasoning-effort", "high", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err,
                         sizeof err) == 0,
              "call: effort vector parses");
        cJSON *llm = call_build_llm(&c);
        buf_init(&b);
        buf_append_tree(&b, llm);
        cJSON_Delete(llm);
        check_str(b.data,
                  "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                  "\"api_base\":\"http://x\",\"inference_options\":"
                  "{\"max_tokens\":512,\"reasoning_effort\":\"high\"}}",
                  "call: effort serializes beside max_tokens");
        buf_free(&b);
        call_cfg_free(&c);
    }
    {
        /* the value is not constrained: providers differ in what they
           accept, any string compiles (requirements sec.4) */
        char *av[] = {"--openai-responses", "http://x",
                      "--reasoning-effort", "banana-42", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err,
                         sizeof err) == 0,
              "call: unconstrained effort value parses");
        cJSON *llm = call_build_llm(&c);
        buf_init(&b);
        buf_append_tree(&b, llm);
        cJSON_Delete(llm);
        check_str(b.data,
                  "{\"type\":\"llm\",\"endpoint_protocol\":"
                  "\"openai_responses\",\"api_base\":\"http://x\","
                  "\"inference_options\":"
                  "{\"reasoning_effort\":\"banana-42\"}}",
                  "call: effort alone creates inference_options");
        buf_free(&b);
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
        char *e13[] = {"--openai", "http://x", "--reasoning-effort",
                       "--prompt", "p"};
        check(call_parse(5, e13, &c, err, sizeof err) == 1,
              "call: missing --reasoning-effort value");
        char *e14[] = {"--openai", "http://x", "--reasoning-effort", "low",
                       "--reasoning-effort", "high", "--prompt", "p"};
        check(call_parse(8, e14, &c, err, sizeof err) == 1,
              "call: --reasoning-effort given twice");
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
    {
        char *av[] = {"--openai", "http://x", "--reasoning-effort", "\xff",
                      "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err, sizeof err) == 0,
              "call: bad-utf8 effort vector parses");
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FILE *out = open_memstream(&ob, &on);
        FILE *er = open_memstream(&eb, &en);
        check(call_run(&c, out, er, "/x", NULL) == EXIT_INVALID_RECORD,
              "call: invalid UTF-8 in --reasoning-effort is invalid_record");
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

/* ================= terminal tools ================= */

static void add_tool_term(engine_t *e, const char *name) {
    add_tool(e, name, "terminal tool");
    mcp_mgr_t *m = (mcp_mgr_t *)e->mcp;
    m->listing.v[m->listing.n - 1].terminal = true;
}

/* the repl tool vector: ftool_exec on the engine repl_run builds */
static wire_t *repl_ftool_factory(engine_t *e) {
    e->tool_exec = ftool_exec;
    return g_factory_wire;
}

/* the repl terminal vector: mark a.t1 on the engine repl_run builds */
static wire_t *repl_term_factory(engine_t *e) {
    add_tool_term(e, "a.t1");
    e->tool_exec = ftool_exec;
    return g_factory_wire;
}

/* a terminal-marked listing entry for the scripted run helper */
static wire_t *term_script_factory(engine_t *e) {
    add_tool_term(e, "fs.echo");
    e->tool_exec = ftool_exec;
    return g_factory_wire;
}


/* ================= repl (design sec.12) ================= */

/* feed a script through a pipe: repl's non-tty input channel */
static int pipe_feed(const char *data, size_t n) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fds[1], data + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fds[1]);
    return fds[0];
}

static void golden_rule(buf_t *b, char glyph) {
    /* the golden token of a stamped rule: the live line carries the
       wall clock and re-measures its width, both normalized away */
    buf_append_str(b, glyph == '=' ? "@=\n" : "@-\n");
}

/* the golden token of the timing line: measured spans, normalized away */
static void golden_timing(buf_t *b) { buf_append_str(b, "@t\n"); }

/* the live rendering's "[HH:MM:SS] " line prefix */
static bool stamped_line(const char *p, size_t len) {
    if (len < 12 || p[0] != '[' || p[9] != ']' || p[10] != ' ') return false;
    for (int i = 1; i <= 8; i++) {
        if (i == 3 || i == 6) {
            if (p[i] != ':') return false;
        } else if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

/* rewrite the nondeterministic display lines of a repl session - the
   stamped rules and the timing line - into the golden tokens, so the
   goldens stay byte-exact against the rendering contract */
static char *repl_norm(const char *ob) {
    buf_t b;
    buf_init(&b);
    const char *p = ob ? ob : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (stamped_line(p, len)) {
            const char *rest = p + 11;
            size_t rlen = len - 11;
            char g = rlen ? rest[0] : 0;
            bool rule = rlen > 0 && (g == '=' || g == '-');
            for (size_t i = 1; i < rlen && rule; i++)
                if (rest[i] != g) rule = false;
            if (rule) golden_rule(&b, g);
            else if (rlen > 12 && !memcmp(rest, "first token ", 12))
                golden_timing(&b);
            else buf_append(&b, p, len + (eol ? 1 : 0));
        } else {
            buf_append(&b, p, len + (eol ? 1 : 0));
        }
        p = eol ? eol + 1 : p + len;
    }
    return buf_steal(&b, NULL);
}

/* a minimal cfg: openai, no key/model, anthropic needs max_tokens */
static void repl_cfg(call_cfg_t *c, int proto) {
    memset(c, 0, sizeof *c);
    c->protocol = proto;
    c->api_base = strdup("http://x");
    c->max_tokens = proto == PROTO_ANTHROPIC ? 512 : -1;
}

typedef struct repl_out {
    char *ob;
    size_t on;
    int rc;
} repl_out_t;

static repl_out_t repl_session(call_cfg_t *c, const char *script,
                               size_t sn) {
    repl_out_t r = { NULL, 0, 0 };
    int fd = pipe_feed(script, sn);
    check(fd >= 0, "repl: pipe feed");
    char *ob = NULL;
    size_t on = 0;
    FILE *out = open_memstream(&ob, &on);
    r.rc = repl_run(c, fd, out, "/x", script_factory);
    fclose(out);
    close(fd);
    r.ob = ob;
    r.on = on;
    return r;
}

static repl_out_t repl_session_ft(call_cfg_t *c, const char *script,
                                  size_t sn) {
    repl_out_t r = { NULL, 0, 0 };
    int fd = pipe_feed(script, sn);
    char *ob = NULL;
    size_t on = 0;
    FILE *out = open_memstream(&ob, &on);
    r.rc = repl_run(c, fd, out, "/x", repl_ftool_factory);
    fclose(out);
    close(fd);
    r.ob = ob;
    r.on = on;
    return r;
}

static void repl_session_free(repl_out_t *r) { free(r->ob); }

static void test_repl(void) {
    call_cfg_t c;
    char err[256];

    /* ---- compilation vectors ---- */
    {
        char *av[] = {"--openai", "http://x", "--prompt", "hi"};
        call_cfg_t t;
        check(call_parse_ex(4, av, &t, err, sizeof err, false) == 1,
              "repl: --prompt is a usage error on the repl surface");
        check(strstr(err, "unknown flag '--prompt'") != NULL,
              "repl: --prompt reported as unknown");
        check(call_parse_ex(4, av, &t, err, sizeof err, true) == 0,
              "repl: call_parse_ex still accepts --prompt for call");
        call_cfg_free(&t);
    }
    {
        cJSON *o = repl_build_options();
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, o);
        check_str(b.data ? b.data : "",
                  "{\"type\":\"options\",\"stream_interval\":0}",
                  "repl: the one compiled options record");
        buf_free(&b);
        cJSON_Delete(o);
    }
    {
        char *av[] = {"http://x"};
        call_cfg_t t;
        check(call_parse_ex(1, av, &t, err, sizeof err, false) == 1,
              "repl: missing protocol flag is a usage error");
        check(call_parse_ex(0, NULL, &t, err, sizeof err, false) == 1,
              "repl: missing api_base is a usage error");
    }

    /* ---- session: rendering golden bytes, off-tty plain ---- */
    {
        /* the wire's real shape: text in the partials, empty-text
           block-close finals carrying signature/usage only - a stale
           armed separator must not leak into the next user block */
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"thinking\",\"text\":\"th\",\"partial\":true}",
                  "{\"type\":\"thinking\",\"text\":\"ink\",\"partial\":true}",
                  "{\"type\":\"response\",\"text\":\"He\",\"partial\":true}",
                  "{\"type\":\"response\",\"text\":\"llo\",\"partial\":true}",
                  "{\"type\":\"thinking\",\"text\":\"\",\"partial\":false,"
                  "\"signature\":\"s\"}",
                  "{\"type\":\"response\",\"text\":\"\",\"partial\":false,"
                  "\"finish_reason\":\"stop\"}"},
              .nrecs = 6, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "hi\nagain\n", 9);
        buf_t want;
        buf_init(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "hi\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "think\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "Hello\n");
        golden_timing(&want); /* the response completed: spans line */
        golden_rule(&want, '=');
        buf_append_str(&want, "again\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "think\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "Hello\n");
        golden_timing(&want);
        check(r.rc == EXIT_OK, "repl: clean session exits 0 at EOF");
        char *norm = repl_norm(r.ob);
        check_str(norm, want.data, "repl: user block, thinking, answer");
        free(norm);
        check(strchr(r.ob, '\033') == NULL,
              "repl: off-tty styling fallback: no escape sequences");
        buf_free(&want);
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- system prompt: bundled default, explicit override, empty ---- */
    check_str(g_seen_system, prompt_system_prompts_repl,
              "repl: the bundled default prompt is compiled in");
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"ok\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        c.system = strdup("custom system text");
        repl_out_t r = repl_session(&c, "hi\n", 3);
        check(r.rc == EXIT_OK, "repl: override session exits 0");
        check_str(g_seen_system, "custom system text",
                  "repl: --system-prompt replaces the bundled default");
        repl_session_free(&r);
        call_cfg_free(&c);

        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        c.system = strdup("");
        r = repl_session(&c, "hi\n", 3);
        check(r.rc == EXIT_OK, "repl: empty-prompt session exits 0");
        check_str(g_seen_system, "",
                  "repl: an explicit empty text beats the bundled default");
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- tool traffic rendered: name+args, full response ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"checking\","
                  "\"partial\":false}",
                  "{\"type\":\"tool_request\",\"tool\":\"fs.echo\","
                  "\"arguments\":{\"x\":1},\"id\":\"c1\"}"},
              .nrecs = 2, .kind = TURN_TOOLS, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"done\","
                  "\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        ftool_t tools[] = { { .tool = "fs.echo", .text = "tool out", .rc = 0 } };
        g_ftools = (ftool_script_t){ tools, 1, 0 };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session_ft(&c, "hi\n", 3);
        buf_t want;
        buf_init(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "hi\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "checking\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "fs.echo {\"x\":1}\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "tool out\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "done\n");
        golden_timing(&want); /* once per turn, after the last block */
        check(r.rc == EXIT_OK, "repl: tool session exits 0");
        char *norm = repl_norm(r.ob);
        check_str(norm, want.data, "repl: tool call and response blocks");
        free(norm);
        buf_free(&want);
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- timing line: the turn's token totals, summed over rounds ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"checking\","
                  "\"partial\":false}",
                  "{\"type\":\"tool_request\",\"tool\":\"fs.echo\","
                  "\"arguments\":{\"x\":1},\"id\":\"c1\","
                  "\"usage\":{\"input_tokens\":100,\"output_tokens\":20}}"},
              .nrecs = 2, .kind = TURN_TOOLS, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"done\","
                  "\"partial\":false,"
                  "\"usage\":{\"input_tokens\":200,\"output_tokens\":30}}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        ftool_t tools[] = { { .tool = "fs.echo", .text = "tool out", .rc = 0 } };
        g_ftools = (ftool_script_t){ tools, 1, 0 };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session_ft(&c, "hi\n", 3);
        check(r.rc == EXIT_OK, "repl: token session exits 0");
        check(strstr(r.ob, "| input 300 tok | output 50 tok "
                           "| total 350 tok") != NULL,
              "repl: timing line sums the turn's usage over its rounds");
        repl_session_free(&r);
        call_cfg_free(&c);

        /* the same shape without usage: spans alone, no token segment */
        fturn_t plain[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"ok\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(plain, 1);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session(&c, "hi\n", 3);
        check(r.rc == EXIT_OK, "repl: usage-less session exits 0");
        check(strstr(r.ob, "| input ") == NULL,
              "repl: no usage reported renders no token segment");
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- empty input ignored: no user record, no turn ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"one\","
                  "\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"two\","
                  "\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "a\n\nb\n", 4);
        check(r.rc == EXIT_OK, "repl: empty-input session exits 0");
        check(strstr(r.ob, "one") != NULL && strstr(r.ob, "two") != NULL,
              "repl: two turns ran");
        {
            /* two user blocks, one timing line per completed turn */
            char *norm = repl_norm(r.ob);
            int rules = 0, timings = 0;
            for (const char *q = norm; (q = strstr(q, "@=\n")) != NULL; q++)
                rules++;
            for (const char *q = norm; (q = strstr(q, "@t\n")) != NULL; q++)
                timings++;
            check(rules == 2,
                  "repl: the empty line ran no turn (two user blocks)");
            check(timings == 2, "repl: one timing line per turn");
            free(norm);
        }
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- SIGINT mid turn: abort, session continues ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"par\",\"partial\":true}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = 1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"ok\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "first\nagain\n", 12);
        buf_t want;
        buf_init(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "first\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "par\n");
        buf_append_str(&want,
                       "! interrupted: conversation stopped externally\n");
        golden_rule(&want, '=');
        buf_append_str(&want, "again\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "ok\n");
        golden_timing(&want); /* no timing on the interrupted turn */
        check(r.rc == EXIT_OK,
              "repl: interrupt then continue: EOF exits with the last "
              "ending");
        char *norm = repl_norm(r.ob);
        check_str(norm, want.data, "repl: interrupted turn then continuation");
        free(norm);
        buf_free(&want);
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- EOF right after an interrupt exits 8; empty pipe exits 0 ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"par\",\"partial\":true}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = 0 },
        };
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "one\n", 4);
        check(r.rc == EXIT_INTERRUPTED,
              "repl: EOF after an interrupted turn exits 8");
        check(strstr(r.ob, "! interrupted:") != NULL,
              "repl: the interrupted error line rendered");
        repl_session_free(&r);
        call_cfg_free(&c);

        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session(&c, "", 0);
        check(r.rc == EXIT_OK, "repl: EOF before any input exits 0");
        check(r.on == 0, "repl: EOF before any input renders nothing");
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- fatal error ends the session with its code ---- */
    {
        fturn_t turns[] = {
            { .recs = NULL, .nrecs = 0, .kind = TURN_FATAL,
              .fatal_json = "{\"type\":\"error\",\"code\":\"api_error\","
                            "\"message\":\"boom\",\"fatal\":true}",
              .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "q\nmore\n", 7);
        check(r.rc == EXIT_API_ERROR, "repl: fatal error exits its code");
        check(strstr(r.ob, "! api_error: boom\n") != NULL,
              "repl: the error line rendered");
        check(strstr(r.ob, "more") == NULL,
              "repl: the session ended, the later line never ran");
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- terminal tool ending: exit 9, session over ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"tool_request\",\"tool\":\"a.t1\","
                  "\"arguments\":{},\"id\":\"c1\"}"},
              .nrecs = 1, .kind = TURN_TOOLS, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"after\","
                  "\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        ftool_t tools[] = { { .tool = "a.t1", .text = "done", .rc = 0 } };
        g_ftools = (ftool_script_t){ tools, 1, 0 };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_OPENAI);
        char *ob = NULL;
        size_t on = 0;
        int fd = pipe_feed("go\nmore\n", 8);
        FILE *out = open_memstream(&ob, &on);
        int rc = repl_run(&c, fd, out, "/x", repl_term_factory);
        fclose(out);
        close(fd);
        check(rc == EXIT_TERMINAL_TOOL, "repl: terminal ending exits 9");
        check(strstr(ob, "done") != NULL, "repl: terminal answer rendered");
        check(strstr(ob, "after") == NULL,
              "repl: no further turn after the terminal tool");
        free(ob);
        call_cfg_free(&c);
    }

    /* ---- input hygiene: NUL and bad UTF-8 take the record tier ---- */
    {
        fturn_t hturns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"x\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(hturns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session(&c, "a\0b\n", 4);
        check(r.rc == EXIT_INVALID_RECORD, "repl: NUL in input exits 2");
        check(strstr(r.ob, "! invalid_record:") != NULL,
              "repl: NUL error line rendered");
        repl_session_free(&r);
        call_cfg_free(&c);

        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session(&c, "\xff\n", 2);
        check(r.rc == EXIT_INVALID_RECORD, "repl: bad UTF-8 exits 2");
        repl_session_free(&r);
        call_cfg_free(&c);
    }

    /* ---- multi-turn anthropic: same session, compiled max_tokens ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"r1\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"r2\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        g_factory_wire = fwire_new(turns, 2);
        repl_cfg(&c, PROTO_ANTHROPIC);
        repl_out_t r = repl_session(&c, "q1\nq2\n", 6);
        check(r.rc == EXIT_OK, "repl: anthropic session exits 0");
        check(strstr(r.ob, "r1") != NULL && strstr(r.ob, "r2") != NULL,
              "repl: both anthropic turns answered");
        repl_session_free(&r);
        call_cfg_free(&c);
    }
}

/* ================= agent (repl surface + extras) ================= */

static repl_out_t repl_session_opt(call_cfg_t *c, const repl_opts_t *o,
                                   const char *script, size_t sn) {
    repl_out_t r = { NULL, 0, 0 };
    int fd = pipe_feed(script, sn);
    check(fd >= 0, "agent: pipe feed");
    char *ob = NULL;
    size_t on = 0;
    FILE *out = open_memstream(&ob, &on);
    r.rc = repl_run_ex(c, o, fd, out, "/x", script_factory);
    fclose(out);
    close(fd);
    r.ob = ob;
    r.on = on;
    return r;
}

static void test_agent(void) {
    call_cfg_t c;
    char err[256];
    char cwd[1024];
    check(getcwd(cwd, sizeof cwd) != NULL, "agent: getcwd");

    /* ---- the one extra flag: extraction, compaction, errors ---- */
    {
        char *av[] = {"--openai", "http://x", "--conversation-store",
                      "/tmp/s.jsonl", "--model", "m"};
        const char *store = NULL;
        int n = repl_store_extract(6, av, &store, err, sizeof err);
        check(n == 4, "agent: store extraction compacts the range");
        check_str(av[0], "--openai", "agent: compaction keeps argv order");
        check_str(av[3], "m", "agent: compaction drops only the flag pair");
        check_str(store, "/tmp/s.jsonl", "agent: store path extracted");
        char *av2[] = {"--openai", "--conversation-store"};
        check(repl_store_extract(2, av2, &store, err, sizeof err) == -1 &&
              strstr(err, "missing value") != NULL,
              "agent: missing store value is a usage error");
        char *av3[] = {"--conversation-store", "a", "--conversation-store",
                       "b"};
        check(repl_store_extract(4, av3, &store, err, sizeof err) == -1 &&
              strstr(err, "given twice") != NULL,
              "agent: store flag twice is a usage error");
    }
    {
        /* the repl surface does not know the flag */
        char *av[] = {"--openai", "http://x", "--conversation-store", "s"};
        check(call_parse_ex(4, av, &c, err, sizeof err, false) == 1 &&
              strstr(err, "unknown flag '--conversation-store'") != NULL,
              "agent: --conversation-store is agent-only");
    }

    /* ---- the tools record: proxies first, the built-in server last ---- */
    {
        char *av[] = {"--openai", "http://x", "--mcp-proxy", "/tmp/fs.jsonl"};
        check(call_parse_ex(4, av, &c, err, sizeof err, false) == 0,
              "agent: proxy vector parses");
        cJSON *t = call_build_agent_tools(&c, "/bin");
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, t);
        cJSON_Delete(t);
        check_str(b.data,
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                  "\"name\":\"fs\",\"command_line\":\"'/bin' mcp-proxy "
                  "'/tmp/fs.jsonl'\"},{\"type\":\"stdio\",\"name\":"
                  "\"builtin\",\"command_line\":\"'/bin' builtin-mcp\"}]}",
                  "agent: tools record carries proxies and the built-in");
        buf_free(&b);
        call_cfg_free(&c);

        char *av2[] = {"--openai", "http://x"};
        check(call_parse_ex(2, av2, &c, err, sizeof err, false) == 0,
              "agent: bare vector parses");
        t = call_build_agent_tools(&c, "/bin");
        buf_init(&b);
        buf_append_tree(&b, t);
        cJSON_Delete(t);
        check_str(b.data,
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                  "\"name\":\"builtin\",\"command_line\":\"'/bin' "
                  "builtin-mcp\"}]}",
                  "agent: without proxies the built-in rides alone");
        buf_free(&b);
        call_cfg_free(&c);
    }

    /* ---- sessions: the agent default prompt, AGENTS.md, builtin ---- */
    system("rm -rf /tmp/llmkit-test-agent");
    system("mkdir -p /tmp/llmkit-test-agent/empty");
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"ok\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        repl_opts_t o = { .default_prompt = prompt_system_prompts_agent,
                          .builtin_mcp = true,
                          .agents_md = true };
        check(chdir("/tmp/llmkit-test-agent/empty") == 0,
              "agent: chdir to the clean dir");
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session_opt(&c, &o, "hi\n", 3);
        check(r.rc == EXIT_OK, "agent: session exits 0");
        check_str(g_seen_system, prompt_system_prompts_agent,
                  "agent: the bundled agent prompt is compiled in");
        /* the built-in server is attached: the fake exe fails to spawn,
           a non required server's connect_failed renders, the turn runs */
        check(strstr(r.ob, "! connect_failed: mcp server 'builtin':") !=
                  NULL,
              "agent: the built-in server is attached (connect attempted)");
        check(strstr(r.ob, "ok\n") != NULL, "agent: the turn still ran");
        repl_session_free(&r);
        call_cfg_free(&c);

        /* AGENTS.md: second system block after the prompt, prefixed */
        write_file("/tmp/llmkit-test-agent/empty/AGENTS.md",
                   "Keep answers short.\n");
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session_opt(&c, &o, "hi\n", 3);
        check(r.rc == EXIT_OK, "agent: AGENTS.md session exits 0");
        {
            buf_t want;
            buf_init(&want);
            buf_appendf(&want, "%s\n"
                               "##### Content of AGENTS.md #####\n"
                               "Keep answers short.\n",
                        prompt_system_prompts_agent);
            check_str(g_seen_system, want.data ? want.data : "",
                      "agent: AGENTS.md rides as the second system block");
            buf_free(&want);
        }
        repl_session_free(&r);
        call_cfg_free(&c);

        /* an explicit --system-prompt stays the first block */
        g_factory_wire = fwire_new(turns, 1);
        repl_cfg(&c, PROTO_OPENAI);
        c.system = strdup("custom");
        r = repl_session_opt(&c, &o, "hi\n", 3);
        check(r.rc == EXIT_OK, "agent: override session exits 0");
        check_str(g_seen_system,
                  "custom\n##### Content of AGENTS.md #####\n"
                  "Keep answers short.\n",
                  "agent: AGENTS.md follows an explicit prompt too");
        repl_session_free(&r);
        call_cfg_free(&c);
    }
    check(chdir(cwd) == 0, "agent: chdir back");

    /* ---- the conversation store: append, byte-exact records ---- */
    {
        fturn_t turns[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"Wor\",\"partial\":true}",
                  "{\"type\":\"response\",\"text\":\"ld\",\"partial\":true}",
                  "{\"type\":\"response\",\"text\":\"\",\"partial\":"
                  "false,\"finish_reason\":\"stop\"}"},
              .nrecs = 3, .kind = TURN_FINAL, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"thinking\",\"text\":\"hmm\",\"partial\":true}",
                  "{\"type\":\"thinking\",\"text\":\"\",\"partial\":false,"
                  "\"signature\":\"s\"}",
                  "{\"type\":\"response\",\"text\":\"Two\",\"partial\":"
                  "false}"},
              .nrecs = 3, .kind = TURN_FINAL, .abort_after = -1 },
            { .recs = (const char *[]){
                  "{\"type\":\"response\",\"text\":\"Three\",\"partial\":"
                  "false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        repl_opts_t o = { .store = "/tmp/llmkit-test-agent/store.jsonl" };
        g_factory_wire = fwire_new(turns, 3);
        repl_cfg(&c, PROTO_OPENAI);
        repl_out_t r = repl_session_opt(&c, &o, "one\ntwo\n", 7);
        check(r.rc == EXIT_OK, "agent: store session exits 0");
        buf_t want;
        buf_init(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "one\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "World\n");
        golden_timing(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "two\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "hmm\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "Two\n");
        golden_timing(&want);
        char *norm = repl_norm(r.ob);
        check_str(norm, want.data, "agent: store session renders as repl");
        free(norm);
        buf_free(&want);
        repl_session_free(&r);
        call_cfg_free(&c);
        /* the store: completed records only, one jsonl line each */
        FILE *sf = fopen("/tmp/llmkit-test-agent/store.jsonl", "r");
        check(sf != NULL, "agent: the store file exists");
        if (sf) {
            char line[512];
            bool ok = true;
            const char *want_lines[] = {
                "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                "\"text\":\"one\"}]}",
                "{\"type\":\"response\",\"text\":\"Wor\",\"partial\":true}",
                "{\"type\":\"response\",\"text\":\"ld\",\"partial\":true}",
                "{\"type\":\"response\",\"text\":\"\",\"partial\":"
                "false,\"finish_reason\":\"stop\"}",
                "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                "\"text\":\"two\"}]}",
                "{\"type\":\"thinking\",\"text\":\"hmm\",\"partial\":true}",
                "{\"type\":\"thinking\",\"text\":\"\",\"partial\":false,"
                "\"signature\":\"s\"}",
                "{\"type\":\"response\",\"text\":\"Two\",\"partial\":false}",
            };
            for (size_t i = 0; i < sizeof want_lines / sizeof want_lines[0];
                 i++) {
                if (!fgets(line, sizeof line, sf)) {
                    ok = false;
                    break;
                }
                line[strcspn(line, "\n")] = '\0';
                if (strcmp(line, want_lines[i])) {
                    ok = false;
                    break;
                }
            }
            check(ok, "agent: the store carries the conversation verbatim");
            check(fgets(line, sizeof line, sf) == NULL,
                  "agent: the store has nothing beyond the turn records");
            fclose(sf);
        }

        /* ---- resume: the store replays fully, then the prompt ---- */
        g_factory_wire = fwire_new(turns, 3);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session_opt(&c, &o, "", 0);
        check(r.rc == EXIT_OK, "agent: replay-only session exits 0");
        buf_init(&want);
        golden_rule(&want, '=');
        buf_append_str(&want, "one\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "World\n");
        golden_rule(&want, '=');
        buf_append_str(&want, "two\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "hmm\n");
        golden_rule(&want, '-');
        buf_append_str(&want, "Two\n");
        char *norm2 = repl_norm(r.ob);
        check_str(norm2, want.data,
                  "agent: an existing store replays before the prompt");
        free(norm2);
        buf_free(&want);
        repl_session_free(&r);
        call_cfg_free(&c);

        /* ---- resume and continue: history rides the transcript ---- */
        {
            fturn_t turn3[] = {
                { .recs = (const char *[]){
                      "{\"type\":\"response\",\"text\":\"Three\","
                      "\"partial\":false}"},
                  .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
            };
            g_factory_wire = fwire_new(turn3, 1);
            repl_cfg(&c, PROTO_OPENAI);
            r = repl_session_opt(&c, &o, "back\n", 5);
            check(r.rc == EXIT_OK, "agent: resume session exits 0");
            check(strstr(r.ob, "World") != NULL &&
                      strstr(r.ob, "Three") != NULL,
                  "agent: replay then the new turn, in order");
            {
                size_t rules = 0;
                char *norm3 = repl_norm(r.ob);
                for (const char *q = norm3; (q = strstr(q, "@=\n")) != NULL;
                     q++)
                    rules++;
                check(rules == 3, "agent: replayed users keep their blocks");
                free(norm3);
            }
            repl_session_free(&r);
            call_cfg_free(&c);
        }
        {
            int lines = 0;
            char linebuf[512];
            FILE *sf2 = fopen("/tmp/llmkit-test-agent/store.jsonl", "r");
            check(sf2 != NULL, "agent: resumed store opens");
            while (sf2 && fgets(linebuf, sizeof linebuf, sf2)) lines++;
            check(lines == 10, "agent: the resumed store grew by two lines");
            if (sf2) fclose(sf2);
        }

        /* ---- a malformed store is the invalid_record tier ---- */
        write_file("/tmp/llmkit-test-agent/bad.jsonl", "{\"type\":\"user\"");
        repl_opts_t bad = { .store = "/tmp/llmkit-test-agent/bad.jsonl" };
        g_factory_wire = fwire_new(turns, 3);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session_opt(&c, &bad, "hi\n", 3);
        check(r.rc == EXIT_INVALID_RECORD,
              "agent: malformed store exits 2 before any turn");
        check(strstr(r.ob, "! invalid_record:") != NULL,
              "agent: the malformed store's error line rendered");
        check(strstr(r.ob, "World") == NULL,
              "agent: no turn ran over a malformed store");
        repl_session_free(&r);
        call_cfg_free(&c);

        /* ---- a store that cannot be opened is the io tier ---- */
        repl_opts_t dir = { .store = "/tmp/llmkit-test-agent" };
        g_factory_wire = fwire_new(turns, 3);
        repl_cfg(&c, PROTO_OPENAI);
        r = repl_session_opt(&c, &dir, "hi\n", 3);
        check(r.rc == EXIT_IO_ERROR, "agent: an unopenable store exits 7");
        check(strstr(r.ob, "! io_error:") != NULL,
              "agent: the unopenable store's error line rendered");
        repl_session_free(&r);
        call_cfg_free(&c);
    }
    system("rm -rf /tmp/llmkit-test-agent");
}

/* ================= mcp-repl (design sec.16) ================= */

/* the golden token of a call's timing line: the measured span normalized
   away (stamped_line + "<n>.nnns") */
static bool mr_timing_line(const char *p, size_t len) {
    if (!stamped_line(p, len)) return false;
    const char *rest = p + 11;
    size_t rlen = len - 11;
    if (rlen < 6 || rest[rlen - 1] != 's' || rest[rlen - 2] < '0' ||
        rest[rlen - 2] > '9')
        return false;
    for (size_t i = 0; i < rlen - 1; i++)
        if ((rest[i] < '0' || rest[i] > '9') && rest[i] != '.')
            return false;
    return true;
}

static char *mr_norm(const char *ob) {
    buf_t b;
    buf_init(&b);
    const char *p = ob ? ob : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (mr_timing_line(p, len)) {
            buf_append_str(&b, "@t\n");
        } else {
            buf_append(&b, p, len + (eol ? 1 : 0));
        }
        p = eol ? eol + 1 : p + len;
    }
    return buf_steal(&b, NULL);
}

typedef struct mr_out {
    char *ob;
    int rc;
} mr_out_t;

static mr_out_t mr_session(const char *script, const char *transport) {
    mr_out_t r = { NULL, 0 };
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);
    mcp_repl_cfg_t c;
    memset(&c, 0, sizeof c);
    c.type = MCP_STDIO;
    c.target = (char *)transport;
    int fd = pipe_feed(script, strlen(script));
    check(fd >= 0, "mcp-repl: pipe feed");
    char *ob = NULL;
    size_t on = 0;
    FILE *out = open_memstream(&ob, &on);
    r.rc = mcp_repl_run(&c, fd, out);
    fclose(out);
    close(fd);
    r.ob = ob;
    return r;
}

static void test_mcp_repl(void) {
    char err[256];

    /* ---- command line vectors ---- */
    {
        mcp_repl_cfg_t c;
        check(mcp_repl_parse(0, NULL, &c, err, sizeof err) == 1 &&
                  strstr(err, "missing transport") != NULL,
              "mcp-repl: no transport flag is a usage error");
        char *av[] = {"--stdio", "x", "--http", "y"};
        check(mcp_repl_parse(4, av, &c, err, sizeof err) == 1 &&
                  strstr(err, "twice") != NULL,
              "mcp-repl: two transport flags are a usage error");
        char *av2[] = {"--stdio"};
        check(mcp_repl_parse(1, av2, &c, err, sizeof err) == 1 &&
                  strstr(err, "missing value") != NULL,
              "mcp-repl: missing flag value is a usage error");
        char *av3[] = {"--bogus"};
        check(mcp_repl_parse(1, av3, &c, err, sizeof err) == 1 &&
                  strstr(err, "unknown flag") != NULL,
              "mcp-repl: unknown flag is a usage error");
        char *av4[] = {"--protocol", "1999-01-01"};
        check(mcp_repl_parse(2, av4, &c, err, sizeof err) == 1 &&
                  strstr(err, "unsupported protocol") != NULL,
              "mcp-repl: unsupported revision is a usage error");
        char *av5[] = {"x"};
        check(mcp_repl_parse(1, av5, &c, err, sizeof err) == 1 &&
                  strstr(err, "unexpected extra argument") != NULL,
              "mcp-repl: a bare positional is a usage error");
        char *av6[] = {"--http", "http://x/mcp", "--header", "a=b",
                       "--header", "c=d", "--protocol", "2025-06-18"};
        check(mcp_repl_parse(8, av6, &c, err, sizeof err) == 0,
              "mcp-repl: full flag set parses");
        check(c.type == MCP_HTTP && c.nhdrs == 2, "mcp-repl: fields set");
        cJSON *t = mcp_repl_build_tools(&c);
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, t);
        check_str(b.data ? b.data : "",
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"http\","
                  "\"name\":\"server\",\"url\":\"http://x/mcp\","
                  "\"protocol\":\"2025-06-18\",\"headers\":"
                  "{\"a\":\"b\",\"c\":\"d\"}}]}",
                  "mcp-repl: the one-entry tools record");
        buf_free(&b);
        cJSON_Delete(t);
        mcp_repl_cfg_free(&c);
        char *av7[] = {"--stdio", "./calc 1 2"};
        check(mcp_repl_parse(2, av7, &c, err, sizeof err) == 0,
              "mcp-repl: stdio command line parses");
        t = mcp_repl_build_tools(&c);
        buf_init(&b);
        buf_append_tree(&b, t);
        check_str(b.data ? b.data : "",
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                  "\"name\":\"server\",\"command_line\":\"./calc 1 2\"}]}",
                  "mcp-repl: stdio record is command_line");
        buf_free(&b);
        cJSON_Delete(t);
        mcp_repl_cfg_free(&c);
    }

    /* ---- record rendering ---- */
    {
        cJSON *tool = cJSON_Parse(
            "{\"name\":\"add\",\"description\":\"sum two numbers\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":"
            "{\"a\":{\"type\":\"number\",\"description\":\"first addend\"},"
            "\"b\":{\"type\":\"number\"}},\"required\":[\"a\",\"b\"]}}");
        buf_t b;
        buf_init(&b);
        mcp_repl_record(tool, &b);
        check_str(b.data ? b.data : "",
                  "add(float a, float b): sum two numbers\n"
                  "- float a: first addend\n",
                  "mcp-repl: record carries names and descriptions");
        cJSON_Delete(tool);
        tool = cJSON_Parse("{\"name\":\"boom\",\"inputSchema\":"
                           "{\"type\":\"object\"}}");
        buf_clear(&b);
        mcp_repl_record(tool, &b);
        check_str(b.data ? b.data : "", "boom()\n",
                  "mcp-repl: no properties renders empty");
        cJSON_Delete(tool);
        tool = cJSON_Parse(
            "{\"name\":\"mix\",\"inputSchema\":{\"properties\":"
            "{\"i\":{\"type\":\"integer\"},\"s\":{\"type\":\"string\"},"
            "\"f\":{\"type\":\"number\"},\"o\":{\"type\":\"object\"},"
            "\"a\":{\"type\":\"array\"},\"bo\":{\"type\":\"boolean\"},"
            "\"x\":{}}}}");
        buf_clear(&b);
        mcp_repl_record(tool, &b);
        check_str(b.data ? b.data : "",
                  "mix(int i, string s, float f, object o, array a, "
                  "bool bo, any x)\n",
                  "mcp-repl: the type words, declaration order");
        cJSON_Delete(tool);
        tool = cJSON_Parse("{\"name\":\"bare\"}");
        buf_clear(&b);
        mcp_repl_record(tool, &b);
        check_str(b.data ? b.data : "", "bare()\n",
                  "mcp-repl: missing inputSchema renders empty");
        cJSON_Delete(tool);
        buf_free(&b);
    }

    /* ---- split: call syntax ---- */
    {
        char *name = NULL;
        cJSON *vals = NULL;
        char *e = mcp_repl_split("add(1, 2)", &name, &vals);
        check(e == NULL, "split: add(1, 2) ok");
        check_str(name ? name : "", "add", "split: name");
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, vals);
        check_str(b.data ? b.data : "", "[1,2]", "split: two literals");
        free(e);
        free(name);
        cJSON_Delete(vals);

        e = mcp_repl_split("boom", &name, &vals);
        check(e == NULL, "split: bare name is a zero-arg call");
        check(cJSON_GetArraySize(vals) == 0, "split: no literals");
        free(name);
        cJSON_Delete(vals);

        e = mcp_repl_split("boom()", &name, &vals);
        check(e == NULL, "split: empty parens are zero args");
        free(name);
        cJSON_Delete(vals);

        e = mcp_repl_split("f(\"a, b\", {\"k\": [1, 2]}, 3)", &name, &vals);
        buf_clear(&b);
        buf_append_tree(&b, vals);
        check(e == NULL &&
                  !strcmp(b.data ? b.data : "", "[\"a, b\",{\"k\":[1,2]},3]"),
              "split: strings, nested containers, commas inside");
        free(name);
        cJSON_Delete(vals);

        e = mcp_repl_split("f(\"a\\\"q\\\"b\")", &name, &vals);
        buf_clear(&b);
        buf_append_tree(&b, vals);
        check(e == NULL && !strcmp(b.data ? b.data : "",
                                  "[\"a\\\"q\\\"b\"]"),
              "split: escaped quote does not close the string");
        free(name);
        cJSON_Delete(vals);

        e = mcp_repl_split("  add ( 1 , 2 ) ", &name, &vals);
        check(e == NULL, "split: spaces around the tokens");
        buf_clear(&b);
        buf_append_tree(&b, vals);
        check_str(b.data ? b.data : "", "[1,2]", "split: literals trimmed");
        free(name);
        cJSON_Delete(vals);
        buf_free(&b);

        struct {
            const char *line;
            const char *want;
        } bad[] = {
            {"(1)", "expected a tool call"},
            {"add 1", "expected '('"},
            {"add(1", "missing ')'"},
            {"add(1,", "missing ')'"},
            {"add(1,2) x", "unexpected text after"},
            {"add(,1)", "is empty"},
            {"add(1,,2)", "is empty"},
            {"add(x)", "not a json literal"},
            {"add(\"unterminated)", "unterminated string"},
            {"add([1, 2)", "unbalanced brackets"},
            {"add(}", "missing ')'"},
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            e = mcp_repl_split(bad[i].line, &name, &vals);
            check(e != NULL && strstr(e, bad[i].want) != NULL,
                  "split: error vector");
            if (e && strstr(e, bad[i].want) == NULL)
                fprintf(stderr, "  line '%s': got '%s' want '%s'\n",
                        bad[i].line, e ? e : "(null)", bad[i].want);
            free(e);
            free(name);
            name = NULL;
            cJSON_Delete(vals);
            vals = NULL;
        }
    }

    /* ---- bind: positional to named ---- */
    {
        cJSON *tool = cJSON_Parse(
            "{\"name\":\"add\",\"inputSchema\":{\"properties\":"
            "{\"a\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"}},"
            "\"required\":[\"a\",\"b\"]}}");
        cJSON *vals = cJSON_Parse("[1,2]");
        cJSON *args = NULL;
        char *e = mcp_repl_bind(tool, vals, &args);
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, args);
        check(e == NULL && !strcmp(b.data ? b.data : "", "{\"a\":1,\"b\":2}"),
              "bind: positionals map to schema names");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);

        vals = cJSON_Parse("[1]"); /* b is required */
        args = NULL;
        e = mcp_repl_bind(tool, vals, &args);
        check(e != NULL && strstr(e, "'b'") != NULL,
              "bind: missing required argument is an error");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);

        vals = cJSON_Parse("[1,2,3]");
        args = NULL;
        e = mcp_repl_bind(tool, vals, &args);
        check(e != NULL && strstr(e, "takes 2") != NULL,
              "bind: too many arguments is an error");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);
        cJSON_Delete(tool);

        /* int literal for a float parameter: json has one number type */
        tool = cJSON_Parse(
            "{\"inputSchema\":{\"properties\":{\"a\":{\"type\":"
            "\"number\"}}}}");
        vals = cJSON_Parse("[2]");
        args = NULL;
        e = mcp_repl_bind(tool, vals, &args);
        const cJSON *a = args
            ? cJSON_GetObjectItemCaseSensitive(args, "a") : NULL;
        check(e == NULL && cJSON_IsNumber(a),
              "bind: an integer literal binds to a float parameter");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);
        cJSON_Delete(tool);

        /* no schema: no arguments accepted */
        tool = cJSON_Parse("{\"name\":\"bare\"}");
        vals = cJSON_Parse("[1]");
        args = NULL;
        e = mcp_repl_bind(tool, vals, &args);
        check(e != NULL, "bind: schema-less tool takes no arguments");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);
        vals = cJSON_Parse("[]");
        args = NULL;
        e = mcp_repl_bind(tool, vals, &args);
        check(e == NULL, "bind: schema-less zero-arg call binds {}");
        free(e);
        cJSON_Delete(args);
        cJSON_Delete(vals);
        cJSON_Delete(tool);
        buf_free(&b);
    }

    /* ---- scripted sessions against the fake server ---- */
    {
        mr_out_t r = mr_session("echo(\"hi\")\ntools\nhelp\nquit\n",
                                "python3 /tmp/llmkit-test-fake-mcp.py");
        check(r.rc == EXIT_OK, "mcp-repl: quit exits 0");
        char *norm = mr_norm(r.ob);
        buf_t want;
        buf_init(&want);
        buf_append_str(&want,
                       "Tools available:\n"
                       "echo(string text): echo text\n"
                       "- string text: the text to echo back\n"
                       "boom()\n"
                       "echo(\"hi\")\n"
                       "echo: hi\n"
                       "@t\n"
                       "tools\n"
                       "Tools available:\n"
                       "echo(string text): echo text\n"
                       "- string text: the text to echo back\n"
                       "boom()\n"
                       "help\n"
                       "lines:\n");
        check(strncmp(norm, want.data, want.len) == 0,
              "mcp-repl: banner, call, timing, re-list, help");
        if (strncmp(norm, want.data, want.len) != 0)
            fprintf(stderr, "  got:\n%s\n", norm);
        buf_free(&want);
        free(norm);
        free(r.ob);
    }
    {
        mr_out_t r = mr_session("boom()\nnope(1)\necho()\necho(1, 2)\n",
                                "python3 /tmp/llmkit-test-fake-mcp.py");
        check(r.rc == EXIT_OK, "mcp-repl: errors do not end the session");
        char *norm = mr_norm(r.ob);
        const char *lines[] = {
            "! tool error: boom", "@t",
            "! syntax: unknown tool 'nope'",
            "! syntax: missing required argument 'text'",
            "! syntax: 2 arguments, the tool takes 1",
        };
        const char *p = norm;
        for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            const char *hit = strstr(p, lines[i]);
            check(hit != NULL, "mcp-repl: error line rendered");
            if (hit) p = hit + strlen(lines[i]);
        }
        free(norm);
        free(r.ob);
    }
    {
        /* EOF ends with the last ending; a dead command line connects
           nothing: exit 3 */
        mr_out_t r = mr_session("echo(hi)\n",
                                "python3 /tmp/llmkit-test-fake-mcp.py");
        check(r.rc == EXIT_OK, "mcp-repl: EOF after a call exits 0");
        free(r.ob);
        r = mr_session("", "nonexistent-command-xyz");
        check(r.rc == EXIT_CONNECT_FAILED,
              "mcp-repl: connect failure exits 3");
        check(strstr(r.ob ? r.ob : "", "connect_failed") != NULL,
              "mcp-repl: connect failure renders its error line");
        free(r.ob);
    }
}

static void test_terminal_tools(void) {
    cap_t cap = { 0 };
    engine_t *e;
    int rc;

    /* terminal tool ends the run: rest of the batch suspended, no error
       record, no further turn */
    fturn_t turns[] = {
        { .recs = (const char *[]){
             "{\"type\":\"response\",\"text\":\"t\",\"partial\":false}",
             "{\"type\":\"tool_request\",\"tool\":\"a.t1\",\"arguments\":{},\"id\":\"c1\"}",
             "{\"type\":\"tool_request\",\"tool\":\"a.t2\",\"arguments\":{},\"id\":\"c2\"}"},
          .nrecs = 3, .kind = TURN_TOOLS, .abort_after = -1 },
        { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"after\",\"partial\":false}"},
          .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
    };
    ftool_t tools[] = { { .tool = "a.t1", .text = "done", .rc = 0 } };
    g_ftools = (ftool_script_t){ tools, 1, 0 };
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "terminal: start ok");
    add_tool_term(e, "a.t1");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_TERMINAL_TOOL, "terminal: exit code 9");
    check_str(e->terminal_id, "c1", "terminal: engine records the id");
    {
        bool c1_ok = false, c2_susp = false, any_error = false,
             any_after = false;
        for (size_t i = 0; i < cap.n; i++) {
            cJSON *r = cJSON_Parse(cap.v[i]);
            int k = rec_classify(r);
            if (k == R_ERROR) any_error = true;
            if (k == R_RESPONSE && rec_bool(r, "partial", true) == false)
                any_after = !strcmp(rec_str(r, "text"), "after");
            if (k == R_TOOL_RESPONSE) {
                const char *id = rec_str(r, "id");
                if (!strcmp(id, "c1") && !rec_bool(r, "is_error", false))
                    c1_ok = strstr(cap.v[i], "done") != NULL;
                if (!strcmp(id, "c2") && rec_bool(r, "is_error", false))
                    c2_susp = strstr(cap.v[i], "terminal") != NULL;
            }
            cJSON_Delete(r);
        }
        check(c1_ok, "terminal: the tool's own answer is emitted");
        check(c2_susp, "terminal: the rest of the batch suspended");
        check(!any_error, "terminal: no error record for the ending");
        check(!any_after, "terminal: the would-be next turn never ran");
    }
    engine_free(e);
    cap_reset(&cap);

    /* failed terminal tool still ends: answered is_error + non-fatal
       tool_failed, exit 9, no retry */
    ftool_t tools2[] = { { .tool = "a.t1", .text = NULL, .rc = 1 } };
    g_ftools = (ftool_script_t){ tools2, 1, 0 };
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "terminal: start ok (failed tool)");
    add_tool_term(e, "a.t1");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_TERMINAL_TOOL, "terminal: failed tool still exits 9");
    {
        bool failed = false;
        for (size_t i = 0; i < cap.n; i++) {
            cJSON *r = cJSON_Parse(cap.v[i]);
            if (rec_classify(r) == R_ERROR &&
                !strcmp(rec_str(r, "code"), EC_TOOL_FAILED) &&
                !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "fatal")))
                failed = true;
            cJSON_Delete(r);
        }
        check(failed, "terminal: non-fatal tool_failed precedes the ending");
    }
    engine_free(e);
    cap_reset(&cap);

    /* steering arriving during the terminal tool: dropped silently */
    ftool_t tools3[] = {
        { .tool = "a.t1", .text = "done", .rc = 0,
          .mid_rec = "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"steer\"}]}",
          .mid_flush = true },
    };
    g_ftools = (ftool_script_t){ tools3, 1, 0 };
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "terminal: start ok (steering case)");
    add_tool_term(e, "a.t1");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_TERMINAL_TOOL, "terminal: steering case exits 9");
    {
        bool io_err = false, start_marker = false;
        for (size_t i = 0; i < cap.n; i++) {
            cJSON *r = cJSON_Parse(cap.v[i]);
            if (rec_classify(r) == R_ERROR) io_err = true;
            if (rec_classify(r) == R_START) start_marker = true;
            cJSON_Delete(r);
        }
        check(!io_err, "terminal: flushed steering dropped, no io_error");
        check(!start_marker, "terminal: no start marker, no resume");
    }
    engine_free(e);
    cap_reset(&cap);

    /* unflushed records during the terminal tool: the drop rule applies */
    ftool_t tools4[] = {
        { .tool = "a.t1", .text = "done", .rc = 0,
          .mid_rec = "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"never\"}]}",
          .mid_flush = false },
    };
    g_ftools = (ftool_script_t){ tools4, 1, 0 };
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "terminal: start ok (drop rule case)");
    add_tool_term(e, "a.t1");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_TERMINAL_TOOL, "terminal: drop rule case exits 9");
    {
        int io_at = -1;
        for (size_t i = 0; i < cap.n; i++) {
            cJSON *r = cJSON_Parse(cap.v[i]);
            if (rec_classify(r) == R_ERROR &&
                !strcmp(rec_str(r, "code"), "io_error")) {
                io_at = (int)i;
                cJSON_Delete(r);
                break;
            }
            cJSON_Delete(r);
        }
        check(io_at >= 0, "terminal: drop rule io_error before the exit");
        if (io_at >= 0)
            check_str(cap_field(&cap, (size_t)io_at, "fatal"), "false",
                      "terminal: drop rule io_error is non-fatal");
    }
    engine_free(e);
    cap_reset(&cap);

    /* SIGINT during the terminal tool: the terminal ending wins, exit 9 */
    ftool_t tools5[] = {
        { .tool = "a.t1", .text = "done", .rc = 0, .stop_after = true },
    };
    g_ftools = (ftool_script_t){ tools5, 1, 0 };
    g_factory_wire = fwire_new(turns, 2);
    e = engine_new(cap_fn, &cap);
    e->wire_factory = script_factory;
    e->tool_exec = ftool_exec;
    e->inq = queue_new();
    feed_line(e, "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\",\"api_base\":\"http://x\"}");
    feed_line(e, "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"q\"}]}");
    check(engine_start(e) == 0, "terminal: start ok (sigint case)");
    add_tool_term(e, "a.t1");
    cap_reset(&cap);
    rc = engine_run(e);
    check(rc == EXIT_TERMINAL_TOOL, "terminal: sigint during tool exits 9");
    {
        bool interrupted = false, c2_term = false;
        for (size_t i = 0; i < cap.n; i++) {
            cJSON *r = cJSON_Parse(cap.v[i]);
            if (rec_classify(r) == R_ERROR &&
                !strcmp(rec_str(r, "code"), EC_INTERRUPTED))
                interrupted = true;
            if (rec_classify(r) == R_TOOL_RESPONSE &&
                !strcmp(rec_str(r, "id"), "c2"))
                c2_term = strstr(cap.v[i], "terminal") != NULL;
            cJSON_Delete(r);
        }
        check(!interrupted, "terminal: no interrupted record");
        check(c2_term, "terminal: suspension names the terminal ending");
    }
    g_stop_flag = 0;
    engine_free(e);
    cap_reset(&cap);

    /* shape validation of terminal_tools */
    {
        char *m = validate_tools(cJSON_Parse(
            "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"a\","
            "\"command_line\":\"x\",\"terminal_tools\":\"echo\"}]}"));
        check(m != NULL, "terminal: terminal_tools must be a list");
        free(m);
        m = validate_tools(cJSON_Parse(
            "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"a\","
            "\"command_line\":\"x\",\"terminal_tools\":[42]}]}"));
        check(m != NULL, "terminal: entries must be strings");
        free(m);
        m = validate_tools(cJSON_Parse(
            "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"a\","
            "\"command_line\":\"x\",\"terminal_tools\":[\"echo\"]}]}"));
        check(m == NULL, "terminal: valid shape passes");
        free(m);
    }

    /* name resolution at reconcile against a real fake server */
    write_file("/tmp/llmkit-test-fake-mcp.py", FAKE_MCP_PY);
    {
        cap_reset(&cap);
        e = engine_new(cap_fn, &cap);
        cJSON *trec = cJSON_Parse(
            "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"srv\","
            "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\","
            "\"terminal_tools\":[\"echo\"]}]}");
        check(mcp_reconcile((mcp_mgr_t *)e->mcp, e, trec) == 0,
              "terminal: reconcile ok with a listed name");
        check(mcp_tool_is_terminal((mcp_mgr_t *)e->mcp, "srv.echo"),
              "terminal: listed name marks the listing entry");
        check(!mcp_tool_is_terminal((mcp_mgr_t *)e->mcp, "srv.boom"),
              "terminal: unmarked tool stays ordinary");
        check(!mcp_tool_is_terminal((mcp_mgr_t *)e->mcp, "srv.nope"),
              "terminal: unknown tool is not terminal");
        cJSON_Delete(trec);
        mcp_kill_all((mcp_mgr_t *)e->mcp);
        engine_free(e);

        e = engine_new(cap_fn, &cap);
        trec = cJSON_Parse(
            "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":\"srv\","
            "\"command_line\":\"python3 /tmp/llmkit-test-fake-mcp.py\","
            "\"terminal_tools\":[\"echo\",\"nope\"]}]}");
        cap_reset(&cap);
        check(mcp_reconcile((mcp_mgr_t *)e->mcp, e, trec) ==
                  EXIT_INVALID_RECORD,
              "terminal: unlisted name is fatal invalid_record");
        check(cap.n == 1 && !strcmp(cap_field(&cap, 0, "code"), "invalid_record") &&
                  !strcmp(cap_field(&cap, 0, "fatal"), "true"),
              "terminal: reconcile emits the fatal record");
        cJSON_Delete(trec);
        engine_free(e);
        cap_reset(&cap);
    }

    /* call: --terminal-tool compilation, folding, usage errors */
    {
        call_cfg_t c;
        char err[256];
        char *av[] = {"--openai", "http://x", "--mcp-proxy", "/tmp/fs.jsonl",
                      "--terminal-tool", "fs.echo", "--terminal-tool",
                      "fs.boom", "--prompt", "p"};
        check(call_parse((int)(sizeof av / sizeof av[0]), av, &c, err,
                         sizeof err) == 0,
              "terminal: call vector parses");
        check(c.nterminals == 2, "terminal: both flags captured");
        cJSON *t = call_build_tools(&c, "/x");
        buf_t b;
        buf_init(&b);
        buf_append_tree(&b, t);
        cJSON_Delete(t);
        check_str(b.data,
                  "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\","
                  "\"name\":\"fs\",\"command_line\":\"'/x' mcp-proxy "
                  "'/tmp/fs.jsonl'\",\"terminal_tools\":[\"echo\","
                  "\"boom\"]}]}",
                  "terminal: flags fold into terminal_tools");
        buf_free(&b);
        call_cfg_free(&c);

        char *u1[] = {"--openai", "http://x", "--prompt", "p",
                      "--terminal-tool", "fs.echo"};
        check(call_parse(6, u1, &c, err, sizeof err) == 1,
              "terminal: --terminal-tool without --mcp-proxy is a usage error");
        char *u2[] = {"--openai", "http://x", "--mcp-proxy", "/tmp/fs.jsonl",
                      "--prompt", "p", "--terminal-tool", "other.tool"};
        check(call_parse(8, u2, &c, err, sizeof err) == 1,
              "terminal: unknown server prefix is a usage error");
        char *u3[] = {"--openai", "http://x", "--mcp-proxy", "/tmp/fs.jsonl",
                      "--prompt", "p", "--terminal-tool", "fsdotless"};
        check(call_parse(8, u3, &c, err, sizeof err) == 1,
              "terminal: missing dot is a usage error");
    }

    /* call end-to-end: terminal ending prints the tool text, exit 9 */
    {
        fturn_t ct[] = {
            { .recs = (const char *[]){
                  "{\"type\":\"tool_request\",\"tool\":\"fs.echo\",\"arguments\":{},\"id\":\"c1\"}"},
              .nrecs = 1, .kind = TURN_TOOLS, .abort_after = -1 },
            { .recs = (const char *[]){"{\"type\":\"response\",\"text\":\"after\",\"partial\":false}"},
              .nrecs = 1, .kind = TURN_FINAL, .abort_after = -1 },
        };
        ftool_t ct_tools[] = { { .tool = "fs.echo", .text = "done", .rc = 0 } };
        g_ftools = (ftool_script_t){ ct_tools, 1, 0 };
        g_factory_wire = fwire_new(ct, 2);
        call_cfg_t c;
        memset(&c, 0, sizeof c);
        c.protocol = PROTO_OPENAI;
        c.api_base = strdup("http://x");
        c.prompt = strdup("q");
        char *ob = NULL, *eb = NULL;
        size_t on = 0, en = 0;
        FILE *out = open_memstream(&ob, &on);
        FILE *er = open_memstream(&eb, &en);
        int rc2 = call_run(&c, out, er, "/x", term_script_factory);
        fclose(out);
        fclose(er);
        check(rc2 == EXIT_TERMINAL_TOOL, "terminal: call exits 9");
        check_str(ob, "done\n", "terminal: call prints the tool text");
        check(en == 0, "terminal: no stderr on a clean terminal ending");
        free(ob);
        free(eb);
        call_cfg_free(&c);
    }

    /* agent-as-tool end-to-end: invoke replies with the terminal answer */
    {
        const char *log = "/tmp/llmkit-test-agent-term.log";
        unlink(log);
        spawn_t sp;
        char url[128];
        if (!endpoint_spawn(&sp, log, url, sizeof url)) {
            check(false, "terminal agent endpoint spawned");
            return;
        }
        char seed[1024];
        snprintf(seed, sizeof seed,
                 "{\"type\":\"llm\",\"endpoint_protocol\":\"openai\","
                 "\"api_base\":\"%s\",\"model\":\"m\",\"inference_options\":"
                 "{\"stream\":false}}\n"
                 "{\"type\":\"tools\",\"tools\":[{\"type\":\"stdio\",\"name\":"
                 "\"fs\",\"command_line\":\"python3 "
                 "/tmp/llmkit-test-fake-mcp.py\",\"terminal_tools\":"
                 "[\"echo\"]}]}\n"
                 "{\"type\":\"agent-as-tool\",\"tool_description\":\"t\","
                 "\"input_description\":\"i\"}\n",
                 url);
        write_file("/tmp/llmkit-test-agent-term-seed.jsonl", seed);

        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) || pipe(out_pipe)) {
            check(false, "terminal agent pipes");
            spawn_kill(&sp);
            return;
        }
        pid_t pid = fork();
        check(pid >= 0, "terminal agent fork");
        if (pid == 0) {
            dup2(in_pipe[0], 0);
            dup2(out_pipe[1], 1);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            int rc3 = cmd_agent("/tmp/llmkit-test-agent-term-seed.jsonl");
            _exit(rc3 == 0 ? 0 : 1);
        }
        close(in_pipe[0]);
        close(out_pipe[1]);
        FILE *to = fdopen(in_pipe[1], "w");
        FILE *from = fdopen(out_pipe[0], "r");
        char line[4096];
        fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                    "\"params\":{\"protocolVersion\":\"2025-11-25\"}}\n");
        fflush(to);
        check(fgets(line, sizeof line, from) != NULL,
              "terminal agent: initialize reply");
        fprintf(to, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
                    "\"params\":{\"name\":\"invoke\",\"arguments\":"
                    "{\"input\":\"go\"}}}\n");
        fflush(to);
        check(fgets(line, sizeof line, from) != NULL &&
                  strstr(line, "echo:") != NULL &&
                  strstr(line, "isError") == NULL,
              "terminal agent: invoke replies with the terminal answer");
        fclose(to);
        int st = 0;
        waitpid(pid, &st, 0);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "terminal agent: exits 0");
        fclose(from);
        spawn_kill(&sp);
    }

    cap_destroy(&cap);
}

/* ================= llmkit proxy ================= */

/* join emitted records into one jsonl string */
typedef struct {
    buf_t b;
} jb_t;

static void jb_fn(void *ctx, cJSON *rec) {
    jb_t *j = ctx;
    buf_append_tree(&j->b, rec);
    buf_append_byte(&j->b, '\n');
}

/* the [HH:MM:SS] stamps are the wall clock: golden-exact after the
   placeholder swap */
static void scrub_stamps(char *s) {
    size_t n = strlen(s), r = 0, w = 0;
    while (r < n) {
        if (r + 11 <= n && s[r] == '[' &&
            s[r + 1] >= '0' && s[r + 1] <= '9' &&
            s[r + 2] >= '0' && s[r + 2] <= '9' && s[r + 3] == ':' &&
            s[r + 4] >= '0' && s[r + 4] <= '9' &&
            s[r + 5] >= '0' && s[r + 5] <= '9' && s[r + 6] == ':' &&
            s[r + 7] >= '0' && s[r + 7] <= '9' &&
            s[r + 8] >= '0' && s[r + 8] <= '9' && s[r + 9] == ']' &&
            s[r + 10] == ' ') {
            s[w++] = '[';
            s[w++] = 'T';
            s[w++] = 'S';
            s[w++] = ']';
            s[w++] = ' ';
            r += 11;
        } else {
            s[w++] = s[r++];
        }
    }
    s[w] = '\0';
}

static void exp_rule(buf_t *b, char glyph) {
    buf_append_str(b, "[TS] ");
    for (int i = 0; i < 80 - 11; i++) buf_append_byte(b, glyph);
    buf_append_byte(b, '\n');
}

static void test_llm_proxy(void) {
    llm_proxy_cfg_t c;
    char err[256];
    jb_t j;

    /* ---- parse ---- */
    {
        char *av[] = {"--openai", "http://x/v1", "--key", "k",
                      "--listen", "0.0.0.0:9090"};
        check(llm_proxy_parse(6, av, &c, err, sizeof err) == 0,
              "proxy: full vector parses");
        check(c.protocol == PROTO_OPENAI && !strcmp(c.api_base, "http://x/v1") &&
                  !strcmp(c.key, "k") && !strcmp(c.listen, "0.0.0.0:9090"),
              "proxy: fields captured");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--anthropic", "https://a/v1"};
        check(llm_proxy_parse(2, av, &c, err, sizeof err) == 0,
              "proxy: anthropic vector parses");
        check(c.protocol == PROTO_ANTHROPIC && c.key == NULL &&
                  !strcmp(c.listen, "127.0.0.1:8080"),
              "proxy: default listen, absent key");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--openai-responses", "http://x"};
        check(llm_proxy_parse(2, av, &c, err, sizeof err) == 0 &&
                  c.protocol == PROTO_RESPONSES,
              "proxy: responses mapping");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--openai", "http://x", "--anthropic", "http://y"};
        check(llm_proxy_parse(4, av, &c, err, sizeof err) == 1,
              "proxy: protocol flag twice rejected");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--openai"};
        check(llm_proxy_parse(1, av, &c, err, sizeof err) == 1,
              "proxy: missing api_base value rejected");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--openai", "http://x", "--banana"};
        check(llm_proxy_parse(3, av, &c, err, sizeof err) == 1,
              "proxy: unknown flag rejected");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--openai", "http://x", "stray"};
        check(llm_proxy_parse(3, av, &c, err, sizeof err) == 1,
              "proxy: positional argument rejected");
        llm_proxy_cfg_free(&c);
    }
    {
        char *av[] = {"--key", "k"};
        check(llm_proxy_parse(2, av, &c, err, sizeof err) == 1,
              "proxy: missing protocol flag rejected");
        llm_proxy_cfg_free(&c);
    }

    /* ---- request mapping ---- */
    {
        const char *body =
            "{\"model\":\"m\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"sys\"},"
            "{\"role\":\"user\",\"content\":\"hi\"},"
            "{\"role\":\"assistant\",\"content\":\"think\",\"tool_calls\":"
            "[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
            "{\"name\":\"get\",\"arguments\":\"{\\\"a\\\":1}\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"res\"}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_OPENAI, body, strlen(body), jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"sys\"}]}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n"
                  "{\"type\":\"response\",\"text\":\"think\","
                  "\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c1\"}\n"
                  "{\"type\":\"tool_response\",\"id\":\"c1\","
                  "\"text\":\"res\"}\n",
                  "proxy: chat request mapping");
        buf_free(&j.b);
    }
    {
        const char *body =
            "{\"model\":\"m\",\"instructions\":\"sys\",\"input\":["
            "{\"type\":\"message\",\"role\":\"user\",\"content\":"
            "[{\"type\":\"input_text\",\"text\":\"hi\"}]},"
            "{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"get\","
            "\"arguments\":\"{\\\"a\\\":1}\"},"
            "{\"type\":\"function_call_output\",\"call_id\":\"c1\","
            "\"output\":\"res\"},"
            "{\"type\":\"reasoning\",\"summary\":"
            "[{\"type\":\"summary_text\",\"text\":\"hm\"}]}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_RESPONSES, body, strlen(body), jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"sys\"}]}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c1\"}\n"
                  "{\"type\":\"tool_response\",\"id\":\"c1\","
                  "\"text\":\"res\"}\n"
                  "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":false,"
                  "\"signature\":\"\"}\n",
                  "proxy: responses request mapping");
        buf_free(&j.b);
    }
    {
        const char *body =
            "{\"system\":\"sys\",\"messages\":["
            "{\"role\":\"user\",\"content\":["
            "{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
            "\"content\":\"res\"},"
            "{\"type\":\"text\",\"text\":\"hi\"}]},"
            "{\"role\":\"assistant\",\"content\":["
            "{\"type\":\"thinking\",\"thinking\":\"hm\",\"signature\":\"s\"},"
            "{\"type\":\"text\",\"text\":\"ans\"},"
            "{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"get\","
            "\"input\":{\"a\":1}}]}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_ANTHROPIC, body, strlen(body), jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"sys\"}]}\n"
                  "{\"type\":\"tool_response\",\"id\":\"c1\","
                  "\"text\":\"res\"}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n"
                  "{\"type\":\"thinking\",\"text\":\"hm\","
                  "\"partial\":false,\"signature\":\"s\"}\n"
                  "{\"type\":\"response\",\"text\":\"ans\","
                  "\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c2\"}\n",
                  "proxy: anthropic request mapping");
        buf_free(&j.b);
    }
    {
        /* the system prompt's other homes: developer roles and
           text-block lists map to the same system record */
        const char *chat =
            "{\"messages\":[{\"role\":\"developer\",\"content\":\"be kind\"},"
            "{\"role\":\"user\",\"content\":\"hi\"}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_OPENAI, chat, strlen(chat), jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"be kind\"}]}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n",
                  "proxy: chat developer role maps to system");
        buf_free(&j.b);
        const char *resp =
            "{\"input\":[{\"type\":\"message\",\"role\":\"system\","
            "\"content\":\"be kind\"},"
            "{\"type\":\"message\",\"role\":\"user\",\"content\":\"hi\"}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_RESPONSES, resp, strlen(resp), jb_fn,
                              &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"be kind\"}]}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n",
                  "proxy: responses system message maps to system");
        buf_free(&j.b);
        const char *anth =
            "{\"system\":[{\"type\":\"text\",\"text\":\"be kind\"},"
            "{\"type\":\"text\",\"text\":\"and brief\"}],"
            "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_ANTHROPIC, anth, strlen(anth), jb_fn,
                              &j);
        check_str(j.b.data,
                  "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"be kind\\nand brief\"}]}\n"
                  "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
                  "\"text\":\"hi\"}]}\n",
                  "proxy: anthropic system block list joins");
        buf_free(&j.b);
    }
    {
        buf_init(&j.b);
        llm_proxy_map_request(PROTO_OPENAI, "not json", 8, jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"error\",\"code\":\"invalid_record\","
                  "\"message\":\"request body is not json\",\"fatal\":true}\n",
                  "proxy: malformed request body");
        buf_free(&j.b);
    }

    /* ---- response mapping (json bodies) ---- */
    {
        const char *body =
            "{\"choices\":[{\"message\":{\"reasoning_content\":\"hm\","
            "\"content\":\"ans\",\"tool_calls\":[{\"id\":\"c1\","
            "\"function\":{\"name\":\"get\",\"arguments\":\"{}\"}}]}}],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":20}}";
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_OPENAI, 200, body, strlen(body), jb_fn,
                               &j);
        check_str(j.b.data,
                  "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":false,"
                  "\"signature\":\"\"}\n"
                  "{\"type\":\"response\",\"text\":\"ans\","
                  "\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{},\"id\":\"c1\","
                  "\"usage\":{\"input_tokens\":10,\"output_tokens\":20}}\n",
                  "proxy: chat response mapping, usage on the last");
        buf_free(&j.b);
    }
    {
        const char *body =
            "{\"output\":[{\"type\":\"reasoning\","
            "\"summary\":[{\"text\":\"hm\"}]},{\"type\":\"message\","
            "\"content\":[{\"type\":\"output_text\",\"text\":\"ans\"}]}],"
            "\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}";
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_RESPONSES, 200, body, strlen(body),
                               jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":false,"
                  "\"signature\":\"\"}\n"
                  "{\"type\":\"response\",\"text\":\"ans\","
                  "\"partial\":false,\"usage\":{\"input_tokens\":5,"
                  "\"output_tokens\":6}}\n",
                  "proxy: responses response mapping");
        buf_free(&j.b);
    }
    {
        const char *body =
            "{\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\","
            "\"signature\":\"s\"},{\"type\":\"text\",\"text\":\"ans\"}],"
            "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}";
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_ANTHROPIC, 200, body, strlen(body),
                               jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":false,"
                  "\"signature\":\"s\"}\n"
                  "{\"type\":\"response\",\"text\":\"ans\","
                  "\"partial\":false,\"usage\":{\"input_tokens\":1,"
                  "\"output_tokens\":2}}\n",
                  "proxy: anthropic response mapping");
        buf_free(&j.b);
    }
    {
        const char *body =
            "{\"error\":{\"message\":\"bad key\",\"type\":"
            "\"invalid_request_error\"}}";
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_OPENAI, 401, body, strlen(body), jb_fn,
                               &j);
        check_str(j.b.data,
                  "{\"type\":\"error\",\"code\":\"api_error\","
                  "\"message\":\"HTTP 401: bad key "
                  "(invalid_request_error)\",\"fatal\":true}\n",
                  "proxy: openai error shaping");
        buf_free(&j.b);
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_ANTHROPIC, 429, body, strlen(body),
                               jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"error\",\"code\":\"api_error\","
                  "\"message\":\"HTTP 429: bad key\",\"fatal\":true}\n",
                  "proxy: anthropic error shaping (no type suffix)");
        buf_free(&j.b);
    }
    {
        buf_init(&j.b);
        llm_proxy_map_response(PROTO_OPENAI, 200, "not json", 8, jb_fn, &j);
        check_str(j.b.data,
                  "{\"type\":\"error\",\"code\":\"http_error\","
                  "\"message\":\"endpoint returned a non-json body\","
                  "\"fatal\":true}\n",
                  "proxy: non-json 2xx body");
        buf_free(&j.b);
    }

    /* ---- response mapping (sse streams) ---- */
    {
        const char *stream =
            "data: {\"choices\":[{\"delta\":{\"content\":\"He\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"llo\"}}]}\n\n"
            "data: {\"choices\":[{\"delta\":"
            "{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":"
            "{\"name\":\"get\",\"arguments\":\"{\\\"a\\\"\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":"
            "{\"tool_calls\":[{\"index\":0,\"function\":"
            "{\"arguments\":\":1}\"}}]}}]}\n\n"
            "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":3,"
            "\"completion_tokens\":4}}\n\n"
            "data: [DONE]\n\n";
        buf_init(&j.b);
        llm_proxy_sse_t *s =
            llm_proxy_sse_new(PROTO_OPENAI, jb_fn, &j);
        check(s != NULL, "proxy: chat sse parser created");
        /* split at the most awkward boundary: one byte at a time */
        for (size_t i = 0; i < strlen(stream); i++)
            llm_proxy_sse_feed(s, stream + i, 1);
        llm_proxy_sse_finish(s);
        llm_proxy_sse_free(s);
        check_str(j.b.data,
                  "{\"type\":\"response\",\"text\":\"He\",\"partial\":true}\n"
                  "{\"type\":\"response\",\"text\":\"llo\","
                  "\"partial\":true}\n"
                  "{\"type\":\"response\",\"text\":\"\",\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c1\","
                  "\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}\n",
                  "proxy: chat sse mapping, byte-split feed");
        buf_free(&j.b);
    }
    {
        const char *stream =
            "event: response.output_text.delta\n"
            "data: {\"delta\":\"An\"}\n\n"
            "event: response.function_call_arguments.delta\n"
            "data: {\"output_index\":0,\"delta\":\"{\\\"a\\\":1}\"}\n\n"
            "event: response.output_item.done\n"
            "data: {\"item\":{\"type\":\"function_call\",\"output_index\":0,"
            "\"call_id\":\"c1\",\"name\":\"get\","
            "\"arguments\":\"{\\\"a\\\":1}\"}}\n\n"
            "event: response.completed\n"
            "data: {\"response\":{\"usage\":{\"input_tokens\":1,"
            "\"output_tokens\":2}}}\n\n";
        buf_init(&j.b);
        llm_proxy_sse_t *s =
            llm_proxy_sse_new(PROTO_RESPONSES, jb_fn, &j);
        llm_proxy_sse_feed(s, stream, strlen(stream));
        llm_proxy_sse_finish(s);
        llm_proxy_sse_free(s);
        check_str(j.b.data,
                  "{\"type\":\"response\",\"text\":\"An\",\"partial\":true}\n"
                  "{\"type\":\"response\",\"text\":\"\",\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c1\","
                  "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}\n",
                  "proxy: responses sse mapping");
        buf_free(&j.b);
    }
    {
        const char *stream =
            "event: message_start\n"
            "data: {\"message\":{\"usage\":{\"input_tokens\":7}}}\n\n"
            "event: content_block_start\n"
            "data: {\"index\":0,\"content_block\":{\"type\":\"thinking\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
            "\"thinking\":\"hm\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"index\":0,\"delta\":{\"type\":\"signature_delta\","
            "\"signature\":\"sig\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"index\":0}\n\n"
            "event: content_block_start\n"
            "data: {\"index\":1,\"content_block\":{\"type\":\"text\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"index\":1,\"delta\":{\"type\":\"text_delta\","
            "\"text\":\"ans\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"index\":1}\n\n"
            "event: content_block_start\n"
            "data: {\"index\":2,\"content_block\":{\"type\":\"tool_use\","
            "\"id\":\"c1\",\"name\":\"get\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"index\":2,\"delta\":{\"type\":\"input_json_delta\","
            "\"partial_json\":\"{\\\"a\\\":1}\"}}\n\n"
            "event: content_block_stop\n"
            "data: {\"index\":2}\n\n"
            "event: message_delta\n"
            "data: {\"usage\":{\"output_tokens\":9}}\n\n"
            "event: message_stop\n"
            "data: {}\n\n";
        buf_init(&j.b);
        llm_proxy_sse_t *s =
            llm_proxy_sse_new(PROTO_ANTHROPIC, jb_fn, &j);
        llm_proxy_sse_feed(s, stream, strlen(stream));
        llm_proxy_sse_finish(s);
        llm_proxy_sse_free(s);
        check_str(j.b.data,
                  "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":true}\n"
                  "{\"type\":\"response\",\"text\":\"ans\","
                  "\"partial\":true}\n"
                  /* the engine's held-final order: a streamed block's
                     final record is emitted at the NEXT block's stop
                     (blk_stop releases the previous pending first), so
                     the thinking final lands after the response partials */
                  "{\"type\":\"thinking\",\"text\":\"\","
                  "\"partial\":false,\"signature\":\"sig\"}\n"
                  "{\"type\":\"response\",\"text\":\"\","
                  "\"partial\":false}\n"
                  "{\"type\":\"tool_request\",\"tool\":\"get\","
                  "\"arguments\":{\"a\":1},\"id\":\"c1\","
                  "\"usage\":{\"input_tokens\":7,\"output_tokens\":9}}\n",
                  "proxy: anthropic sse mapping");
        buf_free(&j.b);
    }
    {
        const char *stream =
            "event: content_block_start\n"
            "data: {\"index\":0,\"content_block\":{\"type\":\"text\"}}\n\n"
            "event: content_block_delta\n"
            "data: {\"index\":0,\"delta\":{\"type\":\"text_delta\","
            "\"text\":\"cut\"}}\n\n";
        buf_init(&j.b);
        llm_proxy_sse_t *s =
            llm_proxy_sse_new(PROTO_ANTHROPIC, jb_fn, &j);
        llm_proxy_sse_feed(s, stream, strlen(stream));
        llm_proxy_sse_finish(s);
        llm_proxy_sse_free(s);
        check_str(j.b.data,
                  "{\"type\":\"response\",\"text\":\"cut\","
                  "\"partial\":true}\n"
                  "{\"type\":\"error\",\"code\":\"api_error\","
                  "\"message\":\"stream ended without message_stop\","
                  "\"fatal\":true}\n",
                  "proxy: anthropic stream without message_stop");
        buf_free(&j.b);
    }
    {
        const char *stream =
            "event: error\n"
            "data: {\"error\":{\"message\":\"boom\"}}\n\n";
        buf_init(&j.b);
        llm_proxy_sse_t *s =
            llm_proxy_sse_new(PROTO_ANTHROPIC, jb_fn, &j);
        llm_proxy_sse_feed(s, stream, strlen(stream));
        llm_proxy_sse_finish(s);
        llm_proxy_sse_free(s);
        check_str(j.b.data,
                  "{\"type\":\"error\",\"code\":\"api_error\","
                  "\"message\":\"boom\",\"fatal\":true}\n",
                  "proxy: sse error event");
        buf_free(&j.b);
    }

    /* ---- the rendered view is prettyprint's ---- */
    {
        char *ob = NULL;
        size_t on = 0;
        FILE *out = open_memstream(&ob, &on);
        check(out != NULL, "proxy: render memstream");
        pretty_live_t *pr = pretty_live_new(out);
        check(pr != NULL, "proxy: live renderer created");
        check(!pretty_live_io_failed(pr), "proxy: renderer healthy");
        /* the anthropic request + json response of the vectors above */
        cJSON *recs[7];
        recs[0] = cJSON_Parse(
            "{\"type\":\"system\",\"content\":[{\"type\":\"text\","
            "\"text\":\"sys\"}]}");
        recs[1] = cJSON_Parse(
            "{\"type\":\"tool_response\",\"id\":\"c1\",\"text\":\"res\"}");
        recs[2] = cJSON_Parse(
            "{\"type\":\"user\",\"content\":[{\"type\":\"text\","
            "\"text\":\"hi\"}]}");
        recs[3] = cJSON_Parse(
            "{\"type\":\"thinking\",\"text\":\"hm\",\"partial\":false,"
            "\"signature\":\"s\"}");
        recs[4] = cJSON_Parse(
            "{\"type\":\"response\",\"text\":\"ans\",\"partial\":false,"
            "\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}");
        recs[5] = cJSON_Parse(
            "{\"type\":\"tool_request\",\"tool\":\"get\","
            "\"arguments\":{\"a\":1},\"id\":\"c2\"}");
        recs[6] = cJSON_Parse(
            "{\"type\":\"error\",\"code\":\"api_error\","
            "\"message\":\"HTTP 500: nope\",\"fatal\":true}");
        for (int i = 0; i < 7; i++) {
            pretty_live_record(pr, recs[i]);
            cJSON_Delete(recs[i]);
        }
        check(!pretty_live_io_failed(pr), "proxy: renderer still healthy");
        pretty_live_free(pr);
        fclose(out);
        scrub_stamps(ob);
        buf_t want;
        buf_init(&want);
        exp_rule(&want, '-'); /* the system prompt: the light rule */
        buf_append_str(&want, "sys\n");
        exp_rule(&want, '-'); /* tool traffic: the light rule */
        buf_append_str(&want, "res\n");
        exp_rule(&want, '=');
        buf_append_str(&want, "hi\n");
        exp_rule(&want, '-');
        buf_append_str(&want, "hm\n");
        exp_rule(&want, '-');
        buf_append_str(&want,
                       "ans\n[TS] input 1 tok | output 2 tok\n");
        exp_rule(&want, '-');
        buf_append_str(&want, "get {\"a\":1}\n");
        buf_append_str(&want, "! api_error: HTTP 500: nope\n");
        check_str(ob, want.data ? want.data : "", "proxy: pretty rendering");
        free(ob);
        buf_free(&want);
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
    fprintf(stderr, "selfcheck: builtin-mcp\n");
    test_builtin();
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
    test_agent_tool_streamed();
    fprintf(stderr, "selfcheck: call\n");
    test_call();
    fprintf(stderr, "selfcheck: repl\n");
    test_repl();
    fprintf(stderr, "selfcheck: agent\n");
    test_agent();
    fprintf(stderr, "selfcheck: mcp-repl\n");
    test_mcp_repl();
    fprintf(stderr, "selfcheck: terminal tools\n");
    test_terminal_tools();
    fprintf(stderr, "selfcheck: llmkit proxy\n");
    test_llm_proxy();
    fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
