#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
struct platform_process {
    HANDLE hProcess;
    DWORD pid;
};
struct platform_pipe {
    HANDLE hRead, hWrite;
};
#else
#include <unistd.h>
#include <sys/wait.h>
struct platform_process {
    pid_t pid;
    int stdin_fd, stdout_fd;
};
struct platform_pipe {
    int fd;
};
#endif

typedef struct platform_process platform_process;
typedef struct platform_pipe platform_pipe;

int platform_process_spawn(const char *cmdline, platform_process *out_proc, platform_pipe *in_pipe,
                           platform_pipe *out_pipe);
int platform_process_kill(platform_process *proc);
int platform_process_wait(platform_process *proc, int64_t timeout_ms);
void platform_process_close(platform_process *proc);

int platform_pipe_read(platform_pipe *p, char *buf, size_t size, int64_t timeout_ms);
int platform_pipe_write(platform_pipe *p, const char *data, size_t len);
void platform_pipe_close(platform_pipe *p);

void platform_random_bytes(void *buf, size_t len);
void platform_timestamp_now(char *buf, size_t len);
int platform_tcp_listen(const char *addr, int port);
int platform_tcp_accept(int fd, int64_t timeout_ms);
bool platform_stderr_is_tty(void);

/* Monotonic clock in milliseconds. Untyped epoch; only deltas are meaningful. */
int64_t platform_now_ms(void);
/* Sleep for the given number of milliseconds. */
void platform_sleep_ms(int64_t ms);

#endif
