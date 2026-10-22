#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* process_exec runs a command line through the shell and waits 10s;
   process_status reports on a pid it spawned - or, minus the output,
   on any other pid in the os; process_wait blocks on one until it
   exits or a timeout passes. Every process owns one
   record for the lifetime of the server: the pid, the exit code once
   reaped, and the output. stdout and stderr arrive on two pipes (the
   two streams stay distinguishable; a line's true writer is only known
   for whole lines - one write() spanning both streams cannot happen,
   but partial-line interleave at the boundary is accepted), a detached
   reader stamps each line "HH:MM:SS: STDOUT: " or "HH:MM:SS: STDERR: "
   per source pipe, appends it to a per-process temp file and rotates
   a three-line tail. The reply quotes at most three leading and three
   trailing lines, each cut at 150 bytes (utf-8 safe) with a note. */

#define BM_PROC_WAIT_MS 10000  /* process_exec completion wait */
#define BM_PROC_WAIT_MAX_S 600 /* process_wait timeout ceiling, seconds */
#define BM_PROC_TAIL 3         /* lines quoted in the reply */
#define BM_PROC_HEAD 3         /* leading lines quoted in the reply */
#define BM_PROC_QUOTE 150      /* bytes of a quoted line before a cut */
#define BM_PROC_LINE_CAP 65536 /* one line's byte cap (newline floods) */

typedef struct proc_rec {
    struct proc_rec *next;
    long pid;
    bool exited;               /* reaped; exit_code is valid */
    int exit_code;
    double t_start;            /* mono clock at spawn, for the duration */
    double t_end;              /* mono clock at reap, when exited */
    bool eof;                  /* reader drained both pipes */
    unsigned long long nlines; /* annotated lines in the temp file */
    char *tail[BM_PROC_TAIL];  /* the last lines, oldest first */
    int ntail;
    char *head[BM_PROC_HEAD];  /* the first lines, oldest first */
    int nhead;
    char *out_path;            /* the temp file with the output */
    FILE *out;                 /* reader-owned while the reader lives */
    int out_fd;                /* the stdout pipe, reader-owned */
    int err_fd;                /* the stderr pipe, reader-owned */
    pthread_mutex_t m;         /* guards the fields the reader fills */
#ifdef _WIN32
    void *hproc; /* HANDLE; kept for the entry's lifetime */
    void *out_r; /* HANDLE, stdout read end; reader-owned */
    void *err_r; /* HANDLE, stderr read end; reader-owned */
#endif
} proc_rec_t;

static proc_rec_t *g_procs; /* every process this server spawned */

/* wall clock HH:MM:SS, the timestamp half of the line prefix */
static void hms_now(char out[9]) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) {
        snprintf(out, 9, "00:00:00");
        return;
    }
    snprintf(out, 9, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* stamp one output line and record it: temp file, counter, tail. src
   is the stream name, "STDOUT" or "STDERR" */
static void proc_emit(proc_rec_t *p, const char *src, const char *s,
                      size_t n) {
    char ts[9];
    hms_now(ts);
    buf_t ln;
    buf_init(&ln);
    buf_appendf(&ln, "%s: %s: ", ts, src);
    for (size_t i = 0; i < n; i++) /* NUL bytes would cut the json reply */
        buf_append_byte(&ln, s[i] ? s[i] : '?');
    if (p->out) {
        fwrite(ln.data, 1, ln.len, p->out);
        fputc('\n', p->out);
        fflush(p->out); /* live readers must see each line */
    }
    pthread_mutex_lock(&p->m);
    if (p->nhead < BM_PROC_HEAD) p->head[p->nhead++] = strdup(ln.data);
    if (p->ntail == BM_PROC_TAIL) {
        free(p->tail[0]);
        memmove(p->tail, p->tail + 1, sizeof(char *) * (BM_PROC_TAIL - 1));
        p->ntail--;
    }
    p->tail[p->ntail++] = buf_steal(&ln, NULL);
    p->nlines++;
    pthread_mutex_unlock(&p->m);
}

/* first writer of the exit code wins (reader thread vs proc_reap) */
static void proc_set_exit(proc_rec_t *p, int code) {
    pthread_mutex_lock(&p->m);
    if (!p->exited) {
        p->exited = true;
        p->exit_code = code;
        p->t_end = mono_now();
    }
    pthread_mutex_unlock(&p->m);
}

#ifndef _WIN32
/* wait status to exit code, shell convention for the signaled case */
static int wait_code(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1; /* unreachable in practice, kept total */
}
#endif

#ifndef _WIN32
/* drain one fd's buffered bytes, emitting one annotated line per
   newline; stops when the read would block or the pipe is empty. the
   caller decides, on eof, when the fd is spent */
static void proc_drain(proc_rec_t *p, int fd, const char *src, buf_t *pend) {
    char chunk[8192];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (chunk[i] != '\n') continue;
            size_t len = i - start;
            if (len && chunk[start + len - 1] == '\r') len--;
            if (pend->len) { /* a partial line from an earlier pass */
                buf_append(pend, chunk + start, len);
                proc_emit(p, src, pend->data, pend->len);
                pend->len = 0;
            } else {
                proc_emit(p, src, chunk + start, len);
            }
            start = i + 1;
        }
        buf_append(pend, chunk + start, (size_t)n - start);
        while (pend->len > BM_PROC_LINE_CAP) { /* a flood without newlines */
            proc_emit(p, src, pend->data, BM_PROC_LINE_CAP);
            size_t keep = pend->len - BM_PROC_LINE_CAP;
            memmove(pend->data, pend->data + BM_PROC_LINE_CAP, keep);
            pend->len = keep;
            pend->data[keep] = '\0';
        }
    }
}

