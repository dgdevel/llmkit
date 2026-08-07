#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <bcrypt.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#endif

int platform_process_spawn(const char *cmdline, platform_process *out_proc, platform_pipe *in_pipe,
                           platform_pipe *out_pipe) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    HANDLE child_stdin_rd, child_stdin_wr;
    HANDLE child_stdout_rd, child_stdout_wr;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&child_stdin_rd, &child_stdin_wr, &sa, 0)) return -1;
    if (!CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0)) {
        CloseHandle(child_stdin_rd);
        CloseHandle(child_stdin_wr);
        return -1;
    }

    if (!SetHandleInformation(child_stdin_wr, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(child_stdin_rd);
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        return -1;
    }
    if (!SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(child_stdin_rd);
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        return -1;
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = child_stdin_rd;
    si.hStdOutput = child_stdout_wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;

    char *cmdline_copy = _strdup(cmdline);
    if (cmdline_copy == NULL) {
        CloseHandle(child_stdin_rd);
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        return -1;
    }

    BOOL success = CreateProcessA(NULL, cmdline_copy, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                                  NULL, &si, &pi);
    free(cmdline_copy);

    if (!success) {
        CloseHandle(child_stdin_rd);
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        CloseHandle(child_stdout_wr);
        return -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(child_stdin_rd);
    CloseHandle(child_stdout_wr);

    out_proc->hProcess = pi.hProcess;
    out_proc->pid = pi.dwProcessId;
    in_pipe->hWrite = child_stdin_wr;
    in_pipe->hRead = INVALID_HANDLE_VALUE;
    out_pipe->hRead = child_stdout_rd;
    out_pipe->hWrite = INVALID_HANDLE_VALUE;
    return 0;
#else
    int stdin_pipe[2], stdout_pipe[2];

    if (pipe(stdin_pipe) != 0) return -1;
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        char *cmdline_copy = strdup(cmdline);
        if (cmdline_copy == NULL) _exit(127);

        char *argv[64];
        int argc = 0;
        char *p = cmdline_copy;
        while (*p) {
            while (*p && *p == ' ') *p++ = '\0';
            if (*p == '\0') break;
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                argv[argc++] = p;
                while (*p && *p != quote) p++;
                if (*p) *p++ = '\0';
            } else {
                argv[argc++] = p;
                while (*p && *p != ' ') p++;
            }
            if (argc >= 60) break;
        }
        if (argc == 0) {
            /* empty/whitespace cmdline: fall back to the raw string so execvp
             * fails cleanly rather than reading an uninitialized argv[0]. */
            argv[0] = cmdline_copy;
            argc = 1;
        }
        argv[argc] = NULL;

        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    out_proc->pid = pid;
    out_proc->stdin_fd = -1;
    out_proc->stdout_fd = -1;
    in_pipe->fd = stdin_pipe[1];
    out_pipe->fd = stdout_pipe[0];
    return 0;
#endif
}

int platform_process_kill(platform_process *proc) {
#ifdef _WIN32
    if (!TerminateProcess(proc->hProcess, 1)) return -1;
    return 0;
#else
    if (kill(proc->pid, SIGTERM) != 0) return -1;
    return 0;
#endif
}

int platform_process_wait(platform_process *proc, int64_t timeout_ms) {
#ifdef _WIN32
    DWORD ret = WaitForSingleObject(proc->hProcess, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    return (ret == WAIT_OBJECT_0) ? 0 : -1;
#else
    int status;
    if (timeout_ms < 0) {
        if (waitpid(proc->pid, &status, 0) < 0) return -1;
        return 0;
    }
    int64_t elapsed = 0;
    while (elapsed < timeout_ms) {
        pid_t ret = waitpid(proc->pid, &status, WNOHANG);
        if (ret == proc->pid) return 0;
        if (ret < 0) return -1;
        struct pollfd pfd = {.fd = -1, .events = 0};
        int64_t remaining = timeout_ms - elapsed;
        int delay = remaining > 10 ? 10 : (int)remaining;
        poll(&pfd, 0, delay);
        elapsed += delay;
    }
    return -1;
#endif
}

void platform_process_close(platform_process *proc) {
#ifdef _WIN32
    CloseHandle(proc->hProcess);
#else
    (void)proc;
#endif
}

int platform_pipe_read(platform_pipe *p, char *buf, size_t size, int64_t timeout_ms) {
#ifdef _WIN32
    if (timeout_ms < 0) {
        DWORD read;
        if (!ReadFile(p->hRead, buf, (DWORD)size, &read, NULL)) return -1;
        return (int)read;
    }
    DWORD avail;
    int64_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (PeekNamedPipe(p->hRead, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD read;
            if (!ReadFile(p->hRead, buf, (DWORD)size, &read, NULL)) return -1;
            return (int)read;
        }
        int64_t remaining = timeout_ms - elapsed;
        DWORD delay = remaining > 10 ? 10 : (DWORD)remaining;
        Sleep(delay);
        elapsed += delay;
    }
    return -1;
#else
    struct pollfd pfd = {.fd = p->fd, .events = POLLIN};
    int ret = poll(&pfd, 1, (int)timeout_ms);
    if (ret <= 0) return -1;
    ssize_t n = read(p->fd, buf, size);
    return (n < 0) ? -1 : (int)n;
#endif
}

int platform_pipe_write(platform_pipe *p, const char *data, size_t len) {
#ifdef _WIN32
    DWORD written;
    if (!WriteFile(p->hWrite, data, (DWORD)len, &written, NULL)) return -1;
    return (int)written;
#else
    ssize_t n = write(p->fd, data, len);
    return (n < 0) ? -1 : (int)n;
#endif
}

void platform_pipe_close(platform_pipe *p) {
#ifdef _WIN32
    if (p->hRead != INVALID_HANDLE_VALUE) CloseHandle(p->hRead);
    if (p->hWrite != INVALID_HANDLE_VALUE) CloseHandle(p->hWrite);
#else
    close(p->fd);
#endif
}

void platform_random_bytes(void *buf, size_t len) {
#ifdef _WIN32
    BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__APPLE__)
    arc4random_buf(buf, len);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[error] failed to open /dev/urandom\n");
        exit(7);
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (unsigned char *)buf + total, len - total);
        if (n <= 0) {
            fprintf(stderr, "[error] failed to read /dev/urandom\n");
            exit(7);
        }
        total += (size_t)n;
    }
    close(fd);
