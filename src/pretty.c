/* pretty.c - llmkit prettyprint: the transcript viewer - the repl's
   display (design sec.12) applied to a recorded conversation instead of
   a live one. One jsonl in - the file argument, else stdin - and out
   comes the whole exchange with the repl's shapes: the heavy rule
   opening each user block, light rules opening system, thinking,
   response and tool blocks, system prompts and thinking italic, user
   lines and tool traffic bold,
   errors as `!` lines. The live-only parts have no record counterpart:
   there is no prompt (the input is the file) and no timing line (a
   transcript carries no clocks) - a response block whose closing
   record reports the turn's token usage closes with a usage line
   instead, the file's own turn totals. The remaining config and
   control records render nothing, exactly the records a live session
   never showed.
   Input hygiene is the runner's byte pipeline; a violation or a
   malformed line is its fatal invalid_record, exit 2. */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= display framing ================= */
/* the typography probe and the framing state machine shared with the
   repl's live sink (design sec.12): one look per process */

void style_probe(style_t *st, FILE *out) {
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

void dspy_write(dspy_t *s, const char *t, size_t n) {
    if (!n) return;
    if (fwrite(t, 1, n, s->out) != n || fflush(s->out) != 0) s->io_fail = true;
    s->wrote = true;
    s->last_nl = t[n - 1] == '\n';
}

void dspy_write_str(dspy_t *s, const char *t) { dspy_write(s, t, strlen(t)); }

void dspy_ensure_nl(dspy_t *s) {
    if (s->wrote && !s->last_nl) dspy_write_str(s, "\n");
}

void dspy_rule(dspy_t *s, char glyph) {
    dspy_ensure_nl(s);
    int w = tty_cols(s->out);
    if (w <= 0) w = 80;
    char line[512];
    if (w > (int)sizeof line - 2) w = (int)sizeof line - 2;
    size_t tsl = stamp_now(line, sizeof line);
    if ((int)tsl > w) tsl = (size_t)w; /* pathological width: stamp only */
    memset(line + tsl, glyph, (size_t)w - tsl);
    line[w] = '\n';
    dspy_write(s, line, (size_t)w + 1);
}

/* the block's light rule, drawn lazily: a block whose text stays empty
   renders nothing, separator included (requirements sec.12) */
void dspy_sep_flush(dspy_t *s) {
    if (!s->sep_pending) return;
    dspy_rule(s, '-');
    s->sep_pending = false;
}

void dspy_styled(dspy_t *s, const char *on, const char *text) {
    if (!text || !*text) return;
    dspy_sep_flush(s);
    if (*on) dspy_write_str(s, on);
    dspy_write_str(s, text);
    if (*on) dspy_write_str(s, s->st->reset);
}

void dspy_error_line(dspy_t *s, const char *code, const char *msg) {
    dspy_ensure_nl(s);
    s->cur = -1; /* an error line closes any open block */
    dspy_write_str(s, "! ");
    dspy_write_str(s, code ? code : "error");
    dspy_write_str(s, ": ");
    dspy_write_str(s, msg ? msg : "");
    dspy_write_str(s, "\n");
}

/* the tool traffic arms both record dispatchers share: the tool name
   with its arguments tree, and the response text, bold */
void dspy_tool_request(dspy_t *s, const cJSON *rec) {
    s->cur = -1;
    const char *tool = rec_str(rec, "tool");
    buf_t line;
    buf_init(&line);
    if (tool) buf_append_str(&line, tool);
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(rec, "arguments");
    if (cJSON_IsObject(args)) {
        if (line.len) buf_append_byte(&line, ' ');
        buf_append_tree(&line, args);
    }
    if (line.len) s->sep_pending = true; /* empty renders nothing */
    dspy_styled(s, s->st->bold, line.data ? line.data : "");
    dspy_ensure_nl(s);
    buf_free(&line);
}

void dspy_tool_response(dspy_t *s, const char *tx) {
    if (tx && *tx) s->sep_pending = true; /* empty renders nothing */
    s->cur = -1;
    dspy_styled(s, s->st->bold, tx);
    dspy_ensure_nl(s);
}

/* the turn-closing usage line - the file's counterpart of the repl's
   timing line, stamped like one: the token totals of the turn as its
   final response record reports them. No usage on the closing record
   (a turn that ended any other way) renders nothing, timing's rule */
static void render_usage(dspy_t *s, const cJSON *rec) {
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(rec, "usage");
    if (!cJSON_IsObject(u)) return;
    char ts[32] = "", line[128];
    stamp_now(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%sinput %.0f tok | output %.0f tok\n",
                     ts, rec_num(u, "input_tokens", 0.0),
                     rec_num(u, "output_tokens", 0.0));
    if (n <= 0) return;
    dspy_ensure_nl(s);
    dspy_write(s, line, (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

/* the record's content text blocks joined with \n - the
   caller-authored shape user and system records share */
void dspy_content_text(const cJSON *rec, buf_t *out) {
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(rec, "content");
    bool any = false;
    for (const cJSON *b = content ? content->child : NULL; b; b = b->next) {
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(b, "text");
        if (!cJSON_IsString(tx) || !tx->valuestring) continue;
        if (any) buf_append_byte(out, '\n');
        buf_append_str(out, tx->valuestring);
        any = true;
    }
}

/* the user block: the heavy rule and the bold text of the content's
   text blocks joined with \n - what the repl's non-tty loop renders for
   a submitted line */
static void render_user_record(dspy_t *s, const cJSON *rec) {
    dspy_rule(s, '='); /* heavy rule opens each user block */
    s->cur = -1;
    buf_t text;
    buf_init(&text);
    dspy_content_text(rec, &text);
    dspy_styled(s, s->st->bold, text.data ? text.data : "");
    dspy_ensure_nl(s);
    buf_free(&text);
}

/* one parsed record, the repl sink's dispatch (design sec.12):
   transcript records render, config and control records never did */
static void pretty_record(dspy_t *s, const cJSON *rec) {
    int k = rec_classify(rec);
    if (k == R_USER) {
        render_user_record(s, rec);
    } else if (k == R_SYSTEM) {
        /* the instructions block: a light rule and italic text, the
           caller's words where thinking carries the model's */
        buf_t text;
        buf_init(&text);
        dspy_content_text(rec, &text);
        if (text.len) s->sep_pending = true; /* empty renders nothing */
        s->cur = -1;
        dspy_styled(s, s->st->italic, text.data);
        dspy_ensure_nl(s);
        buf_free(&text);
    } else if (k == R_THINKING || k == R_RESPONSE) {
        const char *tx = rec_str(rec, "text");
        /* arm the separator only when something will render: the wire's
           block-close records carry empty text (signature, usage) and
           must not open a block that never draws */
        if (tx && *tx && s->cur != k) {
            s->sep_pending = true;
            s->cur = k;
        }
        dspy_styled(s, k == R_THINKING ? s->st->italic : "", tx);
        if (!rec_bool(rec, "partial", false)) {
            if (k == R_RESPONSE) render_usage(s, rec);
            dspy_ensure_nl(s); /* per-block trailing newline, call's rule */
            s->cur = -1;
        }
    } else if (k == R_TOOL_REQUEST) {
        dspy_tool_request(s, rec);
    } else if (k == R_TOOL_RESPONSE) {
        dspy_tool_response(s, rec_str(rec, "text"));
    } else if (k == R_ERROR) {
        dspy_error_line(s, rec_str(rec, "code"), rec_str(rec, "message"));
    }
    /* everything else - ll, tools, options, header, flush, start,
       agent-as-tool - renders nothing */
}

/* ================= live renderer ================= */
/* the same sink pretty_run frames, but fed record by record by a caller
   watching a live wire (llmkit proxy) instead of reading a file. Renders
   exactly what prettyprint renders - that is the point. */

struct pretty_live {
    dspy_t s;
    style_t st; /* owned: the sink keeps a pointer into this */
};

pretty_live_t *pretty_live_new(FILE *out) {
    pretty_live_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    style_probe(&l->st, out);
    l->s.out = out;
    l->s.st = &l->st;
    l->s.cur = -1;
    return l;
}

void pretty_live_record(pretty_live_t *l, cJSON *rec) {
    pretty_record(&l->s, rec);
}

bool pretty_live_io_failed(const pretty_live_t *l) { return l->s.io_fail; }

void pretty_live_free(pretty_live_t *l) { free(l); }

/* ================= input ================= */

typedef struct pretty_ctx {
    dspy_t *s;
    int rc; /* EXIT_INVALID_RECORD once a line failed, else EXIT_OK */
} pretty_ctx_t;

static void pretty_line(void *ctx, char *line /*malloc'd*/) {
    pretty_ctx_t *c = ctx;
    cJSON *rec = jsonl_parse_line(line);
    free(line);
    if (!rec) {
        dspy_error_line(c->s, EC_INVALID_RECORD, "malformed json line");
        c->rc = EXIT_INVALID_RECORD;
        return;
    }
    int k = rec_classify(rec);
    /* the conversation view of the catalogue: expose/hide are proxy
       config, anything unknown is not a record at all */
    if (k == R_UNKNOWN || k == R_EXPOSE || k == R_HIDE) {
        dspy_error_line(c->s, EC_INVALID_RECORD, "unknown record type");
        cJSON_Delete(rec);
        c->rc = EXIT_INVALID_RECORD;
        return;
    }
    if (k == R_USER) { /* the one record whose shape the framing leans on */
        char *m = validate_content(
            cJSON_GetObjectItemCaseSensitive(rec, "content"));
        if (m) {
            dspy_error_line(c->s, EC_INVALID_RECORD, m);
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
    dspy_t sink;
    memset(&sink, 0, sizeof sink);
    sink.out = out;
    sink.st = &st;
    sink.cur = -1;

    pretty_ctx_t c = { &sink, EXIT_OK };
    bool ok = jsonl_read_stream(in, pretty_line, &c);

    int rc = c.rc;
    if (!ok) {
        if (rc == EXIT_OK)
            dspy_error_line(&sink, EC_INVALID_RECORD,
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
