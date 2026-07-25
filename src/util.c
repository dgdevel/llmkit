#include "util.h"
#include "llmkit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <openssl/sha.h>

int64_t util_parse_duration(const char *s) {
    if (s == NULL || *s == '\0') return -1;

    char *end;
    long long val = strtoll(s, &end, 10);
    if (end == s || val < 0) return -1;

    if (*end == '\0') return val;
    if (strcmp(end, "ms") == 0) return (int64_t)val;
    if (strcmp(end, "s") == 0)  return (int64_t)val * 1000;
    if (strcmp(end, "m") == 0)  return (int64_t)val * 60 * 1000;
    if (strcmp(end, "h") == 0)  return (int64_t)val * 60 * 60 * 1000;

    return -1;
}

void util_timestamp_now(char *buf, size_t len) {
    time_t rawtime;
    struct tm utc;

    time(&rawtime);
    gmtime_r(&rawtime, &utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

void util_uuid_v4(char *buf) {
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        log_activity("[error] failed to open /dev/urandom");
        exit(EXIT_INTERNAL_ERR);
    }
    ssize_t n = read(fd, random, 16);
    close(fd);
    if (n != 16) {
        log_activity("[error] failed to read /dev/urandom");
        exit(EXIT_INTERNAL_ERR);
    }

    random[6] = (random[6] & 0x0F) | 0x40;
    random[8] = (random[8] & 0x3F) | 0x80;

    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random[0], random[1], random[2], random[3],
        random[4], random[5], random[6], random[7],
        random[8], random[9], random[10], random[11],
        random[12], random[13], random[14], random[15]);
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

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *buf = (char *)malloc(size + 1);
    if (buf == NULL) {
        close(fd);
        log_activity("[error] OOM reading file");
        exit(EXIT_INTERNAL_ERR);
    }

    ssize_t total = 0;
    while (total < (ssize_t)size) {
        ssize_t n = read(fd, buf + total, size - (size_t)total);
        if (n <= 0) {
            close(fd);
            free(buf);
            return NULL;
        }
        total += n;
    }

    close(fd);
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

void log_activity(const char *fmt, ...) {
    if (!isatty(STDERR_FILENO)) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