#endif
}

void platform_timestamp_now(char *buf, size_t len) {
    time_t rawtime;
    struct tm utc;

    time(&rawtime);
#ifdef _WIN32
    gmtime_s(&utc, &rawtime);
#else
    gmtime_r(&rawtime, &utc);
#endif
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

int platform_tcp_listen(const char *addr, int port) {
#ifdef _WIN32
    static int wsock_initialized = 0;
    if (!wsock_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        wsock_initialized = 1;
    }

    SOCKET fd = WSASocketA(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    if (fd == INVALID_SOCKET) return -1;

    int opt = 1;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = addr ? inet_addr(addr) : INADDR_ANY;

    if (bind((SOCKET)fd, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        closesocket(fd);
        return -1;
    }
    if (listen((SOCKET)fd, 10) == SOCKET_ERROR) {
        closesocket(fd);
        return -1;
    }
    return (int)fd;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = addr ? inet_addr(addr) : INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 10) < 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}

int platform_tcp_accept(int fd, int64_t timeout_ms) {
#ifdef _WIN32
    SOCKET sock = (SOCKET)fd;
    if (timeout_ms >= 0) {
        struct timeval tv;
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) != 0) {
            return -1;
        }
    }
    SOCKET client = accept(sock, NULL, NULL);
    return (client == INVALID_SOCKET) ? -1 : (int)client;
#else
    if (timeout_ms >= 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ret = poll(&pfd, 1, (int)timeout_ms);
        if (ret <= 0) return -1;
    }
    int client = accept(fd, NULL, NULL);
    return client;
#endif
}

bool platform_stderr_is_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

int64_t platform_now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);
    return (int64_t)(counter.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
#endif
}

void platform_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

int platform_stdin_read_nonblocking(char *buf, size_t size, int *out_eof) {
    if (out_eof != NULL) *out_eof = 0;
    if (buf == NULL || size == 0) return 0;

#ifdef _WIN32
    /* Windows console handles do not support a reliable ready-check; only
     * pipe-backed stdin (the common case for wrappers) works here. */
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return -1;

    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        /* PeekNamedPipe fails on non-pipe handles (console). Treat as
         * never-ready rather than an error, so steering is simply inert
         * on console-stdin Windows builds. */
        return 0;
    }
    if (avail == 0) return 0;

    DWORD to_read = (avail < size) ? avail : (DWORD)size;
    DWORD nread = 0;
    if (!ReadFile(h, buf, to_read, &nread, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF || err == ERROR_BROKEN_PIPE) {
            if (out_eof != NULL) *out_eof = 1;
            return 0;
        }
        return -1;
    }
    if (nread == 0 && avail > 0) {
        if (out_eof != NULL) *out_eof = 1;
    }
    return (int)nread;
#else
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int ret = poll(&pfd, 1, 0);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (ret == 0) return 0; /* nothing ready */

    /* POLLHUP/POLLERR without POLLIN: peer closed with nothing left. */
    if ((pfd.revents & (POLLIN)) == 0) {
        if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
            if (out_eof != NULL) *out_eof = 1;
        }
        return 0;
    }

    ssize_t n = read(STDIN_FILENO, buf, size);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (n == 0) {
        /* read() returning 0 on a readable fd means EOF. */
        if (out_eof != NULL) *out_eof = 1;
        return 0;
    }
    return (int)n;
#endif
}

const char *platform_temp_dir(void) {
#ifdef _WIN32
    static char buf[MAX_PATH];
    DWORD len = GetTempPathA(sizeof(buf), buf);
    if (len == 0 || len >= sizeof(buf)) return ".";
    /* GetTempPath returns a trailing separator; strip it. */
    if (len > 0 && (buf[len - 1] == '\\' || buf[len - 1] == '/')) buf[len - 1] = '\0';
    return buf;
#else
    const char *t = getenv("TMPDIR");
    if (t != NULL && t[0] != '\0') return t;
    return "/tmp";
#endif
}

int platform_delete_file(const char *path) {
    if (path == NULL) return -1;
#ifdef _WIN32
    if (DeleteFileA(path)) return 0;
    if (GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
    return -1;
#else
    if (unlink(path) == 0) return 0;
    if (errno == ENOENT) return 0;
    return -1;
#endif
}
