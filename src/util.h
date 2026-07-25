#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

int64_t util_parse_duration(const char *s);
void util_timestamp_now(char *buf, size_t len);
void util_uuid_v4(char *buf);
void util_sha256(const char *data, size_t len, char *hex_out);
char *util_read_file(const char *path);
char *util_strdup(const char *s);
void log_activity(const char *fmt, ...);

#endif
