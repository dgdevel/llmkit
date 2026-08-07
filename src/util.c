#include "util.h"
#include "platform.h"
#include "llmkit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <openssl/sha.h>

int64_t util_parse_duration(const char *s) {
    if (s == NULL || *s == '\0') return -1;

    char *end;
    long long val = strtoll(s, &end, 10);
    if (end == s || val < 0) return -1;

    if (*end == '\0') return val;
    if (strcmp(end, "ms") == 0) return (int64_t)val;
    if (strcmp(end, "s") == 0) return (int64_t)val * 1000;
    if (strcmp(end, "m") == 0) return (int64_t)val * 60 * 1000;
    if (strcmp(end, "h") == 0) return (int64_t)val * 60 * 60 * 1000;

    return -1;
}

int64_t util_fibonacci(int n) {
    /* 1-indexed Fibonacci: F(1)=1, F(2)=1, F(3)=2, F(4)=3, F(5)=5, ...
     * Iterative to avoid recursion and integer overflow for small n. */
    if (n <= 0) return 0;
    int64_t prev = 0; /* F(0) */
    int64_t curr = 1; /* F(1) */
    for (int i = 2; i <= n; i++) {
        int64_t next = prev + curr;
        prev = curr;
        curr = next;
    }
    return curr;
}

void util_timestamp_now(char *buf, size_t len) {
    platform_timestamp_now(buf, len);
}

void util_uuid_v4(char *buf) {
    unsigned char random[16];
    platform_random_bytes(random, 16);

    random[6] = (random[6] & 0x0F) | 0x40;
    random[8] = (random[8] & 0x3F) | 0x80;

    snprintf(buf, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3], random[4], random[5], random[6], random[7],
             random[8], random[9], random[10], random[11], random[12], random[13], random[14],
             random[15]);
}

void util_sha256(const char *data, size_t len, char *hex_out) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, len, hash);

    hex_out[0] = '\0';
    char *p = hex_out;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        p += snprintf(p, 3, "%02x", hash[i]);
    }
}

char *util_read_file(const char *path) {
    if (path == NULL) return NULL;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        log_activity("[error] OOM reading file");
        exit(EXIT_INTERNAL_ERR);
    }

    size_t total = 0;
    while (total < (size_t)size) {
        size_t n = fread(buf + total, 1, (size_t)size - total, fp);
        if (n == 0) {
            fclose(fp);
            free(buf);
            return NULL;
        }
        total += n;
    }

    fclose(fp);
    buf[size] = '\0';
    return buf;
}

char *util_strdup(const char *s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        log_activity("[error] OOM in strdup");
        exit(EXIT_INTERNAL_ERR);
    }
    memcpy(copy, s, len + 1);
    return copy;
}

/* Runtime kill-switch for log_activity(). When disabled, it emits nothing
 * regardless of TTY status. Used to silence progress diagnostics in quiet
 * run mode (where only the final answer should appear). Defaults to enabled
 * so non-agent entry points (proxy) and error paths keep their diagnostics. */
static bool g_log_enabled = true;

void log_activity_set_enabled(bool enabled) {
    g_log_enabled = enabled;
}

void log_activity(const char *fmt, ...) {
    if (!g_log_enabled) return;
    if (!platform_stderr_is_tty()) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