/* detached: drain both pipes, one annotated line per newline, each
   stamped with its stream. the process is reaped here too once its
   output ends, so no zombie survives even when it is never queried
   again */
static void *proc_reader(void *arg) {
    proc_rec_t *p = arg;
    buf_t pend_out, pend_err; /* the lines currently arriving */
    buf_init(&pend_out);
    buf_init(&pend_err);
    struct pollfd fds[2] = { { p->out_fd, POLLIN, 0 },
                             { p->err_fd, POLLIN, 0 } };
    bool open_out = true, open_err = true;
    while (open_out || open_err) {
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (open_out && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            proc_drain(p, p->out_fd, "STDOUT", &pend_out);
            char b; /* read returns 0 only when the pipe holds nothing
                       and the write end is closed: the fd is spent */
            if (read(p->out_fd, &b, 1) == 0) {
                close(p->out_fd);
                p->out_fd = -1;
                open_out = false;
                fds[0].fd = -1;
            }
        }
        if (open_err && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            proc_drain(p, p->err_fd, "STDERR", &pend_err);
            char b;
            if (read(p->err_fd, &b, 1) == 0) {
                close(p->err_fd);
                p->err_fd = -1;
                open_err = false;
                fds[1].fd = -1;
            }
        }
    }
    if (pend_out.len) { /* final lines without their newline */
        if (pend_out.data[pend_out.len - 1] == '\r') pend_out.len--;
        proc_emit(p, "STDOUT", pend_out.data, pend_out.len);
    }
    if (pend_err.len) {
        if (pend_err.data[pend_err.len - 1] == '\r') pend_err.len--;
        proc_emit(p, "STDERR", pend_err.data, pend_err.len);
    }
    buf_free(&pend_out);
    buf_free(&pend_err);
    if (p->out_fd >= 0) close(p->out_fd);
    if (p->err_fd >= 0) close(p->err_fd);
    p->out_fd = -1;
    p->err_fd = -1;
#else
/* drain one pipe's buffered bytes, emitting one annotated line per
   newline; stops when the read fails or the pipe is empty. the caller
   decides, on eof, when the handle is spent */
static void proc_drainw(proc_rec_t *p, HANDLE h, const char *src,
                        buf_t *pend) {
    char chunk[8192];
    DWORD n;
    for (;;) {
        if (!ReadFile(h, chunk, sizeof chunk, &n, NULL) || n == 0) return;
        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (chunk[i] != '\n') continue;
            size_t len = i - start;
            if (len && chunk[start + len - 1] == '\r') len--;
            if (pend->len) { /* a partial line from an earlier pass */
                buf_append(pend, chunk + start, len);
                proc_emit(p, src, pend->data, pend->len);
                pend->len = 0;
            } else {
                proc_emit(p, src, chunk + start, len);
            }
            start = i + 1;
        }
        buf_append(pend, chunk + start, (size_t)n - start);
        while (pend->len > BM_PROC_LINE_CAP) { /* a flood without newlines */
            proc_emit(p, src, pend->data, BM_PROC_LINE_CAP);
            size_t keep = pend->len - BM_PROC_LINE_CAP;
            memmove(pend->data, pend->data + BM_PROC_LINE_CAP, keep);
            pend->len = keep;
            pend->data[keep] = '\0';
        }
    }
}

