/* repl.c - llmkit repl and llmkit agent: the interactive chat front-end
   (design sec.12).
   call's compiler minus --prompt, one session loop over one engine, and
   the display sink: ascii separators, tty-gated bold and italic,
   thinking and tool traffic rendered. Input is the shared raw-mode line
   editor of src/editor.c on a tty - the two Ctrl-C stages and the typed
   echo lean on it - or its plain line loop on anything else. Without
   --system-prompt the session starts from the bundled default prompt
   (src/prompts/system_prompts/repl.txt, tools/gen-prompts.sh).
   `agent` is the same session with the extras of repl_opts_t: the
   built-in mcp server rides the tools record, ./AGENTS.md is injected
   as a second system content block, and --conversation-store persists
   the conversation as jsonl - the runner's record catalogue - replaying
   an existing file's records through the prettyprint renderer before
   the first prompt. */
#include "llmkit.h"
#include "prompts.gen.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the line that prefixes the AGENTS.md content block */
#define AGENTS_MD_PREFIX "##### Content of AGENTS.md #####\n"

/* ================= display sink ================= */
/* the shared framing state machine (pretty.c) plus the session's own
   extras: the conversation store and the turn timing spans */

typedef struct repl_sink {
    dspy_t d; /* the framing state (out, styles, rules) */
    /* the conversation store: appended per completed record when set */
    FILE *store;
    bool store_fail; /* a store write failed: io_error exit 7 */
    /* per-turn timing, the mono clock taken at sink depth: reset by the
       session loop before each engine_run */
    double t_turn;   /* the request clock: prompt processing counts here */
    double t_first;  /* first streamed token of the turn, 0 until it lands */
    double t_think;  /* open thinking generation segment, 0 when closed */
    double t_resp;   /* open response generation segment, 0 when closed */
    double think_s;  /* thinking generation, accumulated over all blocks */
    double resp_s;   /* response generation, accumulated over all blocks */
    bool resp_done;  /* a response block completed this turn */
} repl_sink_t;

/* the store's write side: called from the sink for every emitted record
   and from the loop for every submitted user record */
static void store_persist(repl_sink_t *s, const cJSON *rec) {
    if (!s->store || s->store_fail) return;
    int k = rec_classify(rec);
    /* every transcript record, partials included: a streamed block
       arrives as partial chunks plus an empty-text final, the text only
       exists whole in that sequence (the runner's stdout stream, refolded
       by tlist_ingest on load). Config and control records are not
       conversation. */
    bool keep = k == R_USER || k == R_ERROR || k == R_TOOL_REQUEST ||
                k == R_TOOL_RESPONSE || k == R_THINKING || k == R_RESPONSE;
    if (!keep) return;
    char *p = cJSON_PrintUnformatted((cJSON *)rec);
    if (!p) return;
    size_t n = strlen(p);
    if (fwrite(p, 1, n, s->store) != n || fputc('\n', s->store) == EOF ||
        fflush(s->store) != 0)
        s->store_fail = true;
    cJSON_free(p);
}

/* the turn's timing spans, reset by the session loop at turn start */
static void timing_reset(repl_sink_t *s) {
    s->t_turn = mono_now();
    s->t_first = 0;
    s->t_think = 0;
    s->t_resp = 0;
    s->think_s = 0;
    s->resp_s = 0;
    s->resp_done = false;
}

/* the turn's timing line, drawn when the response block completed: wall
   clock of the completion, then the three spans - first token since the
   request (prompt processing), thinking generation and response
   generation, tool rounds excluded, accumulated over every block */
