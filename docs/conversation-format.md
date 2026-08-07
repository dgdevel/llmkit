# Conversation JSONL Format

LLMKIT records every agent conversation as a **JSONL** (JSON Lines) file — one JSON object per line, terminated by `\n`. Each line is a standalone **entry** typed by a `"type"` field.

The file path is set with `--conversation <file>`; the agent appends to it and
continues prior turns. When `--conversation` is omitted, the agent runs against a
temporary file (in the system temp directory) that is deleted when the run ends,
so the conversation is discarded — useful for one-shot prompts. In both cases the
on-disk format is identical.

```plaintext
{"type":"meta","timestamp":"...","version":1,"config_hash":"sha256:...","run_id":"..."}
{"type":"user","timestamp":"...","content":"What is the weather?","source":"cli"}
{"type":"assistant","timestamp":"...","content":"Let me check...","model":"gpt-4o","usage":{...}}
{"type":"tool_call","timestamp":"...","id":"call_abc","name":"weather_get","arguments":"{\"city\":\"London\"}","mcp_server":"weather-srv"}
{"type":"tool_result","timestamp":"...","call_id":"call_abc","name":"weather_get","result":"{\"temp\":15}","is_error":false,"is_timeout":false,"mcp_server":"weather-srv"}
```

## Entry types

### `meta` — conversation header

The **first line** of every conversation file. Written once at session start.

| Field         | Type   | Description                                       |
|---------------|--------|---------------------------------------------------|
| `type`        | string | Always `"meta"`                                   |
| `timestamp`   | string | ISO 8601 UTC timestamp                            |
| `version`     | number | Schema version (currently `1`)                    |
| `config_hash` | string | SHA-256 hex digest of the resolved YAML config    |
| `run_id`      | string | UUID v4 identifying this agent session            |

```json
{"type":"meta","timestamp":"2026-07-27T14:30:00Z","version":1,"config_hash":"sha256:a1b2c3...","run_id":"550e8400-e29b-41d4-a716-446655440000"}
```

### `user` — user message

A prompt or question from the user.

| Field       | Type   | Description                              |
|-------------|--------|------------------------------------------|
| `type`      | string | Always `"user"`                          |
| `timestamp` | string | ISO 8601 UTC timestamp                   |
| `content`   | string | The prompt text                          |
| `source`    | string | `"cli"` if passed on the command line, `"file"` if read from a file, `"steer"` if injected via the `--steer` stdin channel mid-run |

```json
{"type":"user","timestamp":"2026-07-27T14:30:01Z","content":"What is the weather in London?","source":"cli"}
```

### `assistant` — LLM response

An assistant reply from the LLM. May be followed by `tool_call` and `tool_result` entries (see tool-call grouping below).

| Field       | Type              | Description                                      |
|-------------|-------------------|--------------------------------------------------|
| `type`      | string            | Always `"assistant"`                             |
| `timestamp` | string            | ISO 8601 UTC timestamp                           |
| `content`   | string            | Response text (may be empty if only tool calls)  |
| `reasoning` | string (optional) | Model chain-of-thought, present only for reasoning-capable models and when non-empty |
| `model`     | string            | Model identifier returned by the API, e.g. `"gpt-4o"` |
| `usage`     | object (optional) | Token usage, present when the API provides it    |

**`reasoning` field:**

Some models (e.g. DeepSeek-R1, Qwen3-thinking, OpenAI o-series) return a
`reasoning_content` field alongside `content`. llmkit captures this and stores
it as the `reasoning` field on the assistant entry. It is omitted entirely
(absent from the JSON) when the model returns no reasoning or an empty string.

By default, reasoning is **not** re-sent to the model on subsequent turns.
To retain it in the reconstructed request history, set `retain_reasoning: true`
in the `llm` config (see `docs/configuration.md`).

**`usage` sub-object:**

| Field              | Type   | Description           |
|--------------------|--------|-----------------------|
| `prompt_tokens`    | number | Input tokens          |
| `completion_tokens`| number | Output tokens         |
| `total_tokens`     | number | Sum of the two above  |

```json
{"type":"assistant","timestamp":"2026-07-27T14:30:05Z","content":"The weather in London is 15°C and cloudy.","model":"gpt-4o","usage":{"prompt_tokens":45,"completion_tokens":12,"total_tokens":57}}
```

A response that only makes tool calls (no text):

```json
{"type":"assistant","timestamp":"2026-07-27T14:30:05Z","content":"","model":"gpt-4o","usage":{"prompt_tokens":50,"completion_tokens":8,"total_tokens":58}}
```

