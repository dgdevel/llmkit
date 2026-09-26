/* platform.c - threads, queues, spawn, signals, curl helpers (design sec.2).
   the _WIN32 halves keep the posix-shaped contracts of llmkit.h. */
#include "llmkit.h"

#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- signals ---- */

volatile int g_stop_flag = 0;

static void sigint_handler(int sig) {
    (void)sig;
    /* a second SIGINT terminates immediately: the orderly stop may still be
       waiting for an in-flight (silent) endpoint transfer that only the
       write callback can abort, and the user must always be able to kill */
    if (g_stop_flag) _exit(EXIT_INTERRUPTED);
    g_stop_flag = 1;
}

void signals_init(void) {
#ifdef _WIN32
    /* the crt routes the console ctrl event to signal(); SIGPIPE does not
       exist on windows - a write to a dead pipe is a plain error */
    signal(SIGINT, sigint_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: blocking reads must wake up */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif
}

double mono_now(void) {
#ifdef _WIN32
    /* QPC is monotonic and does not jump on sleep/resume */
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* ---- queue: mutex + condvar, unbounded ---- */

typedef struct qnode {
    struct qnode *next;
    void *data;
} qnode_t;

struct queue {
    pthread_mutex_t m;
    pthread_cond_t c;
    qnode_t *head, *tail;
    bool closed;
};

queue_t *queue_new(void) {
    queue_t *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    return q;
}

static void queue_drop_all(queue_t *q) {
    while (q->head) {
        qnode_t *n = q->head;
        q->head = n->next;
        free(n->data);
        free(n);
    }
    q->tail = NULL;
}

void queue_free(queue_t *q) {
    queue_drop_all(q);
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->c);
    free(q);
}

void queue_push(queue_t *q, void *item) {
    qnode_t *n = calloc(1, sizeof *n);
    n->data = item;
    pthread_mutex_lock(&q->m);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    pthread_cond_signal(&q->c);
    pthread_mutex_unlock(&q->m);
}

void *queue_try_pop(queue_t *q) {
    pthread_mutex_lock(&q->m);
    if (!q->head) { pthread_mutex_unlock(&q->m); return NULL; }
    qnode_t *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
    void *d = n->data;
    free(n);
    return d;
}

void *queue_pop_timeout(queue_t *q, double seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)seconds;
    ts.tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&q->m);
    while (!q->head && !q->closed) {
        if (pthread_cond_timedwait(&q->c, &q->m, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&q->m);
            return NULL;
        }
    }
    if (!q->head) { pthread_mutex_unlock(&q->m); return NULL; }
    qnode_t *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
    void *d = n->data;
    free(n);
    return d;
}

void queue_close(queue_t *q) {
    pthread_mutex_lock(&q->m);
    q->closed = true;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
}

bool queue_closed(queue_t *q) {
    pthread_mutex_lock(&q->m);
    bool c = q->closed;
    pthread_mutex_unlock(&q->m);
    return c;
}

/* block until the producer side closed the queue (reader teardown wait) */
void queue_wait_closed(queue_t *q) {
    pthread_mutex_lock(&q->m);
    while (!q->closed) pthread_cond_wait(&q->c, &q->m);
    pthread_mutex_unlock(&q->m);
}

int thread_start_detached(thread_fn fn, void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc;
}

/* ---- spawn ---- */

#ifdef _WIN32

/* %ComSpec% (cmd.exe) /c <command_line>, stdin/stdout piped, stderr
   passed through; only the three standard handles are inherited - the
   handle-list attribute keeps other servers' pipes out of the child. */