static void render_timing(repl_sink_t *s) {
    char ts[32] = "", line[192];
    stamp_now(ts, sizeof ts);
    int n = snprintf(line, sizeof line,
                     "%sfirst token %.2fs | thinking %.2fs | response %.2fs\n",
                     ts, s->t_first ? s->t_first - s->t_turn : 0.0, s->think_s,
                     s->resp_s);
    if (n <= 0) return;
    dspy_ensure_nl(&s->d);
    dspy_write(&s->d, line,
               (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

static void repl_sink_fn(void *ctx, cJSON *rec) {
    repl_sink_t *s = ctx;
    store_persist(s, rec);
    int k = rec_classify(rec);
    if (k == R_THINKING || k == R_RESPONSE) {
        const char *tx = rec_str(rec, "text");
        /* arm the separator only when something will render: the wire's
           block-close records carry empty text (signature, usage) and
           must not open a block that never draws */
        if (tx && *tx && s->d.cur != k) {
            s->d.sep_pending = true;
            s->d.cur = k;
        }
        if (tx && *tx) { /* a token landed: open its generation segment */
            double now = mono_now();
            if (!s->t_first) s->t_first = now;
            if (k == R_THINKING) {
                if (!s->t_think) s->t_think = now;
            } else if (!s->t_resp) {
                s->t_resp = now;
            }
        }
        dspy_styled(&s->d, k == R_THINKING ? s->d.st->italic : "", tx);
        if (!rec_bool(rec, "partial", false)) {
            double now = mono_now(); /* the block closed its segment */
            if (k == R_THINKING) {
                if (s->t_think) {
                    s->think_s += now - s->t_think;
                    s->t_think = 0;
                }
            } else {
                if (s->t_resp) {
                    s->resp_s += now - s->t_resp;
                    s->t_resp = 0;
                }
                s->resp_done = true;
            }
            dspy_ensure_nl(&s->d); /* per-block trailing newline, call's rule */
            s->d.cur = -1;
        }
    } else if (k == R_TOOL_REQUEST) {
        dspy_tool_request(&s->d, rec);
    } else if (k == R_TOOL_RESPONSE) {
        dspy_tool_response(&s->d, rec_str(rec, "text"));
    } else if (k == R_ERROR) {
        dspy_error_line(&s->d, rec_str(rec, "code"), rec_str(rec, "message"));
    }
    /* everything else - start markers above all - renders nothing */
    cJSON_Delete(rec);
}

/* the user block in non-tty mode: no editor echoed it, the loop renders it */
static void render_user_block(repl_sink_t *s, const char *text) {
    dspy_rule(&s->d, '='); /* heavy rule opens each user block */
    dspy_styled(&s->d, s->d.st->bold, text);
    dspy_ensure_nl(&s->d);
}

/* ================= input: line results ================= */
/* (ED_SUBMIT/ED_EOF/ED_CLEAR/ED_QUIT/ED_BAD live in llmkit.h: the editor
   is shared with mcp-repl, src/editor.c) */

/* ================= conversation store ================= */
/* the store is the conversation as jsonl - the runner's record
   catalogue, exactly what prettyprint renders and what a resumed
   session replays into its transcript. One line per transcript record,
   appended as the session runs. */

/* ---- store load: replay through the prettyprint renderer, records
   into the transcript ---- */

typedef struct store_load {
    engine_t *e;
    pretty_live_t *live;
    int rc; /* EXIT_OK until a line fails */
} store_load_t;

static void store_load_fail(store_load_t *sc, const char *msg) {
    if (sc->rc != EXIT_OK) return;
    sc->rc = EXIT_INVALID_RECORD;
    pretty_live_record(sc->live, rec_error(EC_INVALID_RECORD, msg, true));
}

static void store_load_line(void *ctx, char *line /*malloc'd*/) {
    store_load_t *sc = ctx;
    if (sc->rc != EXIT_OK) {
        free(line);
        return;
    }
    cJSON *rec = jsonl_parse_line(line);
    free(line);
    if (!rec) {
        store_load_fail(sc, "malformed json line in the conversation store");
        return;
    }
    int k = rec_classify(rec);
    switch (k) {
    case R_USER:
    case R_THINKING:
    case R_RESPONSE:
    case R_TOOL_REQUEST:
    case R_TOOL_RESPONSE:
    case R_ERROR: {
        if (k == R_USER) {
            char *m = validate_content(
                cJSON_GetObjectItemCaseSensitive(rec, "content"));
            if (m) {
                store_load_fail(sc, m);
                free(m);
                cJSON_Delete(rec);
                return;
            }
        }
        tlist_ingest(&sc->e->tr, rec); /* duplicates what it needs */
        pretty_live_record(sc->live, rec);
        cJSON_Delete(rec);
        return;
    }
    default: /* config and control records: not conversation */
        cJSON_Delete(rec);
        return;
    }
}

/* replay a store onto out and into the engine transcript; 0 ok - a file
   that does not exist is a fresh conversation - else the exit code */
static int store_load(engine_t *e, FILE *out, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    pretty_live_t *live = pretty_live_new(out);
    if (!live) {
        fclose(f);
        return EXIT_OUT_OF_CHANNEL;
    }
    store_load_t sc = { e, live, EXIT_OK };
    bool ok = jsonl_read_stream(f, store_load_line, &sc);
    fclose(f);
    int rc = sc.rc;
    if (!ok) {
        if (rc == EXIT_OK)
            store_load_fail(&sc,
                            "invalid utf-8 or NUL byte in the conversation "
                            "store");
        rc = EXIT_INVALID_RECORD;
    }
    if (rc == EXIT_OK) {
        /* a store that ends mid-block - an interrupted turn's trailing
           partial: the block closes, the next turn starts a fresh one */
        if (e->tr.n) {
            trec_t *last = e->tr.v[e->tr.n - 1];
            if ((last->kind == T_THINK || last->kind == T_TEXT) &&
                !last->complete)
                last->complete = true;
        }
    }
    if (pretty_live_io_failed(live)) rc = EXIT_OUT_OF_CHANNEL;
    pretty_live_free(live);
    return rc;
}

/* ---- the second system entry: ./AGENTS.md ---- */

/* the prompt's own content block stays first; the file's content rides
   in its own block after it, prefixed with the content line */
static int inject_agents_md(engine_t *e, const char *prompt_text) {
    FILE *f = fopen("AGENTS.md", "r");
    if (!f) return 0; /* absent: nothing to inject */
    buf_t b;
    buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&b, tmp, n);
    bool ioerr = ferror(f) != 0;
    fclose(f);

    int rc = 0;
    if (ioerr) {
        engine_emit_record(e,
                           rec_error(EC_IO_ERROR, "cannot read AGENTS.md", true));
        rc = EXIT_IO_ERROR;
    } else {
        if (b.len >= 3 && !memcmp(b.data, "\xef\xbb\xbf", 3)) {
            memmove(b.data, b.data + 3, b.len - 3); /* editor artifact */
            b.len -= 3;
        }
        if (!utf8_valid((const uint8_t *)(b.data ? b.data : ""), b.len)) {
            engine_emit_record(e,
                               rec_error(EC_INVALID_RECORD,
                                         "invalid UTF-8 in AGENTS.md", true));
            rc = EXIT_INVALID_RECORD;
        } else {
            buf_t tx;
            buf_init(&tx);
            buf_append_str(&tx, AGENTS_MD_PREFIX);
            buf_append(&tx, b.data ? b.data : "", b.len);
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "type", "system");
            cJSON *content = cJSON_AddArrayToObject(sys, "content");
            cJSON *blk = cJSON_CreateObject();
            cJSON_AddStringToObject(blk, "type", "text");
            cJSON_AddStringToObject(blk, "text",
                                    prompt_text ? prompt_text : "");
            cJSON_AddItemToArray(content, blk);
            blk = cJSON_CreateObject();
            cJSON_AddStringToObject(blk, "type", "text");
            cJSON_AddStringToObject(blk, "text", tx.data ? tx.data : "");
            cJSON_AddItemToArray(content, blk);
            char *m = validate_content(content);
            if (m) {
                engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
                free(m);
                rc = EXIT_INVALID_RECORD;
            } else {
                engine_apply_config_record(e, sys);
            }
            cJSON_Delete(sys);
            buf_free(&tx);
        }
    }
    buf_free(&b);
    return rc;
}

/* ================= session ================= */

cJSON *repl_build_options(void) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "options");
    cJSON_AddNumberToObject(t, "stream_interval", 0);
    return t;
}