### `tool_call` — LLM tool invocation

Each tool call the LLM requests. Always appears immediately after the `assistant` entry that produced it.

| Field       | Type   | Description                                    |
|-------------|--------|------------------------------------------------|
| `type`      | string | Always `"tool_call"`                           |
| `timestamp` | string | ISO 8601 UTC timestamp                         |
| `id`        | string | Unique call identifier (from the LLM API)      |
| `name`      | string | Namespaced tool name, e.g. `"weather.get"`    |
| `arguments` | string | JSON string of the tool arguments              |
| `mcp_server`| string | MCP server that provides this tool             |

```json
{"type":"tool_call","timestamp":"2026-07-27T14:30:05Z","id":"call_abc","name":"weather.get","arguments":"{\"city\":\"London\"}","mcp_server":"weather-srv"}
```

### `tool_result` — tool execution outcome

The result of executing a tool call on the MCP backend.

| Field        | Type    | Description                                         |
|--------------|---------|-----------------------------------------------------|
| `type`       | string  | Always `"tool_result"`                              |
| `timestamp`  | string  | ISO 8601 UTC timestamp                              |
| `call_id`    | string  | Matches the `id` of the originating `tool_call`     |
| `name`       | string  | Namespaced tool name                                |
| `result`     | string  | JSON string of the tool's output (or error text)    |
| `is_error`   | boolean | `true` if the tool returned an error                |
| `is_timeout` | boolean | `true` if the tool call timed out                   |
| `mcp_server` | string  | MCP server that executed the call                   |

Successful result:
```json
{"type":"tool_result","timestamp":"2026-07-27T14:30:06Z","call_id":"call_abc","name":"weather.get","result":"{\"temp\":15,\"condition\":\"cloudy\"}","is_error":false,"is_timeout":false,"mcp_server":"weather-srv"}
```

Error result:
```json
{"type":"tool_result","timestamp":"2026-07-27T14:30:06Z","call_id":"call_def","name":"db.query","result":"Connection refused","is_error":true,"is_timeout":false,"mcp_server":"db-srv"}
```

Timeout result:
```json
{"type":"tool_result","timestamp":"2026-07-27T14:30:06Z","call_id":"call_ghi","name":"slow.search","result":"Tool call timed out","is_error":true,"is_timeout":true,"mcp_server":"search-srv"}
```

### `error` — session error

Runtime errors that occur during the agent run. These entries are **not** part of the LLM message history and are skipped during conversation reconstruction.

| Field        | Type    | Description                              |
|--------------|---------|------------------------------------------|
| `type`       | string  | Always `"error"`                         |
| `timestamp`  | string  | ISO 8601 UTC timestamp                   |
| `code`       | number  | Error code                               |
| `message`    | string  | Human-readable error description         |
| `recoverable`| boolean | Whether the session can continue         |

```json
{"type":"error","timestamp":"2026-07-27T14:30:10Z","code":4,"message":"LLM API call failed","recoverable":false}
```

## Tool-call grouping

When the LLM responds with tool calls, the assistant entry is followed by one or more `tool_call` entries, each immediately followed by its corresponding `tool_result`. Example sequence of a single multi-tool turn:

```plaintext
{"type":"assistant","timestamp":"...","content":"","model":"gpt-4o","usage":{...}}
{"type":"tool_call","timestamp":"...","id":"call_1","name":"weather.get","arguments":"{\"city\":\"London\"}","mcp_server":"weather-srv"}
{"type":"tool_result","timestamp":"...","call_id":"call_1","name":"weather.get","result":"{\"temp\":15}","is_error":false,"is_timeout":false,"mcp_server":"weather-srv"}
{"type":"tool_call","timestamp":"...","id":"call_2","name":"news.headlines","arguments":"{\"topic\":\"tech\"}","mcp_server":"news-srv"}
{"type":"tool_result","timestamp":"...","call_id":"call_2","name":"news.headlines","result":"{\"articles\":[...]}","is_error":false,"is_timeout":false,"mcp_server":"news-srv"}
```

When the reconstructed message history is sent to the LLM, the assistant entry carries the `tool_calls` array and each `tool_result` becomes a separate `"role": "tool"` message with the matching `tool_call_id`.

## Common field: `timestamp`

Every entry has a `timestamp` field in ISO 8601 UTC format generated at write time:

```
2026-07-27T14:30:00Z
```

## Reading the file

- Use **`llmkit agent`** to replay and continue a conversation — it reconstructs the full message history from the JSONL and passes it to the LLM, preserving tool-call groupings.

## Stdout output modes (`--mode` flag)

