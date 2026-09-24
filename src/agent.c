/* agent.c - llmkit agent-as-tool (design sec.9). */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* captured record sink */
typedef struct cap_rec {
    cJSON *tree;
    struct cap_rec *next;
} cap_rec_t;

typedef struct agent_sink {
    cap_rec_t *head, *tail;
} agent_sink_t;

static void agent_sink_fn(void *ctx, cJSON *rec) {
    agent_sink_t *s = ctx;
    cap_rec_t *c = calloc(1, sizeof *c);
    c->tree = rec;
    if (s->tail) s->tail->next = c;
    else s->head = c;
    s->tail = c;
}

static void agent_sink_reset(agent_sink_t *s) {
    while (s->head) {
        cap_rec_t *c = s->head;
        s->head = c->next;
        cJSON_Delete(c->tree);
        free(c);
    }
    s->tail = NULL;
}

typedef struct agent_state {
    engine_t *eng;
    cJSON *agent_cfg;
    tlist_t seed; /* seed transcript records */
    bool retain;
    size_t mark; /* transcript length after the last successful invoke */
} agent_state_t;

/* ---- seed loading ---- */

typedef struct seed_ctx {
    agent_state_t *a;
    bool ok;          /* false once a fatal error was printed */
    bool first;
} seed_ctx_t;

static void seed_fatal(seed_ctx_t *sc, const char *msg) {
    fprintf(stderr, "llmkit agent-as-tool: %s\n", msg);
    sc->ok = false;
}

static void seed_on_line(void *ctx, char *line) {
    seed_ctx_t *sc = ctx;
    if (!sc->ok) {
        free(line);
        return;
    }
    cJSON *t = jsonl_parse_line(line);
    free(line);
    if (!t) {
        seed_fatal(sc, "malformed json line in seed");
        return;
    }
    agent_state_t *a = sc->a;
    engine_t *e = a->eng;
    int k = rec_classify(t);
    bool first = sc->first;
    sc->first = false;
    char err[512] = "";
    switch (k) {
    case R_HEADER:
        if (first) {
            double v = rec_num(t, "version", -1);
            if (v != 1) {
                seed_fatal(sc, "unsupported header version in seed");
                cJSON_Delete(t);
                return;
            }
        }
        cJSON_Delete(t);
        return;
    case R_ERROR:
        cJSON_Delete(t); /* inert on input */
        return;
    case R_FLUSH:
    case R_START:
        seed_fatal(sc, "flush and start records are invalid in a seed");
        cJSON_Delete(t);
        return;
    case R_EXPOSE:
    case R_HIDE:
        seed_fatal(sc, "expose/hide records are invalid in a seed");
        cJSON_Delete(t);
        return;
    case R_AGENT:
        cJSON_Delete(a->agent_cfg);
        a->agent_cfg = cJSON_Duplicate(t, 1);
        cJSON_Delete(t);
        return;
    case R_LLM:
        if (validate_llm(t)) {
            seed_fatal(sc, "invalid llm record in seed");
            cJSON_Delete(t);
            return;
        }
        engine_apply_config_record(e, t);
        cJSON_Delete(t);
        return;
    case R_OPTIONS:
        if (validate_options(t)) {
            seed_fatal(sc, "invalid options record in seed");
            cJSON_Delete(t);
            return;
        }
        engine_apply_config_record(e, t);
        cJSON_Delete(t);
        return;
    case R_TOOLS:
        if (validate_tools(t)) {
            seed_fatal(sc, "invalid tools record in seed");
            cJSON_Delete(t);
            return;
        }
        engine_apply_config_record(e, t);
        cJSON_Delete(t);
        return;
    case R_SYSTEM: {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(t, "content");
        char *m = validate_content(c);
        if (m) {
            snprintf(err, sizeof err, "invalid system record in seed: %s", m);
            free(m);
            seed_fatal(sc, err);
            cJSON_Delete(t);
            return;
        }
        engine_apply_config_record(e, t);
        cJSON_Delete(t);
        return;
    }
    default: {
        /* user + transcript history records */
        char *m = NULL;
        if (k == R_USER) {
            m = validate_content(
                cJSON_GetObjectItemCaseSensitive(t, "content"));
            if (m) {
                snprintf(err, sizeof err, "invalid user record in seed: %s", m);
                free(m);
                seed_fatal(sc, err);
                cJSON_Delete(t);
                return;
            }
        }
        tlist_ingest(&a->seed, t);
        cJSON_Delete(t);
        return;
    }
    }
}

static bool load_jsonl_file(const char *path, void (*on_line)(void *, char *),
                            void *ctx) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, on_line, ctx);
    char bbuf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(bbuf, 1, sizeof bbuf, f)) > 0)
        if (jsonl_feed(&p, bbuf, n) != 0) {
            ok = false;
            break;
        }
    if (ok && jsonl_eof(&p) != 0) ok = false;
    jsonl_pusher_free(&p);
    fclose(f);
    return ok;
}

/* ---- invoke ---- */