/* detached: drain both pipes, one annotated line per newline, each
   stamped with its stream. the process is reaped here too once its
   output ends, so no zombie survives even when it is never queried
   again */
static void *proc_reader(void *arg) {
    proc_rec_t *p = arg;
    HANDLE h[2] = { (HANDLE)p->out_r, (HANDLE)p->err_r };
    const char *src[2] = { "STDOUT", "STDERR" };
    buf_t pend[2];
    buf_init(&pend[0]);
    buf_init(&pend[1]);
    int open = 2;
    while (open > 0) {
        DWORD w = WaitForMultipleObjects((DWORD)open, h, FALSE, INFINITE);
        if (w >= WAIT_OBJECT_0 + (DWORD)open) break; /* incl. WAIT_FAILED */
        int i = (int)(w - WAIT_OBJECT_0);
        proc_drainw(p, h[i], src[i], &pend[i]);
        /* a read yielding nothing means the write end is closed and the
           pipe holds no data: the handle is spent; flush its partial
           line now, its stream ends here */
        DWORD got = 0;
        char b;
        if (!ReadFile(h[i], &b, 1, &got, NULL) || got == 0) {
            CloseHandle(h[i]);
            if (pend[i].len) {
                if (pend[i].data[pend[i].len - 1] == '\r') pend[i].len--;
                proc_emit(p, src[i], pend[i].data, pend[i].len);
            }
            buf_free(&pend[i]);
            if (--open > 0) { /* swap the last one into the freed slot */
                h[i] = h[open];
                src[i] = src[open];
                pend[i] = pend[open];
            }
        }
    }
    for (int i = 0; i < open; i++) {
        if (pend[i].len) { /* final lines without their newline */
            if (pend[i].data[pend[i].len - 1] == '\r') pend[i].len--;
            proc_emit(p, src[i], pend[i].data, pend[i].len);
        }
        buf_free(&pend[i]);
    }
#endif
    if (p->out) {
        fclose(p->out);
        p->out = NULL;
    }
#ifdef _WIN32
    if (p->hproc) {
        DWORD code = 0;
        if (WaitForSingleObject((HANDLE)p->hproc, INFINITE) == WAIT_OBJECT_0 &&
            GetExitCodeProcess((HANDLE)p->hproc, &code))
            proc_set_exit(p, (int)code);
    }
#else
    {
        int st = 0;
        if (waitpid((pid_t)p->pid, &st, 0) == (pid_t)p->pid)
            proc_set_exit(p, wait_code(st));
    }
#endif
    pthread_mutex_lock(&p->m);
    p->eof = true;
    pthread_mutex_unlock(&p->m);
    return NULL;
}

/* one temp file per process, holding the full annotated output; the
   reply names it once it holds more than the quoted tail */
static FILE *proc_mktemp(char *path, size_t sz) {
#ifdef _WIN32
    static unsigned seq;
    char dir[264];
    DWORD dn = GetTempPathA(sizeof dir, dir);
    if (!dn || dn >= sizeof dir) return NULL;
    for (int i = 0; i < 4096; i++) {
        snprintf(path, sz, "%sllmkit-proc-%lu-%u.tmp", dir,
                 (unsigned long)GetCurrentProcessId(), ++seq);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT);
            if (fd < 0) {
                CloseHandle(h);
                return NULL;
            }
            return fdopen(fd, "wb");
        }
        DWORD e = GetLastError();
        if (e != ERROR_FILE_EXISTS && e != ERROR_ACCESS_DENIED) return NULL;
    }
    return NULL;
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    if (snprintf(path, sz, "%s/llmkit-proc-XXXXXX", dir) >= (int)sz &&
        snprintf(path, sz, "/tmp/llmkit-proc-XXXXXX") >= (int)sz)
        return NULL; /* an overlong TMPDIR falls back, then gives up */
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    return fdopen(fd, "wb");
#endif
}

#ifdef _WIN32

/* %ComSpec% /c <cmdline>, stdin on NUL, stdout and stderr on two pipes
   so the streams stay distinguishable */
