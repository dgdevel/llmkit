#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

int64_t util_parse_duration(const char *s);
/* Return the Nth Fibonacci number (1-indexed): util_fibonacci(1)=1,
 * util_fibonacci(2)=1, util_fibonacci(3)=2, util_fibonacci(4)=3,
 * util_fibonacci(5)=5, and so on. Returns 0 for n <= 0. Used to compute
 * the per-retry backoff delay in seconds for LLM request retries. */
int64_t util_fibonacci(int n);
void util_timestamp_now(char *buf, size_t len);
void util_uuid_v4(char *buf);
void util_sha256(const char *data, size_t len, char *hex_out);
char *util_read_file(const char *path);
char *util_strdup(const char *s);
void log_activity(const char *fmt, ...);
void log_activity_set_enabled(bool enabled);

#endif
