# LLMKIT - Requirements Document

## Overview

**Project Name:** LLMKIT
**Type:** Lightweight CLI binary written in C
**Purpose:** Execute LLM conversations with MCP tool support via YAML configuration, proxy MCP servers hiding, renaming, redefining descriptions for any item in it
**Design Goal:** Maximum lightweightness, zero dependencies where possible, suitable as a building block for other software

---

## 1. Command Line Interface

### Usage
```
llmkit agent -c <agent_config.yml> --conversation <convo.jsonl> -p <prompt|prompt_file> [--mode <type>]

llmkit proxy -c <proxy_config.yml> [-l <host:port>]

llmkit response --conversation <conversation.jsonl>

```

### `agent` Arguments

| Flag | Required | Description |
|------|----------|-------------|
| `-c`, `--config` | Yes | Path to YAML configuration file |
| `--conversation` | Yes | Path to conversation JSONL file. The agent appends to it and continues prior turns |
| `-p`, `--prompt` | Yes | User prompt text OR path to file containing prompt |
| `--mode` | No | Stdout output mode: `quiet` (default), `debug`, or `stream` |

### `proxy` Arguments

| Flag | Required | Description |
|------|----------|-------------|
| `-c`, `--config` | Yes | Path to YAML configuration file |
| `-l`, `--listen` | No | Hostname and port to expose as HTTP MCP server. If omitted, proxy runs as stdio MCP server (reads from stdin, writes to stdout) |

### `proxy` Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Configuration error (invalid YAML, missing required fields, invalid UTF-8 in config) |
| 2 | Invalid arguments (missing flags, invalid listen address) |
| 3 | Server error (bind failure) |
| 4 | MCP connection error (connection refused, timeout, protocol error) |
| 5 | MCP error (tool execution, call timeout with `behavior=fail`, invalid UTF-8 in MCP communication) |
| 6 | MCP init timeout (server failed to initialize within `init_timeout`) |
| 7 | Internal error (memory, parsing, unexpected) |

### `response` Arguments

| Flag | Required | Description |
|------|----------|-------------|
| `--conversation` | Yes | Path to conversation JSONL file |

Reads a conversation JSONL file and prints the `content` field of the **last** `"type":"assistant"` entry to stdout. Returns empty string if no assistant entry exists.

### `response` Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success (content printed, possibly empty) |
| 3 | File error (cannot read, invalid JSON) |
| 7 | Internal error (memory) |

### `agent` Prompt Resolution Logic
1. Check if `-p` argument is an existing file path
2. If file exists: read prompt text from file (UTF-8, trim trailing newline)
3. If file does not exist: treat argument as literal prompt text
4. If remaining string is empty or whitespace-only after trimming → exit with error code 2
### `agent` Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Configuration error (invalid YAML, missing required fields, **invalid UTF-8 in config**) |
| 2 | Invalid arguments (missing flags, empty prompt) |
| 3 | Conversation file error (cannot read/write, **invalid UTF-8 in JSONL**) |
| 4 | LLM API error (network, auth, rate limit, invalid response, **invalid UTF-8 in response**) |
| 5 | MCP error (connection, protocol, tool execution, **call timeout with behavior=fail**, **invalid UTF-8 in MCP communication**) |
| 6 | **MCP init timeout** (server failed to initialize within init_timeout) |
| 7 | Internal error (memory, parsing, unexpected, **invalid UTF-8 in prompt file**) |

### Config Root Keys

The same YAML schema is shared between `agent` and `proxy` commands. The difference is which root keys are accepted:

- **Agent config root keys:** `llm`, `mcps`, `agent` (all optional except `llm.api_base`)
- **Proxy config root keys:** only `mcps` (required)

The `proxy` command ignores `llm` and `agent` keys if present.

## 2. Configuration YAML Schema

### 2.1 LLM Configuration (`llm`)

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `api_base` | string | Yes | - | Base URL for OpenAI-compatible API (e.g., `http://localhost:11434/v1`) |
| `api_key` | string | No | `""` | API key for Authorization header |
| `model` | string | No | `"gpt-4o-mini"` | Model identifier |
| `headers` | map<string,string> | No | `{}` | Additional HTTP headers |

**Headers Behavior:** All headers merged with `Authorization: Bearer <api_key>` (if api_key provided) and `Content-Type: application/json`. User-provided headers take precedence over auto-generated ones on conflict.

### 2.2 MCP Servers (`mcps`)

Array of MCP server configurations. Each entry:

