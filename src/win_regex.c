/* win_regex.c - posix regcomp/regexec for the windows build. mingw-w64
   ships no <regex.h>, so openbsd's libc regex is vendored (unmodified) in
   src/vendor/regex/ and compiled in here as one translation unit; every
   other target uses the libc implementation and this file is empty. */
#ifdef _WIN32

#pragma GCC diagnostic ignored "-Wsign-compare"     /* vendored code */
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

/* glue the openbsd sources expect from their libc */
#define DEF_WEAK(x)
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX 255
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void *rxw_reallocarray(void *p, size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, n * sz);
}
#define reallocarray rxw_reallocarray

static size_t rxw_strlcpy(char *d, const char *s, size_t sz) {
    size_t l = strlen(s);
    if (sz) {
        size_t c = l < sz - 1 ? l : sz - 1;
        memcpy(d, s, c);
        d[c] = '\0';
    }
    return l;
}
#define strlcpy rxw_strlcpy

#include "vendor/regex/regcomp.c"
#include "vendor/regex/regexec.c" /* includes engine.c */
#include "vendor/regex/regerror.c"
#include "vendor/regex/regfree.c"

#endif /* _WIN32 */
