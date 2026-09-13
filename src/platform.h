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
/* Spawn cmdline under the system shell ("/bin/sh -c cmdline" on POSIX,
 * "<COMSPEC> /c cmdline" on Windows) with stdin detached and both stdout
 * and stderr redirected to out_path (created or truncated). Unlike
 * platform_process_spawn the command is not tokenized: the shell parses
 * it, so pipes, quotes and redirections all work. Returns 0 on success;
 * the caller owns reaping the process. */
int platform_shell_spawn(const char *cmdline, const char *out_path, platform_process *out_proc);
int platform_process_kill(platform_process *proc);
/* Wait for the process, at most timeout_ms (< 0 waits forever). Returns 0
 * when it exited, -1 on timeout or error; *out_code (may be NULL) gets
 * the exit status, with a signal death reported as 128+signal. */
int platform_process_wait(platform_process *proc, int64_t timeout_ms, int *out_code);
/* Non-blocking check: returns 1 when the process exited (*out_code set as
 * in platform_process_wait), 0 while still running, -1 on error. */
int platform_process_trywait(platform_process *proc, int *out_code);
void platform_process_close(platform_process *proc);

int platform_pipe_read(platform_pipe *p, char *buf, size_t size, int64_t timeout_ms);
int platform_pipe_write(platform_pipe *p, const char *data, size_t len);
void platform_pipe_close(platform_pipe *p);

void platform_random_bytes(void *buf, size_t len);
void platform_timestamp_now(char *buf, size_t len);
int platform_tcp_listen(const char *addr, int port);
int platform_tcp_accept(int fd, int64_t timeout_ms);
int platform_socket_set_read_timeout(int client_fd, int64_t timeout_ms);
bool platform_stderr_is_tty(void);

/* Monotonic clock in milliseconds. Untyped epoch; only deltas are meaningful. */
int64_t platform_now_ms(void);
/* Sleep for the given number of milliseconds. */
void platform_sleep_ms(int64_t ms);

/* Read up to `size` bytes from the process's own stdin without blocking.
 * Returns the number of bytes read (>= 0), or -1 on error. Sets *out_eof
 * to 1 if stdin reached end-of-file. When no data is available and stdin
 * is still open, returns 0 with *out_eof = 0. `out_eof` may be NULL.
 *
 * Note: on Windows only pipe-backed stdin is supported (console stdin is
 * reported as never-ready). */
int platform_stdin_read_nonblocking(char *buf, size_t size, int *out_eof);

/* Return the system's temporary directory path (e.g. $TMPDIR or "/tmp" on
 * POSIX, the Windows temp path on Windows). The returned pointer is stable
 * for the process lifetime and is never NULL. */
const char *platform_temp_dir(void);

/* Delete the file at the given path. Returns 0 on success or if the file
 * does not exist; returns -1 on other errors. */
int platform_delete_file(const char *path);

/* Truncate the file at the given path to exactly `length` bytes. Returns 0
 * on success, -1 on error. Used to drop a trailing partial line from an
 * append-only JSONL file after an interrupted write. */
int platform_truncate_file(const char *path, int64_t length);

/* Install the agent's SIGINT handler. The first SIGINT only sets a pending
 * flag (see platform_sigint_pending) so the conversation loop can finish the
 * in-flight tool call, record the outcome and exit cleanly; a second SIGINT
 * while the flag is still pending restores the default disposition and
 * re-raises, terminating the process immediately. Returns 0 on success,
 * -1 if the handler could not be installed. */
int platform_install_sigint_handler(void);

/* Whether an interrupt is pending: a SIGINT was received but the process has
 * not yet shut down. Safe to call from anywhere; the underlying flag is
 * `volatile sig_atomic_t`. */
int platform_sigint_pending(void);

/* Ignore SIGPIPE for the whole process. A dead MCP backend or a client that
 * disconnected mid-response must surface as an EPIPE write error the caller
 * can handle (the proxy/gateway "Backend request failed" paths depend on
 * this); the default disposition would instead kill the process. Windows has
 * no SIGPIPE (writes to closed pipes already fail with an error), so this is
 * a no-op there. Returns 0 on success, -1 on failure. */
int platform_ignore_sigpipe(void);

#endif