int spawn_shell(const char *command_line, spawn_t *out) {
    out->hproc = NULL;
    out->pid = -1;
    out->to_fd = -1;
    out->from_fd = -1;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE in_r, in_w, out_r, out_w; /* child stdin, parent->child */
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        return -1;
    }
    fflush(NULL);

    const char *sh = getenv("ComSpec");
    if (!sh || !*sh) sh = "cmd.exe";
    wchar_t wsh[280], wcl[4096];
    if (MultiByteToWideChar(CP_ACP, 0, sh, -1, wsh, 280) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, command_line, -1, wcl,
                            (int)(sizeof wcl / sizeof *wcl) - 300) == 0) {
        CloseHandle(in_r); CloseHandle(in_w);
        CloseHandle(out_r); CloseHandle(out_w);
        return -1;
    }
    wchar_t cmd[4096];
    /* cmd /c strips the first and last quote of the tail when it starts
       with one (cmd /? rule 2): a leading-quoted command_line gets an
       extra outer pair so the stripped quotes are the spare ones */
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
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    si.StartupInfo.hStdError = err;

    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, asz);
    HANDLE inherit[3];
    DWORD ninh = 2;
    inherit[0] = in_r;
    inherit[1] = out_w;
    if (err != NULL && err != INVALID_HANDLE_VALUE) inherit[ninh++] = err;
    BOOL ok = si.lpAttributeList &&
              InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                                &asz) &&
              UpdateProcThreadAttribute(
                  si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                  inherit, ninh * sizeof(HANDLE), NULL, NULL);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (ok)
        ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                            &si.StartupInfo, &pi);
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    if (!ok) {
        CloseHandle(in_r); CloseHandle(in_w);
        CloseHandle(out_r); CloseHandle(out_w);
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(in_r); /* child ends: owned by the child now */
    CloseHandle(out_w);

    out->to_fd = _open_osfhandle((intptr_t)in_w, _O_BINARY | _O_NOINHERIT);
    out->from_fd = _open_osfhandle((intptr_t)out_r, _O_BINARY | _O_NOINHERIT);
    if (out->to_fd < 0) CloseHandle(in_w);
    if (out->from_fd < 0) CloseHandle(out_r);
    if (out->to_fd < 0 || out->from_fd < 0) {
        spawn_kill(out); /* closes whichever fd did materialize */
        return -1;
    }
    out->hproc = pi.hProcess;
    out->pid = (long)GetProcessId(pi.hProcess);
    return 0;
}

/* terminate + reap + close; resets pid and fds so repeated calls are safe */
void spawn_kill(spawn_t *s) {
    if (s->hproc) {
        TerminateProcess(s->hproc, 1);
        WaitForSingleObject(s->hproc, INFINITE);
        CloseHandle(s->hproc);
        s->hproc = NULL;
    }
    if (s->to_fd >= 0) close(s->to_fd);
    if (s->from_fd >= 0) close(s->from_fd);
    s->pid = -1;
    s->to_fd = -1;
    s->from_fd = -1;
}

void spawn_wait(spawn_t *s) {
    if (s->hproc) {
        WaitForSingleObject(s->hproc, INFINITE);
        CloseHandle(s->hproc);
        s->hproc = NULL;
    }
    s->pid = -1;
}

#else /* posix */

int spawn_shell(const char *command_line, spawn_t *out) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe)) return -1;
    if (pipe(out_pipe)) { close(in_pipe[0]); close(in_pipe[1]); return -1; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* child: stderr passes through untouched (it is the log channel) */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        /* nothing else is inherited: other servers' pipes and any open
           sockets belong to the parent, not to this server */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0) maxfd = 16384;
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", command_line, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    out->pid = pid;
    out->to_fd = in_pipe[1];
    out->from_fd = out_pipe[0];
    return 0;
}

/* SIGTERM + reap + close; resets pid and fds so repeated calls are safe */
void spawn_kill(spawn_t *s) {
    if (s->pid > 0) {
        kill(s->pid, SIGTERM);
        waitpid(s->pid, NULL, 0);
    }
    if (s->to_fd >= 0) close(s->to_fd);
    if (s->from_fd >= 0) close(s->from_fd);
    s->pid = -1;
    s->to_fd = -1;
    s->from_fd = -1;
}

void spawn_wait(spawn_t *s) {
    if (s->pid > 0) {
        waitpid(s->pid, NULL, 0);
        s->pid = -1;
    }
}

#endif

/* ---- tty (repl editor) ---- */

#ifdef _WIN32

static HANDLE con_handle(int fd) {
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)_get_osfhandle(fd);
}

