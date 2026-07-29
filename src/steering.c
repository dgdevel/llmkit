#include "steering.h"
#include "platform.h"
#include "util.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* A simple FIFO of complete messages (ring of malloc'd strings). */
typedef struct msg_node {
    char *text;
    struct msg_node *next;
} msg_node;

static msg_node *s_head = NULL; /* oldest */
static msg_node *s_tail = NULL; /* newest */

/* Accumulator for bytes read from stdin but not yet a complete message. */
static char *s_buf = NULL;
static size_t s_buf_len = 0;
static size_t s_buf_cap = 0;

static bool s_eof = false;

/* Read scratch buffer. Large enough that one poll() usually drains all
 * pending input in a single non-blocking read(). */
#define STEER_READ_SIZE 4096

/* Push a copy of [data, data+len) onto the message queue. */
static void enqueue(const char *data, size_t len) {
    msg_node *n = malloc(sizeof(msg_node));
    if (n == NULL) {
        return; /* drop on OOM rather than abort the run */
    }
    n->text = malloc(len + 1);
    if (n->text == NULL) {
        free(n);
        return;
    }
    if (len > 0) memcpy(n->text, data, len);
    n->text[len] = '\0';
    n->next = NULL;

    if (s_tail != NULL) {
        s_tail->next = n;
    } else {
        s_head = n;
    }
    s_tail = n;
}

/* Append bytes to the accumulator, growing as needed. */
static int append_buf(const char *data, size_t len) {
    size_t need = s_buf_len + len;
    if (need > s_buf_cap) {
        size_t newcap = s_buf_cap ? s_buf_cap : 128;
        while (newcap < need) newcap *= 2;
        char *tmp = realloc(s_buf, newcap);
        if (tmp == NULL) return -1;
        s_buf = tmp;
        s_buf_cap = newcap;
    }
    memcpy(s_buf + s_buf_len, data, len);
    s_buf_len += len;
    return 0;
}

void steering_feed(const char *data, size_t len) {
    if (data == NULL || len == 0) return;
    if (append_buf(data, len) != 0) return;
    steering_drain_pending();
}

void steering_signal_eof(void) {
    s_eof = true;
    steering_drain_pending();
}

/* Scan the accumulator for complete messages delimited by a blank line
 * ("\n\n"). Each completed message (with trailing newlines trimmed) is
 * enqueued and consumed bytes are dropped from the front of s_buf.
 *
 * CR ('\r') bytes are stripped first, so CRLF ("\r\n") line endings collapse
 * to "\n" and a CRLF-style blank line ("\r\n\r\n") becomes "\n\n".
 */
static void scan_messages(void) {
    /* Strip all carriage returns in place. */
    if (s_buf_len > 0) {
        size_t w = 0;
        for (size_t r = 0; r < s_buf_len; r++) {
            if (s_buf[r] == '\r') continue;
            s_buf[w++] = s_buf[r];
        }
        s_buf_len = w;
    }

    size_t i = 0;         /* scan cursor */
    size_t msg_start = 0; /* start of the current message */

    while (i + 1 < s_buf_len) {
        if (s_buf[i] == '\n' && s_buf[i + 1] == '\n') {
            /* Message body is s_buf[msg_start .. i). */
            size_t mlen = i - msg_start;
            char *msg = malloc(mlen + 1);
            if (msg != NULL) {
                memcpy(msg, s_buf + msg_start, mlen);
                while (mlen > 0 && msg[mlen - 1] == '\n') mlen--; /* trim trailing NL */
                msg[mlen] = '\0';
                if (mlen > 0) {
                    enqueue(msg, mlen);
                }
                free(msg);
            }
            /* Resume scanning right after the delimiter. */
            i += 2;
            msg_start = i;
        } else {
            i++;
        }
    }

    /* Keep the unconsumed tail [msg_start, s_buf_len). */
    if (msg_start > 0) {
        size_t remain = s_buf_len - msg_start;
        if (remain > 0) {
            memmove(s_buf, s_buf + msg_start, remain);
        }
        s_buf_len = remain;
    }
}

void steering_poll(void) {
    if (s_eof) return;

    char tmp[STEER_READ_SIZE];
    int eof = 0;

    for (;;) {
        int n = platform_stdin_read_nonblocking(tmp, sizeof(tmp), &eof);
        if (n < 0) {
            /* Treat read error as EOF to stop future polling. */
            s_eof = true;
            break;
        }
        if (n > 0) {
            if (append_buf(tmp, (size_t)n) != 0) {
                /* OOM accumulating; stop polling this round. */
                break;
            }
            if (n < (int)sizeof(tmp)) {
                /* Drained what was available right now. */
                break;
            }
            /* Buffer was full -- there may be more; loop again. */
            continue;
        }
        /* n == 0 */
        if (eof) {
            s_eof = true;
        }
        break;
    }

    steering_drain_pending();
}

/* Process the accumulator: split into complete messages and, if EOF has
 * been signaled, flush any trailing partial message. Split out so tests
 * can feed bytes via steering_feed() and then process them without stdin. */
void steering_drain_pending(void) {
    scan_messages();

    /* On EOF, flush any trailing partial message (no terminating blank line).
     * CRs are already stripped by scan_messages(). */
    if (s_eof && s_buf_len > 0) {
        char *msg = malloc(s_buf_len + 1);
        if (msg != NULL) {
            size_t mlen = s_buf_len;
            memcpy(msg, s_buf, mlen);
            while (mlen > 0 && msg[mlen - 1] == '\n') mlen--;
            msg[mlen] = '\0';
            if (mlen > 0) {
                enqueue(msg, mlen);
            }
            free(msg);
        }
        s_buf_len = 0;
    }
}

char *steering_take(void) {
    if (s_head == NULL) return NULL;
    msg_node *n = s_head;
    s_head = n->next;
    if (s_head == NULL) s_tail = NULL;
    char *text = n->text;
    free(n);
    return text;
}

void steering_reset(void) {
    while (s_head != NULL) {
        msg_node *n = s_head;
        s_head = n->next;
        free(n->text);
        free(n);
    }
    s_tail = NULL;
    free(s_buf);
    s_buf = NULL;
    s_buf_len = 0;
    s_buf_cap = 0;
    s_eof = false;
}
