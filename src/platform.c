#include "platform.h"

int platform_process_spawn(const char *cmdline, platform_process *out_proc,
                           platform_pipe *in_pipe, platform_pipe *out_pipe) {
    (void)cmdline; (void)out_proc; (void)in_pipe; (void)out_pipe;
    return -1;
}

int platform_process_kill(platform_process *proc) {
    (void)proc;
    return -1;
}

int platform_process_wait(platform_process *proc, int64_t timeout_ms) {
    (void)proc; (void)timeout_ms;
    return -1;
}

void platform_process_close(platform_process *proc) {
    (void)proc;
}

int platform_pipe_read(platform_pipe *p, char *buf, size_t size, int64_t timeout_ms) {
    (void)p; (void)buf; (void)size; (void)timeout_ms;
    return -1;
}

int platform_pipe_write(platform_pipe *p, const char *data, size_t len) {
    (void)p; (void)data; (void)len;
    return -1;
}

void platform_pipe_close(platform_pipe *p) {
    (void)p;
}

void platform_random_bytes(void *buf, size_t len) {
    (void)buf; (void)len;
}

void platform_timestamp_now(char *buf, size_t len) {
    (void)buf; (void)len;
}

int platform_tcp_listen(const char *addr, int port) {
    (void)addr; (void)port;
    return -1;
}

int platform_tcp_accept(int fd, int64_t timeout_ms) {
    (void)fd; (void)timeout_ms;
    return -1;
}

bool platform_stderr_is_tty(void) {
    return false;
}
