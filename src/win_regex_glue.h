/* win_regex_glue.h - the openbsd-libc bits the vendored regex sources
   expect and mingw-w64 does not ship. The Makefile's mingw detection
   force-includes this (-include) into every translation unit of the
   windows build - the vendored .c files cannot be modified to include
   anything, and their <regex.h> (reached through -Isrc/vendor/regex)
   leans on openbsd's <sys/cdefs.h> pair; posix builds never see a byte
   of it. The renames keep a future mingw shipment of either libc
   symbol from colliding; static inline, one copy per unit. */
#ifndef LLMKIT_WIN_REGEX_GLUE_H
#define LLMKIT_WIN_REGEX_GLUE_H

#ifdef _WIN32

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* openbsd's <sys/cdefs.h> pair (C build: no extern "C" ever needed) and
   the weak-alias stub its libc macros generate */
#ifndef __BEGIN_DECLS
#define __BEGIN_DECLS
#endif
#ifndef __END_DECLS
#define __END_DECLS
#endif
#define DEF_WEAK(x)

#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX 255
#endif

/* the vendored sources are 1990s openbsd libc code, not warning-clean
   under -Wall -Wextra; the build is one gcc command, so the pragmas
   cannot be scoped to their files - they apply to this windows build
   only */
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized" /* regcomp 'scan' fp */

static inline void *rxw_reallocarray(void *p, size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, n * sz);
}
#define reallocarray rxw_reallocarray

static inline size_t rxw_strlcpy(char *d, const char *s, size_t sz) {
    size_t l = strlen(s);
    if (sz) {
        size_t c = l < sz - 1 ? l : sz - 1;
        memcpy(d, s, c);
        d[c] = '\0';
    }
    return l;
}
#define strlcpy rxw_strlcpy

#endif /* _WIN32 */

#endif /* LLMKIT_WIN_REGEX_GLUE_H */
