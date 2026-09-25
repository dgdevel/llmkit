# the jsonl record catalogue

One section per record type: what it is, its minimal form (required fields
only) and complete form (every field the type has) as a single line of
valid json - jsonl is oneline by definition - then a table describing
every field. Field order in the examples follows the order the runner
itself emits; json objects are unordered, any order parses.

Where each type is legal:

| type | runner stdin | agent seed | mcp-proxy config | runner stdout |
|---|---|---|---|---|
| `header` | first record | first record | first record | always first |
| `llm` | yes | yes (required) | no (fatal) | never |
| `tools` | yes | yes | yes (exactly one, required) | never |
| `options` | yes | yes | no (fatal) | never |
| `system`, `user` | yes | yes | no (fatal) | never |
| `thinking`, `response`, `tool_request`, `tool_response` | yes (replay/continuation) | yes (prefix history) | no (fatal) | yes |
| `flush` | yes | no (fatal) | no (fatal) | never |
| `start`, `error` | yes (inert) | `error` yes (inert), `start` no (fatal) | no (fatal) | yes |
| `agent-as-tool` | no (fatal, unknown type) | yes (required) | no (fatal) | never |
| `expose`, `hide` | no (fatal, unknown type) | no (fatal) | yes | never |

Unknown `type`, malformed json, missing required fields: fatal
`invalid_record` error record, the run stops.

## header

Optional marker of the transcript format version. The runner emits it as
the first record of stdout; as input it is only meaningful (and validated)
as the very first record of a stream, inert anywhere else - continuation
transcripts contain the previous run's stdout, its `header` included. A
stream without a `header` is version 1.

minimal = complete, `version` is the only field:

```
{"type":"header","version":1}
```

| field | type | required | description |
|---|---|---|---|
| `version` | number | yes | transcript format version; `1` is the only supported value, anything else (a missing field included) is a fatal `invalid_record` |

## llm

The llm endpoint configuration. A new `llm` record replaces the previous
one; `inference_options` are per record, absent means defaults - a new
`llm` record resets them.

minimal - a model-less llama.cpp style endpoint:

```
{"type":"llm","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1"}
```

complete - every field, anthropic flavor:

```
{"type":"llm","endpoint_protocol":"anthropic","api_base":"https://api.anthropic.com/v1","model":"claude-sonnet-4-5","api_key":"sk-ant-api03-REPLACE-ME","headers":{"anthropic-version":"2023-06-01"},"inference_options":{"temperature":0.7,"top_p":0.9,"max_tokens":4096,"stop":["\n\nHuman:"],"top_k":40,"thinking_budget":2048,"reasoning_effort":"high","presence_penalty":0.1,"frequency_penalty":0.2,"seed":42,"stream":true}}
```

| field | type | required | description |
|---|---|---|---|
| `endpoint_protocol` | string | yes | `openai` (chat completions), `openai_responses` (responses api) or `anthropic`; any other value is a fatal `invalid_record` |
| `api_base` | string | yes | non-empty base url including the path prefix; used verbatim, no normalization - a trailing slash is the caller's. The runner appends only the final segment: `/chat/completions`, `/responses`, `/messages` |
| `model` | string | no | model name requested from the endpoint; absent means the field is not sent at all - llama.cpp style endpoints work without it, endpoints that require it answer `api_error` |
| `api_key` | string | no | token sent as `Authorization: Bearer ...` on all protocols; must not contain CR or LF (fatal `invalid_record`). The real anthropic api wants `x-api-key` instead - target it through `headers`, as in the example above |
| `headers` | object | no | header name to string value, sent verbatim with every request; CR or LF in a name or value is a fatal `invalid_record`. An `Authorization` entry (matched case-insensitively) overrides `api_key` |
| `inference_options` | object | no | the normalized option set below; absent means defaults |

`inference_options` - the same names for every protocol, mapped per
`endpoint_protocol`. Options the current protocol does not have are
silently not sent (`-` below). Unknown names are fatal `invalid_record`.
Value ranges are not clamped; violations surface as `api_error` from the
endpoint.

