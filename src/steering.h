#ifndef STEERING_H
#define STEERING_H

#include <stddef.h>

/*
 * Steering input layer.
 *
 * Captures user-supplied messages from stdin while the agent runs, so they
 * can be injected into the conversation at the next turn boundary (the
 * earliest point the LLM could legally see them).
 *
 * Wire format: messages are separated by a blank line ("\n\n"). A message
 * is one or more lines of text. Lines accumulate until a blank line closes
 * the message. Any trailing partial line (no terminating blank line) is
 * held until more input arrives or EOF flushes it.
 *
 *   "first message\n\nsecond line A\nsecond line B\n\n" -> 2 messages
 *
 * Usage: call steering_poll() at each turn boundary, then loop
 * steering_take() until it returns NULL to drain all complete messages.
 * The caller frees each returned string.
 */

/* Drain available bytes from stdin (non-blocking) into the internal
 * accumulator and split into complete messages. Safe to call repeatedly.
 * Once EOF is seen, further calls are no-ops. */
void steering_poll(void);

/* Pop the next complete steering message, or NULL if none.
 * The caller must free() the returned string. */
char *steering_take(void);

/* --- Test/utility hooks (not used by the agent loop) ------------------- */

/* Push raw bytes directly into the accumulator and split any complete
 * messages. Lets unit tests exercise the parser without touching stdin. */
void steering_feed(const char *data, size_t len);

/* Mark the stream as ended; flushes any trailing partial message. */
void steering_signal_eof(void);

/* Process the internal accumulator: split complete messages and, on EOF,
 * flush the trailing partial. Called internally by steering_poll/feed. */
void steering_drain_pending(void);

/* Reset all internal state (accumulator, queue). Used by tests. */
void steering_reset(void);

#endif /* STEERING_H */