/* hygiene of a submitted line: strict UTF-8 (CR and NUL never arrive) */
static bool line_valid(const buf_t *b) {
    return utf8_valid((const uint8_t *)(b->data ? b->data : ""), b->len);
}

int repl_run(const call_cfg_t *c, int in_fd, FILE *out, const char *exe_path,
             wire_t *(*factory)(engine_t *)) {
    repl_opts_t o = { .default_prompt = prompt_system_prompts_repl };
    return repl_run_ex(c, &o, in_fd, out, exe_path, factory);
}

int repl_run_ex(const call_cfg_t *c, const repl_opts_t *o, int in_fd,
                FILE *out, const char *exe_path,
                wire_t *(*factory)(engine_t *)) {
    style_t st;
    style_probe(&st, out);
    repl_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.d.out = out;
    sink.d.st = &st;
    sink.d.cur = -1;

    engine_t *e = engine_new(repl_sink_fn, &sink);
    if (factory) e->wire_factory = factory;
    e->keep_mcp = true; /* the session continues; engine_free tears down */

    /* input first: every goto done below releases what it meets */
    bool tty = isatty(in_fd);
    editor_t ed;
    plain_reader_t pr;
    char prompt[48]; /* the editor draws it; escapes cost no columns */
    snprintf(prompt, sizeof prompt, "%s> ", st.bold);
    if (tty)
        /* the typed line is the user block: the prompt carries the bold
           sequence, the editor counts its width past the escape */
        editor_init(&ed, out, in_fd, prompt);
    else
        plain_init(&pr, in_fd);
    int last_ending = EXIT_OK; /* EOF before any input exits 0 */
    bool need_rule = true;     /* rule vs no-rule prompt redraws */
    buf_t line;
    buf_init(&line);

    /* the bundled default system prompt: an explicit --system-prompt
       replaces it, and (per the call contract) a different empty text vs
       no record stays distinguishable on the wire. the cast is safe: the
       config is read-only past this point (call_compile never writes
       through the pointer, cmd_repl frees the original). */
    call_cfg_t cfg = *c;
    if (!cfg.system && o->default_prompt)
        cfg.system = (char *)o->default_prompt;

    int rc = call_compile(&cfg, e, exe_path);
    if (rc) goto done;
    {
        /* the one record call does not compile: display latency is the
           point, every streamed chunk renders as it arrives */
        cJSON *opts = repl_build_options();
        engine_apply_config_record(e, opts);
        cJSON_Delete(opts);
    }
    if (o->agents_md) {
        rc = inject_agents_md(e, cfg.system);
        if (rc) goto done;
    }
    if (o->builtin_mcp) {
        /* the built-in server rides the proxies' tools record: one
           record owns the whole server set (engine_apply replaces) */
        cJSON *tools = call_build_agent_tools(&cfg, exe_path);
        char *m = validate_tools(tools);
        if (m) {
            engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
            free(m);
            cJSON_Delete(tools);
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        engine_apply_config_record(e, tools);
        cJSON_Delete(tools);
    }
    if (o->store && *o->store) {
        /* an existing store replays fully - display and transcript -
           before the first prompt; either way the session appends */
        rc = store_load(e, out, o->store);
        if (rc) goto done;
        sink.store = fopen(o->store, "a");
        if (!sink.store) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "cannot open the conversation store '%s'", o->store);
            engine_emit_record(e, rec_error(EC_IO_ERROR, msg, true));
            rc = EXIT_IO_ERROR;
            goto done;
        }
    }

    for (;;) {
        int r;
        /* a SIGINT racing the read start still meets the stage rule: at a
           fresh prompt the line is empty by construction, so it quits */
        if (g_stop_flag) {
            g_stop_flag = 0;
            r = ED_QUIT;
            goto handled;
        }
        if (tty) {
            if (need_rule) dspy_rule(&sink.d, '=');
            r = editor_line(&ed); /* prompt, echo and the closing newline
                                     are the editor's (linenoise) */
            dspy_write_str(&sink.d, st.reset); /* the bold user block closed */
        } else {
            r = plain_line(&pr, &line);
        }
    handled:
        if (r == ED_QUIT) { /* Ctrl-C at a clear prompt: exit 8 */
            rc = EXIT_INTERRUPTED;
            goto done;
        }
        if (r == ED_EOF) { /* EOF: the last ending's code */
            rc = last_ending;
            goto done;
        }
        if (r == ED_BAD) {
            dspy_error_line(&sink.d, EC_INVALID_RECORD,
                            "NUL byte in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        if (r == ED_CLEAR) { /* discarded input, fresh prompt line */
            need_rule = false;
            continue;
        }
        /* ED_SUBMIT: the editor owns its buffer, the loop works on
           line - transfer before the shared checks */
        if (tty) {
            buf_clear(&line);
            if (ed.line.len)
                buf_append(&line, ed.line.data, ed.line.len);
        }
        if (!line.len) { /* empty input: ignored, no user record, no turn */
            need_rule = false;
            continue;
        }
        if (!line_valid(&line)) {
            dspy_error_line(&sink.d, EC_INVALID_RECORD,
                            "invalid UTF-8 in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        need_rule = true;
        if (tty) editor_hist_push(&ed, line.data);

        if (!tty) render_user_block(&sink, line.data);

        {
            cJSON *user = call_build_user(line.data);
            char *m = validate_content(
                cJSON_GetObjectItemCaseSensitive(user, "content"));
            if (m) {
                dspy_error_line(&sink.d, EC_INVALID_RECORD, m);
                free(m);
                cJSON_Delete(user);
                rc = EXIT_INVALID_RECORD;
                goto done;
            }
            store_persist(&sink, user); /* the store's turn opener */
            tlist_ingest(&e->tr, user);
            cJSON_Delete(user);
        }

        /* a turn runs against the cooked terminal: raw mode cycles per
           line now (the editor enters and leaves it), so ctrl-c raises
           the stop signal on both platforms - no byte queues behind it */
        timing_reset(&sink);
        rc = engine_start(e);
        if (rc == 0) rc = engine_run(e);
        if (sink.resp_done) render_timing(&sink); /* response completed */
        last_ending = rc;
        if (rc == EXIT_INTERRUPTED) {
            g_stop_flag = 0; /* consumed: the session continues */
            continue;
        }
        if (rc == EXIT_OK) continue;
        goto done; /* fatal codes and the terminal 9 end the session */
    }

done:
    buf_free(&line);
    if (tty) editor_free(&ed);
    else plain_free(&pr);
    if (sink.store) fclose(sink.store);
    if (sink.store_fail && rc == EXIT_OK) {
        /* the session ran, its transcript did not land whole */
        engine_emit_record(e, rec_error(EC_IO_ERROR,
                                        "the conversation store write failed",
                                        true));
        rc = EXIT_IO_ERROR;
    }
    engine_free(e); /* process teardown: the mcp children die here */
    if (sink.d.io_fail) rc = EXIT_OUT_OF_CHANNEL;
    return rc;
}

/* ================= command ================= */

static void repl_usage(FILE *out) {
    fputs("usage: llmkit repl (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "                [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "                [--reasoning-effort <value>] "
          "[--system-prompt <text>]\n"
          "                [--header <name=value>]...\n"
          "                [--mcp-proxy <config>]... "
          "[--terminal-tool <name.tool>]...\n",
          out);
}

int cmd_repl(int argc, char **argv) {
    signals_init(); /* SIGINT is an input control here: the stage rule at
                       the prompt, the orderly stop in a turn */
    call_cfg_t c;
    char err[256] = "";
    if (call_parse_ex(argc - 2, argv + 2, &c, err, sizeof err, false) != 0) {
        fprintf(stderr, "llmkit repl: %s\n", err);
        repl_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    char exe[4096];
    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
    int rc = repl_run(&c, STDIN_FILENO, stdout, exe, NULL);
    call_cfg_free(&c);
    return rc;
}

/* ================= agent command ================= */

int repl_store_extract(int argc, char **argv, const char **store,
                       char *err, size_t errsz) {
    int n = 0;
    *store = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--conversation-store")) {
            if (*store) {
                snprintf(err, errsz, "--conversation-store given twice");
                return -1;
            }
            if (i + 1 >= argc) {
                snprintf(err, errsz,
                         "missing value for --conversation-store");
                return -1;
            }
            *store = argv[i + 1];
            i++;
            continue;
        }
        argv[n++] = argv[i];
    }
    return n;
}

static void agent_usage(FILE *out) {
    fputs("usage: llmkit agent (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "                [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "                [--reasoning-effort <value>] "
          "[--system-prompt <text>]\n"
          "                [--header <name=value>]...\n"
          "                [--mcp-proxy <config>]... "
          "[--terminal-tool <name.tool>]...\n"
          "                [--conversation-store <file>]\n",
          out);
}

/* the repl surface plus the agent extras: the built-in mcp server,
   AGENTS.md injected after the system prompt, the bundled agent prompt,
   and the conversation store when --conversation-store names one */
int cmd_agent_repl(int argc, char **argv) {
    signals_init(); /* SIGINT is an input control here, as in repl */
    char err[256] = "";
    const char *store = NULL;
    int n = repl_store_extract(argc - 2, argv + 2, &store, err, sizeof err);
    if (n < 0) {
        fprintf(stderr, "llmkit agent: %s\n", err);
        agent_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    call_cfg_t c;
    if (call_parse_ex(n, argv + 2, &c, err, sizeof err, false) != 0) {
        fprintf(stderr, "llmkit agent: %s\n", err);
        agent_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    char exe[4096];
    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
    repl_opts_t o = { .default_prompt = prompt_system_prompts_agent,
                      .builtin_mcp = true,
                      .agents_md = true,
                      .store = store };
    int rc = repl_run_ex(&c, &o, STDIN_FILENO, stdout, exe, NULL);
    call_cfg_free(&c);
    return rc;
}