| option | type | openai | openai_responses | anthropic |
|---|---|---|---|---|
| `temperature` | number | `temperature` | `temperature` | `temperature` |
| `top_p` | number | `top_p` | `top_p` | `top_p` |
| `max_tokens` | number | `max_tokens` | `max_output_tokens` | `max_tokens`, required: absent under anthropic is a fatal `invalid_record` |
| `stop` | string or array of strings | `stop` | - | `stop_sequences` |
| `top_k` | number | - | - | `top_k` |
| `thinking_budget` | number | - | - | `thinking: {type:"enabled", budget_tokens}`; its presence enables thinking, and it must be lower than `max_tokens` (else fatal `invalid_record`) |
| `reasoning_effort` | string | `reasoning_effort` | `reasoning: {effort}` | - |
| `presence_penalty` | number | `presence_penalty` | - | - |
| `frequency_penalty` | number | `frequency_penalty` | - | - |
| `seed` | number | `seed` | - | - |
| `stream` | boolean | `stream` | `stream` | `stream`; default true, false calls the endpoint without streaming - the turn then emits final records only, the same final records a streamed turn would emit |

`stop` entries must be strings; the array form is sent verbatim under both
protocols that have the option (the openai api itself caps it at 4
strings - the runner does not).

## tools

The mcp servers whose tools are offered to the model. A new `tools` record
replaces the entire list; servers no longer listed are disconnected. Tool
names exposed to the model are prefixed `server.tool`.

minimal - one stdio server:

```
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp"}]}
```

complete - one server per transport, every field:

```
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp","protocol":"2025-11-25","required":true,"terminal_tools":["read_text_file"]},{"type":"http","name":"context7","url":"https://mcp.context7.com/mcp","protocol":"2025-11-25","required":false,"headers":{"Authorization":"Bearer demo-token"}},{"type":"sse","name":"legacy","url":"https://example.invalid/mcp/sse","protocol":"2024-11-05"}]}
```

| field | type | required | description |
|---|---|---|---|
| `tools` | array | yes | the server list; entries are objects with the fields below |

Server entry fields:

| field | type | required | description |
|---|---|---|---|
| `type` | string | yes | `stdio`, `http` (streamable http) or `sse` (legacy) |
| `name` | string | yes | server name, unique within the record (a duplicate is a fatal `invalid_record`); the prefix of the tool names the model sees (`name.tool_name`) |
| `command_line` | string | stdio only | full shell command including arguments; the caller owns quoting |
| `url` | string | http/sse only | server url, used verbatim |
| `headers` | object | no | header name to string value, sent with every request to this server (http/sse); CR or LF in a name or value is a fatal `invalid_record` |
| `protocol` | string | no | mcp revision, default `2025-11-25`; accepted: the v1 revisions `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25` and the stateless v2 `2026-07-28` (no `initialize` handshake). Anything else is a fatal `invalid_record` |
| `required` | boolean | no | default false. True: a connect failure stops the conversation (fatal `connect_failed`). False: the failure is a non-fatal error record and the run continues without that server's tools |
| `terminal_tools` | array of strings | no | tool names - as the server lists them, the part after the server prefix - whose selection and use ends the conversation, see below |

`terminal_tools` rules:

- The ending triggers on execution: the call runs and is answered like
  any other, then the runner terminates - a requested ending, not an
  error: no `error` record, a dedicated exit code (9). Every not yet
  started `tool_request` of the same turn is suspended and answered with
  `is_error` true, which keeps transcripts replayable.
- Order is irrelevant and a repeated name is not an error. A non-string
  entry, or a name a connected server does not list, is a fatal
  `invalid_record` - catches typos. A server that failed to connect is
  not validated; its tools are not offered anyway.
- `llmkit mcp-proxy` accepts and ignores the attribute: the proxy has no
  conversation to end. `llmkit call --terminal-tool` and
  `llmkit agent-as-tool` seeds honor it.

## options

Execution parameters of the runner itself. Every field is optional;
absent keeps the previous value (a new `options` record merges, it does
not replace). Unknown names are fatal `invalid_record`.

minimal:

```
{"type":"options","max_tool_rounds":8}
```

complete - every field, with its default spelled out:

```
{"type":"options","max_tool_rounds":8,"tool_call_timeout":30,"stream_interval":1,"llm_connect_timeout":5,"llm_read_timeout":1200,"retain_context":false}
```

| field | type | default | description |
|---|---|---|---|
| `max_tool_rounds` | number | no limit | llm round trips before a fatal `max_tool_rounds_exceeded`; a turn counts as one round trip however many `tool_request` records it carries, and the turn that would exceed the limit is not started |
| `tool_call_timeout` | number (seconds) | no timeout | budget for one tool call; an expired call is answered with an `is_error` `tool_response` plus a non-fatal `tool_timeout` error, and the loop continues |
| `stream_interval` | number (seconds) | `1` | time between streamed partial records; chunks received within the interval are merged into one record. Fractional values are allowed, `0` emits one partial record per chunk. Partials exist only while `stream` is true |
| `llm_connect_timeout` | number (seconds) | `5` | budget to establish the connection to the llm endpoint; expiry surfaces as a fatal `connect_failed` |
| `llm_read_timeout` | number (seconds) | `1200` | budget for response data from the endpoint, first byte and between chunks alike; expiry surfaces as a fatal `http_error` |
| `retain_context` | boolean | `false` | only meaningful in an `agent-as-tool` seed (whether `invoke` keeps one growing conversation); the runner accepts and ignores it |

## system

The system prompt. Last `system` record wins; a new one replaces the
previous instructions.

minimal:

```
{"type":"system","content":[{"type":"text","text":"Answer in rhymes."}]}
```

complete - multiple blocks; anthropic sends them as N native blocks,
openai protocols join them with `\n`:

```
{"type":"system","content":[{"type":"text","text":"You are a concise unix expert."},{"type":"text","text":"Never suggest rm -rf."}]}
```

| field | type | required | description |
|---|---|---|---|
| `content` | array | yes | list of content blocks, at least one; only `text` blocks exist in this version |

Content block fields (`user` uses the same shape):

| field | type | required | description |
|---|---|---|---|
| `type` | string | yes | `text`, the only block type |
| `text` | string | yes | the block's text |

Block boundaries do not round-trip on the openai protocols: they are
joined with `\n` into one string content there.

## user

One user turn. Each `user` record after a `response` is the next user
reply; several in a row are consecutive user messages.

minimal:

```
{"type":"user","content":[{"type":"text","text":"hello"}]}
```

complete:

```
{"type":"user","content":[{"type":"text","text":"Two facts, two blocks:"},{"type":"text","text":"the sky is blue; snow is cold."}]}
```

| field | type | required | description |
|---|---|---|---|
| `content` | array | yes | list of `text` content blocks, at least one - same shape and rules as `system`'s, see above |

## thinking

A reasoning-trace block, emitted while the model thinks. On stdout it
arrives as `partial` records, then one final record closing the block. On
input (replay) partials concatenate before the final one.

minimal:

```
{"type":"thinking","text":"The user wants a directory listing."}
```

complete - a final, signed record as a transcript stores it:

```
{"type":"thinking","text":"The user wants a directory listing; I should call the fs tool.","partial":false,"signature":"EqoBCkgIBxgCKkVA7VA7"}
```

| field | type | required | description |
|---|---|---|---|
| `text` | string | yes | the reasoning trace; each streamed record carries only the text received since the previous `thinking` record of the same block |
| `partial` | boolean | no | true on every streamed record except the last one of its block, and it stays true on the last emitted record when the turn is stopped externally - trailing partials are how a transcript denotes that. On input an absent field counts as true |
| `signature` | string | no | opaque token the endpoint returns when it signs thinking blocks (the anthropic `signature`; under openai_responses the complete serialized `{type:"reasoning"}` item). On the final record of its block only - always present there, an empty string when the endpoint does not use one - and replayed verbatim |

## response

Assistant text. Final record of a turn without tool calls; final for the
conversation when no steering is pending. Text before tool calls in the
same turn also arrives as `response` records (no `usage`, no
`finish_reason` there).

minimal:

```
{"type":"response","text":"Hello! How can I help?"}
```