static int proc_spawn(const char *cmdline, proc_rec_t *p) {
    p->hproc = NULL;
    p->out_fd = -1;
    p->err_fd = -1;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    HANDLE in_r = CreateFileA("NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
    if (in_r == INVALID_HANDLE_VALUE ||
        !CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&err_r, &err_w, &sa, 0)) {
        if (in_r != INVALID_HANDLE_VALUE) CloseHandle(in_r);
        if (out_r) CloseHandle(out_r);
        if (out_w) CloseHandle(out_w);
        if (err_r) CloseHandle(err_r);
        if (err_w) CloseHandle(err_w);
        return -1;
    }
    fflush(NULL);
    const char *sh = getenv("ComSpec");
    if (!sh || !*sh) sh = "cmd.exe";
    wchar_t wsh[280], wcl[4096];
    if (MultiByteToWideChar(CP_ACP, 0, sh, -1, wsh, 280) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcl,
                            (int)(sizeof wcl / sizeof *wcl) - 300) == 0) {
        CloseHandle(in_r);
        CloseHandle(out_r);
        CloseHandle(out_w);
        CloseHandle(err_r);
        CloseHandle(err_w);
        return -1;
    }
    wchar_t cmd[4096];
    /* cmd /c strips the first and last quote of the tail when it starts
       with one (cmd /? rule 2): a leading-quoted cmdline gets an extra
       outer pair so the stripped quotes are the spare ones */
    if (wcl[0] == L'"')
        _snwprintf(cmd, 4096, L"\"%ls\" /c \"%ls\"", wsh, wcl);
    else
        _snwprintf(cmd, 4096, L"\"%ls\" /c %ls", wsh, wcl);

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, asz);
    HANDLE inherit[3] = { in_r, out_w, err_w };
    BOOL ok = si.lpAttributeList &&
              InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                                &asz) &&
              UpdateProcThreadAttribute(
                  si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                  inherit, sizeof inherit, NULL, NULL);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (ok)
        ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT |
                                CREATE_NEW_PROCESS_GROUP, NULL, NULL,
                            &si.StartupInfo, &pi);
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    CloseHandle(in_r); /* child ends: owned by the child now */
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        return -1;
    }
    CloseHandle(pi.hThread);
    p->hproc = pi.hProcess; /* the failure paths below leave the child to
                               the caller's kill-and-wipe cleanup */
    p->out_r = out_r;
    p->err_r = err_r;
    p->pid = (long)GetProcessId(pi.hProcess);
    return 0;
}

/* non-blocking check; records the code when the process ended */
static void proc_reap(proc_rec_t *p) {
    pthread_mutex_lock(&p->m);
    bool done = p->exited;
    pthread_mutex_unlock(&p->m);
    if (done || !p->hproc) return;
    DWORD code = 0;
    if (WaitForSingleObject((HANDLE)p->hproc, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess((HANDLE)p->hproc, &code))
        proc_set_exit(p, (int)code);
}

#else /* posix */

/* /bin/sh -c <cmdline>, stdin on /dev/null, stdout and stderr on two
   pipes so the streams stay distinguishable */
static int proc_spawn(const char *cmdline, proc_rec_t *p) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe)) return -1;
    if (pipe(err_pipe)) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* stdin must not be ours (the json-rpc channel): the child reads
           /dev/null instead; each output stream gets its own pipe */
        int nul = open("/dev/null", O_RDONLY);
        if (nul < 0) _exit(127);
        dup2(nul, 0);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        /* nothing else is inherited: other servers' pipes and any open
           sockets belong to the parent, not to this process */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0) maxfd = 16384;
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    p->pid = (long)pid;
    p->out_fd = out_pipe[0];
    p->err_fd = err_pipe[0];
    return 0;
}

/* non-blocking check; records the code when the process ended */
static void proc_reap(proc_rec_t *p) {
    pthread_mutex_lock(&p->m);
    bool done = p->exited;
    pthread_mutex_unlock(&p->m);
    if (done) return;
    int st = 0;
    if (waitpid((pid_t)p->pid, &st, WNOHANG) == 0) return; /* running */
    proc_set_exit(p, wait_code(st));
}

#endif

/* one quoted line, capped at BM_PROC_QUOTE bytes; a cut at a utf-8
   seam would corrupt the json string, so it steps back to whole
   characters */