typedef struct invoke_out {
    bool ok;
    char *text;      /* final response text (owned) */
    char *err_code;  /* fatal error code (owned) */
    char *err_msg;
} invoke_out_t;

/* rebuild a transcript from the seed records. Text-like trecs carry no
   record tree (the transcript stores their concatenated text), so their
   records are re-synthesized; the other kinds keep theirs. */
static void seed_reingest(tlist_t *dst, const tlist_t *seed) {
    for (size_t i = 0; i < seed->n; i++) {
        const trec_t *r = seed->v[i];
        cJSON *rec = NULL;
        if (r->kind == T_THINK)
            rec = rec_thinking_final(r->text ? r->text : "", r->signature);
        else if (r->kind == T_TEXT)
            rec = rec_text("response", r->text ? r->text : "", false);
        else if (r->tree)
            rec = cJSON_Duplicate(r->tree, 1);
        if (rec) {
            tlist_ingest(dst, rec);
            cJSON_Delete(rec);
        }
    }
}

static void invoke_reset_transcript(agent_state_t *a) {
    if (!a->retain) {
        tlist_clear(&a->eng->tr);
        seed_reingest(&a->eng->tr, &a->seed);
    }
}

static void invoke_run(agent_state_t *a, const char *input,
                       invoke_out_t *out) {
    engine_t *e = a->eng;
    agent_sink_t sink = { 0 };
    e->emit = agent_sink_fn;
    e->emit_ctx = &sink;

    cJSON *urec = cJSON_Parse(
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"\"}]}");
    cJSON *blocks = cJSON_GetObjectItemCaseSensitive(urec, "content");
    cJSON *blk = blocks ? blocks->child : NULL;
    if (blk)
        cJSON_ReplaceItemInObjectCaseSensitive(blk, "text",
                                               cJSON_CreateString(input));
    tlist_ingest(&e->tr, urec);
    cJSON_Delete(urec);

    free(e->terminal_id); /* per invoke: no stale id from a previous run */
    e->terminal_id = NULL;

    int rc = engine_run(e);

    /* find the fatal error, the final response or the terminal answer */
    const char *fatal_code = NULL, *fatal_msg = NULL;
    const char *tresp_text = NULL;
    bool tresp_is_error = false;
    buf_t final;
    buf_init(&final);
    for (cap_rec_t *c = sink.head; c; c = c->next) {
        int k = rec_classify(c->tree);
        if (k == R_ERROR) {
            const cJSON *fat =
                cJSON_GetObjectItemCaseSensitive(c->tree, "fatal");
            if (cJSON_IsTrue(fat)) {
                fatal_code = rec_str(c->tree, "code");
                fatal_msg = rec_str(c->tree, "message");
            }
        } else if (k == R_RESPONSE &&
                   !rec_bool(c->tree, "partial", true)) {
            buf_clear(&final);
            buf_append_str(&final, rec_str(c->tree, "text"));
        } else if (k == R_TOOL_RESPONSE && e->terminal_id) {
            /* ids are unique per request: the record carrying the
               terminal id is the terminal tool's own answer - later
               synthesized suspensions carry other ids */
            const char *id = rec_str(c->tree, "id");
            if (id && !strcmp(id, e->terminal_id)) {
                tresp_text = rec_str(c->tree, "text");
                tresp_is_error = rec_bool(c->tree, "is_error", false);
            }
        }
    }

    memset(out, 0, sizeof *out);
    bool term_answer = rc == EXIT_TERMINAL_TOOL && tresp_text != NULL;
    if (term_answer && tresp_is_error) {
        /* failed terminal tool: failed call carrying the message */
        out->ok = false;
        out->err_code = strdup(EC_TOOL_FAILED);
        out->err_msg = strdup(tresp_text ? tresp_text : "tool failed");
    } else if (term_answer) {
        out->ok = true;
        out->text = strdup(tresp_text ? tresp_text : "");
    } else if (rc == EXIT_OK && !fatal_code) {
        out->ok = true;
        out->text = final.data ? strdup(final.data) : strdup("");
    } else {
        out->ok = false;
        out->err_code = strdup(fatal_code ? fatal_code : "io_error");
        out->err_msg = strdup(fatal_msg ? fatal_msg : "conversation failed");
    }
    buf_free(&final);
    agent_sink_reset(&sink);
}

/* ---- rpc handlers ---- */

