/* editor.c - the shared line input: the raw-mode tty editor with history
   (repl's, design sec.12) and the non-tty plain reader, plus the wall-clock
   stamp both front-ends draw. The editor is display-append-only: bytes echo
   as typed, backspaces erase, history recall rewrites by erasure - and it
   hosts the one extension mcp-repl needs, a tab completion hook that may
   rewrite the line or print a candidate listing below it. */
#include "llmkit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HIST_CAP 128 /* in-memory ring, arrow recall only (design sec.12) */

/* ================= tty editor (raw mode) ================= */

static void ed_echo(editor_t *ed, const char *t, size_t n) {
    if (!n) return;
    /* best effort: a failing write surfaces through the sink later */
    fwrite(t, 1, n, ed->out);
    fflush(ed->out);
}

static void ed_erase(editor_t *ed, size_t n) {
    for (size_t i = 0; i < n; i++) ed_echo(ed, "\b \b", 3);
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

void editor_init(editor_t *ed, FILE *out, int in_fd, const char *prompt) {
    memset(ed, 0, sizeof *ed);
    ed->out = out;
    ed->prompt = prompt ? prompt : "";
    buf_init(&ed->line);
    tty_raw_on(&ed->raw, in_fd);
}

void editor_hist_push(editor_t *ed, const char *line) {
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

/* rewrite the whole line: erase, refill, echo (history recall, completion) */
void editor_set_line(editor_t *ed, const char *s, size_t n) {
    ed_erase(ed, ed->line.len);
    buf_clear(&ed->line);
    buf_append(&ed->line, s, n);
    ed_echo(ed, ed->line.data ? ed->line.data : "", ed->line.len);
}

/* print below the line (a candidate listing), then redraw prompt + line -
   the editor's one backward step, owed to completion only */
void editor_note(editor_t *ed, const char *text) {
    ed_echo(ed, "\n", 1);
    if (text) ed_echo(ed, text, strlen(text));
    ed_echo(ed, "\n", 1);
    ed_echo(ed, ed->prompt, strlen(ed->prompt));
    ed_echo(ed, ed->line.data ? ed->line.data : "", ed->line.len);
}

/* the two Ctrl-C stages: typed input clears, a clear prompt quits
   (requirements sec.12). Returns ED_CLEAR / ED_QUIT. */
static int ed_sigint_stage(editor_t *ed) {
    g_stop_flag = 0; /* consumed here */
    if (ed->line.len) {
        ed_erase(ed, ed->line.len);
        buf_clear(&ed->line);
        ed->hist_pos = -1;
        return ED_CLEAR;
    }
    return ED_QUIT;
}

int editor_line(editor_t *ed) {
    buf_clear(&ed->line);
    ed->hist_pos = -1;
    for (;;) {
        unsigned char c;
        int r = tty_read_byte(&ed->raw, &c);
        if (r < 0) return ed_sigint_stage(ed);
        if (r == 0) return ED_EOF;
        if (c == '\n') return ED_SUBMIT;
        if (c == 0x03) /* windows byte-mode ctrl-c: the same stage rule */
            return ed_sigint_stage(ed);
        if (c == '\r' || c == 0) continue; /* hygiene: CR dropped */
        if (c == 0x04) {                   /* Ctrl-D: submit / EOF on empty */
            if (ed->line.len) return ED_SUBMIT;
            return ED_EOF;
        }
        if (c == 0x09) { /* Tab: completion hook, when there is one */
            if (ed->on_tab) ed->on_tab(ed, ed->tab_ctx);
            continue;
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
            if (tty_read_byte(&ed->raw, &s1) != 1) continue;
            if (s1 != '[') continue;
            if (tty_read_byte(&ed->raw, &s2) != 1) continue;
            if (s2 == 'A') { /* up: older, clamped at the oldest */
                int next = ed->hist_pos < 0 ? (int)ed->nhist - 1
                                            : ed->hist_pos - 1;
                if (next < 0) continue;
                ed->hist_pos = next;
                editor_set_line(ed, ed->hist[next], strlen(ed->hist[next]));
            } else if (s2 == 'B' && ed->hist_pos >= 0) { /* down */
                if ((size_t)ed->hist_pos + 1 >= ed->nhist) {
                    ed->hist_pos = -1; /* back to the live line */
                    editor_set_line(ed, "", 0);
                } else {
                    ed->hist_pos++;
                    editor_set_line(ed, ed->hist[ed->hist_pos],
                                    strlen(ed->hist[ed->hist_pos]));
                }
            }
            continue;
        }
        buf_append_byte(&ed->line, (char)c);
        ed_echo(ed, (const char *)&c, 1);
    }
}

void editor_free(editor_t *ed) {
    tty_raw_off(&ed->raw);
    buf_free(&ed->line);
    for (size_t i = 0; i < ed->nhist; i++) free(ed->hist[i]);
    free(ed->hist);
}

/* ================= non-tty line reader ================= */

void plain_init(plain_reader_t *pr, int fd) {
    memset(pr, 0, sizeof *pr);
    pr->fd = fd;
    buf_init(&pr->hold);
}

void plain_free(plain_reader_t *pr) { buf_free(&pr->hold); }

int plain_line(plain_reader_t *pr, buf_t *line) {
    buf_clear(line);
    for (;;) {
        /* serve from the hold buffer first */
        for (size_t i = 0; i < pr->hold.len; i++) {
            if (pr->hold.data[i] == '\n') {
                buf_append(line, pr->hold.data, i);
                memmove(pr->hold.data, pr->hold.data + i + 1,
                        pr->hold.len - i - 1);
                pr->hold.len -= i + 1;
                return ED_SUBMIT;
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
                    if (line->len) return ED_CLEAR;
                    return ED_QUIT;
                }
                continue;
            }
            return ED_EOF;
        }
        if (n == 0) {
            if (line->len) return ED_SUBMIT; /* held fragment at EOF */
            return ED_EOF;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == 0) return ED_BAD; /* NUL rejected */
            if (c == '\r') continue;   /* hygiene: CR dropped */
            buf_append_byte(&pr->hold, c);
        }
    }
}

/* ================= wall clock stamp ================= */

/* "[HH:MM:SS] " - the wall-clock stamp of a separator or the timing line.
   localtime's static buffer is safe here: both callers draw from their own
   single thread. Returns the length written. */
size_t stamp_now(char *dst, size_t cap) {
    if (cap < 12) return 0;
    time_t t = time(NULL);
    struct tm *p = localtime(&t);
    if (!p) return 0;
    int n = snprintf(dst, cap, "[%02d:%02d:%02d] ", p->tm_hour, p->tm_min,
                     p->tm_sec);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}