static void proc_quote(buf_t *out, const char *line, bool *cut) {
    size_t n = strlen(line);
    if (n <= BM_PROC_QUOTE) {
        buf_append_str(out, line);
        return;
    }
    size_t end = BM_PROC_QUOTE;
    while (end > 0 && ((unsigned char)line[end] & 0xC0) == 0x80)
        end--; /* back onto the start byte of the character */
    buf_append(out, line, end);
    *cut = true;
}

/* the shared reply body: state line, quoted output, temp file,
   advice */
static void proc_report(buf_t *out, proc_rec_t *p) {
    pthread_mutex_lock(&p->m);
    if (p->exited) {
        buf_appendf(out, "Exit code: %d\n", p->exit_code);
        buf_appendf(out, "Duration: %.3fs\n",
                    (p->t_end ? p->t_end : mono_now()) - p->t_start);
    } else {
        buf_appendf(out, "PID %ld is still running.\n", p->pid);
    }
    buf_append_str(out, "First and last three output lines:\n");
    bool cut = false;
    for (int i = 0; i < p->nhead; i++) {
        proc_quote(out, p->head[i], &cut);
        buf_append_byte(out, '\n');
    }
    /* short output: head and tail overlap, so each line prints once */
    int skip = 0;
    if (p->nlines > (unsigned)BM_PROC_HEAD + (unsigned)BM_PROC_TAIL)
        buf_append_str(out, "[...]\n");
    else if (p->nhead + p->ntail > (int)p->nlines)
        skip = p->nhead + p->ntail - (int)p->nlines;
    for (int i = skip; i < p->ntail; i++) {
        proc_quote(out, p->tail[i], &cut);
        buf_append_byte(out, '\n');
    }
    if (cut)
        buf_appendf(out,
                    "Output was cut at 150 characters, see full output "
                    "file for full data\n");
    if (cut || p->nlines > (unsigned)BM_PROC_HEAD + (unsigned)BM_PROC_TAIL) {
        /* "currently" only makes sense while output can still grow */
        buf_appendf(out, "Process output available in %s (%s%llu lines)\n",
                    p->out_path ? p->out_path : "?",
                    p->exited ? "" : "currently ", p->nlines);
    }
    if (!p->exited)
        buf_append_str(out, "Use process_status or process_wait.\n");
    pthread_mutex_unlock(&p->m);
}

/* whether a pid exists on this machine, whoever spawned it. a zombie
   still counts: only its parent can tell the difference, and a
   permission wall (eperm / access denied) proves one is there */
