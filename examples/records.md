# the jsonl record catalogue

One section per record type. Each example is a single line of valid json -
jsonl is oneline by definition - with the minimal form (required fields
only) and the complete form (every field the type has). Field order in the
examples follows the order the runner itself emits; json objects are
unordered, any order parses.

Where each type is legal:

| type | runner stdin | agent seed | mcp-proxy config | runner stdout |
|---|---|---|---|---|
| `header` | first record | first record | first record | always first |
| `llm` | yes | yes (required) | no | never |
| `tools` | yes | yes | yes (exactly one, required) | never |
| `options` | yes | yes | no | never |
| `system`, `user` | yes | yes | no | never |
| `thinking`, `response`, `tool_request`, `tool_response` | yes (replay/continuation) | yes (prefix history) | no | yes |
| `flush` | yes | no (fatal) | no (fatal) | never |
| `start`, `error` | yes (inert) | `error` yes (inert), `start` no (fatal) | no (fatal) | yes |
| `agent-as-tool` | no (fatal, unknown type) | yes (required) | no (fatal) | never |
| `expose`, `hide` | no (fatal, unknown type) | no (fatal) | yes | never |

Unknown `type`, malformed json, missing required fields: fatal
`invalid_record` error record, the run stops.

## header

Optional marker of the transcript format version. The runner emits it as
the first record of stdout; as input it is only meaningful (and validated)
as the very first record of a stream, inert anywhere else.

minimal = complete, `version` is the only field:

```
{"type":"header","version":1}
```

## llm

The llm endpoint configuration. A new `llm` record replaces the previous
one; `inference_options` are per record, absent means defaults.

minimal - a model-less llama.cpp style endpoint:

```
{"type":"llm","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1"}
```

complete - every field, anthropic flavor:

```
{"type":"llm","endpoint_protocol":"anthropic","api_base":"https://api.anthropic.com/v1","model":"claude-sonnet-4-5","api_key":"sk-ant-api03-REPLACE-ME","headers":{"anthropic-version":"2023-06-01"},"inference_options":{"temperature":0.7,"top_p":0.9,"max_tokens":4096,"stop":["\n\nHuman:"],"top_k":40,"thinking_budget":2048,"reasoning_effort":"high","presence_penalty":0.1,"frequency_penalty":0.2,"seed":42,"stream":true}}
```

- `endpoint_protocol`: `openai` (chat completions), `openai_responses`
  (responses api) or `anthropic`.
- `api_base`: base url including the path prefix; the runner appends only
  the final segment (`/chat/completions`, `/responses`, `/messages`).
- `api_key`: sent as `Authorization: Bearer ...` on all protocols. The real
  anthropic api wants `x-api-key` instead - target it through `headers`,
  as in the example above.
- `headers`: sent verbatim with every request; an `Authorization` entry
  overrides `api_key`.
- `inference_options`: the normalized option set, the same names for every
  protocol; options the current protocol does not have are silently not
  sent (`reasoning_effort` is not sent under anthropic, `top_k` is not sent
  under openai). Unknown names are fatal. Under anthropic `max_tokens` is
  required and `thinking_budget` must be lower than it.

## tools

The mcp servers whose tools are offered to the model. A new `tools` record
replaces the entire list. Tool names exposed to the model are prefixed
`server.tool`.

minimal - one stdio server:

```
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp"}]}
```

complete - one server per transport, every field:

```
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp","protocol":"2025-11-25","required":true},{"type":"http","name":"context7","url":"https://mcp.context7.com/mcp","protocol":"2025-11-25","required":false,"headers":{"Authorization":"Bearer demo-token"}},{"type":"sse","name":"legacy","url":"https://example.invalid/mcp/sse","protocol":"2024-11-05"}]}
```

- `type`: `stdio`, `http` (streamable http) or `sse` (legacy).
- `name`: unique per record (`invalid_record` if duplicated).
- `command_line` (stdio, required): full shell command, the caller owns
  quoting. `url` (http/sse, required), with optional `headers`.
- `protocol`: mcp revision, default `2025-11-25`; any published v1 revision
  from `2024-11-05` to `2025-11-25`, or v2 `2026-07-28` (stateless, no
  `initialize` handshake).
- `required`: when true, a connect failure stops the conversation
  (fatal `connect_failed`); when false the failure is a non-fatal error
  record and the run continues without that server's tools.

## options

Execution parameters of the runner itself. Every field is optional;
absent keeps the previous value (a new `options` record merges, it does
not replace).

minimal:

```
{"type":"options","max_tool_rounds":8}
```

complete - every field, with its default spelled out:

```
{"type":"options","max_tool_rounds":8,"tool_call_timeout":30,"stream_interval":1,"llm_connect_timeout":5,"llm_read_timeout":1200,"retain_context":false}
```

- `max_tool_rounds`: llm round trips before a fatal
  `max_tool_rounds_exceeded`; absent means no limit.
- `tool_call_timeout`: seconds for one tool call; expired calls are
  answered with an `is_error` `tool_response` plus a non-fatal
  `tool_timeout` error.
- `stream_interval`: seconds between streamed partial records, default 1;
  `0` emits one partial record per chunk.
- `llm_connect_timeout` / `llm_read_timeout`: endpoint connect / read
  budgets in seconds.
- `retain_context`: only meaningful in an `agent-as-tool` seed; the runner
  accepts and ignores it.

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

## thinking

A reasoning-trace block, emitted while the model thinks. On stdout it
arrives as `partial` records, then one final record carrying `signature`
(when the endpoint signs thinking - anthropic, openai_responses). On input
(replay) partials concatenate before the final one.

minimal:

```
{"type":"thinking","text":"The user wants a directory listing."}
```

complete - a final, signed record as a transcript stores it:

```
{"type":"thinking","text":"The user wants a directory listing; I should call the fs tool.","partial":false,"signature":"EqoBCkgIBxgCKkKVA7VA7"}
```

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

`finish_reason` is normalized across protocols: `stop`, `length`,
`content_filter`, `tool_use` (other endpoint values pass through verbatim).

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

Codes: `invalid_record`, `connect_failed`, `http_error`, `api_error`,
`tool_failed` (non-fatal), `tool_timeout` (non-fatal),
`max_tool_rounds_exceeded`, `io_error`, `interrupted`.

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

`arguments` maps upstream argument names (top level schema properties) to
`{name, description}`, both fields optional.

## hide

Removes one upstream tool from what `llmkit mcp-proxy` exposes. Valid only
in a proxy config. Minimal = complete:

```
{"type":"hide","tool":"fs.write_file"}
```

Mixing `expose` and `hide` in one config is fatal: expose records form a
whitelist, hide records a blacklist, and no selection at all exposes
everything.