complete - a streamed final pair as a transcript stores it, the second
record carrying the turn totals:

```
{"type":"response","text":"The directory holds ","partial":true}
{"type":"response","text":"two files: notes.txt and report.md.","partial":false,"usage":{"input_tokens":501,"output_tokens":22},"finish_reason":"stop"}
```

| field | type | required | description |
|---|---|---|---|
| `text` | string | yes | the response text; each streamed record carries only the text received since the previous `response` record of the same block |
| `partial` | boolean | no | as `thinking`'s: true on every streamed record except the last one of its block, and on the last emitted record of an externally stopped turn; absent counts as true on input |
| `usage` | object | no | token counts, on the final record of the turn only; fields below |
| `finish_reason` | string | no | why the turn ended, on the final record of the turn only; normalized across protocols: `stop`, `length`, `content_filter`, `tool_use` (other endpoint values pass through verbatim) |

`usage` fields:

| field | type | description |
|---|---|---|
| `input_tokens` | number | tokens of the turn's request, as the endpoint reports them |
| `output_tokens` | number | tokens of the turn's generated output |

## tool_request

A tool call emitted by the model at the end of a turn, the alternative
ending to a final `response`. Emitted complete, never streamed.

minimal:

```
{"type":"tool_request","tool":"fs.list_directory","arguments":{"path":"/tmp"},"id":"call_01"}
```

complete - the last request of its turn also carries the turn totals
(`arguments` is optional, an omitted object means no arguments):

```
{"type":"tool_request","tool":"fs.read_text_file","arguments":{"path":"/tmp/notes.txt"},"id":"call_02","usage":{"input_tokens":412,"output_tokens":36},"finish_reason":"tool_use"}
```

| field | type | required | description |
|---|---|---|---|
| `tool` | string | yes | tool name as exposed to the model, prefixed with the mcp server name (`server.tool_name`) |
| `arguments` | object | no | the tool arguments; the runner always emits an `arguments` object (`{}` for no arguments), on input an omitted field means none. A non-object value is a fatal `invalid_record` |
| `id` | string | yes | unique identifier within the conversation; pairs the answering `tool_response` to this request |
| `usage` | object | no | turn totals, on the final `tool_request` of the turn only; same fields as `response`'s |
| `finish_reason` | string | no | normalized end-of-turn reason, on the final `tool_request` of the turn only; `tool_use` in practice |

## tool_response

The runner's answer to a `tool_request`, paired by `id`. Synthesized with
`is_error` true for failed, timed out or suspended calls - a request is
never left unanswered, which is what keeps transcripts replayable.

minimal:

```
{"type":"tool_response","id":"call_01","text":"notes.txt\nreport.md"}
```

complete:

```
{"type":"tool_response","id":"call_02","text":"tool not found: fs.missing_tool","is_error":true}
```

| field | type | required | description |
|---|---|---|---|
| `id` | string | yes | the `id` of the `tool_request` this record answers |
| `text` | string | yes | the payload as text; multiple text content blocks of one mcp result are joined with `\n`. A result with non-text (or no) content is a failed call: `is_error` true plus a non-fatal `tool_failed` |
| `is_error` | boolean | no | default false; the runner emits the field only when true |

## error

In-channel error report. On input it is inert, so transcripts containing
errors stay replayable.

minimal - `fatal` defaults to true:

```
{"type":"error","code":"api_error","message":"401 unauthorized"}
```

complete:

```
{"type":"error","code":"connect_failed","message":"mcp server 'fs' failed to connect","fatal":false}
```

| field | type | required | description |
|---|---|---|---|
| `code` | string | yes | machine readable code, from the catalog below |
| `message` | string | yes | human readable description |
| `fatal` | boolean | no | default true; the runner always emits the field explicitly. True stops the conversation loop and exits nonzero |

Codes (`fatal` follows the default except where noted):

