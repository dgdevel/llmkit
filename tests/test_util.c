#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

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

#define CHECK_EQ(a, b, msg)                                                                       \
    do {                                                                                          \
        tests_run++;                                                                              \
        if ((a) != (b)) {                                                                         \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected %lld, got %lld\n", __FILE__, __LINE__, \
                    msg, (long long)(b), (long long)(a));                                         \
            tests_failed++;                                                                       \
        }                                                                                         \
    } while (0)

#define CHECK_STR_EQ(a, b, msg)                                                             \
    do {                                                                                    \
        tests_run++;                                                                        \
        if (strcmp((a), (b)) != 0) {                                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected \"%s\", got \"%s\"\n", __FILE__, \
                    __LINE__, msg, (b), (a));                                               \
            tests_failed++;                                                                 \
        }                                                                                   \
    } while (0)

static void test_parse_duration(void) {
    CHECK_EQ(util_parse_duration("0"), 0, "zero");
    CHECK_EQ(util_parse_duration("500"), 500, "plain number");
    CHECK_EQ(util_parse_duration("100ms"), 100, "ms");
    CHECK_EQ(util_parse_duration("1s"), 1000, "seconds");
    CHECK_EQ(util_parse_duration("30s"), 30000, "30 seconds");
    CHECK_EQ(util_parse_duration("10m"), 600000, "10 minutes");
    CHECK_EQ(util_parse_duration("1h"), 3600000, "1 hour");
    CHECK_EQ(util_parse_duration("0s"), 0, "zero seconds");
    CHECK_EQ(util_parse_duration(NULL), -1, "NULL input");
    CHECK_EQ(util_parse_duration(""), -1, "empty string");
    CHECK_EQ(util_parse_duration("-1"), -1, "negative number");
    CHECK_EQ(util_parse_duration("abc"), -1, "non-numeric");
    CHECK_EQ(util_parse_duration("10x"), -1, "unknown suffix");
    CHECK_EQ(util_parse_duration("10"), 10, "plain number no suffix");
}

static void test_timestamp_now(void) {
    char buf[64];
    util_timestamp_now(buf, sizeof(buf));
    CHECK(strlen(buf) == 20, "timestamp length should be 20");
    CHECK(buf[4] == '-', "timestamp format: YYYY-");
    CHECK(buf[7] == '-', "timestamp format: MM-");
    CHECK(buf[10] == 'T', "timestamp format: T");
    CHECK(buf[13] == ':', "timestamp format: HH:");
    CHECK(buf[16] == ':', "timestamp format: MM:");
    CHECK(buf[19] == 'Z', "timestamp format: ends with Z");
}

static void test_uuid_v4(void) {
    char buf[64];
    util_uuid_v4(buf);
    CHECK(strlen(buf) == 36, "UUID v4 length should be 36");
    CHECK(buf[8] == '-', "UUID format: dash after 8");
    CHECK(buf[13] == '-', "UUID format: dash after 13");
    CHECK(buf[18] == '-', "UUID format: dash after 18");
    CHECK(buf[23] == '-', "UUID format: dash after 23");
    CHECK(buf[14] == '4', "UUID version nibble should be 4");
    char variant = buf[19];
    CHECK((variant == '8' || variant == '9' || variant == 'a' || variant == 'b' || variant == 'A' ||
           variant == 'B'),
          "UUID variant nibble should be 8/9/A/B");

    /* verify uniqueness across two calls */
    char buf2[64];
    util_uuid_v4(buf2);
    CHECK(strcmp(buf, buf2) != 0, "UUIDs should be unique");
}

static void test_sha256(void) {
    char hex[65];

    util_sha256("", 0, hex);
    CHECK_STR_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 "SHA256 of empty string");

    util_sha256("hello", 5, hex);
    CHECK_STR_EQ(hex, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                 "SHA256 of 'hello'");

    util_sha256("abc", 3, hex);
    CHECK_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "SHA256 of 'abc'");
}

static void test_read_file(void) {
    /* non-existent file */
    char *result = util_read_file("/tmp/nonexistent_llmkit_test_file_xyz123");
    CHECK(result == NULL, "read non-existent file should return NULL");

    /* create temp file and read it back */
    char tmpname[] = "/tmp/llmkit_test_XXXXXX";
    int fd = mkstemp(tmpname);
    CHECK(fd >= 0, "mkstemp should succeed");
    const char *content = "hello world\nline 2";
    size_t len = strlen(content);
    ssize_t n = write(fd, content, len);
    CHECK(n == (ssize_t)len, "write to temp file should succeed");
    close(fd);

    result = util_read_file(tmpname);
    CHECK(result != NULL, "read temp file should succeed");
    CHECK_STR_EQ(result, content, "read file content should match");
    free(result);
    unlink(tmpname);
}

static void test_strdup(void) {
    /* NULL input */
    char *copy = util_strdup(NULL);
    CHECK(copy == NULL, "strdup(NULL) should return NULL");

    /* normal string */
    copy = util_strdup("hello");
    CHECK(copy != NULL, "strdup should succeed");
    CHECK_STR_EQ(copy, "hello", "strdup content should match");
    free(copy);

    /* empty string */
    copy = util_strdup("");
    CHECK(copy != NULL, "strdup empty string should succeed");
    CHECK_STR_EQ(copy, "", "strdup empty string content");
    free(copy);
}

static void test_log_activity(void) {
    /* just verify no crash */
    log_activity("[test] this is a log message: %s", "ok");
    log_activity("[test] plain message");
}

int main(void) {
    fprintf(stderr, "=== test_util ===\n");

    test_parse_duration();
    test_timestamp_now();
    test_uuid_v4();
    test_sha256();
    test_read_file();
    test_strdup();
    test_log_activity();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
