# llmkit crash report

When llmkit dies on a fatal signal, the process writes one crash record
to a file before it goes down. This applies to every subcommand: the
handler is registered in `main()` right after `platform_init()`. By
default each crash gets its own file; `LLMKIT_CRASH_FILE` pins one
explicit append-only log instead (see Where).
The handler is async-signal-safe: no allocator, no stdio - the record
is assembled in a stack buffer with hand-rolled number formatting and
written with raw `open`/`write`/`close`. After writing, the default
signal disposition is restored and the signal re-raised, so core dumps
and the shell's signal status are unchanged.

The report's primary motivation is the stdio servers, which are
stateless json-rpc loops: a crash is only observable from outside the
process, and without the file there is no way to tell which call was in
flight. Registration is now process-wide in `main()`, so a crash in any
subcommand is recorded the same way.

## Where

- default: one file per crash, `<tmpdir>/llmkit-crash-<epoch>-<pid>.log`
  (tmpdir = `$TMPDIR`, falling back to `/tmp`; `%TEMP%` on Windows). The
  epoch and pid are the crash time and the crashed process, so two
  crashes never share a file - easy to attach one to a bug report, no
  interleaving, nothing to trim. The name is built in the signal handler
  from `time()`/`getpid()` (async-signal-safe) with hand-formatted
  digits.
- override: `LLMKIT_CRASH_FILE=<path>` (any writable path) - one
  explicit append-only file, unchanged. Records are appended and
  self-delimiting, so one file per run is not required.

## Environment variables

| variable | default | effect |
| --- | --- | --- |
| `LLMKIT_CRASH_FILE` | unset | report file path override (single append-only file instead of one file per crash) |
| `LLMKIT_CRASH_BACKTRACE` | unset | `1` appends a `backtrace_symbols_fd` dump (Linux only, see caveat below) |

## What one record says

```
=== llmkit crash ===
time_epoch: 1730000000        # seconds since the epoch (time() is
                              # async-signal-safe; a formatted utc date
                              # would need gmtime_r, which is not)
pid: 12345
signal: 11 SIGSEGV
version: 1.0                  # the binary's LLMKIT_VERSION
argv0: /usr/local/bin/llmkit
method: tools/call            # last json-rpc method served, set in
                              # serve_line just before dispatch; only the
                              # stdio servers set it (builtin-mcp,
                              # mcp-proxy, agent-as-tool)
```

Read `time_epoch` with `date -d @<n>` (or `date -r <n>` on BSD/macOS).
A missing `method:` line means the crash happened before the first call,
between calls, or in a subcommand that is not a stdio server (no
breadcrumb is set there).

## Caveats

- **backtrace is not guaranteed safe**: `backtrace()` /
  `backtrace_symbols_fd` are not on the POSIX async-signal-safe list;
  libgcc documents them as *usually* safe. That is why the feature is
  off by default and gated behind `LLMKIT_CRASH_BACKTRACE=1` - the
  default path stays strictly safe.
- **`g_last_method` is a breadcrumb, not a log**: it holds only the most
  recent method name, bounded at 127 bytes.
- only fatal signals are reported (`SIGSEGV`, `SIGBUS`, `SIGILL`,
  `SIGFPE`, `SIGABRT`). `SIGINT` handling (orderly stop, hard exit on a
  second hit) is unchanged and lives in `platform.c`.
- the handler never deletes or truncates a file. The per-crash default
  files never grow either, but many small ones accumulate; sweeping old
  `llmkit-crash-*.log` out of `/tmp` (or your tmp reaper) is your job.
  The `LLMKIT_CRASH_FILE` override is one growing file; trim it
  yourself.

## Finding the actual bug

The report tells you *that* it crashed and *what was being served*. For
the *why*, use the sanitizer build - same recipe for any subcommand,
shown here with `builtin-mcp`:

```
make asan
LLMKIT_CRASH_BACKTRACE=1 ./build/asan/llmkit-asan builtin-mcp
```

Reproduce the load; asan prints the defect (heap overflow, uaf, stack
overflow) and the crash file records the surrounding call.
