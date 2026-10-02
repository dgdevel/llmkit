# vendored openbsd regex

The posix `regcomp`/`regexec`/`regerror`/`regfree` implementation for the
**windows build only** (mingw-w64 ships no `<regex.h>`; every other target
uses the libc one). Pinned, unmodified snapshot of openbsd `src`:

- origin: https://github.com/openbsd/src tree `lib/libc/regex/`
  (`regcomp.c regexec.c regerror.c regfree.c engine.c regex2.h utils.h
  cclass.h cname.h`) plus `include/regex.h`
- commit: 3ce1f3f79392ae4d60ce67bea5835d517caaa2ca (2026-10)
- license: BSD-3 (Regents of the University of California / Henry Spencer,
  headers kept in each file)

Compiled through `src/win_regex.c`, which `#include`s the .c files under
`#ifdef _WIN32` and provides the three openbsd-libc bits mingw lacks
(`reallocarray`, `strlcpy`, `_POSIX2_RE_DUP_MAX`) and the `-I` that makes
their `<regex.h>` resolve to the vendored header comes from
`tools/pkg/win.sh`. POSIX builds never compile anything here: plain `make`
on linux/mac links the system regex.

Differences to know about when patterns come from users or llms:

- `regcomp("", ...)` is `REG_EMPTY` here (glibc accepts it); the builtin
  tool wrapper translates an empty pattern to `.*` so both behave alike.
- `{n,m}` bounds are capped at 255 (`DUPMAX`); glibc allows more.
- duplicate repetition operators (`a**`) are an error here, accepted by
  glibc - undefined in posix either way.