#### Common Fields
| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `name` | string | Yes | - | Unique identifier for this MCP server |
| `type` | enum | No | `stdio` | `stdio` (default), `http`, `sse` |
| `init_timeout` | string (duration) | No | `"30s"` | Max time to wait for **full MCP initialization sequence** — process spawn (stdio), connection + `initialize` handshake, and `tools/list` to retrieve tool definitions for LLM. **Always fails on timeout** — conversation cannot start. |
| `call_timeout` | string (duration) | No | `"10m"` | Max time to wait for a single `tools/call` request. |
| `call_timeout_behavior` | enum | No | `"fail"` | Behavior on tool call timeout: `"fail"` (exit code 5, write error entry) or `"continue"` (return timeout error as tool result to LLM, let conversation continue). |
| `hide` | bool | No | `false` | Proxy only: hide all tools/resources/prompts from this server (useful for internal-only servers) |
| `namespace` | string | No | server `name` | Proxy only: namespace prefix for tools/resources/prompts from this server |
| `rename` | map<string,string> | No | `{}` | Proxy only: rename specific items — key is original namespaced name, value is new exposed name |
| `redefine` | map<string,string> | No | `{}` | Proxy only: redefine descriptions — key is original namespaced name, value is new description |
| `whitelist` | string[] | No | `[]` | Proxy only: only expose items matching these namespaced names (empty = expose all) |
| `blacklist` | string[] | No | `[]` | Proxy only: exclude items matching these namespaced names (empty = exclude none) |

#### Type-specific Fields by Type

**stdio (default):**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmdline` | string | Yes | Full command line to spawn MCP server process |

**http:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `url` | string | Yes | HTTP endpoint URL (e.g., `http://localhost:8080/mcp`) |
| `headers` | map<string,string> | No | Additional headers for HTTP requests |

**sse:**
| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `url` | string | Yes | - | SSE endpoint URL (e.g., `http://localhost:8080/mcp/sse`) |
| `headers` | map<string,string> | No | `{}` | Additional headers for SSE connection |
| `reconnect_delay` | string (duration) | No | `"1s"` | Wait time between SSE reconnect attempts |
| `max_reconnect` | int | No | `3` | Maximum SSE reconnection attempts before giving up |

### 2.3 Agent Configuration (`agent`)

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `system_prompt` | string | No | `""` | System prompt prepended to conversation |

---

### Namespacing & Transparent Translation

The proxy automatically namespaces all tools, resources, and prompts from each backend server using the `namespace` field (default: server `name`). This prevents name collisions when proxying multiple servers.

**Exposed naming format:** `{namespace}.{original_name}`

| Capability | Exposed as | Transparent Translation |
|------------|------------|------------------------|
| Tool `get_file` from server `namespace=fs` | `fs.get_file` | Client calls `fs.get_file` → proxy forwards `get_file` to backend |
| Resource `file:///etc/passwd` from `namespace=fs` | `fs.file:///etc/passwd` | Client reads `fs.file:///etc/passwd` → proxy reads `file:///etc/passwd` |
| Prompt `generate_code` from `namespace=github` | `github.generate_code` | Client calls `github.generate_code` → proxy forwards `generate_code` |

**Translation behavior:**
- **Forward (client → backend):** Strip namespace prefix from tool/resource/prompt names before forwarding
- **Reverse (backend → client):** Prepend namespace prefix to names in responses (`tools/list`, `resources/list`, `prompts/list`)
- **Tool calls:** Client calls `{namespace}.{tool}` with args; proxy calls `{tool}` with same args on backend
- **Resource reads:** Client reads `{namespace}.{uri}`; proxy reads `{uri}` on backend
- **Prompt gets:** Client calls `{namespace}.{prompt}` with args; proxy calls `{prompt}` with same args
- **Redefinition rules:** Applied *after* namespacing (match against `{namespace}.{original_name}`)
- **Whitelist/Blacklist:** Applied *after* namespacing (match against namespaced name)

This allows clients to use a single MCP endpoint while the proxy transparently routes to multiple backend servers.

## 3. Conversation JSONL Format

### File Structure
- One JSON object per line (JSONL)
- File created if not exists
- Appended to on each run
- UTF-8 encoded

### Entry Types

Each line is a JSON object with a `type` field discriminating the entry type.

#### 3.1 Metadata Entry (First Line)
```json
{
  "type": "meta",
  "version": 1,
  "timestamp": "2026-07-25T10:30:00Z",
  "config_hash": "sha256:abc123...",
  "run_id": "uuid-v4"
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"meta"` |
| `version` | integer | Format version (current: 1) |
| `timestamp` | string | ISO 8601 UTC timestamp of run start |
| `config_hash` | string | SHA256 of config YAML content (prefixed with `sha256:`) |
| `run_id` | string | UUID v4 for this execution |

