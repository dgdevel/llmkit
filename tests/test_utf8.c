#include <stdio.h>
#include <string.h>
#include "utf8.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, msg); \
        tests_failed++; \
    } \
} while(0)

int main(void) {
    fprintf(stderr, "=== test_utf8 ===\n");

    /* empty string */
    CHECK(utf8_validate("", 0), "empty string");

    /* ASCII only */
    CHECK(utf8_validate("hello", 5), "ASCII 'hello'");
    CHECK(utf8_validate("", 0), "empty ASCII");
    CHECK(utf8_validate("abc123!@#", 9), "ASCII with symbols");

    /* valid 2-byte sequences */
    CHECK(utf8_validate("\xC2\x80", 2), "2-byte U+0080");
    CHECK(utf8_validate("\xDF\xBF", 2), "2-byte U+07FF");
    CHECK(utf8_validate("a\xC2\x80z", 4), "2-byte surrounded by ASCII");

    /* valid 3-byte sequences */
    CHECK(utf8_validate("\xE0\xA0\x80", 3), "3-byte U+0800");
    CHECK(utf8_validate("\xED\x9F\xBF", 3), "3-byte U+D7FF (before surrogates)");
    CHECK(utf8_validate("\xEE\x80\x80", 3), "3-byte U+E000 (after surrogates)");
    CHECK(utf8_validate("\xEF\xBF\xBF", 3), "3-byte U+FFFF");
    CHECK(utf8_validate("\xE1\x80\x80", 3), "3-byte U+4000");

    /* valid 4-byte sequences */
    CHECK(utf8_validate("\xF0\x90\x80\x80", 4), "4-byte U+10000");
    CHECK(utf8_validate("\xF4\x8F\xBF\xBF", 4), "4-byte U+10FFFF (max)");
    CHECK(utf8_validate("\xF3\x80\x80\x80", 4), "4-byte F1-F3 range");

    /* invalid: lone continuation byte */
    CHECK(!utf8_validate("\x80", 1), "lone continuation 0x80");
    CHECK(!utf8_validate("\xBF", 1), "lone continuation 0xBF");

    /* invalid: truncated sequences */
    CHECK(!utf8_validate("\xC2", 1), "truncated 2-byte (missing cont)");
    CHECK(!utf8_validate("\xE0\xA0", 2), "truncated 3-byte (missing cont)");
    CHECK(!utf8_validate("\xF0\x90\x80", 3), "truncated 4-byte (missing cont)");

    /* invalid: bad continuation byte */
    CHECK(!utf8_validate("\xC2\xC0", 2), "bad cont byte 0xC0 after lead");
    CHECK(!utf8_validate("\xE0\xA0\xC0", 3), "bad cont byte 0xC0 in 3-byte");

    /* invalid: overlong sequences */
    CHECK(!utf8_validate("\xC0\x80", 2), "overlong 2-byte encoding of null");
    CHECK(!utf8_validate("\xC1\xBF", 2), "overlong 2-byte encoding of U+7F");
    CHECK(!utf8_validate("\xE0\x80\x80", 3), "overlong 3-byte encoding of null");
    CHECK(!utf8_validate("\xF0\x80\x80\x80", 4), "overlong 4-byte encoding of null");
    CHECK(!utf8_validate("\xE0\x9F\xBF", 3), "overlong 3-byte encoding of U+07FF");

    /* invalid: surrogate halves */
    CHECK(!utf8_validate("\xED\xA0\x80", 3), "surrogate U+D800");
    CHECK(!utf8_validate("\xED\xAF\xBF", 3), "surrogate U+DBFF");
    CHECK(!utf8_validate("\xED\xB0\x80", 3), "surrogate U+DC00");
    CHECK(!utf8_validate("\xED\xBF\xBF", 3), "surrogate U+DFFF");

    /* invalid: codepoint above U+10FFFF */
    CHECK(!utf8_validate("\xF4\x90\x80\x80", 4), "codepoint U+110000 (over max)");
    CHECK(!utf8_validate("\xF4\xBF\xBF\xBF", 4), "codepoint beyond max");

    /* invalid: bytes 0xFE and 0xFF */
    CHECK(!utf8_validate("\xFE", 1), "byte 0xFE");
    CHECK(!utf8_validate("\xFF", 1), "byte 0xFF");

    /* utf8_validate_c_string wrapper */
    CHECK(utf8_validate_c_string("hello"), "c_string ASCII");
    CHECK(utf8_validate_c_string("\xC2\x80"), "c_string valid 2-byte");
    CHECK(!utf8_validate_c_string("\xC0\x80"), "c_string overlong");
    CHECK(!utf8_validate_c_string(NULL), "c_string NULL");

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