bool tty_raw_on(tty_raw_t *t, int fd) {
    t->fd = fd;
    t->on = false;
    HANDLE h = con_handle(fd);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode))
        return false; /* not a console */
    t->orig = mode;
    /* byte mode: no line buffering, no echo, no ctrl-c cooking (0x03 is
       read as a byte instead - the editor's stage rule); VT input makes
       arrow keys arrive as the escape sequences the editor parses */
    DWORD raw = mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(h, raw | ENABLE_VIRTUAL_TERMINAL_INPUT))
        if (!SetConsoleMode(h, raw))
            return false;
    t->on = true;
    return true;
}

void tty_raw_off(tty_raw_t *t) {
    if (!t->on) return;
    SetConsoleMode(con_handle(t->fd), t->orig);
    t->on = false;
}

int tty_read_byte(tty_raw_t *t, unsigned char *c) {
    HANDLE h = con_handle(t->fd);
    DWORD n = 0;
    /* a read error ends input like EOF (same rule as the posix side) */
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, c, 1, &n, NULL) || n == 0)
        return 0;
    if (*c == '\r') *c = '\n'; /* enter arrives as CR in byte mode */
    return 1;
}

int tty_cols(FILE *out) {
    CONSOLE_SCREEN_BUFFER_INFO ci;
    HANDLE h = con_handle(fileno(out));
    if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &ci))
        return 0;
    return ci.srWindow.Right - ci.srWindow.Left + 1;
}

bool tty_vt_enabled(int fd) {
    DWORD mode = 0;
    HANDLE h = con_handle(fd);
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) &&
           (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void msleep(int ms) { Sleep((DWORD)ms); }

/* one-time process setup: utf-8 console, VT escapes on, byte-exact
   pipes. the console codepage is process-wide and outlives us - the
   standard price of utf-8 console output on windows. */
void platform_init(void) {
    DWORD mode = 0, dummy = 0;
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(hout, &mode)) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    } else {
        /* redirected stdout must stay byte-exact: no \n -> \r\n cooking */
        _setmode(_fileno(stdout), _O_BINARY);
    }
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &dummy))
        _setmode(_fileno(stdin), _O_BINARY);
    if (!GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &dummy))
        _setmode(_fileno(stderr), _O_BINARY);
}

void self_exe(char *out, size_t sz, const char *argv0) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)sz);
    if (n > 0 && (size_t)n < sz) return;
    snprintf(out, sz, "%s", argv0 ? argv0 : "llmkit");
}

#else /* posix */

bool tty_raw_on(tty_raw_t *t, int fd) {
    t->fd = fd;
    t->on = false;
    struct termios tm;
    if (tcgetattr(fd, &tm) != 0) return false;
    t->orig = tm;
    tm.c_lflag &= ~(tcflag_t)(ICANON | ECHO); /* ISIG stays: ctrl-c is a signal */
    tm.c_cc[VMIN] = 1;
    tm.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tm) != 0) return false;
    t->on = true;
    return true;
}

void tty_raw_off(tty_raw_t *t) {
    if (!t->on) return;
    tcsetattr(t->fd, TCSANOW, &t->orig);
    t->on = false;
}

int tty_read_byte(tty_raw_t *t, unsigned char *c) {
    for (;;) {
        ssize_t n = read(t->fd, c, 1);
        if (n == 1) return 1;
        if (n == 0) return 0; /* EOF */
        if (errno == EINTR) {
            if (g_stop_flag) return -1; /* SIGINT: the stage rule */
            continue;
        }
        return 0; /* a read error ends input like EOF (ponytail) */
    }
}

int tty_cols(FILE *out) {
    int fd = fileno(out);
    if (fd < 0) return 0;
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 0;
}

bool tty_vt_enabled(int fd) {
    (void)fd;
    return true; /* the caller's own TERM check decides */
}

void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void platform_init(void) { /* nothing to arrange on posix */ }

void self_exe(char *out, size_t sz, const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n > 0 && (size_t)n < sz - 1) {
        out[n] = '\0';
        return;
    }
    snprintf(out, sz, "%s", argv0 ? argv0 : "llmkit");
}

#endif

/* ---- curl ---- */

typedef struct http_aux {
    http_req_t *r;
    bool saw_status;
    bool is_json; /* application/json body: buffer even when streaming */
} http_aux_t;