#### 3.2 User Message Entry
```json
{
  "type": "user",
  "timestamp": "2026-07-25T10:30:01Z",
  "content": "What is the capital of France?",
  "source": "cli"
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"user"` |
| `timestamp` | string | ISO 8601 UTC |
| `content` | string | User prompt text |
| `source` | enum | `"cli"` (from -p), `"file"` (from file path) |

#### 3.3 Assistant Message Entry
```json
{
  "type": "assistant",
  "timestamp": "2026-07-25T10:30:02Z",
  "content": "The capital of France is Paris.",
  "model": "gpt-4o-mini",
  "usage": {
    "prompt_tokens": 45,
    "completion_tokens": 12,
    "total_tokens": 57
  }
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"assistant"` |
| `timestamp` | string | ISO 8601 UTC |
| `content` | string | Assistant response text |
| `model` | string | Model used for this response |
| `usage` | object | Token usage (optional, if provided by API) |

#### 3.4 Tool Call Entry (Assistant → Tool)
```json
{
  "type": "tool_call",
  "timestamp": "2026-07-25T10:30:03Z",
  "id": "call_abc123",
  "name": "get_weather",
  "arguments": "{\"location\": \"Paris\", \"unit\": \"celsius\"}",
  "mcp_server": "weather-stdio"
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"tool_call"` |
| `timestamp` | string | ISO 8601 UTC |
| `id` | string | Tool call ID from LLM |
| `name` | string | Tool name |
| `arguments` | string | JSON string of arguments (not parsed object) |
| `mcp_server` | string | Name of MCP server that provides this tool |

#### 3.5 Tool Result Entry (Tool → Assistant)
```json
{
  "type": "tool_result",
  "timestamp": "2026-07-25T10:30:04Z",
  "call_id": "call_abc123",
  "name": "get_weather",
  "result": "{\"temperature\": 22, \"condition\": \"sunny\"}",
  "is_error": false,
  "is_timeout": false,
  "mcp_server": "weather-stdio"
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"tool_result"` |
| `timestamp` | string | ISO 8601 UTC |
| `call_id` | string | Matching tool_call.id |
| `name` | string | Tool name |
| `result` | string | JSON string of result (or error message) |
| `is_error` | boolean | True if tool execution failed |
| `is_timeout` | boolean | True if result is due to call_timeout with behavior=continue |
| `mcp_server` | string | Name of MCP server that executed the tool |

