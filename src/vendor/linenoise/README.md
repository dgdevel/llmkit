# linenoise (vendored)

Salvatore Sanfilippo's line editing library, <https://github.com/antirez/linenoise>,
vendored at upstream master commit `a473823d74b93eab2ba83480df16ed37617493f2`
(2-clause BSD, the license notice is kept in the file headers).

Upstream is posix-only (termios raw mode, `TIOCGWINSZ`, `read()`; its own
todo list carries "Win32 support"). This copy runs on windows too by routing
the terminal mechanics through llmkit's own platform layer (`src/platform.c`):
posix keeps `ISIG` on (ctrl-c is the SIGINT of the two-stage rule), windows
uses the byte-mode console with VT input, so the escape parsing and the
refresh rendering are shared verbatim.

The local patches are listed in the `LLMKIT LOCAL PATCHES` block at the top
of `linenoise.c` and marked `/* LOCAL */` where they live. In short:

1. raw mode, byte reads and terminal width through the platform layer
   (win32 support); no DSR cursor-position probe (it could eat a keystroke)
2. bracketed paste not armed - llmkit submits one input per pasted line
   (requirements sec.12), and the tty queue is never flushed (`TCSANOW`),
   so type-ahead survives the per-line raw cycle
3. `'\n'` submits like `'\r'` (the platform reader normalizes CR to NL)
4. ctrl-c and an interrupted read (EINTR) leave buffer and display
   untouched, pop the live history entry and return `NULL`/`EAGAIN` -
   `src/editor.c` reads the buffer length for the two ctrl-c stages
   (requirements sec.12) and repaints
5. ctrl-d submits when the line has text, EOF when empty; a NUL byte
   returns `EILSEQ` (llmkit's `invalid_record` tier)
6. the prompt may carry ANSI escapes (the bold user block); refresh
   cursor math counts printable columns, not bytes
7. tab completion is llmkit's contract (requirements sec.13): unique
   match taken, longest common prefix, or the candidate listing below
   the line - no cycling mode
8. history persistence removed (in-memory only, design sec.12); the one
   non-ascii upstream comment is ascii-fied (the repo's ascii rule)

Refreshing the vendor: re-vendor from upstream, then re-apply the patch
list above (each is small and independently grep-able via `LOCAL`).