When `llmkit agent` is invoked with `--mode stream`, it emits real-time JSONL
events to stdout in addition to writing to the conversation file. Each event is
a single JSON line, newline-terminated, flushed immediately. An invoker can read
stdout line-by-line as the agent runs.

With `--mode debug`, timestamped human-readable lines are printed instead of
JSONL, allowing easy monitoring during development.

With `--mode quiet` (the default), only the final assistant response text (or
errors) is printed to stdout — nothing else.

### Stream event types (JSONL — `--mode stream`)

#### `turn_start`

Emitted at the beginning of each conversation turn.

| Field       | Type   | Description                        |
|-------------|--------|------------------------------------|
| `type`      | string | Always `"turn_start"`              |
| `turn`      | number | 1-based turn number                |
| `timestamp` | string | ISO 8601 UTC timestamp             |

```json
{"type":"turn_start","turn":1,"timestamp":"2026-07-27T14:30:01Z"}
```

#### `steer`

Emitted when a steering user message is injected from stdin (requires the
`--steer` flag). Fires at the top of a turn, before the `turn_start` LLM
call, when the message is written to the conversation. Not emitted in quiet
mode.

| Field       | Type   | Description                                |
|-------------|--------|--------------------------------------------|
| `type`      | string | Always `"steer"`                           |
| `content`   | string | The steering message text                  |
| `timestamp` | string | ISO 8601 UTC timestamp                     |

```json
{"type":"steer","content":"Focus only on pricing.","timestamp":"2026-07-27T14:30:01Z"}
```

#### `assistant`

Emitted when the LLM responds (mirrors the file entry).

| Field       | Type              | Description                              |
|-------------|-------------------|------------------------------------------|
| `type`      | string            | Always `"assistant"`                      |
| `content`   | string            | Response text (may be empty)             |
| `model`     | string            | Model identifier                         |
| `usage`     | object (optional) | Token usage, present when the API provides it |
| `timestamp` | string            | ISO 8601 UTC timestamp                   |

```json
{"type":"assistant","content":"Hello!","model":"gpt-4o","usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15},"timestamp":"2026-07-27T14:30:02Z"}
```

#### `tool_call`

Emitted before executing a tool.

| Field        | Type   | Description                             |
|--------------|--------|-----------------------------------------|
| `type`       | string | Always `"tool_call"`                     |
| `id`         | string | Tool call ID from the LLM               |
| `name`       | string | Namespaced tool name                    |
| `arguments`  | string | JSON string of tool arguments           |
| `mcp_server` | string | MCP server that provides this tool      |
| `timestamp`  | string | ISO 8601 UTC timestamp                  |

```json
{"type":"tool_call","id":"call_1","name":"weather.get","arguments":"{\"city\":\"London\"}","mcp_server":"weather-srv","timestamp":"2026-07-27T14:30:03Z"}
```

#### `tool_result`

Emitted after a tool completes.

| Field        | Type    | Description                              |
|--------------|---------|------------------------------------------|
| `type`       | string  | Always `"tool_result"`                    |
| `call_id`    | string  | Matches the originating `tool_call` id   |
| `name`       | string  | Tool name                                |
| `result`     | string  | JSON string of tool output or error text |
| `is_error`   | boolean | `true` if the tool returned an error     |
| `is_timeout` | boolean | `true` if the call timed out             |
| `mcp_server` | string  | MCP server that executed the call        |
| `timestamp`  | string  | ISO 8601 UTC timestamp                   |

```json
{"type":"tool_result","call_id":"call_1","name":"weather.get","result":"{\"temp\":15}","is_error":false,"is_timeout":false,"mcp_server":"weather-srv","timestamp":"2026-07-27T14:30:04Z"}
```

#### `done`

Emitted when the conversation completes successfully.

| Field       | Type   | Description                        |
|-------------|--------|------------------------------------|
| `type`      | string | Always `"done"`                    |
| `turns`     | number | Total number of turns executed     |
| `timestamp` | string | ISO 8601 UTC timestamp             |

```json
{"type":"done","turns":2,"timestamp":"2026-07-27T14:30:05Z"}
```

#### `error`

Emitted on a fatal error that stops the conversation.

| Field       | Type   | Description                        |
|-------------|--------|------------------------------------|
| `type`      | string | Always `"error"`                   |
| `code`      | number | Exit code (see exit code table)    |
| `message`   | string | Human-readable error description   |
| `timestamp` | string | ISO 8601 UTC timestamp             |

```json
{"type":"error","code":4,"message":"LLM API call failed","timestamp":"2026-07-27T14:30:02Z"}
```