static size_t hdr_cb(char *buf, size_t sz, size_t nm, void *ud) {
    http_aux_t *a = ud;
    size_t n = sz * nm;
    /* the status line is delivered to the header callback too */
    if (n > 5 && memcmp(buf, "HTTP/", 5) == 0) {
        const char *sp = memchr(buf, ' ', n);
        if (sp) {
            a->r->status = strtol(sp + 1, NULL, 10);
            a->saw_status = true;
        }
        return n;
    }
    if (n >= 14 && strncasecmp(buf, "Content-Type:", 13) == 0) {
        const char *v = buf + 13;
        size_t vn = n - 13;
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ')) vn--;
        buf_clear(&a->r->content_type);
        buf_append(&a->r->content_type, v, vn);
        if (vn >= strlen("application/json") &&
            strncasecmp(v, "application/json", strlen("application/json")) == 0)
            a->is_json = true;
    } else if (n >= 15 && strncasecmp(buf, "Mcp-Session-Id:", 15) == 0) {
        const char *v = buf + 15;
        size_t vn = n - 15;
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ')) vn--;
        if (vn < sizeof a->r->session_id) {
            memcpy(a->r->session_id, v, vn);
            a->r->session_id[vn] = '\0';
        }
    }
    return n;
}

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    http_aux_t *a = ud;
    size_t n = sz * nm;
    a->r->got_data = true;
    if (a->r->on_data && !a->is_json && a->r->status >= 200 &&
        a->r->status < 300) {
        a->r->on_data(a->r->cb_ctx, ptr, n);
    } else {
        buf_append(&a->r->resp, ptr, n);
    }
    if (g_stop_flag) return 0; /* abort the transfer (short count) */
    return n;
}

int http_perform(http_req_t *r) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    http_aux_t aux;
    memset(&aux, 0, sizeof aux);
    aux.r = r;

    curl_easy_setopt(c, CURLOPT_URL, r->url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, r->hdrs);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &aux);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &aux);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "llmkit/" LLMKIT_VERSION);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    if (r->connect_to > 0)
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)r->connect_to);
    if (r->read_to > 0) { /* first byte and inter-chunk alike (design sec.5) */
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)r->read_to);
    }
    if (r->total_to > 0)
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(r->total_to * 1000.0));

    CURLcode res = curl_easy_perform(c);
    r->curl_res = res;
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    r->status = status;
    curl_easy_cleanup(c);

    if (res == CURLE_OK) {
        if (status >= 200 && status < 300) return 0;
        return 1;
    }
    return -1;
}

/* a header name/value pair safe for the request line: CR/LF anywhere
   would split into extra headers (header injection), ':' or whitespace
   would break the name/value split */
static bool hdr_str_ok(const char *s, bool is_name) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n') return false;
        if (is_name && (c == ':' || c == ' ' || c == '\t')) return false;
    }
    return true;
}

bool http_hdr_add(struct curl_slist **list, const char *name, const char *value) {
    if (!hdr_str_ok(name, true) || !hdr_str_ok(value, false)) return false;
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s: %s", name, value);
    struct curl_slist *nl = curl_slist_append(*list, b.data);
    buf_free(&b);
    if (!nl) return false;
    *list = nl;
    return true;
}

void http_hdr_add_json(struct curl_slist **list) {
    *list = curl_slist_append(*list, "Content-Type: application/json");
}

bool http_hdrs_from_json(struct curl_slist **list, const cJSON *obj,
                         char *err, size_t errsz) {
    if (!obj) return true;
    if (!cJSON_IsObject(obj)) {
        if (err) snprintf(err, errsz, "headers must be an object");
        return false;
    }
    for (const cJSON *it = obj->child; it; it = it->next) {
        if (!cJSON_IsString(it) || !it->valuestring) {
            if (err)
                snprintf(err, errsz, "header value of '%s' must be a string",
                         it->string);
            return false;
        }
        if (!http_hdr_add(list, it->string, it->valuestring)) {
            if (err)
                snprintf(err, errsz,
                         "header '%s': names and values must not contain "
                         "CR or LF",
                         it->string);
            return false;
        }
    }
    return true;
}