| code | fatal | description |
|---|---|---|
| `invalid_record` | per case | the jsonl protocol on stdin was violated |
| `connect_failed` | yes, except non-required mcp server | mcp server or llm endpoint connection failure |
| `http_error` | yes | transport level failure talking to the llm endpoint |
| `api_error` | yes | the llm endpoint returned an application error |
| `tool_failed` | no | tool execution failed; the call is answered `is_error` true |
| `tool_timeout` | no | `tool_call_timeout` expired; same answer rule as `tool_failed` |
| `max_tool_rounds_exceeded` | yes | `max_tool_rounds` loop cap reached |
| `io_error` | yes; no in the dropped-records case | stdin failure, or records that never received a `flush` and were dropped at exit |
| `interrupted` | yes | the conversation was stopped externally (SIGINT) |

## flush

Control record: start (or resume) the conversation with all records
received so far. Integrators that keep stdin open for steering emit it;
the runner answers each consumed `flush` with a `start` marker on stdout.
No fields, minimal = complete:

```
{"type":"flush"}
```

A `flush` with no new records since the previous one is a no-op answered
with a non-fatal `invalid_record`.

## start

Marker the runner emits on stdout when a `flush` actually starts or
resumes the conversation. In input it is inert (this is what makes
transcripts replayable); it is invalid in a seed or proxy config.
No fields, minimal = complete:

```
{"type":"start"}
```

## agent-as-tool

Configures the single `invoke` tool that `llmkit agent-as-tool` exposes on
its mcp interface. Valid only in a seed file; the runner treats it as an
unknown type (fatal). Both fields are optional.

minimal:

```
{"type":"agent-as-tool"}
```

complete:

```
{"type":"agent-as-tool","tool_description":"A research tool that goes online and returns useful information","input_description":"keywords to be searched"}
```

| field | type | required | description |
|---|---|---|---|
| `tool_description` | string | no | description of the `invoke` tool offered to the model in the tool list; absent means no description is sent |
| `input_description` | string | no | description of the `invoke` tool's single `input` argument; absent means no description is sent |

## expose

Declares one upstream tool exposed by `llmkit mcp-proxy`, with its exposed
presentation. Valid only in a proxy config.

minimal - expose under the upstream name, unchanged:

```
{"type":"expose","tool":"fs.read_text_file"}
```

complete - renamed tool and renamed, redescribed argument:

```
{"type":"expose","tool":"fs.list_directory","name":"list_dir","description":"List one directory","arguments":{"path":{"name":"folder","description":"absolute folder path to list"}}}
```

| field | type | required | description |
|---|---|---|---|
| `tool` | string | yes | upstream tool selector, `server.tool` naming a server of the config's `tools` record; a server absent from it, or a tool a connected server does not list, is a fatal startup error |
| `name` | string | no | name the proxy exposes; default the `tool` selector itself. Exposed names must stay unique (fatal) |
| `description` | string | no | tool description the proxy exposes; default the upstream description |
| `arguments` | object | no | renames and description overrides for the tool's arguments; see below |

`arguments` maps upstream argument names (top level input schema
properties) to objects with both fields optional. A named argument that
is not a top level property keeps its identity. Renames carry the
schema's `required` list along; the resulting argument names must stay
unique (fatal). Calls map the names back before forwarding.

| field | type | required | description |
|---|---|---|---|
| `name` | string | no | exposed argument name, renames the upstream property |
| `description` | string | no | exposed argument description, overrides the upstream one |

## hide

Removes one upstream tool from what `llmkit mcp-proxy` exposes. Valid only
in a proxy config. Minimal = complete:

```
{"type":"hide","tool":"fs.write_file"}
```

| field | type | required | description |
|---|---|---|---|
| `tool` | string | yes | upstream tool selector, `server.tool` naming a server of the config's `tools` record; same resolution rules as `expose`'s |

Mixing `expose` and `hide` in one config is fatal: expose records form a
whitelist, hide records a blacklist, and no selection at all exposes
everything. Each upstream tool may be named by at most one `expose`/`hide`
record (fatal otherwise). Whitelist mode follows the `expose` record
order; default and blacklist mode follow the `tools` record server order,
then each server's own listing order.

The conversation rules around these records - the loop, steering, replay
constraints, the per-protocol wire mapping - are specified in
[requirements.md](requirements.md); runnable files using every record type
live in [examples/](../examples/).
