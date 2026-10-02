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

Compiled as four translation units of the windows build. The Makefile
detects the mingw compiler target (`-dumpmachine`, so the cross build of
`tools/pkg/win.sh` and a native msys2 `make` both qualify), adds the
four .c files to the build and force-includes `src/win_regex_glue.h`
(`-include`) into every unit: it provides the openbsd-libc bits mingw
lacks (`reallocarray`, `strlcpy`, `_POSIX2_RE_DUP_MAX`) plus the
`__BEGIN_DECLS`/`__END_DECLS` pair the vendored `<regex.h>` takes from
openbsd's `<sys/cdefs.h>`. Separate units are not optional: `regex2.h`
is a private header with no include guard and anonymous-struct typedefs,
so it can be included once per unit only - an amalgamated single unit
cannot compile. POSIX builds never compile anything here: plain `make`
on linux/mac links the system regex.

Differences to know about when patterns come from users or llms:

- `regcomp("", ...)` is `REG_EMPTY` here (glibc accepts it); the builtin
  tool wrapper translates an empty pattern to `.*` so both behave alike.
- `{n,m}` bounds are capped at 255 (`DUPMAX`); glibc allows more.
- duplicate repetition operators (`a**`) are an error here, accepted by
  glibc - undefined in posix either way.
