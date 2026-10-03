/* pretty.c - llmkit prettyprint: the transcript viewer - the repl's
   display (design sec.12) applied to a recorded conversation instead of
   a live one. One jsonl in - the file argument, else stdin - and out
   comes the whole exchange with the repl's shapes: the heavy rule
   opening each user block, light rules opening thinking, response and
   tool blocks, thinking italic, user lines and tool traffic bold,
   errors as `!` lines. The live-only parts have no record counterpart:
   there is no prompt (the input is the file) and no timing line (a
   transcript carries no clocks) - a response block whose closing
   record reports the turn's token usage closes with a usage line
   instead, the file's own turn totals. Config and control records
   render nothing, exactly the records a live session never showed.
   Input hygiene is the runner's byte pipeline; a violation or a
   malformed line is its fatal invalid_record, exit 2. */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= typography probe ================= */
/* (repl.c's probe, verbatim: one look per process, the same contract) */

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
/* (repl.c's sink minus the mono clock: the framing state machine only) */

typedef struct pretty_sink {
    FILE *out;
    const style_t *st;
    bool io_fail;    /* stdout write failed: out-of-channel exit 1 */
    bool wrote;      /* any transcript byte written */
    bool last_nl;    /* last written byte was \n */
    int cur;         /* open streamed block: R_THINKING/R_RESPONSE, -1 none */
    bool sep_pending;/* a block opened, its rule not yet drawn (lazy) */
} pretty_sink_t;

static void pwr(pretty_sink_t *s, const char *t, size_t n) {
    if (!n) return;
    if (fwrite(t, 1, n, s->out) != n || fflush(s->out) != 0) s->io_fail = true;
    s->wrote = true;
    s->last_nl = t[n - 1] == '\n';
}

static void pwr_str(pretty_sink_t *s, const char *t) { pwr(s, t, strlen(t)); }

static void ensure_nl(pretty_sink_t *s) {
    if (s->wrote && !s->last_nl) pwr_str(s, "\n");
}

static void draw_rule(pretty_sink_t *s, char glyph) {
    ensure_nl(s);
    int w = tty_cols(s->out);
    if (w <= 0) w = 80;
    char line[512];
    if (w > (int)sizeof line - 2) w = (int)sizeof line - 2;
    size_t tsl = stamp_now(line, sizeof line);
    if ((int)tsl > w) tsl = (size_t)w; /* pathological width: stamp only */
    memset(line + tsl, glyph, (size_t)w - tsl);
    line[w] = '\n';
    pwr(s, line, (size_t)w + 1);
}

/* the block's light rule, drawn lazily: a block whose text stays empty
   renders nothing, separator included (requirements sec.12) */
static void sep_flush(pretty_sink_t *s) {
    if (!s->sep_pending) return;
    draw_rule(s, '-');
    s->sep_pending = false;
}

static void styled(pretty_sink_t *s, const char *on, const char *text) {
    if (!text || !*text) return;
    sep_flush(s);
    if (*on) pwr_str(s, on);
    pwr_str(s, text);
    if (*on) pwr_str(s, s->st->reset);
}

static void render_error_line(pretty_sink_t *s, const char *code,
                              const char *msg) {
    ensure_nl(s);
    s->cur = -1; /* an error line closes any open block */
    pwr_str(s, "! ");
    pwr_str(s, code ? code : "error");
    pwr_str(s, ": ");
    pwr_str(s, msg ? msg : "");
    pwr_str(s, "\n");
}

/* the turn-closing usage line - the file's counterpart of the repl's
   timing line, stamped like one: the token totals of the turn as its
   final response record reports them. No usage on the closing record
   (a turn that ended any other way) renders nothing, timing's rule */