#### 3.6 Error Entry
```json
{
  "type": "error",
  "timestamp": "2026-07-25T10:30:05Z",
  "code": 4,
  "message": "LLM API returned 429: Rate limit exceeded",
  "recoverable": true
}
```
| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"error"` |
| `timestamp` | string | ISO 8601 UTC |
| `code` | integer | Exit code that would be returned |
| `message` | string | Human-readable error description |
| `recoverable` | boolean | Whether retry might succeed |

### Conversation Flow Example

```
{"type":"meta","version":1,"timestamp":"2026-07-25T10:30:00Z","config_hash":"sha256:abc...","run_id":"uuid-1"}
{"type":"user","timestamp":"2026-07-25T10:30:01Z","content":"What's the weather in Paris?","source":"cli"}
{"type":"assistant","timestamp":"2026-07-25T10:30:02Z","content":"","model":"gpt-4o-mini","usage":{"prompt_tokens":50,"completion_tokens":0,"total_tokens":50}}
{"type":"tool_call","timestamp":"2026-07-25T10:30:03Z","id":"call_1","name":"get_weather","arguments":"{\"location\":\"Paris\"}","mcp_server":"weather-http"}
{"type":"tool_result","timestamp":"2026-07-25T10:30:04Z","call_id":"call_1","name":"get_weather","result":"{\"temp\":22}","is_error":false,"is_timeout":false,"mcp_server":"weather-http"}
{"type":"assistant","timestamp":"2026-07-25T10:30:05Z","content":"It's 22°C in Paris.","model":"gpt-4o-mini","usage":{"prompt_tokens":60,"completion_tokens":15,"total_tokens":75}}
```

### Reconstructing Conversation for Next Turn

To feed conversation history to LLM:
1. Read all lines from JSONL file
2. Skip `meta` and `error` entries
3. Build message array:
   - `user` → `{role: "user", content: content}`
   - `assistant` → `{role: "assistant", content: content}` (include `tool_calls` if any)
   - `tool_call` + matching `tool_result` → assistant message with `tool_calls`, then tool message with `tool_call_id`

---

## 4. Runtime Behavior

### 4.1 Startup Sequence
1. Parse CLI arguments
2. Load and parse config YAML
3. Validate required fields (`llm.api_base`)
4. Open/create conversation JSONL (append mode)
5. Write `meta` entry
6. Resolve prompt (file or literal)
7. Write `user` entry

### 4.2 Conversation Loop
1. Build message history from JSONL (see Reconstructing Conversation for Next Turn)
2. Fetch available tools from all configured MCP servers
3. Call LLM API with messages + tools
4. Write `assistant` entry (even if empty content with tool_calls)
5. If tool_calls present:
   - For each tool_call: execute via appropriate MCP server
   - Write `tool_call` entry
   - Write `tool_result` entry
   - Loop back to step 1 (continue conversation)
6. If no tool_calls: write final `assistant` entry, exit 0

### 4.3 MCP Communication

#### stdio
- Spawn process per server at startup
- Communicate via stdin/stdout (JSON-RPC 2.0)
- Keep process alive for duration of run
- Terminate on exit
- **Init timeout:** Kill process and exit code 6 if not ready within `init_timeout`

#### http
- POST to `{url}` with JSON-RPC 2.0 request
- Expect JSON-RPC 2.0 response
- Timeout: 30s default for HTTP layer; tool call uses `call_timeout`

#### sse
- Connect to `{url}` via SSE
- Send requests via POST to same base URL
- Receive responses via SSE stream
- Reconnect on disconnect (up to `max_reconnect_attempts`, with `reconnect_delay` between attempts)
- Tool call uses `call_timeout`

#### Tool Call Timeout Behavior
- **`call_timeout_behavior: "fail"` (default):** Tool call returns error to LLM, conversation stops, exit code 5
- **`call_timeout_behavior: "continue"`:** Tool call returns timeout error as tool result (`is_timeout: true`), LLM receives it and conversation continues

### 4.4 Tool Execution Model
**Sequential, synchronous, blocking execution:**
- When LLM returns multiple `tool_calls`, they are executed **one at a time, in order**
- Each `tools/call` completes fully before the next begins
- **No parallelism** — no threads, no async, no concurrent requests
- Tool results are collected and sent back to LLM as a batch in the next turn
- This applies to all MCP transport types (stdio, http, sse)

### 4.5 Tool Discovery
On startup, for each MCP server:
1. Call `tools/list` method
2. Cache tool definitions (name, description, inputSchema)
3. Prefix tool names with `{mcp_server_name}.` to avoid collisions
4. Pass all tools to LLM in `tools` parameter

### 4.6 Activity Indication (stderr)

The program writes progress/status lines to stderr so stdout/pipes remain clean:

- `[init] Loading config...`
- `[init] Connecting to MCP servers...`
- `[init] LLM API ready`
- `[progress] Waiting for LLM response...`
- `[tool] <server>.<tool_name>` — when a tool is invoked
- `[done] Conversation complete`

All stderr output is plain ASCII, one line per event, newline-terminated. stderr is not used for structured data — only the JSONL file and exit codes carry program results. Activity lines may be suppressed when stderr is not a TTY.

**Quiet mode (`--mode quiet`):** all progress/stats lines are suppressed entirely — even when stderr is a TTY — so the agent prints only the final assistant response on stdout and nothing on stderr. This is the default mode and is intended for scripting. Use `--mode debug` or `--mode stream` to keep progress output during a run.

---

## 5. Non-Functional Requirements

### 5.1 Performance Targets (Goals, Not Hard Limits)

These are optimization targets we aim for, but they are **not blocking** for project success:

- Binary size: < 2 MB (statically linked)
- Startup time: < 50ms (excluding MCP process spawn)
- Memory usage: < 10 MB baseline
- No garbage collection pauses

### 5.2 Encoding

- **All text files MUST be valid UTF-8**
- Configuration YAML, conversation JSONL, prompt files, MCP stdio communication, LLM API requests/responses — any invalid UTF-8 sequence causes immediate failure with exit code 1 (configuration error) or 7 (internal error)
- No implicit encoding detection or fallback; reject on invalid sequences
- Output files (conversation JSONL) written as valid UTF-8
- **Required:** libcurl, libyaml, cJSON (or similar lightweight JSON)
- **Optional:** OpenSSL (if not using system cert store)
- **Standard C library only:** No heavy frameworks
- Static linking preferred

### 5.3 Portability
- Target: Linux (x86_64, aarch64), macOS (x86_64, arm64)
- Windows: best effort (via cross-compile)
- No OS-specific APIs where avoidable

### 5.4 Error Handling
- All allocations checked
- All file descriptors closed on error
- Clean MCP process termination on exit
- Partial conversation preserved in JSONL on error

### 5.5 Security
- API key never logged (redacted in debug output)
- No shell injection (execve with argv array)
- Input validation on all external data
- TLS verification enabled by default


