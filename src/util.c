#include "util.h"

int64_t util_parse_duration(const char *s) {
    (void)s;
    return 0;
}

void util_timestamp_now(char *buf, size_t len) {
    (void)buf; (void)len;
}

void util_uuid_v4(char *buf) {
    (void)buf;
}

void util_sha256(const char *data, size_t len, char *hex_out) {
    (void)data; (void)len; (void)hex_out;
}

char *util_read_file(const char *path) {
    (void)path;
    return NULL;
}

char *util_strdup(const char *s) {
    (void)s;
    return NULL;
}

void log_activity(const char *fmt, ...) {
    (void)fmt;
}
