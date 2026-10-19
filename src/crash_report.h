/* crash_report.h - fatal-signal crash report for the stdio servers
   (see src/crash_report.c and docs/crash-report.md). the handler stays
   async-signal-safe: no malloc, no stdio, only open/write/close and
   manual number formatting. */
#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

#include <signal.h>

/* last json-rpc method served. src/mcp.c serve_line copies the method
   name here (bounded, plain array, no heap) just before dispatch so a
   crash report can name the call that was in flight. always
   NUL-terminated; reads from the signal handler are bounded. */
#define CRASH_METHOD_MAX 128
extern char g_last_method[CRASH_METHOD_MAX];

/* program name for the report; main() sets it once, NULL until then */
extern const char *g_argv0;

/* one-time setup (idempotent): resolve the report file (env
   LLMKIT_CRASH_FILE, else TMPDIR | %TEMP% | /tmp +
   llmkit-builtin-mcp-crash.log), pre-format the constant report lines
   and register the SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT handlers.
   pass NULL to read the default env vars. */
void crash_report_init(const char *path_env);

#endif
