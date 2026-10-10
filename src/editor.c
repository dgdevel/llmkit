/* editor.c - the shared line input: vendored linenoise (src/vendor/
   linenoise) wrapped into llmkit's editor contract - the prompt, the
   echo, the refreshes and the closing newline of a line are linenoise's,
   the two Ctrl-C stages (requirements sec.12) are the wrapper's - plus
   the non-tty plain reader and the wall-clock stamp both front-ends
   draw. Raw mode cycles per line (linenoise enters and leaves it), so
   a turn always runs against a cooked terminal and ctrl-c reaches the
   engine's stop flag on both platforms alike. */
#include "llmkit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HIST_CAP 128       /* in-memory, arrow recall only (design sec.12) */
#define LNBUF_INIT 4096    /* linenoise's initial edit buffer */
#define LNBUF_MAX (1024 * 1024) /* linenoise's LINENOISE_MAX_LINE: the
                                   line-length ceiling, paste included */

/* ================= tty editor (linenoise) ================= */

/* the process runs one interactive front-end, so one editor at a time:
   linenoise's completion callback carries no context pointer, the
   bridge reaches the live editor through here */
static editor_t *ed_active;

static void ed_complete_bridge(const char *line, linenoiseCompletions *lc) {
    editor_t *ed = ed_active;
    if (ed && ed->on_tab) ed->on_tab(ed->tab_ctx, line, lc);
}

void editor_add_candidate(editor_candidates_t *c, const char *line) {
    linenoiseAddCompletion(c, line);
}

void editor_init(editor_t *ed, FILE *out, int in_fd, const char *prompt) {
    memset(ed, 0, sizeof *ed);
    ed->out = out;
    ed->in_fd = in_fd;
    ed->prompt = prompt ? prompt : "";
    buf_init(&ed->line);
    ed->lbuf = malloc(LNBUF_INIT);
    linenoiseHistorySetMaxLen(HIST_CAP);
    linenoiseSetCompletionCallback(ed_complete_bridge);
    ed_active = ed;
}

void editor_hist_push(editor_t *ed, const char *line) {
    (void)ed; /* linenoise's history is its own single table */
    if (!line || !*line) return;
    linenoiseHistoryAdd(line); /* consecutive duplicates not repeated */
}

int editor_line(editor_t *ed) {
    buf_clear(&ed->line);
    if (!ed->lbuf) return ED_EOF;
    if (linenoiseEditStart(&ed->ls, ed->in_fd, fileno(ed->out), ed->lbuf,
                           LNBUF_INIT, ed->prompt) == -1)
        return ED_EOF; /* not a terminal: the front-ends never get here */
    ed->ls.buflen_max = LNBUF_MAX; /* grow like the blocking API does */

    char *res = linenoiseEditMore;
    while (res == linenoiseEditMore) res = linenoiseEditFeed(&ed->ls);

    int rc = ED_EOF; /* the default exit: I/O error or ENOMEM */
    if (res) {
        buf_append(&ed->line, res, strlen(res));
        linenoiseFree(res);
        rc = ED_SUBMIT;
    } else if (errno == EAGAIN) {
        /* ctrl-c as a byte (windows) or the SIGINT the interrupted read
           surfaced (posix): linenoise popped the live history entry and
           left buffer and display untouched. The stages read the buffer:
           typed input clears - the line is wiped, the prompt redrawn -
           a clear prompt quits (requirements sec.12) */
        g_stop_flag = 0; /* consumed: not the engine's orderly stop */
        if (ed->ls.len > 0) {
            ed->ls.buf[0] = '\0';
            ed->ls.pos = ed->ls.len = 0;
            ed->ls.fold_count = 0;
            linenoiseHide(&ed->ls);  /* erase the line, cursor to col 0 */
            linenoiseShow(&ed->ls);  /* the prompt, redrawn empty */
            rc = ED_CLEAR;
        } else {
            rc = ED_QUIT;
        }
    } else if (errno == ENOENT) {
        rc = ED_EOF; /* ctrl-d on the empty line */
    } else if (errno == EILSEQ) {
        rc = ED_BAD; /* NUL byte: the invalid_record tier */
    }
    linenoiseEditStop(&ed->ls); /* cooked terminal back, the line closed */
    return rc;
}

void editor_free(editor_t *ed) {
    linenoiseSetCompletionCallback(NULL);
    if (ed_active == ed) ed_active = NULL;
    free(ed->lbuf);
    ed->lbuf = NULL;
    buf_free(&ed->line);
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
    return strftime(dst, cap, "[%H:%M:%S] ", p);
}
