/* repl.c - llmkit repl: the interactive chat front-end (design sec.12).
   call's compiler minus --prompt, one session loop over one engine, and
   the display sink: ascii separators, tty-gated bold and italic,
   thinking and tool traffic rendered. Input is a program-owned raw-mode
   line buffer on a tty - the two Ctrl-C stages and the typed echo lean
   on it - or a plain line loop on anything else. */
#include "llmkit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define HIST_CAP 128 /* in-memory ring, arrow recall only (design sec.12) */

/* ================= typography probe ================= */

typedef struct style {
    char bold[32], italic[32], reset[32]; /* "" = attribute off */
} style_t;

static void style_probe(style_t *st, FILE *out) {
    memset(st, 0, sizeof *st);
    int fd = fileno(out);
    if (fd < 0 || !isatty(fd)) return;
    const char *term = getenv("TERM");
    if (!term || !*term || !strcmp(term, "dumb")) return;
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
    int w = 80;
    int fd = fileno(s->out);
    if (fd >= 0 && isatty(fd)) {
        struct winsize ws;
        if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) w = ws.ws_col;
    }
    char line[512];
    if (w > (int)sizeof line - 2) w = (int)sizeof line - 2;
    memset(line, glyph, (size_t)w);
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
        styled(s, k == R_THINKING ? s->st->italic : "", tx);
        if (!rec_bool(rec, "partial", false)) {
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

enum {
    RLINE_SUBMIT = 0, /* line ready in the buffer */
    RLINE_EOF,        /* end of input: session ends */
    RLINE_CLEAR,      /* Ctrl-C with typed input: discarded, prompt again */
    RLINE_QUIT,       /* Ctrl-C at a clear prompt: session ends, exit 8 */
    RLINE_BAD,        /* byte hygiene violation: invalid_record tier */
};

/* ================= tty editor (raw mode) ================= */

typedef struct editor {
    int fd;
    struct termios orig;
    bool raw_on;
    FILE *out;
    const style_t *st;
    buf_t line;
    char **hist;
    size_t nhist;
    int hist_pos; /* -1: the live line */
} editor_t;

static void ed_echo(editor_t *ed, const char *t, size_t n) {
    if (!n) return;
    /* best effort: a failing write surfaces through the sink later */
    fwrite(t, 1, n, ed->out);
    fflush(ed->out);
}

static void ed_erase(editor_t *ed, size_t n) {
    for (size_t i = 0; i < n; i++) ed_echo(ed, "\b \b", 3);
}

static bool ed_raw_on(editor_t *ed) {
    struct termios t;
    if (tcgetattr(ed->fd, &t) != 0) return false;
    ed->orig = t;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(ed->fd, TCSANOW, &t) != 0) return false;
    ed->raw_on = true;
    return true;
}

static void ed_raw_off(editor_t *ed) {
    if (ed->raw_on) tcsetattr(ed->fd, TCSANOW, &ed->orig);
    ed->raw_on = false;
}

static void hist_push(editor_t *ed, const char *line) {
    if (!line || !*line) return;
    char *d = strdup(line);
    if (!d) return;
    if (ed->nhist == HIST_CAP) {
        free(ed->hist[0]);
        memmove(ed->hist, ed->hist + 1, (HIST_CAP - 1) * sizeof(char *));
        ed->nhist--;
    }
    char **h = realloc(ed->hist, (ed->nhist + 1) * sizeof(char *));
    if (!h) {
        free(d);
        return;
    }
    ed->hist = h;
    ed->hist[ed->nhist++] = d;
}

static void ed_set_line(editor_t *ed, const char *s, size_t n) {
    ed_erase(ed, ed->line.len);
    buf_clear(&ed->line);
    buf_append(&ed->line, s, n);
    ed_echo(ed, ed->line.data ? ed->line.data : "", ed->line.len);
}

static void ed_backspace(editor_t *ed) {
    if (!ed->line.len) return;
    size_t drop = 1;
    while (ed->line.len - drop > 0 &&
           (ed->line.data[ed->line.len - drop] & 0xc0) == 0x80)
        drop++;
    ed_erase(ed, drop);
    ed->line.len -= drop;
}

/* the two Ctrl-C stages: typed input clears, a clear prompt quits
   (requirements sec.12). Returns RLINE_CLEAR / RLINE_QUIT. */
static int ed_sigint_stage(editor_t *ed) {
    g_stop_flag = 0; /* consumed here */
    if (ed->line.len) {
        ed_erase(ed, ed->line.len);
        buf_clear(&ed->line);
        ed->hist_pos = -1;
        return RLINE_CLEAR;
    }
    return RLINE_QUIT;
}

static int ed_read_byte(editor_t *ed, unsigned char *c) {
    for (;;) {
        ssize_t n = read(ed->fd, c, 1);
        if (n == 1) return 1;
        if (n == 0) return 0; /* EOF */
        if (errno == EINTR) {
            if (g_stop_flag) return -1; /* SIGINT: the stage rule */
            continue;
        }
        return 0; /* a read error ends input like EOF (ponytail) */
    }
}

