/* repl.c - llmkit repl: the interactive chat front-end (design sec.12).
   call's compiler minus --prompt, one session loop over one engine, and
   the display sink: ascii separators, tty-gated bold and italic,
   thinking and tool traffic rendered. Input is the shared raw-mode line
   editor of src/editor.c on a tty - the two Ctrl-C stages and the typed
   echo lean on it - or its plain line loop on anything else. */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= typography probe ================= */

typedef struct style {
    char bold[32], italic[32], reset[32]; /* "" = attribute off */
} style_t;

static void style_probe(style_t *st, FILE *out) {
    memset(st, 0, sizeof *st);
    int fd = fileno(out);
    if (fd < 0 || !isatty(fd)) return;
#ifdef _WIN32
    /* SGR escapes render only where the VT output mode was armed
       (platform_init, win10+); windows consoles have no TERM contract */
    if (!tty_vt_enabled(fd)) return;
#else
    const char *term = getenv("TERM");
    if (!term || !*term || !strcmp(term, "dumb")) return;
#endif
    /* SGR escapes: universal in every terminal TERM admits here */
    snprintf(st->bold, sizeof st->bold, "\033[1m");
    snprintf(st->italic, sizeof st->italic, "\033[3m");
    snprintf(st->reset, sizeof st->reset, "\033[0m");
}

/* ================= display sink ================= */