static int agent_handle(void *ctx, const char *method, cJSON *params,
                        cJSON *id, cJSON **result_out, char **errmsg_out) {
    agent_state_t *a = ctx;
    (void)id;
    (void)errmsg_out;
    if (!strcmp(method, "initialize")) {
        cJSON *res = cJSON_CreateObject();
        const char *pr = rec_str(params, "protocolVersion");
        const char *use =
            (pr && (!strcmp(pr, "2026-07-28") || !strcmp(pr, "2025-11-25") ||
                    !strcmp(pr, "2025-06-18") || !strcmp(pr, "2025-03-26") ||
                    !strcmp(pr, "2024-11-05")))
                ? pr
                : "2025-11-25";
        cJSON_AddStringToObject(res, "protocolVersion", use);
        cJSON *caps = cJSON_CreateObject();
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(res, "capabilities", caps);
        cJSON *si = cJSON_CreateObject();
        cJSON_AddStringToObject(si, "name", "llmkit-agent-as-tool");
        cJSON_AddStringToObject(si, "version", LLMKIT_VERSION);
        cJSON_AddItemToObject(res, "serverInfo", si);
        *result_out = res;
        return 0;
    }
    if (!strcmp(method, "notifications/initialized") ||
        !strcmp(method, "ping"))
        return 2;
    if (!strcmp(method, "tools/list")) {
        cJSON *res = cJSON_CreateObject();
        cJSON *tools = cJSON_CreateArray();
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", "invoke");
        const char *d = rec_str(a->agent_cfg, "tool_description");
        if (d) cJSON_AddStringToObject(tool, "description", d);
        cJSON *schema = cJSON_Parse(
            "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":"
            "\"string\"}},\"required\":[\"input\"]}");
        if (schema) cJSON_AddItemToObject(tool, "inputSchema", schema);
        const char *id2 = rec_str(a->agent_cfg, "input_description");
        if (id2 && schema) {
            cJSON *props =
                cJSON_GetObjectItemCaseSensitive(schema, "properties");
            cJSON *inp =
                props ? cJSON_GetObjectItemCaseSensitive(props, "input")
                      : NULL;
            if (inp) cJSON_AddStringToObject(inp, "description", id2);
        }
        cJSON_AddItemToArray(tools, tool);
        cJSON_AddItemToObject(res, "tools", tools);
        *result_out = res;
        return 0;
    }
    if (!strcmp(method, "tools/call")) {
        const char *name = rec_str(params, "name");
        if (!name || strcmp(name, "invoke")) {
            *errmsg_out = strdup("unknown tool");
            return 1;
        }
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const char *input = rec_str(args, "input");
        if (!input) {
            *errmsg_out = strdup("invoke requires a string argument 'input'");
            return 1;
        }
        invoke_reset_transcript(a);
        invoke_out_t out;
        invoke_run(a, input, &out);
        cJSON *res = cJSON_CreateObject();
        cJSON *content = cJSON_CreateArray();
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        if (out.ok) {
            cJSON_AddStringToObject(blk, "text", out.text);
        } else {
            char msg[768];
            snprintf(msg, sizeof msg, "%s: %s", out.err_code, out.err_msg);
            cJSON_AddStringToObject(blk, "text", msg);
            cJSON_AddBoolToObject(res, "isError", true);
        }
        cJSON_AddItemToArray(content, blk);
        cJSON_AddItemToObject(res, "content", content);
        *result_out = res;
        if (out.ok) {
            if (a->retain) a->mark = a->eng->tr.n;
        } else {
            /* rollback to the last successful invoke (the bare seed if none) */
            tlist_truncate(&a->eng->tr, a->retain ? a->mark : 0);
        }
        free(out.text);
        free(out.err_code);
        free(out.err_msg);
        return 0;
    }
    *errmsg_out = strdup("method not found");
    return 1;
}

int cmd_agent(const char *seed_path) {
    signals_init();
    agent_state_t a;
    memset(&a, 0, sizeof a);
    a.eng = engine_new(agent_sink_fn, NULL);
    a.retain = false;

    seed_ctx_t sc = { &a, true, true };
    if (!load_jsonl_file(seed_path, seed_on_line, &sc) || !sc.ok) {
        fprintf(stderr, "llmkit agent-as-tool: fatal seed error in %s\n",
                seed_path);
        return 1;
    }
    if (!a.eng->llm || !a.agent_cfg) {
        fprintf(stderr,
                "llmkit agent-as-tool: seed requires llm and agent-as-tool "
                "records\n");
        return 1;
    }
    /* options: retain_context */
    if (a.eng->options) {
        const cJSON *rc =
            cJSON_GetObjectItemCaseSensitive(a.eng->options, "retain_context");
        if (cJSON_IsTrue(rc)) a.retain = true;
    }
    /* retain mode keeps one accumulated transcript: it starts as the bare
       seed (the rollback marker); each invoke appends to it. Forget mode
       re-ingests the seed per invoke instead. */
    if (a.retain) seed_reingest(&a.eng->tr, &a.seed);
    /* connect the seed's own mcp servers; failures are out of channel */
    if (a.eng->tools_cfg) {
        a.eng->emit = NULL; /* no record channel during bootstrap */
        int rc = mcp_reconcile((mcp_mgr_t *)a.eng->mcp, a.eng, a.eng->tools_cfg);
        if (rc) {
            fprintf(stderr,
                    "llmkit agent-as-tool: required mcp server failed to "
                    "connect\n");
            return 1;
        }
    }
    a.mark = a.eng->tr.n; /* bare seed marker */

    rpc_handler_t h = { agent_handle, &a };
    rpc_serve_stdio(&h);
    mcp_kill_all((mcp_mgr_t *)a.eng->mcp);
    engine_free(a.eng);
    tlist_clear(&a.seed);
    cJSON_Delete(a.agent_cfg);
    return 0;
}
