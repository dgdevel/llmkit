# llmkit design

Implements [docs/requirements.md](requirements.md), transcript format version 1.
Every deferral named there is answered here: libraries in §1, the exit code table
in §11, `cache_control` placement and openai caching in §7, wire-level protocol
details in §5 and §6. Where this document picks a ceiling, it is marked
`ponytail:` with the upgrade path.

## table of contents

1. [technology choices](#1-technology-choices)
2. [process and threading model](#2-process-and-threading-model)
3. [input pipeline](#3-input-pipeline)
4. [conversation engine](#4-conversation-engine)
5. [endpoint clients](#5-endpoint-clients)
6. [mcp client](#6-mcp-client)
7. [serialization and prefix cache](#7-serialization-and-prefix-cache)
8. [external stop](#8-external-stop)
9. [windows specifics](#9-windows-specifics)
10. [agent-as-tool](#10-agent-as-tool)
11. [exit codes](#11-exit-codes)
12. [build and packaging](#12-build-and-packaging)
13. [testing](#13-testing)

## 1. technology choices

| need | choice | why |
|---|---|---|
| language | C11 | per requirements; no compiler exceptions, both targets |
| json | cJSON, system installed (`-lcjson`, any 1.x; `libcjson-dev` / `mingw-w64-x86_64-cjson` in the cross sysroot) | object fields are a linked list in insertion order: serialization is deterministic by construction, which §7 depends on; two-file library, nothing to vendor |
| http/tls | libcurl (easy api), version floor 7.80 | tls, chunked transfer, sse body reading, connect and low-speed timeouts, one code path for both platforms; alternatives (json-c, jansson, a hand-rolled http client) add a build system, lose ordering, or re-implement tls |
| concurrency | pthreads on linux, win32 primitives on windows, behind `platform.c` | no dependency, no event loop to debug |

No other third-party code. mcp, sse parsing, jsonl, utf-8 validation are in-tree.

## 2. process and threading model

One process per conversation (`llmkit runner`) or per agent server
(`llmkit agent-as-tool`). Threads:

- **main** — owns the conversation engine, all stdout writes and the curl
  easy handle. All sequencing (turns, tool calls) is single-threaded here.
- **stdin reader** — blocking `read` loop, byte-level rules of §3, pushes
  parsed records into a queue. Keeps reading during turns: this is what makes
  steering observable while the stream runs.
- **one reader per stdio mcp server** — blocking line reads from the child,
  pushes json-rpc messages into that server's queue.

Queues are mutex + condition variable, bounded at one entry per producer
(records are consumed, not accumulated, by main). Threads are the lazy correct
primitive: curl reads and child reads are blocking calls, and only main
mutates conversation state, so there is no locking above the queues.

stdout: one `fwrite` + `fflush` per record from main only; records are never
interleaved or split.

## 3. input pipeline

Byte level, in order, per chunk read from stdin:

1. drop every `0x0d` byte (covers `\r\n` line endings; a raw CR inside a json
   string is illegal json anyway, so dropping all is safe and matches the
   requirements).
2. split on `0x0a`; a trailing fragment without `\n` is held for the next
   chunk (or processed at EOF).
3. validate UTF-8 strictly (reject overlongs, surrogates, > U+10FFFF):
   `invalid_record`, fatal. A leading BOM fails this or json parsing, per
   requirements it is not stripped.
4. parse with cJSON: malformed json → `invalid_record`, fatal.
5. dispatch on `type`:

| input `type` | handling |
|---|---|
| `header` | first record of a stream: validate `version`, else fatal; anywhere else: skip |
| `llm`, `tools`, `options`, `system`, `user` | validate required fields, store/append; unknown extra fields are ignored (the requirements only make unknown option names and unknown `type` fatal) |
| `flush` | control, see state machine below |
| `start`, `error` | inert, skip |
| `agent-as-tool`, anything else | `invalid_record`, fatal |

Record state machine (main thread view; `records_since_flush` counts
non-control records since the last consumed `flush`):

| state | event | action |
|---|---|---|
| `reading` | stdin closes or `flush` with `records_since_flush > 0` | validate `llm` present + ≥1 `user`; connect servers; → `running` (first turn); `flush` consumed, `start` marker emitted |
| `reading` | `flush` with `records_since_flush == 0` | non-fatal `invalid_record`, no-op — unless it is the very first `flush`, which always attempts the start |
| `running` (turn in flight) | record or `flush` arrives | buffered as candidate steering; applied at the turn boundary |
| turn boundary | buffered records **and** ≥1 `flush` | steering: records append as next user turns (config records apply here too); `flush` consumed, `start` marker emitted |
| turn boundary | buffered records, **no** `flush` | records stay pending; loop continues; if this boundary ends with a final `response`, emit one `io_error` naming the pending count, then the `response`, exit 0 (the drop rule) |
| turn boundary | no buffered records | `flush` with no new records → non-fatal `invalid_record` no-op; otherwise proceed |

Accepted race: records arriving between the boundary check and process exit
after a final `response` are lost with no `io_error`; the exit is already
committed. The requirements' observable contract (io_error precedes the final
response) holds for everything enqueued before the boundary.

## 4. conversation engine

Main thread, per turn:

1. drain the stdin queue (state machine above).
2. build the request from the transcript (§7) and the effective config
   (`llm`, `tools`, `options`, `system` snapshots; change timing per
   requirements §5).
3. pre-send validations that depend on effective values: anthropic
   `max_tokens` present, `thinking_budget < max_tokens`, first-message-user
   under anthropic. All `invalid_record`, fatal, before any bytes hit the
   socket.
4. call the endpoint (§5). Stream deltas are buffered per open content block
   and flushed to stdout every `stream_interval` (monotonic clock; `0` =
   flush per delta event). Block stop or turn end always flushes.
5. turn end: `response` final record, or complete `tool_request` records.
   Tool turns: execute each request strictly in order (§6), emit
   `tool_response` per request plus non-fatal `tool_failed` / `tool_timeout`
   on failure; then next turn.
6. stop conditions: final `response` with no steering pending (exit 0),
   `max_tool_rounds` (the would-exceed turn is not started; fatal
   `max_tool_rounds_exceeded`), fatal `error`, external stop (§8).

`usage` and `finish_reason` attach to the final record of the turn
(`response`, or the last `tool_request` on tool turns). `signature` attaches
to the thinking block's final record at that block's stop.

## 5. endpoint clients

One shared sse line parser (event/data accumulation, blank-line dispatch,
`ping` ignored) used by all three wire clients and by mcp http. One curl easy
handle per endpoint, reused across turns; connection reuse is irrelevant to
server side prefix caches, fresh or reused both work.

Timeouts:

| option | mechanism |
|---|---|
| `llm_connect_timeout` | `CURLOPT_CONNECTTIMEOUT` |
| `llm_read_timeout` | `CURLOPT_LOW_SPEED_LIMIT=1`, `CURLOPT_LOW_SPEED_TIME=timeout` — aborts when the transfer sustains under 1 byte/s for the window, covering first byte and inter-chunk alike. `ponytail:` a ≥1 byte/s trickle never triggers; replace with a write-callback watchdog if a pathological endpoint exists |

Auth: `Authorization: Bearer <api_key>` when `api_key` set; `headers` entries
merge over it. Real anthropic is reached through `headers` per the
requirements recipe.

### openai wire (chat completions)

- Endpoint: `<api_base>/chat/completions`, POST, `Content-Type:
  application/json`.
- Body field order is fixed by construction (§7): `model`?, `messages`,
  `tools`?, sampling fields in the §4 order of the requirements, `stream`,
  `stream_options: {"include_usage": true}` when streaming (constant, not
  volatile; needed for `usage`).
- `system` record → first message, role `system`. Consecutive same-role
  messages are never merged. Tool turns: assistant message carries `content`
  (the turn's concatenated text) and `tool_calls` (N entries); each
  `tool_response` becomes a `tool` role message. `thinking` records are never
  resent.
- Streaming mapping:

| wire | record |
|---|---|
| `choices[].delta.content` | `response` partials |
| `choices[].delta.reasoning_content` or `reasoning` | `thinking` partials, `signature` empty |
| `choices[].delta.tool_calls[]` | merged by `index`: `id`/`function.name` arrive once, `function.arguments` fragments concatenate; the complete `tool_request` is emitted at turn end |
| `choices[].delta.finish_reason` | normalized `finish_reason` on the turn's final record |
| final `usage` chunk | `usage` on the turn's final record |
| `data: [DONE]` | end of body |

- Non-2xx: read the error body, emit `api_error` (4xx/5xx with a json error
  body) or `http_error` (everything else), fatal. Values outside the
  `finish_reason` table pass through verbatim.

### openai_responses wire (responses api)

- Endpoint: `<api_base>/responses`, POST.
- Body order fixed: `model`?, `instructions` (from the `system` record)?,
  `input`, `tools`?, sampling fields, `max_output_tokens`?, `stream`, plus
  two constants: `store: false` — the runner is stateless, nothing is read
  back server side — and `include: ["reasoning.encrypted_content"]`, which
  makes reasoning resolvable client side so turns replay without server
  state. Constants are fixed bytes, not volatile data.
- Tools shape: `{type:"function", name, description, parameters}` — flat,
  unlike the chat completions nested `function` object.
- A tool turn: one assistant `message` item (the turn's text, if any) then N
  `function_call` items; each `tool_response` becomes a `function_call_output`
  item, same order, paired by `call_id`.
- Streaming mapping:

| event | record |
|---|---|
| `response.output_text.delta` | `response` partials |
| `response.reasoning_summary_text.delta` | `thinking` partials |
| `response.function_call_arguments.delta` | argument fragments concatenate per `output_index` |
| `response.output_item.done` (`reasoning`) | id + `encrypted_content` held for the thinking block's final record (`signature`) |
| `response.output_item.done` (`function_call`) | complete `tool_request`, emitted at turn end |
| `response.completed` / `response.incomplete` | `usage` (`input_tokens`/`output_tokens`); normalized `finish_reason` from status + `incomplete_details` |
| `response.failed`, `error` | `api_error` with the body's message |

- Non-2xx handling identical to openai.

### anthropic wire

- Endpoint: `<api_base>/messages`, POST. Body order fixed: `model`?, `system`
  (string, from the `system` record)?, `messages`, `tools`?, sampling fields,
  `max_tokens` (always present — required, §4 of the requirements), `stream`.
- `thinking_budget` → `thinking: {"type":"enabled","budget_tokens":N}`;
  absent → the field is omitted, thinking disabled.
- Tool turns: one assistant message with `{type:"text"}` block (the turn's
  text, if any) then `{type:"tool_use"}` blocks; `tool_response` records
  become one `user` message with `{type:"tool_result", tool_use_id}` blocks,
  same order.
- Signed thinking: resent verbatim (`{type:"thinking", thinking, signature}`)
  when that turn ended in tool requests; dropped per the requirements'
  unsigned-thinking rule otherwise (tool records of that turn omitted from
  the request too).
- Streaming mapping:

| event | record |
|---|---|
| `message_start` | `usage.input_tokens` captured |
| `content_block_start` (thinking/text/tool_use) | opens a block accumulator |
| `content_block_delta` `thinking_delta` / `text_delta` | `thinking` / `response` partials |
| `content_block_delta` `signature_delta` | held for the thinking block's final record |
| `content_block_delta` `input_json_delta` | argument fragments concatenate per tool id |
| `content_block_stop` | flush that block: thinking final record gets `partial:false` + `signature`; complete `tool_request` records are emitted at `message_stop` |
| `message_delta` | `stop_reason` → normalized `finish_reason`; `usage.output_tokens` |
| `ping`, `message_stop` | ignored / end of turn |

- Non-2xx handling identical to openai.

## 6. mcp client

- Framing: json-rpc 2.0. stdio transport = newline-delimited json on the
  child's stdin/stdout; the child's stderr is passed through to the runner's
  stderr untouched (it is the log channel). http/sse transport = streamable
  http: POST json, accept `text/event-stream` or `application/json` responses,
  `Mcp-Session-Id` echoed when the server issues one; `sse` type speaks the
  legacy http+sse transport.
- `protocol` field maps to the `protocolVersion` sent in `initialize`; both
  revisions of the requirements are supported, differences (v2 structured
  results, keepalive) handled inside this module.
- Handshake at connect: `initialize` → `initialized` → `tools/list`. The
  listing is a snapshot: tool order exposed to the model is the `tools`
  record's server order, then each server's own listing order. It changes
  only when a `tools` record changes it (§7).
- `tools/call` `{name, arguments}`; the `name` sent to the server is the part
  after the server prefix. Result: text content blocks concatenate with `\n`
  into `tool_response.text`; `isError: true` → `is_error: true` plus
  non-fatal `tool_failed`; non-text content blocks → `tool_failed` per the
  requirements; structured-only results (v2) with no text → `tool_failed`.
- Timeouts: `tool_call_timeout` is a timed wait on the server's queue (stdio)
  or `CURLOPT_TIMEOUT` (http). Expiry → `tool_response` `is_error` +
  non-fatal `tool_timeout`.
- `required: true` connect failure → fatal `connect_failed`; otherwise
  non-fatal and the conversation proceeds without those tools. A stdio child
  that dies mid-conversation fails its next call as `tool_failed`.
- Spawn: linux `/bin/sh -c <command_line>`; windows `cmd /d /s /c
  <command_line>` — the caller owns quoting per the requirements. `ponytail:`
  no shell-less spawn mode; add one if a server needs argv-precise control.

## 7. serialization and prefix cache

The invariant from requirements §6 — the message prefix of turn N is byte
identical to the request of turn N-1 — is enforced structurally:

- Every record is parsed into a cJSON tree once, at ingest. Trees are never
  reordered, mutated or re-parsed; cJSON serializes a given tree to the same
  bytes every time (insertion-ordered fields, fixed number format). So
  re-serializing the same records can't drift.
- Per conversation, an append-only byte buffer holds the serialized `messages`
  array (openai) or `messages` (anthropic). A new turn appends its message
  bytes. The request body is envelope + buffer; only envelope fields
  (`max_tokens`, sampling, `stream`, ...) are rebuilt per request, so
  sampling-only changes never touch the prefix bytes.
- Tool `arguments` and thinking text round-trip through the same rule: same
  tree in, same bytes out. (A number lexeme like `1e3` may print as `1000`
  — consistently, every turn, which is all the invariant needs.)
- Invalidation: a `system`, `tools`, or `llm` change, or transcript growth by
  a new turn, are the only prefix writers; each rebuilds from its point
  onward. Under openai a `system` change rewrites `messages[0]` — a miss from
  the top, inherent to the protocol. An `llm` change rebuilds everything and
  resets cache expectations.
- No volatile data anywhere in body bytes: no timestamps, counters, ids. The
  curl handle's own headers (Authorization, custom `headers`) are outside the
  body.

Anthropic `cache_control` breakpoints, 4 maximum, **append only**: a marker,
once written into a block, is never moved or removed — moving or removing one
rewrites old prefix bytes and invalidates every cache entry behind it, which
would make the runner the cause of a full miss.

Marker budget, in order:

1. last tool definition — one marker covers the whole `system` + `tools`
   prefix, since system precedes tools in the body; a separate system marker
   would waste a slot.
2. + 3. + 4. the last content block of the last message of each completed
   turn, placed as the turn completes. A breakpoint caches everything before
   it, so slots spent as late as possible cover the most prefix; the cost of
   a turn left unmarked is that its request re-reads from the last marker.

When all 4 slots are used the frontier freezes: later turns append without
markers and re-read the tail beyond the frozen frontier each turn.
`ponytail:` the per-turn tail re-read beyond the frontier is the named
ceiling — it is the price of ≤4 append-only breakpoints; revisit (breakpoint
moving with accepted one-segment misses, or 1h TTL batching) only if
measurements on long conversations say it matters.

Markers are an overlay the design adds on top of the record→wire mapping; the
byte-identity rule above governs the mapped bytes, markers are appended once
and never rewritten, so a continuation prefix never drifts.

Both openai protocols: caching is automatic above 1024 prefix tokens; there
is nothing to place, deterministic bytes are the whole job. Under
openai_responses the `system` record maps to the envelope-side
`instructions` parameter, so a `system` change does not invalidate the
`input` prefix at all.

## 8. external stop

- linux: `sigaction` on SIGINT sets an atomic flag. The curl write callback
  and the engine's wait points check it; the callback aborts the transfer by
  returning a short count (connection closed). Windows:
  `SetConsoleCtrlHandler` for `CTRL_C_EVENT` sets the same flag from the
  console thread.
- Orderly stop, main thread: flush the open block's buffered text as the last
  partial (`partial: true` stays), let a running tool finish and emit its
  `tool_response`, answer suspended `tool_request`s with `is_error`, drop
  unapplied steering, apply the drop rule for unflushed records (`io_error`
  first), emit fatal `interrupted`, kill stdio children, exit.
- The stdin reader stops enqueueing the moment the flag is set: records
  received after the stop are not read.
- Second SIGINT: the handler ` _exit()`s / `ExitProcess` with the
  `interrupted` exit code immediately, no further records.
- Before the conversation starts: same flag path, drop rule + `interrupted`,
  nonzero exit.

## 9. windows specifics

- stdout and stdin are set to binary mode (`_setmode(..., _O_BINARY)`);
  records are `\n`-terminated, never `\r\n`. UTF-8 is bytes end to end; no
  console code page APIs are touched because both channels are pipes in every
  supported launch shape.
- Process spawn and the ctrl handler go through `platform.c` (CreateProcess,
  `cmd /d /s /c`). winsock is initialized once at startup.
- Cross-compile with `x86_64-w64-mingw32-gcc`, statically linked curl built
  on the schannel backend (no openssl). `ponytail:` if the toolchain fights
  static curl, shipping `libcurl.dll` next to the exe is the accepted
  fallback; single-file exe is the goal, not a hill to die on.

## 10. agent-as-tool

Same core, second entry point. `llmkit agent-as-tool <seed.jsonl>`:

- Startup: read and validate the seed with the runner's pipeline (minus the
  `user` requirement), connect the seed's mcp servers. Fatal seed or startup
  errors are out of channel: stderr, exit nonzero, no records — per the
  requirements' boundary rule. After bootstrap stdout is the mcp channel.
- The mcp server loop exposes exactly `invoke(input: string)`; descriptions
  come from the `agent-as-tool` record.
- Each `invoke` builds a record list — seed records + `user` carrying
  `input` — and runs the engine over it unchanged (mapping, options, error
  rules). Reply: concatenated final `response` text. A fatal `error` becomes
  a failed tool call carrying code + message.
- `retain_context` false: the record list is rebuilt per invoke from the
  immutable seed; the seed prefix serializes identically every time, so cache
  hits accumulate.
- `retain_context` true: one accumulated transcript; each invoke appends its
  `user` record and turns. On fatal error the store truncates back to the
  last successful invoke's marker (the bare seed if none) before the next
  invoke.
- Nested agents are ordinary child processes; spawn cycles are not detected
  (per the requirements).

## 11. exit codes

| code | meaning |
|---|---|
| 0 | conversation ended with a successful final `response` (non-fatal error records may precede it) |
| 1 | out-of-channel failure: startup errors on stderr, usage errors, stdout write failure |
| 2 | `invalid_record` (fatal) |
| 3 | `connect_failed` (fatal) |
| 4 | `http_error` |
| 5 | `api_error` |
| 6 | `max_tool_rounds_exceeded` |
| 7 | `io_error` (fatal case) |
| 8 | `interrupted`, including the second-SIGINT hard exit |

When several fatal records could apply, the first one emitted decides.
Non-fatal error records never influence the exit code.

## 12. build and packaging

Plain Makefile, no build system:

- `make` → `llmkit` (cc, linux)
- `make check` → builds and runs `test/selfcheck` (§13)
- `make CROSS=x86_64-w64-mingw32-` → `llmkit.exe`

Sources: `src/main.c` (subcommands), `src/agent.c` (agent-as-tool),
`src/engine.c` (state machine + turn loop), `src/jsonl.c` (input pipeline,
validation), `src/wire_openai.c`, `src/wire_anthropic.c`, `src/sse.c`,
`src/mcp.c`, `src/buf.c` (byte buffers, the §7 append-only buffer),
`src/platform.c` (threads, spawn, signals, binary mode). Link flags:
`-lcjson -lcurl` on both targets; the cross sysroot provides both.

## 13. testing

`make check` builds one `selfcheck` binary, plain asserts, no framework:

- **prefix invariant**: scripted conversations (all three protocols, with
  tools, steering, `llm` switch) where the serialized prefix of turn N is
  byte-compared against turn N-1's request — the cache rule tested directly.
- **sse parser**: fixed vectors for openai chunks (both apis), anthropic
  events, mcp responses, split at awkward byte boundaries.
- **validation**: BOM, raw CR dropping, invalid UTF-8, missing `llm`/`user`,
  duplicate server name, bare `flush`, missing anthropic `max_tokens`,
  `thinking_budget >= max_tokens`.
- **state machine**: flush consumption, steering application, drop rule,
  applied against an in-process fake endpoint function.

`ponytail:` no network integration tests in-tree; a `test/live.sh` hitting a
real endpoint is added when the first endpoint bug shows up.