static int ed_line(editor_t *ed) {
    buf_clear(&ed->line);
    ed->hist_pos = -1;
    for (;;) {
        unsigned char c;
        int r = ed_read_byte(ed, &c);
        if (r < 0) return ed_sigint_stage(ed);
        if (r == 0) return RLINE_EOF;
        if (c == '\n') return RLINE_SUBMIT;
        if (c == '\r' || c == 0) continue; /* hygiene: CR dropped */
        if (c == 0x04) {                   /* Ctrl-D: submit / EOF on empty */
            if (ed->line.len) return RLINE_SUBMIT;
            return RLINE_EOF;
        }
        if (c == 0x15) { /* Ctrl-U: kill the line */
            ed_erase(ed, ed->line.len);
            buf_clear(&ed->line);
            continue;
        }
        if (c == 0x7f || c == 0x08) {
            ed_backspace(ed);
            continue;
        }
        if (c == 0x1b) { /* escape: arrow history, the rest swallowed */
            unsigned char s1, s2;
            if (ed_read_byte(ed, &s1) != 1) continue;
            if (s1 != '[') continue;
            if (ed_read_byte(ed, &s2) != 1) continue;
            if (s2 == 'A') { /* up: older, clamped at the oldest */
                int next = ed->hist_pos < 0 ? (int)ed->nhist - 1
                                            : ed->hist_pos - 1;
                if (next < 0) continue;
                ed->hist_pos = next;
                ed_set_line(ed, ed->hist[next], strlen(ed->hist[next]));
            } else if (s2 == 'B' && ed->hist_pos >= 0) { /* down */
                if ((size_t)ed->hist_pos + 1 >= ed->nhist) {
                    ed->hist_pos = -1; /* back to the live line */
                    ed_set_line(ed, "", 0);
                } else {
                    ed->hist_pos++;
                    ed_set_line(ed, ed->hist[ed->hist_pos],
                                strlen(ed->hist[ed->hist_pos]));
                }
            }
            continue;
        }
        buf_append_byte(&ed->line, (char)c);
        ed_echo(ed, (const char *)&c, 1);
    }
}

static void ed_free(editor_t *ed) {
    ed_raw_off(ed);
    buf_free(&ed->line);
    for (size_t i = 0; i < ed->nhist; i++) free(ed->hist[i]);
    free(ed->hist);
}

/* ================= non-tty line reader ================= */

typedef struct plain_reader {
    int fd;
    buf_t hold; /* bytes after the last \n of the previous chunk */
} plain_reader_t;

static int plain_line(plain_reader_t *pr, buf_t *line) {
    buf_clear(line);
    for (;;) {
        /* serve from the hold buffer first */
        for (size_t i = 0; i < pr->hold.len; i++) {
            if (pr->hold.data[i] == '\n') {
                buf_append(line, pr->hold.data, i);
                memmove(pr->hold.data, pr->hold.data + i + 1,
                        pr->hold.len - i - 1);
                pr->hold.len -= i + 1;
                goto served;
            }
        }
        buf_append(line, pr->hold.data, pr->hold.len);
        pr->hold.len = 0;
        char tmp[4096];
        ssize_t n = read(pr->fd, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop_flag) { /* the stage rule, plain variant */
                    g_stop_flag = 0;
                    if (line->len) return RLINE_CLEAR;
                    return RLINE_QUIT;
                }
                continue;
            }
            return RLINE_EOF;
        }
        if (n == 0) {
            if (line->len) return RLINE_SUBMIT; /* held fragment at EOF */
            return RLINE_EOF;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == 0) return RLINE_BAD; /* NUL rejected */
            if (c == '\r') continue;      /* hygiene: CR dropped */
            buf_append_byte(&pr->hold, c);
        }
    }
served:
    return RLINE_SUBMIT;
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
    memset(&ed, 0, sizeof ed);
    plain_reader_t pr;
    memset(&pr, 0, sizeof pr);
    if (tty) {
        ed.fd = in_fd;
        ed.out = out;
        ed.st = &st;
        buf_init(&ed.line);
        ed_raw_on(&ed);
    } else {
        pr.fd = in_fd;
        buf_init(&pr.hold);
    }

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
            r = RLINE_QUIT;
            goto handled;
        }
        if (tty) {
            if (need_rule) draw_rule(&sink, '=');
            rwr_str(&sink, "> ");
            rwr_str(&sink, st.bold); /* the typed line is the user block */
            r = ed_line(&ed);
            rwr_str(&sink, st.reset);
            rwr_str(&sink, "\n");
        } else {
            r = plain_line(&pr, &line);
        }
    handled:
        if (r == RLINE_QUIT) { /* Ctrl-C at a clear prompt: exit 8 */
            rc = EXIT_INTERRUPTED;
            goto done;
        }
        if (r == RLINE_EOF) { /* EOF: the last ending's code */
            rc = last_ending;
            goto done;
        }
        if (r == RLINE_BAD) {
            render_error_line(&sink, EC_INVALID_RECORD,
                              "NUL byte in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        if (r == RLINE_CLEAR) { /* discarded input, fresh prompt line */
            need_rule = false;
            continue;
        }
        /* RLINE_SUBMIT: the editor owns its buffer, the loop works on
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
        if (tty) hist_push(&ed, line.data);

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

        rc = engine_start(e);
        if (rc == 0) rc = engine_run(e);
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
    if (tty) ed_free(&ed);
    else buf_free(&pr.hold);
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
          "                [--system-prompt <text>] "
          "[--header <name=value>]...\n"
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