/* ---- llm endpoint glue shared by both wires ---- */

cJSON *http_transport_error(http_req_t *req) {
    char msg[512];
    if (req->curl_res == CURLE_OPERATION_TIMEDOUT && !req->got_data) {
        snprintf(msg, sizeof msg, "connect to llm endpoint timed out");
        return rec_error(EC_CONNECT_FAILED, msg, true);
    }
    if (req->curl_res == CURLE_COULDNT_RESOLVE_HOST ||
        req->curl_res == CURLE_COULDNT_CONNECT ||
        req->curl_res == CURLE_SSL_CONNECT_ERROR) {
        snprintf(msg, sizeof msg, "cannot reach llm endpoint: %s",
                 curl_easy_strerror(req->curl_res));
        return rec_error(EC_CONNECT_FAILED, msg, true);
    }
    snprintf(msg, sizeof msg, "llm endpoint transport error: %s",
             curl_easy_strerror(req->curl_res));
    return rec_error(EC_HTTP_ERROR, msg, true);
}

cJSON *http_status_error(http_req_t *req, bool with_type) {
    cJSON *body =
        cJSON_ParseWithLength(req->resp.data ? req->resp.data : "", req->resp.len);
    char msg[768];
    const cJSON *err = body ? cJSON_GetObjectItemCaseSensitive(body, "error") : NULL;
    if (err) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(err, "type");
        if (cJSON_IsString(m) && m->valuestring) {
            if (with_type && cJSON_IsString(t) && t->valuestring)
                snprintf(msg, sizeof msg, "HTTP %ld: %s (%s)", req->status,
                         m->valuestring, t->valuestring);
            else
                snprintf(msg, sizeof msg, "HTTP %ld: %s", req->status,
                         m->valuestring);
            cJSON_Delete(body);
            return rec_error(EC_API_ERROR, msg, true);
        }
    }
    cJSON_Delete(body);
    size_t n = req->resp.len > 200 ? 200 : req->resp.len;
    char cut[256] = "";
    if (n) {
        memcpy(cut, req->resp.data, n);
        cut[n] = '\0';
    }
    snprintf(msg, sizeof msg, "HTTP %ld: %s", req->status, cut);
    return rec_error(EC_HTTP_ERROR, msg, true);
}

void llm_http_setup(engine_t *e, const char *path, bool stream,
                    void (*on_data)(void *, const char *, size_t), void *ctx,
                    buf_t *url, struct curl_slist **hdrs, http_req_t *r) {
    buf_init(url);
    buf_append_str(url, rec_str(e->llm, "api_base"));
    buf_append_str(url, path);

    *hdrs = NULL;
    http_hdr_add_json(hdrs);
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(e->llm, "headers");
    bool auth_override = false;
    if (cJSON_IsObject(custom))
        for (const cJSON *it = custom->child; it; it = it->next)
            if (it->string && !strcasecmp(it->string, "Authorization"))
                auth_override = true;
    const char *key = rec_str(e->llm, "api_key");
    if (key && key[0] && !auth_override) {
        buf_t auth;
        buf_init(&auth);
        buf_appendf(&auth, "Bearer %s", key);
        /* guards CR/LF: validate_llm rejects them up front; this drops
           the header rather than letting a crafted key inject one */
        http_hdr_add(hdrs, "Authorization", auth.data);
        buf_free(&auth);
    }
    /* header shape: validate_llm rejected bad ones up front */
    http_hdrs_from_json(hdrs, custom, NULL, 0);
    if (stream) http_hdr_add(hdrs, "Accept", "text/event-stream");

    memset(r, 0, sizeof *r);
    buf_init(&r->resp);
    buf_init(&r->content_type);
    r->url = url->data;
    r->hdrs = *hdrs;
    r->connect_to = e->llm_connect_timeout;
    r->read_to = e->llm_read_timeout;
    if (stream) {
        r->on_data = on_data;
        r->cb_ctx = ctx;
    }
}

void llm_http_teardown(buf_t *url, struct curl_slist *hdrs, http_req_t *r) {
    buf_free(&r->resp);
    buf_free(&r->content_type);
    curl_slist_free_all(hdrs);
    buf_free(url);
}