typedef struct repl_sink {
    FILE *out;
    const style_t *st;
    bool io_fail;    /* stdout write failed: out-of-channel exit 1 */
    bool wrote;      /* any transcript byte written */
    bool last_nl;    /* last written byte was \n */
    int cur;         /* open streamed block: R_THINKING/R_RESPONSE, -1 none */
    bool sep_pending;/* a block opened, its rule not yet drawn (lazy) */
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

static void rwr(repl_sink_t *s, const char *t, size_t n) {
    if (!n) return;
    if (fwrite(t, 1, n, s->out) != n || fflush(s->out) != 0) s->io_fail = true;
    s->wrote = true;
    s->last_nl = t[n - 1] == '\n';
}

static void rwr_str(repl_sink_t *s, const char *t) { rwr(s, t, strlen(t)); }

static void ensure_nl(repl_sink_t *s) {
    if (s->wrote && !s->last_nl) rwr_str(s, "\n");
}

static void draw_rule(repl_sink_t *s, char glyph) {
    ensure_nl(s);
    int w = tty_cols(s->out);
    if (w <= 0) w = 80;
    char line[512];
    if (w > (int)sizeof line - 2) w = (int)sizeof line - 2;
    size_t tsl = stamp_now(line, sizeof line);
    if ((int)tsl > w) tsl = (size_t)w; /* pathological width: stamp only */
    memset(line + tsl, glyph, (size_t)w - tsl);
    line[w] = '\n';
    rwr(s, line, (size_t)w + 1);
}

/* the block's light rule, drawn lazily: a block whose text stays empty
   renders nothing, separator included (requirements sec.12) */
static void sep_flush(repl_sink_t *s) {
    if (!s->sep_pending) return;
    draw_rule(s, '-');
    s->sep_pending = false;
}

static void styled(repl_sink_t *s, const char *on, const char *text) {
    if (!text || !*text) return;
    sep_flush(s);
    if (*on) rwr_str(s, on);
    rwr_str(s, text);
    if (*on) rwr_str(s, s->st->reset);
}

static void render_error_line(repl_sink_t *s, const char *code,
                              const char *msg) {
    ensure_nl(s);
    s->cur = -1; /* an error line closes any open block */
    rwr_str(s, "! ");
    rwr_str(s, code ? code : "error");
    rwr_str(s, ": ");
    rwr_str(s, msg ? msg : "");
    rwr_str(s, "\n");
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
    ensure_nl(s);
    rwr(s, line, (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

static void repl_sink_fn(void *ctx, cJSON *rec) {
    repl_sink_t *s = ctx;
    int k = rec_classify(rec);
    if (k == R_THINKING || k == R_RESPONSE) {
        const char *tx = rec_str(rec, "text");
        /* arm the separator only when something will render: the wire's
           block-close records carry empty text (signature, usage) and
           must not open a block that never draws */
        if (tx && *tx && s->cur != k) {
            s->sep_pending = true;
            s->cur = k;
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
        styled(s, k == R_THINKING ? s->st->italic : "", tx);
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
            ensure_nl(s); /* per-block trailing newline, call's rule */
            s->cur = -1;
        }
    } else if (k == R_TOOL_REQUEST) {
        s->sep_pending = true;
        s->cur = -1;
        const char *tool = rec_str(rec, "tool");
        buf_t line;
        buf_init(&line);
        if (tool) buf_append_str(&line, tool);
        const cJSON *args =
            cJSON_GetObjectItemCaseSensitive(rec, "arguments");
        if (cJSON_IsObject(args)) {
            if (line.len) buf_append_byte(&line, ' ');
            buf_append_tree(&line, args);
        }
        styled(s, s->st->bold, line.data ? line.data : "");
        buf_free(&line);
        ensure_nl(s);
    } else if (k == R_TOOL_RESPONSE) {
        const char *tx = rec_str(rec, "text");
        if (tx && *tx) s->sep_pending = true; /* empty renders nothing */
        s->cur = -1;
        styled(s, s->st->bold, tx);
        ensure_nl(s);
    } else if (k == R_ERROR) {
        render_error_line(s, rec_str(rec, "code"), rec_str(rec, "message"));
    }
    /* everything else - start markers above all - renders nothing */
    cJSON_Delete(rec);
}

/* the user block in non-tty mode: no editor echoed it, the loop renders it */
static void render_user_block(repl_sink_t *s, const char *text) {
    draw_rule(s, '='); /* heavy rule opens each user block */
    styled(s, s->st->bold, text);
    ensure_nl(s);
}

/* ================= input: line results ================= */
/* (ED_SUBMIT/ED_EOF/ED_CLEAR/ED_QUIT/ED_BAD live in llmkit.h: the editor
   is shared with mcp-repl, src/editor.c) */

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
    style_t st;
    style_probe(&st, out);
    repl_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.out = out;
    sink.st = &st;
    sink.cur = -1;

    engine_t *e = engine_new(repl_sink_fn, &sink);
    if (factory) e->wire_factory = factory;
    e->keep_mcp = true; /* the session continues; engine_free tears down */
    bool tty = false;

    int rc = call_compile(c, e, exe_path);
    if (rc) goto done;
    {
        /* the one record call does not compile: display latency is the
           point, every streamed chunk renders as it arrives */
        cJSON *opts = repl_build_options();
        engine_apply_config_record(e, opts);
        cJSON_Delete(opts);
    }

    tty = isatty(in_fd);
    editor_t ed;
    plain_reader_t pr;
    if (tty)
        editor_init(&ed, out, in_fd, "> ");
    else
        plain_init(&pr, in_fd);

    int last_ending = EXIT_OK; /* EOF before any input exits 0 */
    bool need_rule = true;     /* rule vs no-rule prompt redraws */
    buf_t line;
    buf_init(&line);

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
            if (need_rule) draw_rule(&sink, '=');
            rwr_str(&sink, "> ");
            rwr_str(&sink, st.bold); /* the typed line is the user block */
            r = editor_line(&ed);
            rwr_str(&sink, st.reset);
            rwr_str(&sink, "\n");
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
            render_error_line(&sink, EC_INVALID_RECORD,
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
            render_error_line(&sink, EC_INVALID_RECORD,
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
                render_error_line(&sink, EC_INVALID_RECORD, m);
                free(m);
                cJSON_Delete(user);
                rc = EXIT_INVALID_RECORD;
                goto done;
            }
            tlist_ingest(&e->tr, user);
            cJSON_Delete(user);
        }

#ifdef _WIN32
        /* cooked console while a turn runs: ctrl-c must raise the orderly
           stop signal instead of queueing a 0x03 byte for the next line */
        if (tty) tty_raw_off(&ed.raw);
#endif
        timing_reset(&sink);
        rc = engine_start(e);
        if (rc == 0) rc = engine_run(e);
#ifdef _WIN32
        if (tty) tty_raw_on(&ed.raw, in_fd);
#endif
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
    engine_free(e); /* process teardown: the mcp children die here */
    if (sink.io_fail) rc = EXIT_OUT_OF_CHANNEL;
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
