# Fuzzing

libFuzzer harnesses over the project's offline parsing surfaces. Each
harness links every `src/*.c` translation unit (same set as the
selfcheck, minus `main.c`) so coverage instrumentation sees the real
code paths.

## Prerequisites

clang with libFuzzer (`-fsanitize=fuzzer`), and the usual llmkit deps
(`libcjson`, `libcurl`, `pthread`). No other install: libFuzzer ships
with clang (`apt install clang` / `pacman -S clang` / Xcode clang).
Vendoring is neither needed nor useful - libFuzzer is a compiler-rt
runtime, not a library you drop into a repo, and the project's own
parser code is the target, not a fuzz engine. (AFL++ would be the
tool to vendor/download separately, `afl-clang-fast` replacing `cc`.)

## Build

```sh
make fuzz                        # all six, asan + fuzzer
make fuzz/fuzz_sse               # one
make fuzz FUZZFLAGS=-fsanitize=address,undefined   # ubsan too
```

## Run

```sh
./fuzz/fuzz_sse  fuzz/seeds/sse                 # one-off short run
./fuzz/fuzz_jsonl -max_total_time=600 fuzz/out/jsonl fuzz/seeds/jsonl
./fuzz/fuzz_html  -runs=1000000 -timeout=10 fuzz/out/html fuzz/seeds/html
```

Write corpus growth to `fuzz/out/<name>/` (gitignored) and keep
`fuzz/seeds/<name>/` as the curated, checked-in seed set.

## Harnesses

| target | surface | notes |
|---|---|---|
| `fuzz_sse` | `sse_feed`/`sse_eof` (src/sse.c) | fed in two data-split chunks; exercises chunk safety |
| `fuzz_jsonl` | `jsonl_feed`/`jsonl_eof` + `jsonl_parse_line` + `rec_classify` + validators (src/jsonl.c) | two-chunk split; validators run per complete record |
| `fuzz_html` | `builtin_html_to_markdown` + `builtin_ddg_results` (src/builtin/web.c) | first input byte routes the two entry points |
| `fuzz_regex` | `builtin_regex_match` (src/builtin/util.c) | input = `pattern \0 text`, halves truncated at 511b |
| `fuzz_proxy` | `proxy_config_line` (src/proxy.c) | config grammar only; resolve/build is NOT called (it spawns the configured upstream mcp servers - never fuzz a spawner without a sandbox) |
| `fuzz_mcp` | `builtin_handle` json-rpc dispatch (src/builtin.c) | pure methods only (initialize/ping/tools/list/unknown). **no tools/call**: that runs the file/process/web tools on fuzzer bytes - only in a sandbox |

## Findings so far (from the initial bring-up)

- src/builtin/web.c attr loop: a stray `=` (empty attribute name before
  `=`) made the attribute scanner spin forever - `><dy=` hung the
  parser. Remote DoS via `web_fetch` on any page containing it. Fixed
  (consume the stray `=`); the regressor is kept as
  `fuzz/seeds/html/3.txt`.
- glibc `regcomp` DFA blowup: ~70 bytes of `[\++++...` pattern make
  glibc allocate >2GB in `regcomp` (reached through
  `builtin_regex_match`). glibc bug class, not project code; the
  artifact lives in the fuzzing logs (oom-*). Mitigations: none in
  project code (pattern length already capped at tool level by the
  harness); consider a regcomp timeout or a bounded regex engine
  upstream.

## Findings from the docker bring-up (fuzz_mcp_tools)

- `tool_file_create` (src/builtin/files.c): the "missing required
  argument" and the exclusive-create failure paths returned without
  freeing `path` (leak), and one early-error path freed it twice
  (double free). Fixed: every exit now goes through the `done:` label.
- `tool_file_copy_move` (src/builtin/files.c): same pattern - early
  errors returned with `src`/`dst` live (leak), `dst` was freed
  uninitialized when the first `bm_need_path` failed (the long-standing
  -Wsometimes-uninitialized warning), and `src` was freed twice.
  Fixed the same way.
- regressors: `fuzz/seeds/mcp_tools/file_create-{dfree,leak-*}.bin`.
- caveat: CORPUSDIR is writable inside the container, and the harness
  runs the real file tools - on long runs the fuzzer can discover the
  corpus and delete/move it (observed: a 200k-run emptied the corpus;
  a 5k-run kept it). crash/oom/timeout artifacts are written on
  discovery and survive. for long soaks prefer -fork=N corpora or
  check the corpus count after a run.

## Adding a harness

1. `fuzz/fuzz_<name>.c` with `int LLVMFuzzerTestOneInput(const uint8_t *, size_t)`.
2. add the name to `FUZZ_HARNESS` in the Makefile.
3. `fuzz/seeds/<name>/` with 1-3 minimal inputs.
4. keep the harness offline: no spawning, no network, no fs writes
   outside /tmp.
