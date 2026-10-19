/* crash_report.c - fatal-signal crash report for the stdio servers
   (llmkit builtin-mcp). a crash of the stateless json-rpc loop is
   observable only from outside the process, so on SIGSEGV/SIGBUS/
   SIGILL/SIGFPE/SIGABRT the handler appends a one-crash record to a
   log file before dying, then restores the default disposition and
   re-raises so core dumps still happen. see docs/crash-report.md.

   the handler is async-signal-safe: no malloc, no stdio, no snprintf -
   the constant lines are pre-formatted at init time, numbers (pid,
   signal, epoch seconds) are converted by hand, and only open/write/
   close (and optionally backtrace/backtrace_symbols_fd) are called.
   g_last_method is a plain bounded array written by serve_line before
   dispatch, safe to read from the handler: it is never freed and never
   changes shape. */
#include "llmkit.h"
#include "crash_report.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <errno.h>
#endif

#ifdef __linux__
#include <execinfo.h>
#endif

/* ---- shared with the handler ---- */

char g_last_method[CRASH_METHOD_MAX];
const char *g_argv0 = NULL;

/* handler inputs resolved or pre-formatted once at init: g_report_path
   (opened lazily in the handler: the crash may come hours after init),
   g_hdr_* constant report lines. */
static char g_report_path[512];
static volatile sig_atomic_t g_enabled = 0;
static volatile sig_atomic_t g_want_bt = 0;

/* LLMKIT_VERSION comes from llmkit.h (release builds stamp it). the
   lengths are cached at init so the handler never calls strlen. */
static char g_hdr_ver[96];   /* "version: <x.y.z>\n" */
static char g_hdr_argv[512]; /* "argv0:   <path>\n"  (empty until known) */
static size_t g_hdr_ver_len = 0;
static size_t g_hdr_argv_len = 0;

/* ---- signal names (compile-time macros, no syscall) ---- */

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
#ifdef SIGBUS
    case SIGBUS:  return "SIGBUS";
#endif
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default:      return "SIG?";
    }
}

/* ---- async-signal-safe helpers ---- */

/* append the decimal form of v to buf; returns the new end pointer */
static char *put_u(char *p, unsigned long v) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_s(char *p, const char *s, size_t n) {
    while (n--) *p++ = *s++;
    return p;
}

#ifdef __linux__
/* used only by the optional backtrace path */
static void write_str(int fd, const char *s, size_t n) { (void)write(fd, s, n); }
#endif

/* ---- the handler ---- */

static void crash_handler(int sig) {
    /* assemble the record in one local buffer; static storage would not
       be reentrant on a second fatal signal during the report write */
    char buf[1024];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;
    const char *name = sig_name(sig);

    p = put_s(p, "=== llmkit crash ===\n", 21);
    p = put_s(p, "time_epoch: ", 12);
    p = put_u(p, (unsigned long)time(NULL));
    p = put_s(p, "\npid: ", 6);
#ifdef _WIN32
    p = put_u(p, (unsigned long)GetCurrentProcessId());
#else
    p = put_u(p, (unsigned long)getpid());
#endif
    p = put_s(p, "\nsignal: ", 9);
    p = put_u(p, (unsigned long)sig);
    *p++ = ' ';
    while (*name && p < end) *p++ = *name++;
    *p++ = '\n';
    p = put_s(p, g_hdr_ver, g_hdr_ver_len);
    if (g_argv0) p = put_s(p, g_hdr_argv, g_hdr_argv_len);
    /* g_last_method is NUL-terminated or full: bounded read either way */
    p = put_s(p, "method: ", 8);
    for (int i = 0; i < CRASH_METHOD_MAX && g_last_method[i]; i++)
        if (p < end) *p++ = g_last_method[i];
    if (p < end) *p++ = '\n';

#ifdef _WIN32
    int fd = _open(g_report_path, _O_WRONLY | _O_CREAT | _O_APPEND,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(g_report_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
    if (fd >= 0) {
        const char *q = buf;
        size_t n = (size_t)(p - buf);
        while (n) {
#ifdef _WIN32
            int w = _write(fd, q, (unsigned)n);
#else
            ssize_t w = write(fd, q, n);
#endif
            if (w <= 0) break;
            q += w;
            n -= (size_t)w;
        }
#ifdef __linux__
        /* optional, off by default: backtrace()/backtrace_symbols_fd are
           not guaranteed async-signal-safe (libgcc says "usually") but
           are the only in-process option; gated behind an env var */
        if (g_want_bt) {
            void *bt[64];
            int m = backtrace(bt, 64);
            write_str(fd, "\nbacktrace:\n", 12);
            backtrace_symbols_fd(bt, m, fd);
        }
#endif
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }

    /* die as if we had never intercepted: default disposition, re-raise.
       core dumps (when enabled) and the shell's signal status survive. */
#ifdef _WIN32
    signal(sig, SIG_DFL);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
#endif
    raise(sig);
}

/* ---- init ---- */

void crash_report_init(const char *path_env) {
    if (g_enabled) return;
    if (!path_env) path_env = getenv("LLMKIT_CRASH_FILE");
    if (path_env && *path_env) {
        strncpy(g_report_path, path_env, sizeof(g_report_path) - 1);
        g_report_path[sizeof(g_report_path) - 1] = '\0';
    } else {
        const char *dir = NULL;
#ifdef _WIN32
        char wdir[MAX_PATH];
        if (GetTempPathA(sizeof wdir, wdir)) dir = wdir;
#else
        dir = getenv("TMPDIR");
#endif
        if (!dir || !*dir) dir = "/tmp";
        size_t dl = strlen(dir);
        char sep = (dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\'))
                       ? '\0' : '/';
        int n = snprintf(g_report_path, sizeof(g_report_path), "%s%c%s", dir,
                         sep, "llmkit-builtin-mcp-crash.log");
        (void)n;
        g_report_path[sizeof(g_report_path) - 1] = '\0';
    }

    /* pre-format the constant lines (snprintf is fine here: init runs in
       normal context, not from the handler) */
    snprintf(g_hdr_ver, sizeof g_hdr_ver, "version: %s\n", LLMKIT_VERSION);
    g_hdr_ver_len = strlen(g_hdr_ver);
    if (g_argv0)
        g_hdr_argv_len = (size_t)snprintf(g_hdr_argv, sizeof g_hdr_argv,
                                          "argv0: %s\n", g_argv0);

    const char *bt = getenv("LLMKIT_CRASH_BACKTRACE");
    g_want_bt = (bt && !strcmp(bt, "1"));

#ifdef _WIN32
    signal(SIGSEGV, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGABRT, crash_handler);
#else
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = crash_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND | SA_NODEFER;
        sigaction(sigs[i], &sa, NULL);
    }
#endif
    g_enabled = 1;
}
