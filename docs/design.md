# llmkit design

Implements [docs/requirements.md](requirements.md), transcript format version 1.
Every deferral named there is answered here: libraries in sec.1, the exit code table
in sec.13, `cache_control` placement and openai caching in sec.7, wire-level protocol
details in sec.5 and sec.6. Where this document picks a ceiling, it is marked
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
9. [agent-as-tool](#9-agent-as-tool)
10. [mcp-proxy](#10-mcp-proxy)
11. [call](#11-call)
12. [repl](#12-repl)
13. [exit codes](#13-exit-codes)
14. [build and packaging](#14-build-and-packaging)
15. [testing](#15-testing)

## 1. technology choices

| need | choice | why |
|---|---|---|
| language | C11 | per requirements; no compiler exceptions |
| json | cJSON, system installed (`-lcjson`, any 1.x; `libcjson-dev`) | object fields are a linked list in insertion order: serialization is deterministic by construction, which sec.7 depends on; two-file library, nothing to vendor |
| http/tls | libcurl (easy api), version floor 7.80 | tls, chunked transfer, sse body reading, connect and low-speed timeouts; alternatives (json-c, jansson, a hand-rolled http client) add a build system, lose ordering, or re-implement tls |
| concurrency | pthreads, behind `platform.c` | no dependency, no event loop to debug |

No other third-party code. mcp, sse parsing, jsonl, utf-8 validation are in-tree.

## 2. process and threading model

One process per conversation (`llmkit runner`) or per agent server
(`llmkit agent-as-tool`). Threads:

- **main** - owns the conversation engine, all stdout writes and the curl
  easy handle. All sequencing (turns, tool calls) is single-threaded here.
- **stdin reader** - blocking `read` loop, byte-level rules of sec.3, pushes
  parsed records into a queue. Keeps reading during turns: this is what makes
  steering observable while the stream runs.
- **one reader per stdio mcp server** - blocking line reads from the child,
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
4. parse with cJSON: malformed json -> `invalid_record`, fatal.
5. dispatch on `type`:

| input `type` | handling |
|---|---|
| `header` | first record of a stream: validate `version`, else fatal; anywhere else: skip |
| `llm`, `tools`, `options`, `system`, `user` | validate required fields, store/append; unknown extra fields are ignored (the requirements only make unknown option names and unknown `type` fatal) |
| `flush` | control, see state machine below |
| `start`, `error` | inert, skip |
| `agent-as-tool`, anything else | `invalid_record`, fatal |

A failing `read` on stdin (an error, not EOF) is the error catalog's stdin
failure: a fatal `io_error` record, emitted at detection like every error
record; from then on no input exists - EOF semantics for everything else,
so the drop rule has nothing left to drop.

Record state machine (main thread view; `records_since_flush` counts
non-control records since the last consumed `flush`):

| state | event | action |
|---|---|---|
| `reading` | stdin closes or `flush` with `records_since_flush > 0` | validate `llm` present + >=1 `user`; connect servers; -> `running` (first turn); `flush` path: consumed, `start` marker emitted - stdin close path: no marker, markers pair with consumed `flush` records only |
| `reading` | `flush` with `records_since_flush == 0` | non-fatal `invalid_record`, no-op - unless it is the very first `flush`, which always attempts the start |
| `running` (turn in flight) | record or `flush` arrives | buffered as candidate steering; applied at the turn boundary |
| turn boundary | buffered records **and** >=1 `flush` | steering: records append as next user turns (config records apply here too); `flush` consumed, `start` marker emitted |
| turn boundary | buffered records, **no** `flush` | records stay pending; loop continues; if this boundary ends with a final `response`, emit one `io_error` naming the pending count, then the `response`, exit 0 (the drop rule) |
| turn boundary | no buffered records | `flush` with no new records -> non-fatal `invalid_record` no-op; otherwise proceed |

Accepted race: records arriving between the boundary check and process exit
after a final `response` are lost with no `io_error`; the exit is already
committed. The requirements' observable contract (io_error precedes the final
response) holds for everything enqueued before the boundary.

## 4. conversation engine

Main thread, per turn:

1. drain the stdin queue (state machine above).
2. build the request from the transcript (sec.7) and the effective config
   (`llm`, `tools`, `options`, `system` snapshots; change timing per
   requirements sec.5).
3. pre-send validations that depend on effective values: anthropic
   `max_tokens` present, `thinking_budget < max_tokens`, first-message-user
   under anthropic. All `invalid_record`, fatal, before any bytes hit the
   socket.
4. call the endpoint (sec.5). Stream deltas are buffered per open content block
   and flushed to stdout every `stream_interval` (monotonic clock; `0` =
   flush per delta event). Block stop or turn end always flushes.
5. turn end: `response` final record, or complete `tool_request` records.
   Tool turns: execute each request strictly in order (sec.6), emit
   `tool_response` per request plus non-fatal `tool_failed` / `tool_timeout`
   on failure; then next turn. A terminal tool (sec.6) ends the run at its
   own `tool_response`, failure and timeout included - no retry, the model
   never sees the result: the remaining requests of the turn are suspended
   and answered `is_error` (the steering synthesis), unflushed records get
   sec.3's drop rule (their non-fatal `io_error`, at detection), no error
   record is emitted for the ending, the exit code is 9 (sec.13).
6. stop conditions: final `response` with no steering pending (exit 0),
   terminal tool answered (exit 9, no error record), `max_tool_rounds` (the
   would-exceed turn is not started; fatal `max_tool_rounds_exceeded`),
   fatal `error`, external stop (sec.8).

`usage` and `finish_reason` attach to the final record of the turn
(`response`, or the last `tool_request` on tool turns). `signature` attaches
to the thinking block's final record at that block's stop. `partial` is
block-scoped the same way: false only on the final record of each streamed
block.

## 5. endpoint clients

One shared sse line parser (event/data accumulation, blank-line dispatch,
`ping` ignored) used by all three wire clients and by mcp http. One curl easy
handle per endpoint, reused across turns; connection reuse is irrelevant to
server side prefix caches, fresh or reused both work.

Non-streaming (`stream` false): the complete body maps to the same final
records a streamed turn would emit - no partials, one record per content
block. openai: `choices[0].message` (`content` -> `response`,
`reasoning_content`/`reasoning` -> `thinking`, `tool_calls[]` ->
`tool_request`s), plus `finish_reason` and `usage`. openai_responses:
`output` items (`message` -> `response` text, `reasoning` -> `thinking` +
`signature`, `function_call` -> `tool_request`), with
`status`/`incomplete_details` -> normalized `finish_reason`, and `usage`.
anthropic: `content` blocks (`text`/`thinking`/`tool_use`), `stop_reason`,
`usage`. Stream parity - both modes produce identical final records - is
asserted by the selfcheck (sec.15).

Timeouts:

| option | mechanism |
|---|---|
| `llm_connect_timeout` | `CURLOPT_CONNECTTIMEOUT` |
| `llm_read_timeout` | `CURLOPT_LOW_SPEED_LIMIT=1`, `CURLOPT_LOW_SPEED_TIME=timeout` - aborts when the transfer sustains under 1 byte/s for the window, covering first byte and inter-chunk alike. `ponytail:` a >=1 byte/s trickle never triggers; replace with a write-callback watchdog if a pathological endpoint exists |

Auth: `Authorization: Bearer <api_key>` when `api_key` set; `headers` entries
merge over it. Real anthropic is reached through `headers` per the
requirements recipe. Both curl timeout options take whole seconds:
fractional `llm_connect_timeout`/`llm_read_timeout` values are floored;
`stream_interval` and `tool_call_timeout` keep fractional precision.

### openai wire (chat completions)

- Endpoint: `<api_base>/chat/completions`, POST, `Content-Type:
  application/json`.
- Body field order is fixed by construction (sec.7): `model`?, `messages`,
  `tools`?, sampling fields in the sec.4 order of the requirements, `stream`,
  `stream_options: {"include_usage": true}` when streaming (constant, not
  volatile; needed for `usage`).
- `system` record -> first message, role `system`. Consecutive same-role
  messages are never merged. Multi-block `system`/`user` content joins with
  `\n` into one string (anthropic sends the blocks natively; requirements
  sec.7). Tool turns: assistant message carries `content`
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
  two constants: `store: false` - the runner is stateless, nothing is read
  back server side - and `include: ["reasoning.encrypted_content"]`, which
  makes reasoning resolvable client side so turns replay without server
  state. Constants are fixed bytes, not volatile data.
- Tools shape: `{type:"function", name, description, parameters}` - flat,
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
| `response.output_item.done` (`reasoning`) | the complete serialized item is held for the thinking block's final record (`signature`) |
| `response.output_item.done` (`function_call`) | complete `tool_request`, emitted at turn end |
| `response.completed` / `response.incomplete` | `usage` (`input_tokens`/`output_tokens`); normalized `finish_reason` from status + `incomplete_details` |
| `response.failed`, `error` | `api_error` with the body's message |

- Non-2xx handling identical to openai.

### anthropic wire

- Endpoint: `<api_base>/messages`, POST. Body order fixed: `model`?, `system`
  (string, from the `system` record)?, `messages`, `tools`?, sampling fields,
  `max_tokens` (always present - required, sec.4 of the requirements), `stream`.
- `thinking_budget` -> `thinking: {"type":"enabled","budget_tokens":N}`;
  absent -> the field is omitted, thinking disabled.
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
| `message_delta` | `stop_reason` -> normalized `finish_reason`; `usage.output_tokens` |
| `ping`, `message_stop` | ignored / end of turn |
| `error` | fatal `api_error` with the event's message |

- Non-2xx handling identical to openai.

## 6. mcp client

- Framing: json-rpc 2.0. stdio transport = newline-delimited json on the
  child's stdin/stdout; the child's stderr is passed through to the runner's
  stderr untouched (it is the log channel). http transport = streamable
  http: POST json, accept `text/event-stream` or `application/json`
  responses. `sse` type speaks the legacy http+sse transport: one long-lived
  GET opens the event stream, the server's first `endpoint` event names the
  uri every request POSTs to (its query carries the session id when there is
  one); replies come back in the POST response body, server pushes arrive as
  events on the GET stream.
- `protocol` picks the revision line - v1 and v2 are different protocols,
  not versions of one. Any published v1 revision from `2024-11-05` to
  `2025-11-25` (default) or v2 `2026-07-28`.
- **v1 (stateful)**: connect runs `initialize` (carrying `protocolVersion`)
  -> `notifications/initialized` -> `tools/list`. The `initialize` result
  names the revision to use: another supported v1 revision is adopted;
  anything else is a connect failure, the `required` rule applies. A
  `Mcp-Session-Id` response header is echoed on every later http request.
- **v2 (stateless)**: the `initialize` handshake and `Mcp-Session-Id` are
  gone. Every request carries `_meta` with
  `io.modelcontextprotocol/protocolVersion`, `clientCapabilities` and a
  static `clientInfo` (`llmkit` plus build version - a constant, nothing
  volatile enters any request). `server/discover` is optional; the runner
  goes straight to `tools/list`. An `UnsupportedProtocolVersionError` reply
  is a connect failure.
- Either way the connect ends with `tools/list`. The listing is a snapshot:
  tool order exposed to the model is the `tools` record's server order, then
  each server's own listing order. It changes only when a `tools` record
  changes it (sec.7).
- `terminal_tools` (requirements sec.8, sec.2.7): per server entry, two
  validation tiers - shape (list of strings) at record validation, name
  resolution at reconcile against that server's own listing: a name a
  connected server does not list is fatal `invalid_record`, the proxy's
  expose/hide typo rule; a server that failed to connect is not validated,
  its tools are not offered anyway. The resolved set lives on the server
  struct, re-derived by every reconcile, so a `tools` record change
  revalidates at the boundary it applies at. The engine resolves an exposed
  `name.tool` through the same prefix split `tools/call` uses; execution of
  a terminal tool is the sec.4 stop condition. The attribute is config, it
  never enters the serialized tool list - no request byte changes.
- `tools/call` `{name, arguments}`; the `name` sent to the server is the part
  after the server prefix. Result: text content blocks concatenate with `\n`
  into `tool_response.text`; `isError: true` -> `is_error: true` plus
  non-fatal `tool_failed`; non-text content blocks -> `tool_failed` per the
  requirements; structured-only results (v2) with no text -> `tool_failed`.
- Timeouts: `tool_call_timeout` is a timed wait on the server's queue (stdio)
  or `CURLOPT_TIMEOUT` (http). Expiry -> `tool_response` `is_error` +
  non-fatal `tool_timeout`.
- `required: true` connect failure -> fatal `connect_failed`; otherwise
  non-fatal and the conversation proceeds without those tools. A stdio child
  that dies mid-conversation fails its next call as `tool_failed`.
- Spawn: `/bin/sh -c <command_line>` - the caller owns quoting per the
  requirements. `ponytail:`
  no shell-less spawn mode; add one if a server needs argv-precise control.

## 7. serialization and prefix cache

The invariant from requirements sec.6 - the message prefix of turn N is byte
identical to the request of turn N-1 - is enforced structurally:

- Every record is parsed into a cJSON tree once, at ingest. Trees are never
  reordered, mutated or re-parsed; cJSON serializes a given tree to the same
  bytes every time (insertion-ordered fields, fixed number format). So
  re-serializing the same records can't drift.
- Per conversation, an append-only byte buffer holds the serialized message
  array - `messages` for openai and anthropic, `input` for openai_responses.
  A new turn appends its message bytes. The request body is envelope + buffer; only envelope fields
  (`max_tokens`, sampling, `stream`, ...) are rebuilt per request, so
  sampling-only changes never touch the prefix bytes.
- Tool `arguments` and thinking text round-trip through the same rule: same
  tree in, same bytes out. (A number lexeme like `1e3` may print as `1000`
  - consistently, every turn, which is all the invariant needs. Integers
  beyond 2^53 in tool arguments round-trip through doubles: precision can
  be lost, deterministically; `ponytail:` store raw argument bytes only if
  a real tool ever needs exact big integers.)
- Invalidation: a `system`, `tools`, or `llm` change, or transcript growth by
  a new turn, are the only prefix writers; each rebuilds from its point
  onward. Under openai a `system` change rewrites `messages[0]` - a miss from
  the top, inherent to the protocol. An `llm` change rebuilds everything and
  resets cache expectations.
- No volatile data anywhere in body bytes: no timestamps, counters, ids. The
  curl handle's own headers (Authorization, custom `headers`) are outside the
  body.

Anthropic `cache_control` breakpoints, 4 maximum, **append only**: a marker,
once written into a block, is never moved or removed - moving or removing one
rewrites old prefix bytes and invalidates every cache entry behind it, which
would make the runner the cause of a full miss.

Marker budget, in order:

1. the end of the static prefix - the last tool definition when tools
   exist, else the last block of the `system` param (anthropic accepts a
   block array there) - one marker covers `system` + `tools`, since system
   precedes tools in the body; a separate system marker would waste a slot.
2. + 3. + 4. the last content block of the last message of each completed
   turn, placed as the turn completes. A breakpoint caches everything before
   it, so slots spent as late as possible cover the most prefix; the cost of
   a turn left unmarked is that its request re-reads from the last marker.

When all 4 slots are used the frontier freezes: later turns append without
markers and re-read the tail beyond the frozen frontier each turn.
`ponytail:` the per-turn tail re-read beyond the frontier is the named
ceiling - it is the price of <=4 append-only breakpoints; revisit (breakpoint
moving with accepted one-segment misses, or 1h TTL batching) only if
measurements on long conversations say it matters.

Markers are an overlay the design adds on top of the record->wire mapping; the
byte-identity rule above governs the mapped bytes, markers are appended once
and never rewritten, so a continuation prefix never drifts.

Both openai protocols: caching is automatic above 1024 prefix tokens; there
is nothing to place, deterministic bytes are the whole job. Under
openai_responses the `system` record maps to the envelope-side
`instructions` parameter, so a `system` change does not invalidate the
`input` prefix at all.

## 8. external stop

- `sigaction` on SIGINT sets an atomic flag. The curl write callback
  and the engine's wait points check it; the callback aborts the transfer by
  returning a short count (connection closed).
- Orderly stop, main thread: flush the open block's buffered text as the last
  partial (`partial: true` stays), let a running tool finish and emit its
  `tool_response`, answer suspended `tool_request`s with `is_error`, drop
  unapplied steering, apply the drop rule for unflushed records (`io_error`
  first), emit fatal `interrupted`, kill stdio children, exit. `repl` stops
  one step earlier (sec.12): its session continues after the stop, so the
  children live and the engine returns the code instead of the process
  exiting.
- Terminal precedence: when the tool that just finished - or one answered
  earlier in the batch - is terminal (sec.6), the terminal ending replaces
  the orderly stop: no `interrupted` record, exit 9. Once a terminal tool's
  `tool_response` is emitted the ending is committed; a SIGINT after that
  moment cannot turn it into `interrupted`. A SIGINT before the terminal
  tool started is the orderly stop above, unchanged.
- The stdin reader stops enqueueing the moment the flag is set: records
  received after the stop are not read.
- Second SIGINT: the handler `_exit()`s with the
  `interrupted` exit code immediately, no further records.
- Before the conversation starts: same flag path, drop rule only - the
  non-fatal `io_error` records - nonzero exit; no `interrupted` record,
  there is no conversation to interrupt (requirements sec.2.6).

## 9. agent-as-tool

Same core, second entry point. `llmkit agent-as-tool <seed.jsonl>`:

- Startup: read and validate the seed with the runner's pipeline (minus the
  `user` requirement), connect the seed's mcp servers. Fatal seed or startup
  errors are out of channel: stderr, exit nonzero, no records - per the
  requirements' boundary rule. After bootstrap stdout is the mcp channel.
- The mcp server loop exposes exactly `invoke(input: string)`; descriptions
  come from the `agent-as-tool` record.
- Engine output goes to an in-memory sink, never to stdout - after
  bootstrap stdout carries only mcp traffic. Concurrent `tools/call`
  requests are serialized, one conversation at a time; replies are written
  in request order.
- Each `invoke` builds a record list - seed records + `user` carrying
  `input` - and runs the engine over it unchanged (mapping, options, error
  rules). Reply: concatenated final `response` text; a run ended by a
  terminal tool (sec.6) replies with that tool's `tool_response` text
  instead, a failed one (`is_error`) as a failed call carrying the message.
  A fatal `error` becomes a failed tool call carrying code + message.
- `retain_context` false: the record list is rebuilt per invoke from the
  immutable seed; the seed prefix serializes identically every time, so cache
  hits accumulate.
- `retain_context` true: one accumulated transcript; each invoke appends its
  `user` record and turns. On fatal error the store truncates back to the
  last successful invoke's marker (the bare seed if none) before the next
  invoke. A terminal ending is a successful ending: retained like a final
  `response`, no truncation.
- Nested agents are ordinary child processes; spawn cycles are not detected
  (per the requirements).

## 10. mcp-proxy

Same core, third entry point. `llmkit mcp-proxy <config.jsonl>`:

- Startup: parse the config with the runner's pipeline (sec.3 dispatch
  restricted to `header`, `tools`, `expose`, `hide`; anything else is
  `invalid_record`, fatal), then connect the upstream servers and run
  `tools/list` on each (sec.6 rules), and resolve every `expose`/`hide` against
  the listings. Fatal config or startup errors are out of channel: stderr,
  exit 1, no mcp traffic.
- The exposed list is built once at startup and frozen. Renames rewrite the
  upstream listing in place - a renamed argument keeps its position in the
  schema - so the serialized listing is deterministic across restarts, which
  a parent runner's cache rule (sec.7 of the requirements) leans on. Order:
  `expose` record order in whitelist mode, else server order then listing
  order. `ponytail:` the listing is a startup snapshot,
  `notifications/tools/list_changed` from an upstream is ignored; relay it
  only if an upstream needs it.
- `terminal_tools` in the config's `tools` record is accepted and ignored:
  the proxy relays calls and stays up, it has no conversation to end. Only
  the shape is validated (a non-string entry is still `invalid_record`);
  name resolution does not apply - the marking is the runner's rule, and an
  upstream listing carries no field to propagate it through.
- Serving: the stdio json-rpc server loop of sec.9, tools capability only.
  `tools/call` maps the exposed name back to server + upstream tool name and
  applies the inverse argument renaming before forwarding through the sec.6
  client; the result is relayed verbatim. Calls are serialized, one at a
  time, replies in request order; a failed upstream call is a json-rpc error
  response and the proxy stays up.
- Threads: main plus one reader per stdio upstream, same queues as sec.2.

## 11. call

Same core, fourth entry point, first front-end: `llmkit call` compiles argv
to records and runs one conversation, plain text out (requirements sec.11). No
new wire, mcp or loop code; one new `src/call.c`, a dispatch line in
`src/main.c`.

- **flag parsing** - hand-rolled loop, no getopt: exact string match, the
  value is the next argv token (a missing value is a usage error), any
  order, no `--flag=value` form, no short forms, no abbreviation.
  Repeatable flags append in argv order. The parser decides CLI shape
  only; value problems stay record validation, per the requirements' two
  error tiers.
- **compilation** - the agent seed path verbatim: `signals_init`,
  `engine_new` with the call sink, `engine_apply_config_record` for the
  `llm` (carrying `inference_options.max_tokens` when `--max-tokens` is
  given - the one inference knob, anthropic makes it mandatory),
  optional `system` and optional `tools` records, `tlist_ingest`
  for the `user` record, then `engine_start` + `engine_run`, exit code
  passed through. No stdin reader thread, no queue, no header record: the
  record channel does not exist.
- **`--terminal-tool <name.tool>`** - appends to the `terminal_tools` list
  of the matching server entry of the compiled `tools` record, argv order;
  without `--mcp-proxy` servers there is no entry to attach to. The server
  prefix must name a `--mcp-proxy` server of the same command line, else
  usage error - which entry to mark is CLI shape; whether that server lists
  the tool stays record validation, the runner's `invalid_record` tier.
- **`--prompt -`** - stdin is read to EOF before anything else runs. Byte
  hygiene is sec.3's minus the line splitting: every `0x0d` dropped, strict
  UTF-8, a leading BOM rejected, NUL rejected - violations take the
  `invalid_record` tier (stderr line, exit 2): the compiled `user` record
  would fail the same validation.
- **sink** - `response` records: one `fwrite` + `fflush` of `text`, the sec.2
  rule (deltas are small, interactive use wants them immediate). `error`
  records, fatal or not: one stderr line each, `llmkit call: <code>:
  <message>`. Everything else - `thinking` above all - is dropped. The
  sink tracks the last byte written; on exit 0 one `\n` is added when the
  answer does not already end with one, an empty answer writes nothing.
  Out-of-channel errors use the same prefix with no code:
  `llmkit call: <what>`, exit 1.
- **terminal ending** - the sink prints the `text` of the terminal tool's
  `tool_response` instead of a final `response` (there is none), same
  `fwrite` + `fflush` and trailing-newline rules; the exit code is 9
  (sec.13), so scripts branch on it.
- **`--mcp-proxy` spawn** - the `tools` record `command_line` runs through
  `/bin/sh -c` (sec.6), so both paths are POSIX single-quote quoted
  (`'` becomes `'\''`): `'<exe>' mcp-proxy '<config>'`. `<exe>` is
  `readlink("/proc/self/exe")` - linux only, read once, only when the
  flag is present, `argv[0]` as fallback if the readlink fails. Server
  `name`: the config path's basename with the last extension dropped (any
  extension, not just `.jsonl`); empty or repeated is a usage error
  before anything runs. All such servers non required: a connect failure
  is the runner's non-fatal `connect_failed`, one stderr line, the
  conversation continues without those tools.
- **`--header` merge** - compiled into the `headers` object at flag-parse
  time; a later same-name flag replaces the earlier value in place. The
  tree is built once, before any turn: sec.7 determinism is unaffected.
- Threads: none of its own beyond the engine's stdio server readers.

## 12. repl

Same core, fifth entry point, second front-end: `llmkit repl` compiles
argv with `call`'s compiler and runs a session loop over one engine
(requirements sec.12). One new `src/repl.c`; the flag parser and record
compiler of `src/call.c` are factored into a shared `call_compile` - the
flag set differs only by `--prompt`'s absence, and repl appends one
`options` record (`stream_interval: 0`) after the compiled `llm` record.
No new wire, mcp or loop code.

- **session loop** - one engine for the whole session, transcript intact:
  draw the prompt, read one input line, feed the `user` record,
  `engine_run` to the turn's ending, render, prompt again. `engine_run`
  returns its code - the call path already passes it through instead of
  exiting inside the engine - and repl branches on the ending: a final
  `response`, or the interrupted code after an external stop, returns to
  the prompt; every other fatal code and the terminal-tool 9 end the
  session, exit code passed through. An EOF ending exits with the last
  ending the session saw, 0 before the first input. A `keep_mcp` flag on
  the engine skips every mid-run child kill - sec.8's kills belong to
  process teardown, which repl does not run mid-session; `engine_free`
  tears the servers down once, at session end.
- **input** - termios on stdin: `ICANON` and `ECHO` off, `ISIG` stays on.
  The program owns the line buffer - which is what the two Ctrl-C stages
  and the typed echo both need - and the driver's queue flush on the
  signal then flushes nothing, raw mode reads bytes as they are typed.
  The editor: printable bytes append, backspace deletes, Ctrl-U kills the
  line, up/down recall history, enter and Ctrl-D submit; Ctrl-D on an
  empty buffer is EOF, session end. Hygiene per the requirements: `0x0d`
  dropped, NUL and invalid UTF-8 rejected - the `invalid_record` tier,
  exit 2. `ponytail:` no cursor motion, input appends at the end only;
  add left/right editing if it is ever missed.
- **history** - in-memory ring, 128 lines, arrow recall only, no
  persistence, no search. `ponytail:` grow it when someone complains.
- **echo** - the editor echoes typed bytes wrapped in the bold sequence:
  the typed line is the user block itself, under the heavy rule the
  prompt drew; nothing re-renders on submit. When stdin is not a terminal
  there is no editor and no echo - a plain `read` loop, line per input -
  and the sink renders the `user` record instead. One bold user block
  either way.
- **typography** - the bold, italic and reset sequences are probed once at
  startup: `isatty(stdout)`, `TERM` set and not `dumb`, then `tput bold`,
  `tput sitm`, `tput sgr0`. A failed probe (no `tput`) falls back to the
  hardcoded SGR (`ESC[1m`, `ESC[3m`, `ESC[0m`); an empty `sitm` degrades
  italic to unstyled - the per-attribute rule - and an empty `sgr0` with a
  present `bold`/`sitm` falls back to `ESC[0m`, so a style never leaks.
  Not a tty: no sequences at all. The strings are output bytes only, sec.7 is untouched.
  `ponytail:` terminfo through `tput` rather than an in-tree database
  parser.
- **separators and prompt** - heavy rule `=`, light rule `-`, `>` prompt
  glyph; rule width is `TIOCGWINSZ` of stdout when it is a tty, else 80,
  read at draw time - rendering stays append only, rules never redraw.
  Error lines: `! <code>: <message>`, unstyled.
- **sink** - `call`'s discipline, one `fwrite` + `fflush` per record, with
  the block framing: a separator opens each block - the first record of
  its kind since the last block closed, and only when its text is
  non-empty: the wire's block-close records carry the signature or usage
  with empty text, and a separator armed by one would leak into the next
  user block; `partial: true` continues the open block. User bold, thinking italic, response raw, `tool_request`
  bold `name` plus compact `arguments` on one line, `tool_response` bold
  text, `start` markers nothing. The trailing newline rule is call's,
  per block; a block whose text is empty renders nothing, separator
  included.
- **interrupts** - the sec.8 flag and orderly stop unchanged, with two
  repl branch points. At the prompt the read returns `EINTR`
  (`SA_RESTART` off) and the stage rule applies: buffer non-empty -
  cleared, fresh prompt line; empty - exit 8, nothing rendered. During a
  turn the stop ends the conversation, the engine returns the interrupted
  code, the session continues. The second-SIGINT hard `_exit(8)` is
  sec.8's, unchanged.
- Threads: none of its own beyond the engine's stdio server readers -
  input is read only at the prompt and turns run alone; there is no
  steering to observe, so no stdin reader thread.

## 13. exit codes

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
| 9 | conversation ended by a terminal tool (requirements sec.2.7): the last record is that tool's `tool_response`, no error record |

When several fatal records could apply, the first one emitted decides.
Non-fatal error records never influence the exit code. Exit 9 pairs with no
error record at all - the terminal ending is requested, not an error - and
outranks a simultaneous `interrupted` (sec.8).

## 14. build and packaging

Plain Makefile, no build system:

- `make` -> `llmkit` (cc, linux)
- `make check` -> builds and runs `test/selfcheck` (sec.15)

Sources: `src/main.c` (subcommands), `src/agent.c` (agent-as-tool),
`src/call.c` (argv->record compiler, plain-text sink for `llmkit call`),
`src/proxy.c` (mcp-proxy config, rewrite and resolution),
`src/engine.c` (state machine + turn loop), `src/jsonl.c` (input pipeline,
validation), `src/wire_openai.c`, `src/wire_anthropic.c`, `src/sse.c`,
`src/mcp.c`, `src/buf.c` (byte buffers, the sec.7 append-only buffer),
`src/platform.c` (threads, spawn, signals),
`src/repl.c` (repl session loop, raw-mode line editor, display sink).
Link flags: `-lcjson -lcurl`.

## 15. testing

`make check` builds one `selfcheck` binary, plain asserts, no framework:

- **prefix invariant**: scripted conversations (all three protocols, with
  tools, steering, `llm` switch) where the serialized prefix of turn N is
  byte-compared against turn N-1's request - the cache rule tested directly.
- **sse parser**: fixed vectors for openai chunks (both apis), anthropic
  events, mcp responses, split at awkward byte boundaries.
- **stream parity**: the same scripted endpoint response replayed with
  `stream` on and off produces byte-identical final records, partials
  aside.
- **validation**: BOM, raw CR dropping, invalid UTF-8, missing `llm`/`user`,
  duplicate server name, bare `flush`, missing anthropic `max_tokens`,
  `thinking_budget >= max_tokens`.
- **state machine**: flush consumption, steering application, drop rule,
  applied against an in-process fake endpoint function.
- **mcp proxy**: expose/hide resolution and validation, schema rewrite
  determinism, inverse argument mapping on call forwarding.
- **call**: argv->record compilation vectors through the serialization seam
  - protocol mapping, absent `system`, `--header` replace, `--mcp-proxy`
  quoting and name derivation, usage errors exiting 1 - plus one
  fake-endpoint end-to-end per protocol: text on stdout, `thinking`
  dropped, trailing newline, exit 0.
- **repl**: compilation vectors through the same seam - `--prompt`
  rejected, the `options` record appended - plus scripted stdin sessions
  against the fake endpoint: multi-turn rendering, block separators and
  golden sink bytes, thinking and tool traffic, empty input, interrupt
  then continue, terminal ending exit 9, the EOF exit codes, and the
  off-tty styling fallback. `ponytail:` the raw-mode editor and the two
  ctrl-c stages at the prompt need a pty; they are a manual smoke pass,
  no in-tree pty harness.
- **terminal tools**: engine vectors through the fake endpoint and the tool
  exec seam - a terminal `tool_request` ends the run with exit 9 and no
  error record, the rest of the batch suspended `is_error`, a failed or
  timed out terminal tool still ends, steering and unflushed records
  dropped by the rules, sigint during the terminal tool exits 9 not 8;
  validation vectors (non-string entry, name missing from a connected
  listing); `call` `--terminal-tool` compilation, folding and usage
  errors; the agent invoke terminal reply and its retention under
  `retain_context`.

`ponytail:` no network integration tests in-tree; a `test/live.sh` hitting a
real endpoint is added when the first endpoint bug shows up.