static void render_usage(pretty_sink_t *s, const cJSON *rec) {
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(rec, "usage");
    if (!cJSON_IsObject(u)) return;
    char ts[32] = "", line[128];
    stamp_now(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%sinput %.0f tok | output %.0f tok\n",
                     ts, rec_num(u, "input_tokens", 0.0),
                     rec_num(u, "output_tokens", 0.0));
    if (n <= 0) return;
    ensure_nl(s);
    pwr(s, line, (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

/* the user block: the heavy rule and the bold text of the content's
   text blocks joined with \n - what the repl's non-tty loop renders for
   a submitted line */
static void render_user_record(pretty_sink_t *s, const cJSON *rec) {
    draw_rule(s, '='); /* heavy rule opens each user block */
    s->cur = -1;
    buf_t text;
    buf_init(&text);
    bool any = false;
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(rec, "content");
    for (const cJSON *b = content ? content->child : NULL; b; b = b->next) {
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
        if (!cJSON_IsString(tx) || !tx->valuestring) continue;
        if (any) buf_append_byte(&text, '\n');
        buf_append_str(&text, tx->valuestring);
        any = true;
    }
    styled(s, s->st->bold, text.data ? text.data : "");
    ensure_nl(s);
    buf_free(&text);
}

/* one parsed record, the repl sink's dispatch (design sec.12):
   transcript records render, config and control records never did */
static void pretty_record(pretty_sink_t *s, const cJSON *rec) {
    int k = rec_classify(rec);
    if (k == R_USER) {
        render_user_record(s, rec);
    } else if (k == R_THINKING || k == R_RESPONSE) {
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
            if (k == R_RESPONSE) render_usage(s, rec);
            ensure_nl(s); /* per-block trailing newline, call's rule */
            s->cur = -1;
        }
    } else if (k == R_TOOL_REQUEST) {
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
        if (line.len) s->sep_pending = true; /* empty renders nothing */
        styled(s, s->st->bold, line.data ? line.data : "");
        ensure_nl(s);
        buf_free(&line);
    } else if (k == R_TOOL_RESPONSE) {
        const char *tx = rec_str(rec, "text");
        if (tx && *tx) s->sep_pending = true; /* empty renders nothing */
        s->cur = -1;
        styled(s, s->st->bold, tx);
        ensure_nl(s);
    } else if (k == R_ERROR) {
        render_error_line(s, rec_str(rec, "code"), rec_str(rec, "message"));
    }
    /* everything else - llm, tools, options, system, header, flush,
       start, agent-as-tool - renders nothing */
}

/* ================= input ================= */

typedef struct pretty_ctx {
    pretty_sink_t *s;
    int rc; /* EXIT_INVALID_RECORD once a line failed, else EXIT_OK */
} pretty_ctx_t;

static void pretty_line(void *ctx, char *line /*malloc'd*/) {
    pretty_ctx_t *c = ctx;
    cJSON *rec = jsonl_parse_line(line);
    free(line);
    if (!rec) {
        render_error_line(c->s, EC_INVALID_RECORD, "malformed json line");
        c->rc = EXIT_INVALID_RECORD;
        return;
    }
    int k = rec_classify(rec);
    /* the conversation view of the catalogue: expose/hide are proxy
       config, anything unknown is not a record at all */
    if (k == R_UNKNOWN || k == R_EXPOSE || k == R_HIDE) {
        render_error_line(c->s, EC_INVALID_RECORD, "unknown record type");
        cJSON_Delete(rec);
        c->rc = EXIT_INVALID_RECORD;
        return;
    }
    if (k == R_USER) { /* the one record whose shape the framing leans on */
        char *m = validate_content(
            cJSON_GetObjectItemCaseSensitive(rec, "content"));
        if (m) {
            render_error_line(c->s, EC_INVALID_RECORD, m);
            free(m);
            cJSON_Delete(rec);
            c->rc = EXIT_INVALID_RECORD;
            return;
        }
    }
    pretty_record(c->s, rec);
    cJSON_Delete(rec);
}

/* ================= command ================= */

/* render the conversation read from in onto out; the exit code:
   0 rendered, 2 invalid record, 1 stdout write failure */
int pretty_run(FILE *in, FILE *out) {
    style_t st;
    style_probe(&st, out);
    pretty_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.out = out;
    sink.st = &st;
    sink.cur = -1;

    pretty_ctx_t c = { &sink, EXIT_OK };
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, pretty_line, &c);
    char bbuf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(bbuf, 1, sizeof bbuf, in)) > 0)
        if (jsonl_feed(&p, bbuf, n) != 0) {
            ok = false; /* invalid utf-8 or a NUL byte */
            break;
        }
    if (ok && jsonl_eof(&p) != 0) ok = false;
    jsonl_pusher_free(&p);

    int rc = c.rc;
    if (!ok) {
        if (rc == EXIT_OK)
            render_error_line(&sink, EC_INVALID_RECORD,
                              "invalid utf-8 or NUL byte in input");
        rc = EXIT_INVALID_RECORD;
    }
    if (sink.io_fail) rc = EXIT_OUT_OF_CHANNEL;
    return rc;
}

static void pretty_usage(FILE *out) {
    fputs("usage: llmkit prettyprint [conversation.jsonl]\n"
          "       (no argument, or '-', reads stdin)\n",
          out);
}

int cmd_prettyprint(const char *path) {
    FILE *in = stdin;
    if (path && strcmp(path, "-")) {
        in = fopen(path, "r");
        if (!in) {
            fprintf(stderr, "llmkit prettyprint: cannot open '%s'\n", path);
            pretty_usage(stderr);
            return EXIT_OUT_OF_CHANNEL;
        }
    }
    int rc = pretty_run(in, stdout);
    if (in != stdin) fclose(in);
    return rc;
}
