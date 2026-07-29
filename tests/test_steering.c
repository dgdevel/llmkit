/*
 * Unit tests for the steering input layer (src/steering.c).
 *
 * Exercises the blank-line-delimited message parser without touching real
 * stdin, using the steering_feed() / steering_signal_eof() test hooks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "steering.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, msg); \
            tests_failed++;                                                   \
        }                                                                     \
    } while (0)

/* Drain the queue, comparing the first `expected_n` popped messages against
 * `expected` (NULL-terminated array). Asserts the queue is then empty. */
static void check_messages(const char *const *expected, int expected_n, const char *label) {
    for (int i = 0; i < expected_n; i++) {
        char *m = steering_take();
        tests_run++;
        if (m == NULL) {
            fprintf(stderr, "  FAIL (%s:%d): %s: message %d missing\n", __FILE__, __LINE__, label,
                    i);
            tests_failed++;
            continue;
        }
        if (strcmp(m, expected[i]) != 0) {
            fprintf(stderr, "  FAIL (%s:%d): %s: message %d expected [%s] got [%s]\n", __FILE__,
                    __LINE__, label, i, expected[i], m);
            tests_failed++;
        }
        free(m);
    }
    char *extra = steering_take();
    tests_run++;
    if (extra != NULL) {
        fprintf(stderr, "  FAIL (%s:%d): %s: unexpected extra message [%s]\n", __FILE__, __LINE__,
                label, extra);
        tests_failed++;
        free(extra);
    }
}

static void test_two_messages_one_feed(void) {
    steering_reset();
    steering_feed("first\n\nsecond\n\n", 16);
    const char *expected[] = {"first", "second"};
    check_messages(expected, 2, "two-messages-one-feed");
}

static void test_partial_across_feeds(void) {
    steering_reset();
    steering_feed("hel", 3);
    CHECK(steering_take() == NULL, "partial message should not yield a message");
    steering_feed("lo\n\n", 5);
    const char *expected[] = {"hello"};
    check_messages(expected, 1, "partial-across-feeds");
}

static void test_multiline_message(void) {
    steering_reset();
    steering_feed("line1\nline2\nline3\n\n", 19);
    const char *expected[] = {"line1\nline2\nline3"};
    check_messages(expected, 1, "multiline-message");
}

static void test_crlf_normalization(void) {
    /* CRLF line endings and a CRLF blank line ("\r\n\r\n") */
    steering_reset();
    steering_feed("a\r\nb\r\n\r\n", 8);
    const char *expected[] = {"a\nb"};
    check_messages(expected, 1, "crlf-normalization");
}

static void test_adjacent_delimiters(void) {
    /* "a\n\n\n\n" -> "a" then an empty message (skipped) */
    steering_reset();
    steering_feed("a\n\n\n\n", 5);
    const char *expected[] = {"a"};
    check_messages(expected, 1, "adjacent-delimiters");
}

static void test_eof_flushes_partial(void) {
    steering_reset();
    steering_feed("complete\n\nincomplete-no-delim", 30);
    const char *first[] = {"complete"};
    check_messages(first, 1, "eof-flush-before");
    steering_signal_eof();
    const char *second[] = {"incomplete-no-delim"};
    check_messages(second, 1, "eof-flush-after");
}

static void test_empty_and_delimiters_only(void) {
    steering_reset();
    steering_feed("", 0);
    CHECK(steering_take() == NULL, "empty input yields nothing");

    steering_reset();
    steering_feed("\n\n\n\n", 4);
    CHECK(steering_take() == NULL, "delimiters only yield nothing");
}

static void test_trailing_newlines_trimmed(void) {
    steering_reset();
    steering_feed("msg\n\n\n", 6);
    const char *expected[] = {"msg"};
    check_messages(expected, 1, "trailing-newlines-trimmed");
}

static void test_empty_messages_skipped(void) {
    steering_reset();
    steering_feed("real\n\n\n\ntrailing\n\n", 19);
    const char *expected[] = {"real", "trailing"};
    check_messages(expected, 2, "empty-messages-skipped");
}

static void test_multiple_feeds_build_message(void) {
    /* Build one message byte-by-byte across many feeds. */
    steering_reset();
    const char *parts[] = {"one", " ", "two", " ", "three\n", "\n"};
    for (int i = 0; i < 6; i++) {
        steering_feed(parts[i], strlen(parts[i]));
    }
    const char *expected[] = {"one two three"};
    check_messages(expected, 1, "multiple-feeds-build");
}

int main(void) {
    test_two_messages_one_feed();
    test_partial_across_feeds();
    test_multiline_message();
    test_crlf_normalization();
    test_adjacent_delimiters();
    test_eof_flushes_partial();
    test_empty_and_delimiters_only();
    test_trailing_newlines_trimmed();
    test_empty_messages_skipped();
    test_multiple_feeds_build_message();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
