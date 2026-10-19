# llmkit crash report

When `llmkit builtin-mcp` dies on a fatal signal, the server appends one
crash record to a log file before it goes down. The handler is
async-signal-safe: no allocator, no stdio - the record is assembled in a
stack buffer with hand-rolled number formatting and written with raw
`open`/`write`/`close`. After writing, the default signal disposition is
restored and the signal re-raised, so core dumps and the shell's signal
status are unchanged.

The report exists because the server is a stateless json-rpc loop: a
crash is only observable from outside the process, and without the file
there is no way to tell which call was in flight.

## Where

- default: `$TMPDIR/llmkit-builtin-mcp-crash.log`, falling back to
  `/tmp/llmkit-builtin-mcp-crash.log` (on Windows: `%TEMP%\...`)
- override: `LLMKIT_CRASH_FILE=<path>` (any writable path; one file per
  run is not required, records are appended and self-delimiting)

## Environment variables

| variable | default | effect |
| --- | --- | --- |
| `LLMKIT_CRASH_FILE` | unset | report file path override |
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
                              # serve_line just before dispatch
```

Read `time_epoch` with `date -d @<n>` (or `date -r <n>` on BSD/macOS).
A missing `method:` line means the crash happened before the first call
or between calls.

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
- the handler never deletes or truncates the file; trim it yourself.

## Finding the actual bug

The report tells you *that* it crashed and *what was being served*. For
the *why*, use the sanitizer build:

```
make asan
LLMKIT_CRASH_BACKTRACE=1 ./build/asan/llmkit-asan builtin-mcp
```

Reproduce the load; asan prints the defect (heap overflow, uaf, stack
overflow) and the crash file records the surrounding call.
