# llmkit

A single executable built for linux and cross-compiled for windows.
Multiple commands, all related to llm interaction from shell / other programs.
Support for any openai-compatible (chat completions and responses apis) and
anthropic-compatible llm endpoints.
Written in C. Third-party libraries are chosen in the design phase.

This document specifies behavior only. Design decisions — libraries, the full
exit code table, wire-level details such as anthropic `cache_control`
placement — are deliberately left out; each deferral is named where it
applies. Rules marked *(proposed)* are working desiderata, not yet frozen.

## table of contents

1. [cli conventions](#1-cli-conventions)
2. [llmkit runner](#2-llmkit-runner)
   - [i/o model](#21-io-model) · [start, continuation, replay](#22-start-continuation-replay) · [conversation loop](#23-conversation-loop) · [steering](#24-steering) · [process lifecycle](#25-process-lifecycle) · [external stop](#26-external-stop)
3. [record to api mapping](#3-record-to-api-mapping)
4. [inference options](#4-inference-options)
5. [duplicate and change semantics](#5-duplicate-and-change-semantics)
6. [prefix cache stability](#6-prefix-cache-stability)
7. [content model](#7-content-model)
8. [jsonl record types](#8-jsonl-record-types)
   - header · llm · tools · options · agent-as-tool · flush · start · system · user · thinking · tool_request · tool_response · response · error
9. [llmkit agent-as-tool](#9-llmkit-agent-as-tool)

## 1. cli conventions

Commands are subcommands: `llmkit <command>`. Built-ins: `llmkit help`,
`llmkit version`. No `--help`/`--version` flags. The full exit code table is
defined in the design phase.

## 2. llmkit runner

### 2.1 i/o model

LLM session runner. Input via stdin in jsonl format, output via stdout in
jsonl format; the stderr channel is only for out-of-channel errors.

I/O jsonl records have multiple types, distinguished by a `type` field:

```
{"type":"llm", ...}
{"type":"tools", ...}
...etc...
```

See [jsonl record types](#8-jsonl-record-types) for the input specs.

### 2.2 start, continuation, replay

- The conversation starts once stdin is closed OR once a `flush` record is
  received.
- The `flush` record is used by applications integrated to the kit that need
  to keep stdin open for steering, and works identically on linux and windows.
- Steering applies at the first turn boundary after the `flush` record; see
  [steering](#24-steering).

Continuation rule: the input with the output appended forms the new input for
a continuation, with one replacement: every `flush` record of the old input is
replaced in place by the `start` marker the runner emitted for it. A replayed
`flush` would start the conversation when it is not meant to; a replayed
`start` is inert.

Example without steering:

```
echo <jsonl_input> | llmkit > out.jsonl
```

With steering enabled from an application (pseudo code):

```
p = process.exec('llmkit runner')
p.feed(input)
p.feed('{"type":"flush"}')
... after more time ...
if p.is_running() {
    p.feed(more_input)
    p.feed('{"type":"flush"}')
}
```

### 2.3 conversation loop

The runner drives the loop between model and tools:

1. All records fed so far form the conversation sent to the llm endpoint.
2. A turn ends with either a final `response` record or one or more
   `tool_request` records. Text streamed before the tool calls is kept: it
   is emitted as `response` records of the same turn and maps as the text
   content of the same assistant message.
3. For each `tool_request`, in order:
   - The runner invokes the tool on the mcp server that provides it and emits
     the matching `tool_response` record.
   - A failed, timed out, or non-text tool is still answered: `tool_response`
     with `is_error` true, plus a non-fatal `error` record (`tool_failed` /
     `tool_timeout`); the loop continues so the model can react *(proposed)*.
   - anthropic rejects a request whose `tool_use` lacks its `tool_result`, so
     a tool request is never left unanswered.
4. After the last `tool_response` a new turn starts.
5. The loop ends when a turn ends with `response` (final answer) or with a
   fatal `error`.

Tool calls are strictly sequential, never parallel or concurrent. Loop limits
and timeouts live in the `options` record.

### 2.4 steering

Records received while a turn is in flight become steering when their `flush`
arrives; they apply at the first turn boundary. Records never flushed follow
the drop rule in [process lifecycle](#25-process-lifecycle):

- While the endpoint is streaming a turn the stream is not interrupted: the
  runner waits for the turn to end with a `response` or with `tool_request`
  records.
- While a tool is executing, the running tool completes and emits its
  `tool_response`. No further tool of the batch starts: every not yet started
  `tool_request` is suspended and answered with a `tool_response` record,
  `is_error` true, stating that the user steered the conversation.
- The steering records then append as the next user turns: the next request
  carries them together with the tool responses.

### 2.5 process lifecycle

One conversation per process; the transcript lives as long as the process.

- The conversation starts when stdin closes or when a `flush` record arrives.
- The final `response` record of a turn ends it. It is the final record of
  the conversation when the turn carries no `tool_request` and no steering
  is pending: the runner exits right after emitting it, whether stdin is
  open or closed.
- Steering is pending when records and a `flush` arrived while the turn was
  in flight; the conversation then resumes with those records, see steering.
- Records that never receive a `flush` never run: when the runner exits with
  such records pending it drops them and emits a non-fatal `error` record
  (code `io_error`) to keep the trace. The exit code stays 0: the
  conversation itself ended with a successful final `response`. `error`
  records are emitted the moment the error is detected, never buffered to the
  end, so this `io_error` precedes the final `response` record.
- Exit code 0 only when the conversation ended with a successful final
  `response`. Fatal `error` records and out-of-channel failures exit nonzero.
- An external stop (SIGINT) aborts the in-flight turn, stores the text
  received so far as trailing partial records and ends the conversation; see
  [external stop](#26-external-stop).

### 2.6 external stop

SIGINT is handled as an orderly external stop of the conversation: the runner
never dies mid-record and never discards text already received from the
endpoint. On windows the same handling is wired to the console ctrl handler
(`CTRL_C_EVENT`), so the behavior is identical on both platforms.

- While a turn is streaming, the endpoint request is aborted and the
  connection closed. Text already received but not yet emitted (still
  buffered or merged by `stream_interval`) is emitted first, as the last
  partial record of the turn. That last record keeps `partial: true`:
  trailing partial records are how a transcript denotes the external stop. A
  turn stopped externally has no final record, no `finish_reason`, no `usage`.
- With `stream` false, or when nothing was received yet, there is no partial
  text and no partial record: the turn is simply aborted.
- While a tool is executing, the running tool completes and emits its
  `tool_response`; every not yet started `tool_request` of the batch is
  suspended and answered with `is_error` true, as under steering. A
  `tool_request` is never left unanswered, so the transcript stays replayable.
- The aborted turn then ends with a fatal `error` record, code `interrupted`,
  emitted after the last record of the turn. Steering records not yet applied
  are dropped; records that never received a `flush` follow the drop rule
  above, with their non-fatal `io_error`.
- Before the conversation starts (no `flush`, stdin still open) SIGINT just
  ends the process; pending records are dropped per the drop rule above, with
  their non-fatal `io_error` records.
- The conversation is over: the runner exits nonzero, since no successful
  final `response` was emitted. SIGINT exits nonzero whatever the instant it
  happened, before or after the conversation started. Records received after
  the external stop are not read.
- A second SIGINT while the orderly stop is still running terminates the
  process immediately, with no further records *(proposed)*.
- On continuation the runner drops the unsigned `thinking` partials of the
  externally stopped turn when building requests, as on an `llm` change:
  their `signature` exists on the final record only, which never came. Under
  anthropic with thinking enabled the turn's tool records are omitted with
  them, see [record to api mapping](#3-record-to-api-mapping) *(proposed)*.

## 3. record to api mapping

Conceptual layer, identical across protocols:

- `system` maps to the system role
- `user` maps to the user role
- `response` maps to the assistant role

Wire layer, per `endpoint_protocol`:

| record | openai | openai_responses | anthropic |
|---|---|---|---|
| `system` | message with role `system` inside `messages` | top level `instructions` parameter, never inside `input` | top level `system` parameter, never inside `messages` |
| `user` | message role `user` | input item with role `user`, string content | message role `user`, text content block |
| `response` | assistant message `content` | input item `{type:"message", role:"assistant"}` with `output_text` content | assistant message `{type:"text"}` content block |
| `thinking` | not resent: reasoning lives server side; emitted as record only when the endpoint returns a summary | `{type:"reasoning"}` item resent verbatim, its id and encrypted content carried by `signature`, when that turn ended in tool requests | `{type:"thinking"}` content block inside the assistant message, resent verbatim with `signature` when that turn ended in tool requests |
| `tool_request` | entry of the `tool_calls` array on the assistant message, `id` becomes `tool_call_id` | `{type:"function_call", call_id, name, arguments}` output item of the assistant turn | `{type:"tool_use", id, name, input}` content block inside the assistant message |
| `tool_response` | own message role `tool`, carries `tool_call_id` | `{type:"function_call_output", call_id, output}` input item | message role `user` holding `{type:"tool_result", tool_use_id}` content blocks |

`llm`, `tools`, `options`, `flush`, `start`, `header` and `error` records are
never sent to the endpoint.

Grouping rule: one turn is `thinking`?, `response`?, then either a final
`response` or one or more `tool_request` records. Partial records of one turn
concatenate into the full text before mapping. All records of one turn form a
single assistant message: its text records become the text content, N
`tool_request` records become N `tool_calls` entries (openai), N
`function_call` items (openai_responses) or N `tool_use` blocks (anthropic). The matching `tool_response` records follow in the same
order, paired by `id`.

Replay constraints:

- anthropic requires the first message to have role `user`; a transcript that
  would start otherwise is rejected with an `error` record, code
  `invalid_record`, before any request is sent — at conversation start and
  again whenever a request is built after a mid conversation `llm` switch to
  anthropic.
- anthropic requires `thinking` blocks with valid `signature` to be resent
  whenever the following message carries `tool_result` blocks.
- Under anthropic with thinking enabled, the assistant turn carrying
  `tool_use` must carry signed `thinking`. When that turn's `thinking`
  records lack a valid `signature` (external stop, `llm` change), the runner
  omits that turn's `tool_request` records and their `tool_response` records
  from the request; the transcript is unchanged.
- Consecutive same role records are sent as consecutive same role messages:
  no protocol requires merging them, so the runner does not merge
  *(proposed)*.

## 4. inference options

Normalized option set inside `inference_options`, mapped per
`endpoint_protocol`:

| option | openai | openai_responses | anthropic |
|---|---|---|---|
| `temperature` | `temperature` | `temperature` | `temperature` |
| `top_p` | `top_p` | `top_p` | `top_p` |
| `max_tokens` | `max_tokens` (deprecated upstream, kept for compatibility with openai-compatible servers) | `max_output_tokens` | `max_tokens`, required: absent under anthropic is `invalid_record`, fatal |
| `stop` | `stop`, at most 4 strings | not sent | `stop_sequences` |
| `top_k` | not sent | not sent | `top_k` |
| `thinking_budget` | not sent | not sent | `thinking: {type:"enabled", budget_tokens}`; its presence enables thinking |
| `reasoning_effort` | `reasoning_effort` | `reasoning: {effort}` | not sent |
| `presence_penalty` | `presence_penalty` | not sent | not sent |
| `frequency_penalty` | `frequency_penalty` | not sent | not sent |
| `seed` | `seed` | not sent | not sent |
| `stream` | `stream` | `stream` | `stream` |

Rules *(proposed)*:

- `stream` defaults to true; when false the endpoint is called without
  streaming and the turn emits final records only, no partials.
- Unknown option names produce an `error` record with code `invalid_record`,
  fatal. Catches typos.
- Options that exist in the set but not for the current `endpoint_protocol`
  are silently not sent.
- No arbitrary key pass-through: strict openai-compatible endpoints reject
  unknown body fields. `repeat_penalty` and other ollama/llama.cpp style
  parameters are deliberately out; each addition is an explicit spec change.
- The runner does not clamp value ranges (openai temperature 0..2, anthropic
  0..1); range violations surface as `api_error` from the endpoint.
- `thinking_budget` on anthropic requires `max_tokens` greater than the
  budget; the runner rejects the combination with `invalid_record` before
  sending.

## 5. duplicate and change semantics

Config records are snapshots of runner state, never part of the transcript
sent to the endpoint:

- `llm`: a new `llm` record replaces the previous one. From the next turn on,
  the whole transcript so far is sent to the new endpoint. Records already
  emitted on stdout stay unchanged. Mid conversation endpoint switch is
  therefore supported. `inference_options` are per `llm` record: absent means
  defaults, a new `llm` record resets them.
- `tools` *(proposed)*: a new `tools` record replaces the entire server list;
  servers no longer listed are disconnected; servers added mid conversation
  connect at the boundary where the record applies, connection rules as at
  conversation start.
- `options` *(proposed)*: fields present in a new `options` record override
  the previous value, absent fields keep it.
- `system` *(proposed)*: last `system` record wins and replaces the previous
  instructions.

Transcript records:

- `user`: appends a new turn. Multiple `user` records are normal: each one
  after a `response` is the next user reply, typed in the external
  application UI.
- `thinking` *(proposed)*: on an `llm` change the runner drops earlier
  `thinking` records when building requests, since signatures are only valid
  for the model that produced them.

Change timing: config record changes apply at the first turn boundary at or
after the record.

Validation: unknown `type`, malformed json line, or missing required fields
produce an `error` record with code `invalid_record`, fatal.

## 6. prefix cache stability

Both protocols cache the request prefix and bill cache hits far below full
input price; the runner must never be the cause of an avoidable miss.

- Append only: a continuation request is the previous request with the new
  turns appended. Records already mapped once are never re-serialized
  differently; the message prefix of turn N is byte identical to the request
  of turn N-1, save for per-request envelope fields the protocol requires
  outside the message array (`max_tokens` and the like).
- Deterministic serialization: the same records always map to the same wire
  bytes, independent of timing, streaming chunk boundaries and
  `stream_interval` grouping. Partial records exist only on stdout; the
  endpoint receives the concatenated full text, so grouping never leaks into
  the request.
- No volatile injection: the runner never adds timestamps, counters, request
  ids or any per-request varying data to `system`, the tool list or the
  messages.
- Stable tool order: the tools array is built deterministically from the
  `tools` record and the servers' own tool listings, and changes only when a
  `tools` record changes it.
- Invalidation boundaries: only prefix-affecting changes invalidate, and only
  from the turn they apply at (a new `system`, `tools` or `llm` record, or
  transcript edits). Sampling-only changes (`temperature`, `top_p`, ...) must
  leave the serialized prefix untouched.
- Cache scope is per endpoint: an `llm` change resets the expectation, the
  transcript then rebuilds cache on the new endpoint.
- anthropic `cache_control` breakpoints and openai automatic caching
  thresholds (both openai protocols) are design phase; this section demands
  only that the runner itself never causes a miss.

## 7. content model

- Text only in this version: no images, no other multimodal blocks. mcp tool
  results carrying non-text content blocks are unsupported: the call is
  answered as a failed tool (`tool_failed`, non-fatal).
- UTF-8 everywhere, enforced: a record containing invalid UTF-8 produces an
  `error` record with code `invalid_record`, fatal.
- Newlines are `\n` only, identical on linux and windows: carriage returns
  received on stdin are silently dropped, records on stdout are terminated
  with `\n` and never `\r\n`.
- No bom: a leading bom is not stripped, it makes the first line malformed
  json (`invalid_record`, fatal).
- Caller authored records (`system`, `user`) carry a `content` field: a list
  of content blocks. Block shape: `{type:"text", text:"..."}`. Only `text`
  blocks exist in this version.
- Runner emitted records (`thinking`, `response`, `tool_response`) keep a
  flat `text` string *(proposed: simpler output; moving them to content
  blocks later is additive and breaks nothing)*.

## 8. jsonl record types

### header

Optional marker of the transcript format version. The runner emits
`{"type":"header","version":1}` as the first record of stdout.

| field | required | description |
|---|---|---|
| `version` | yes | integer, transcript format version; this document is version 1 |

- Meaningful only as the first record of a stream (runner stdin or agent
  seed): there it is validated, and a `version` the runner does not support
  is `invalid_record`, fatal. A stream without a `header` is version 1.
- Anywhere else it is inert, recognized and skipped: continuation transcripts
  contain the previous run's stdout, its `header` included.
- Never sent to the endpoint.

### llm

A record holding the configuration of the llm endpoint.

| field | required | description |
|---|---|---|
| `endpoint_protocol` | yes | `openai` (chat completions api), `openai_responses` (responses api) or `anthropic` |
| `api_base` | yes | http or https url of the base address, path prefix included (e.g. `.../v1`); the runner appends only the final segment: `/chat/completions` for openai, `/responses` for openai_responses, `/messages` for anthropic; used verbatim, no normalization: a trailing slash is the caller's, the segment is appended as-is |
| `model` | no | name of the model requested to the endpoint; when absent the field is simply not sent, llama.cpp style endpoints do not require it, endpoints that do answer with `api_error` |
| `api_key` | no | token for authentication, passed verbatim, no environment variable expansion; the caller already owns the secret and feeds it through stdin; sent as `Authorization: Bearer <api_key>` on all protocols, a `headers` entry setting `Authorization` overrides it. The real anthropic api wants `x-api-key` and requires `anthropic-version`: callers target it through `headers`, e.g. `{"x-api-key":"...","anthropic-version":"2023-06-01"}` |
| `inference_options` | no | sampling and inference parameters, normalized set, see [inference options](#4-inference-options) |
| `headers` | no | object of header name to value, sent with every request to the endpoint |

### tools

A record holding the list of mcp tools offered to the model to be used. The
`tools` attribute is a list of server records.

Common fields:

| field | required | description |
|---|---|---|
| `type` | yes | `stdio`, `http` or `sse` |
| `name` | yes | unique name of the mcp server, uniqueness enforced (`invalid_record`, fatal); used as prefix for the tool names exposed to the model (`name.tool_name`) |
| `protocol` | no | mcp protocol revision: `2025-11-25` (v1) by default, `2026-07-28` (v2) supported |
| `required` | no | boolean, signals if a failure to connect and offer the tool to the model is a deal breaker and should stop processing the conversation instead |

Fields per `type`:

- `stdio`: `command_line` (required) — full command invocation, including
  arguments, executed through the system shell; the caller owns quoting,
  linux and windows shell differences are accepted as-is.
- `http` / `sse`: `url` (required), with optional `headers` object of header
  name to value.

Connection rules:

- All mcp servers connect at conversation start, before the first turn, and
  only after the runner validated the presence of an `llm` record and of at
  least one `user` record: either missing is a protocol error
  (`invalid_record`, fatal), the conversation never starts and nothing
  connects or executes.
- A non required server that fails to connect emits a non-fatal
  `connect_failed` error record and the conversation continues without its
  tools.
- stdio servers run as child processes spawned by the runner.

### options

A record holding execution parameters for the runner.

| field | description |
|---|---|
| `max_tool_rounds` | maximum number of llm round trips before the conversation is stopped with a fatal `max_tool_rounds_exceeded` error record; a turn counts as one round trip however many `tool_request` records it carries; the turn that would exceed the limit is not started; absent means no limit |
| `tool_call_timeout` | timeout in seconds for a single tool call; on expiry the call is aborted and answered with a `tool_response`, `is_error` true, plus a non-fatal `tool_timeout` error record *(proposed)*; absent means no timeout |
| `stream_interval` | seconds between emitted partial records during streaming, default 1, fractional values allowed (e.g. 0.05); chunks received within the interval are merged into one record; 0 disables grouping and every chunk becomes its own partial record; partial records only exist while `stream` is true |
| `llm_connect_timeout` | seconds allowed to establish the connection to the llm endpoint, default 5; expiry surfaces as a fatal `connect_failed` error record |
| `llm_read_timeout` | seconds the runner waits for response data from the endpoint, first byte and between chunks alike, default 1200 (20 minutes: a local llm on a slow machine can think for a long time before the first token); expiry surfaces as a fatal `http_error` |
| `retain_context` | boolean, default false; meaningful only in an `llmkit agent-as-tool` seed, see the invoke tool; the runner accepts and ignores it |

### agent-as-tool

Configures the `invoke` tool that `llmkit agent-as-tool` exposes on its mcp
interface. Valid only in a seed file: the runner treats it as an unknown
`type` (`invalid_record`, fatal).

| field | description |
|---|---|
| `tool_description` | description of the `invoke` tool offered to the model in the tool list |
| `input_description` | description of the `input` argument offered to the model |

Example:

```
{"type":"agent-as-tool", "tool_description":"A research tool that will go online and provide useful information", "input_description":"keywords to be searched"}
```

### flush

Control record: start or resume the conversation using all records received
so far. No fields.

- Emitted by integrators that keep stdin open for steering; replaces any
  signal based trigger so linux and windows behave identically.
- A `flush` always attempts to start or resume the conversation. It is
  *consumed* when it actually does so, at the turn boundary where it applies,
  and each consumed `flush` makes the runner emit a `start` marker record on
  stdout at that moment, one marker per consumed `flush`.
- A `flush` with no new records since the previous one is a no-op: it never
  starts or resumes anything, never marks steering pending — including when
  it arrives while a turn is in flight — emits no `start` marker, and emits a
  non-fatal `invalid_record` error record.
- The no-op rule needs a previous `flush` to compare with: the very first
  `flush` always attempts to start. With no records at all the start fails
  validation (missing `llm` record, `invalid_record`, fatal) — a conversation
  that cannot start is fatal, a minor issue during a running one is not.
- On continuation a no-op `flush` has no marker and is simply dropped: the
  replacement rule maps every `flush` to the marker it produced, none in this
  case.

### start

Marker record emitted on stdout each time a `flush` starts or resumes the
conversation. No fields.

In input a `start` record is inert: recognized, skipped, it never starts
anything. That is what makes transcripts replayable, see the continuation
rule in [start, continuation, replay](#22-start-continuation-replay).

### system

The system/developer instructions text (e.g. system prompt).

| field | description |
|---|---|
| `content` | list of content blocks, see [content model](#7-content-model) |

### user

The user input text.

| field | description |
|---|---|
| `content` | list of content blocks, see [content model](#7-content-model) |

### thinking

A block holding the thinking trace emitted during inference.

| field | description |
|---|---|
| `text` | the reasoning trace; during streaming each record carries only the text received since the previous `thinking` record of the same turn |
| `partial` | boolean flag set on every streamed record except the last one of the turn; it stays set on the last emitted record when the turn is stopped externally (SIGINT), see [external stop](#26-external-stop) |
| `signature` | opaque token returned by the endpoint when it requires signed thinking blocks (the anthropic `signature`; the openai_responses reasoning item id and encrypted content); replayed verbatim on continuation, empty when the endpoint does not use one; present on the final record of the turn only |

### tool_request

The tool invocation request emitted by the llm at the end of a turn,
alternative to the `response`. Emitted complete, never streamed in parts
*(proposed)*.

| field | description |
|---|---|
| `tool` | tool name as exposed to the model, prefixed with the mcp server name (`name.tool_name`) |
| `arguments` | json object with the tool arguments |
| `id` | unique identifier within the conversation, pairs the response to the request |
| `usage` | optional object with token counts (`input_tokens`, `output_tokens`) when the endpoint reports them; on the final `tool_request` of the turn only |
| `finish_reason` | optional, why the turn ended, from the normalized set of `response`; on the final `tool_request` of the turn only |

### tool_response

The tool response content, emitted by the runner after executing a
`tool_request`, or synthesized for a suspended, failed or externally stopped
one (see [steering](#24-steering), [external stop](#26-external-stop)).

| field | description |
|---|---|
| `id` | identifier of the `tool_request` this record answers |
| `text` | response payload as text; multiple text content blocks of one mcp result are concatenated with `\n` |
| `is_error` | optional boolean, default false, marks a failed tool execution |

### response

The llm response text; final record of a turn without `tool_request`
records, final for the conversation when no steering is pending. A turn that
ends in `tool_request` records may carry `response` records before them: they
hold the turn's text and carry no `usage` and no `finish_reason`.

| field | description |
|---|---|
| `text` | the response text; during streaming each record carries only the text received since the previous `response` record of the same turn |
| `partial` | boolean flag set on every streamed record except the last one of the turn; it stays set on the last emitted record when the turn is stopped externally (SIGINT), see [external stop](#26-external-stop) |
| `usage` | optional object with token counts (`input_tokens`, `output_tokens`) when the endpoint reports them; on the final record of the turn only |
| `finish_reason` | optional, why the turn ended, from the normalized set below; on the final record only |

Normalized `finish_reason`, identical across protocols so a transcript
replays across them:

| normalized | openai | openai_responses | anthropic |
|---|---|---|---|
| `stop` | `stop` | status `completed`, no function calls | `end_turn`, `stop_sequence` |
| `length` | `length` | status `incomplete`, `incomplete_details` `max_output_tokens` | `max_tokens` |
| `content_filter` | `content_filter` | status `incomplete`, `incomplete_details` `content_filter` | `refusal` |
| `tool_use` | `tool_calls`, `function_call` | status `completed` with `function_call` items in the output | `tool_use` |

Values outside the table pass through verbatim, nothing is dropped.

Streaming rule: `thinking` and `response` arrive from the endpoint in chunks.
The runner merges chunks received within `stream_interval` into one partial
record. Consecutive partial records plus the final record of one turn
concatenate to the full text; the transcript invariant stays one logical
response text per turn when replayed. A turn stopped externally (SIGINT) ends on its
trailing `partial` records with no final record; replay concatenates them
like any partials, the invariant applies to complete turns only.

### error

Errors in processing, of any type, are stored in their own record type.

| field | description |
|---|---|
| `code` | machine readable error code, from the catalog below |
| `message` | human readable description |
| `fatal` | boolean, true stops the conversation loop and makes the runner exit with a nonzero status; fatal is the default, the non-fatal cases are named in the catalog |

Error catalog (initial list). Fatality follows the `fatal` default unless the
catalog states otherwise:

| code | fatal | description |
|---|---|---|
| `invalid_record` | per case | the llmkit jsonl protocol on stdin was violated; every case is listed below |
| `connect_failed` | fatal, except non required mcp server | mcp server or llm endpoint connection failure; non-fatal for a non required mcp server, the conversation continues without its tools |
| `http_error` | fatal (default) | transport level failure talking to the llm endpoint |
| `api_error` | fatal (default) | llm endpoint returned an application error |
| `tool_failed` | non-fatal *(proposed)* | tool execution returned an error; the failed call is answered with a `tool_response`, `is_error` true |
| `tool_timeout` | non-fatal *(proposed)* | `tool_call_timeout` expired; same answer rule as `tool_failed` |
| `max_tool_rounds_exceeded` | fatal (default) | loop cap reached |
| `io_error` | fatal (default); non-fatal in the pending-records drop case | stdin failure, reported as an error record; a stdout failure is out of channel, the runner exits nonzero without emitting records |
| `interrupted` | fatal | the conversation was stopped externally (SIGINT, see external stop) |

`invalid_record` cases:

- Malformed json line (fatal).
- Unknown `type` (fatal).
- Missing required fields (fatal).
- Invalid UTF-8 (fatal).
- Unknown option name (fatal).
- `thinking_budget` not lower than `max_tokens` (fatal).
- Missing `max_tokens` under anthropic (fatal).
- Missing `llm` record at conversation start (fatal).
- No `user` record in the transcript (fatal).
- Duplicate server `name` in a `tools` record (fatal).
- Transcript that would start with a non-user message under anthropic
  (fatal).
- Unsupported `header` `version` (fatal).
- Bare `flush`, no new records since the previous one (non-fatal, no-op).

On input an `error` record is inert *(proposed)*: recognized, skipped, so
transcripts containing error records stay replayable.

Boundary rule: every error that belongs to the conversation becomes an error
record on stdout, stdin failures included. stderr carries only out-of-channel
errors (runner startup failures, stdout failures), which exit without
emitting records.

## 9. llmkit agent-as-tool

Second command: a stdio mcp server exposing one agent conversation as a
single tool. Launched as `llmkit agent-as-tool <seed.jsonl>`, normally
spawned by a parent runner through a `tools` record `command_line`. Speaks
the mcp revisions the `tools` record defines: `2025-11-25` (v1) default,
`2026-07-28` (v2) supported.

The seed file is jsonl, same record format as the runner stdin, and forms the
fixed prefix of every conversation:

- Required: `llm` and `agent-as-tool`. Optional: `header` (as the first
  record), `tools`, `options`, `system`, and transcript records as prefix
  history.
- `flush` and `start` are control records for live stdin and are invalid in a
  seed (`invalid_record`, fatal). `error` records and non-first `header`
  records are inert on input and legal in a seed.
- At startup the seed is validated — same record validation as the runner's
  input, minus the `user` requirement, which each `invoke` brings with it —
  and the agent's own mcp servers connect, same connection rules as the
  runner's conversation start. No conversation runs yet.
- Fatal seed or startup errors are out of channel: reported on stderr, no
  records emitted, the process exits nonzero. After bootstrap stdout belongs
  to the mcp channel and carries no records.

### the invoke tool

The server exposes exactly one tool, `invoke`, with exactly one string
argument `input`; their descriptions come from the `agent-as-tool` record.

- Each call runs one conversation — the seed plus a `user` record carrying
  `input` — under the runner's conversation loop, mapping, options and error
  rules, unchanged.
- `retain_context` false (default): no state is kept between calls, each
  `invoke` is an independent conversation, the seed stays a stable prefix and
  cache hits accumulate across invokes (see
  [prefix cache stability](#6-prefix-cache-stability)).
- `retain_context` true: the agent keeps one growing conversation for the
  life of the process; each `invoke` appends its `user` record and its turns
  to the accumulated transcript. Only conversations ended by a final
  `response` are retained: a fatal `error` rolls the transcript back to the
  last successful `invoke`, the bare seed if none. Either way the transcript
  stays append only and cache hits keep accumulating.
- The reply is the concatenated text of the final `response` record, nothing
  else: no partial records, no `thinking`, no intermediate tool traffic
  exists on this interface.
- A conversation that ends in a fatal `error` instead of a final `response`
  is returned as a failed tool call carrying the code and message.
- Nesting is allowed: an agent seed may list other `agent-as-tool` servers
  among its `tools`, each as its own process. A seed must not list itself,
  directly or indirectly: that is a spawn cycle and the kit does not detect
  it in this version.