static bool pid_exists(long long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

/* the reply for a pid this server did not spawn: the state alone. the
   exit code and the pipes belong to whoever spawned it, so neither is
   reportable here */
static void proc_report_foreign(buf_t *out, long long pid, bool alive) {
    if (alive) {
        buf_appendf(out, "PID %lld is still running.\n", pid);
        buf_append_str(out, "Use process_wait to wait for it.\n");
    } else {
        buf_appendf(out, "PID %lld is not running.\n", pid);
    }
    buf_append_str(out,
                   "No exit code or output: this pid was not spawned by "
                   "this server.\n");
}

/* ---- process tools ---- */

void tool_process_exec(const cJSON *args, buf_t *out, bool *is_error) {
    const char *cmdline = NULL;
    if (!bm_need_str(args, "cmdline", &cmdline, out)) {
        *is_error = true;
        return;
    }
    if (!strlen(cmdline)) {
        buf_clear(out);
        buf_append_str(out, "cmdline must not be empty");
        *is_error = true;
        return;
    }
    proc_rec_t *p = calloc(1, sizeof *p);
    char path[BM_PATH_MAX];
    if (!p) {
        buf_clear(out);
        buf_append_str(out, "process_exec failed: out of memory");
        *is_error = true;
        return;
    }
    p->out_fd = -1;
    p->err_fd = -1;
    p->t_start = mono_now(); /* before spawn: the spawn itself counts */
    p->out = proc_mktemp(path, sizeof path);
    if (!p->out) {
        buf_clear(out);
        buf_append_str(out, "process_exec failed: cannot create temp output file");
        *is_error = true;
        free(p);
        return;
    }
    p->out_path = strdup(path);
    pthread_mutex_init(&p->m, NULL);
    if (proc_spawn(cmdline, p) || thread_start_detached(proc_reader, p) != 0) {
        /* nothing is watching the child yet: kill, reap, wipe */
#ifdef _WIN32
        if (p->hproc) {
            TerminateProcess((HANDLE)p->hproc, 1);
            CloseHandle((HANDLE)p->hproc);
        }
        if (p->out_r) CloseHandle((HANDLE)p->out_r);
        if (p->err_r) CloseHandle((HANDLE)p->err_r);
#else
        if (p->pid > 0) {
            kill((pid_t)p->pid, SIGTERM);
            waitpid((pid_t)p->pid, NULL, 0);
        }
#endif
        if (p->out_fd >= 0) close(p->out_fd);
        if (p->err_fd >= 0) close(p->err_fd);
        fclose(p->out);
        remove(p->out_path);
        free(p->out_path);
        for (int i = 0; i < p->nhead; i++) free(p->head[i]);
        for (int i = 0; i < p->ntail; i++) free(p->tail[i]);
        pthread_mutex_destroy(&p->m);
        free(p);
        buf_clear(out);
        buf_appendf(out, "process_exec failed: cannot spawn '%s'", cmdline);
        *is_error = true;
        return;
    }
    p->next = g_procs;
    g_procs = p;
    /* wait for completion, 10s, polled */
    double t0 = mono_now();
    for (;;) {
        proc_reap(p);
        if (p->exited) break;
        if (mono_now() - t0 >= BM_PROC_WAIT_MS / 1000.0) break;
        msleep(50);
    }
    if (p->exited) {
        /* the reader still holds what the pipe buffered: give it a
           moment, bounded - a grandchild may hold the pipe open forever */
        double t1 = mono_now();
        pthread_mutex_lock(&p->m);
        bool eof = p->eof;
        pthread_mutex_unlock(&p->m);
        while (!eof && mono_now() - t1 < 2.0) {
            msleep(25);
            pthread_mutex_lock(&p->m);
            eof = p->eof;
            pthread_mutex_unlock(&p->m);
        }
    }
    proc_report(out, p);
    bm_sanitize_utf8(out);
}

/* the record of a pid this server spawned, NULL when there is none */
static proc_rec_t *proc_find(long long pid) {
    for (proc_rec_t *it = g_procs; it; it = it->next)
        if ((long long)it->pid == pid) return it;
    return NULL;
}

void tool_process_status(const cJSON *args, buf_t *out, bool *is_error) {
    long long pid = 0;
    if (!bm_need_int(args, "pid", &pid, out)) {
        *is_error = true;
        return;
    }
    proc_rec_t *p = proc_find(pid);
    if (p) { /* this server spawned it: the full report */
        proc_reap(p);
        proc_report(out, p);
    } else if (pid > 0) { /* any other pid in the os, no output */
        proc_report_foreign(out, pid, pid_exists(pid));
    } else {
        buf_clear(out);
        buf_append_str(out, "pid must be a positive process id");
        *is_error = true;
        return;
    }
    bm_sanitize_utf8(out);
}

/* process_status plus a bounded wait: polls until the pid exits or the
   timeout passes, then answers the same report */
void tool_process_wait(const cJSON *args, buf_t *out, bool *is_error) {
    long long pid = 0, timeout = 0;
    if (!bm_need_int(args, "pid", &pid, out) ||
        !bm_need_int(args, "timeout", &timeout, out)) {
        *is_error = true;
        return;
    }
    if (timeout < 0 || timeout > BM_PROC_WAIT_MAX_S) {
        buf_clear(out);
        buf_appendf(out, "timeout must be between 0 and %d seconds",
                    BM_PROC_WAIT_MAX_S);
        *is_error = true;
        return;
    }
    proc_rec_t *p = proc_find(pid);
    if (p) { /* this server spawned it: the full report */
        double t0 = mono_now();
        for (;;) {
            proc_reap(p);
            if (p->exited) break;
            if (mono_now() - t0 >= (double)timeout) break;
            msleep(50);
        }
        proc_report(out, p);
    } else if (pid > 0) { /* any other pid in the os, no output */
        bool alive = pid_exists(pid);
        double t0 = mono_now();
        while (alive && mono_now() - t0 < (double)timeout) {
            msleep(50);
            alive = pid_exists(pid);
        }
        proc_report_foreign(out, pid, alive);
    } else {
        buf_clear(out);
        buf_append_str(out, "pid must be a positive process id");
        *is_error = true;
        return;
    }
    bm_sanitize_utf8(out);
}

/* ---- process_send_signal ---- */

enum {
    BMSIG_INT, BMSIG_TERM, BMSIG_HUP, BMSIG_USR1, BMSIG_USR2, BMSIG_KILL,
    BMSIG_N
};
static const char *const BMSIG_NAMES[BMSIG_N] = {
    "int", "term", "hup", "usr1", "usr2", "kill",
};

static int sig_parse(const char *name) {
    for (int i = 0; i < BMSIG_N; i++)
        if (!strcmp(name, BMSIG_NAMES[i])) return i;
    return -1;
}

void tool_process_send_signal(const cJSON *args, buf_t *out,
                                     bool *is_error) {
    long long pid = 0;
    const char *signame = NULL;
    if (!bm_need_int(args, "pid", &pid, out) ||
        !bm_need_str(args, "signal", &signame, out)) {
        *is_error = true;
        return;
    }
    if (pid <= 0) {
        buf_clear(out);
        buf_append_str(out, "pid must be a positive process id");
        *is_error = true;
        return;
    }
    int sig = sig_parse(signame);
    if (sig < 0) {
        buf_clear(out);
        buf_appendf(out, "unknown signal '%s': use one of int, term, hup, "
                         "usr1, usr2, kill", signame);
        *is_error = true;
        return;
    }
    proc_rec_t *p = proc_find(pid);
#ifdef _WIN32
    /* the crt's SIGINT/SIGTERM exist but reach only this process; the
       only cross-process mechanisms are ctrl events and Terminate */
    if (sig != BMSIG_KILL && sig != BMSIG_INT && sig != BMSIG_TERM) {
        buf_clear(out);
        buf_appendf(out, "signal '%s' does not exist on windows; use "
                         "int, term or kill", signame);
        *is_error = true;
        return;
    }
    if (sig == BMSIG_KILL) {
        HANDLE h = p && p->hproc ? (HANDLE)p->hproc
                                 : OpenProcess(PROCESS_TERMINATE, FALSE,
                                               (DWORD)pid);
        if (!h) {
            DWORD e = GetLastError();
            buf_clear(out);
            buf_appendf(out, "cannot open pid %lld: %s", pid,
                        e == ERROR_ACCESS_DENIED ? "access denied"
                                                 : "no such process");
            *is_error = true;
            return;
        }
        BOOL ok = TerminateProcess(h, 1);
        DWORD e = GetLastError();
        if (h != (HANDLE)p->hproc) CloseHandle(h);
        if (!ok) {
            buf_clear(out);
            buf_appendf(out, "TerminateProcess failed on pid %lld "
                             "(error %lu)", pid, (unsigned long)e);
            *is_error = true;
            return;
        }
        buf_appendf(out, "pid %lld force-terminated (exit code 1); no "
                         "cleanup handlers ran", pid);
    } else {
        /* ctrl events: only our own children - they are the only targets
           with a known process group; needs a shared console too */
        if (!p || !p->hproc) {
            buf_clear(out);
            buf_append_str(out, "int/term only work on processes this "
                                "server spawned; use kill for anything "
                                "else");
            *is_error = true;
            return;
        }
        /* children spawn with CREATE_NEW_PROCESS_GROUP, so the group id
           is the pid; ctrl-break (not ctrl-c: disabled in new groups) */
        if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, (DWORD)pid)) {
            buf_clear(out);
            buf_appendf(out, "console ctrl event failed (error %lu): the "
                             "child may not share this console; use kill",
                        (unsigned long)GetLastError());
            *is_error = true;
            return;
        }
        buf_appendf(out, "ctrl-break sent to process group %lld; some "
                         "programs ignore it - use kill if it survives",
                    pid);
    }
#else
    static const int nums[BMSIG_N] = {
        SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGKILL
    };
    if (kill(p ? (pid_t)p->pid : (pid_t)pid, nums[sig]) != 0) {
        buf_clear(out);
        const char *why = errno == ESRCH ? "no such process"
                        : errno == EPERM ? "permission denied"
                        : strerror(errno);
        buf_appendf(out, "kill %lld %s failed: %s", pid, signame, why);
        *is_error = true;
        return;
    }
    if (p) proc_reap(p);
    if (p && p->exited)
        buf_appendf(out, "signal %s sent to pid %lld; it exited with "
                         "code %d", signame, pid, p->exit_code);
    else
        buf_appendf(out, "signal %s sent to pid %lld", signame, pid);
#endif
    bm_sanitize_utf8(out);
}

/* ---- skills tools ---- */
