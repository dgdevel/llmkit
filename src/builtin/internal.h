/* internal.h - shared interface of the builtin-mcp tool modules
   (the .c files under src/builtin, dispatched by src/builtin.c).
   Everything here is private to the binary: the tool tables and the
   json-rpc handler stay in src/builtin.c, each tool family implements
   its tool_* functions and draws on the helpers below. not part of
   llmkit.h on purpose: nothing here escapes the translation units
   including this file. */
#ifndef LLMKIT_BUILTIN_INTERNAL_H
#define LLMKIT_BUILTIN_INTERNAL_H

#include "llmkit.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---- platform shims (windows/posix) ---- */

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h> /* SHFileOperationW (recycle bin) */
#include <windows.h>
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif
#define BM_ACCESS _access
#define BM_MKDIR(p) _mkdir(p)
#define BM_RMDIR(p) _rmdir(p)
#define BM_LSTAT(p, s) stat((p), (s))
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#define BM_ACCESS access
#define BM_MKDIR(p) mkdir((p), 0777)
#define BM_RMDIR(p) rmdir(p)
#define BM_LSTAT lstat
#endif

/* posix regcomp for the file tool patterns. mingw ships no <regex.h>:
   there the vendored openbsd implementation is compiled in (src/
   vendor/regex, added by the Makefile's mingw detection, its libc shims
   force-included from src/win_regex_glue.h) and this include reaches
   its header directly. */
#ifdef _WIN32
#include "vendor/regex/regex.h"
#else
#include <regex.h>
#endif

/* ---- filesystem caps ---- */

#define BM_PATH_MAX 4096
#define BM_FILE_CAP (64ull * 1024 * 1024)        /* per-file read cap */
#define BM_SEARCH_CAP (16ull * 1024 * 1024)      /* content-search cap */

/* exclusive file creation: the open is the existence check, so two
   concurrent writers cannot both claim the path */
#ifdef _WIN32
#include <sys/stat.h>
#define BM_OPEN_NEW(p)                                                         \
    _open((p), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE)
#define BM_FDOPEN(fd) _fdopen((fd), "wb")
#define BM_CLOSE(fd) _close(fd)
#define BM_CHMOD(p, m) _chmod((p), (m))
/* the crt rename() follows ms semantics: it refuses an existing
   destination (EEXIST) where posix replaces it atomically. this goes
   through MoveFileExW, which also handles the non-ascii paths the
   ansi crt calls mangle (same utf-8 -> utf-16 as src/platform.c). */
#include <errno.h>
static inline int bm_rename_replace(const char *old, const char *nw) {
    wchar_t wold[BM_PATH_MAX], wnew[BM_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, old, -1, wold, BM_PATH_MAX) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, nw, -1, wnew, BM_PATH_MAX) == 0) {
        errno = EACCES;
        return -1;
    }
    if (!MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING)) {
        errno = EACCES; /* good enough for the error paths */
        return -1;
    }
    return 0;
}
#define BM_RENAME bm_rename_replace
#else
#define BM_OPEN_NEW(p) open((p), O_CREAT | O_EXCL | O_WRONLY, 0666)
#define BM_FDOPEN(fd) fdopen((fd), "wb")
#define BM_CLOSE(fd) close(fd)
#define BM_CHMOD(p, m) chmod((p), (m))
#define BM_RENAME rename
#endif

/* ---- tool plumbing (implemented in src/builtin.c) ---- */

typedef void (*tool_fn)(const cJSON *args, buf_t *out, bool *is_error);

/* ---- shared small helpers (builtin/util.c) ---- */

/* textual-file probe result: ok (file read), textual (utf-8, no NUL),
   lines (line count) */
typedef struct {
    bool ok;
    bool textual;
    unsigned long long lines;
} tscan_t;

/* case-insensitive substring test */
bool bm_ci_contains(const char *hay, const char *needle);
/* qsort comparator for a char *[] of names (util.c) */
int bm_cmp_names(const void *a, const void *b);
bool bm_is_ws(char c);
size_t bm_lead_ws(const char *s, size_t n);
size_t bm_trail_ws(const char *s, size_t n);
void bm_collapse_ws(buf_t *b);
/* decode html entities (&amp; &lt; &#123; ...) into out */
void bm_ent_decode(buf_t *out, const char *s, size_t n);
/* percent-encode into out; pct_decode reverses */
void bm_url_encode(buf_t *out, const char *s);
void bm_pct_decode(buf_t *out, const char *s, size_t n);
/* the regex helpers wrap posix regcomp/regexec: bm_rx_compile compiles
   (false + err on failure), bm_rx_match runs one unanchored search; the
   one-shot variant lives in llmkit.h as builtin_regex_match() */
bool bm_rx_compile(regex_t *rx, const char *pattern, char *err, size_t errsz);
bool bm_rx_match(const regex_t *rx, const char *s, size_t n);
/* rwx string of a path's permission bits */
void bm_perms_of(const char *path, char out[4]);
/* "1.2K" style human size */
void bm_human_size(unsigned long long v, char *out, size_t sz);
/* whole-file read with a size cap; 0 on success, -1 read error,
   -2 over cap (out is left empty) */
int bm_read_whole(const char *path, buf_t *out, unsigned long long cap);
/* one-file textual probe: read (up to cap), utf-8/NUL check, line
   count. ok=false when unreadable, textual=false when binary */
tscan_t bm_scan_text(const char *path, unsigned long long cap);
/* utf8_put writes one code point to out; sanitize_utf8 rewrites invalid
   bytes to U+FFFD in place */
void bm_utf8_put(buf_t *b, unsigned long cp);
void bm_sanitize_utf8(buf_t *b);
/* required-argument getters: fill *out or append the error to err and
   return false */
bool bm_need_str(const cJSON *args, const char *name, const char **out,
                 buf_t *err);
bool bm_need_int(const cJSON *args, const char *name, long long *out,
                 buf_t *err);
/* required path argument: like bm_need_str but the value is dup'd and
   slash-normalized ('\' -> '/', windows input); free() the copy.
   NULL + error appended when absent */
char *bm_need_path(const cJSON *args, const char *name, buf_t *err);

/* mkdir -p for the parent directory of a path (builtin/files.c) */
bool bm_ensure_parent_dirs(const char *path);

/* ---- skills helpers (builtin/skills.c) ---- */

/* the existing skill roots, search order (0..2 of them) */
size_t bm_skill_roots(char roots[][BM_PATH_MAX]);
/* read-only names of a directory's entries, sorted; free with
   bm_dir_names_free */
char **bm_dir_names(const char *path, size_t *nout);
void bm_dir_names_free(char **names, size_t n);
/* SKILL.md path of dir entry (root/entry/SKILL.md), false if not one */
bool bm_skill_file(const char *root, const char *entry, char *out, size_t sz);
/* next '\n'-separated line [ls, ll) of s starting at *i */
bool bm_next_line(const char *s, size_t len, size_t *i, const char **ls,
                  size_t *ll);
/* value of the yaml frontmatter key in text (malloc'd), NULL if absent */
char *bm_fm_value(const char *text, size_t len, const char *key);

#endif
