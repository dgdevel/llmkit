# ACP Research: `llmkit acp` readiness

Research for a new `llmkit acp` subcommand that serves the Agent Client
Protocol (ACP) so editors like Zed can drive llmkit as a coding agent.

## 1. What ACP is

ACP (agentclientprotocol.com, spec at agentclientprotocol.com/protocol/v1)
is Zed Industries' protocol standardizing editor <-> coding-agent
communication, same role LSP plays for language servers.

- Wire: JSON-RPC 2.0, **newline-delimited JSON over stdio** (one message
  per line). NOT Content-Length framed like LSP.
- Protocol version: `1` (integer, breaking-change only).
- Roles: client = editor (Zed, JetBrains), agent = us.
- Transport-agnostic on paper, but editors spawn agents as **stdio
  subprocesses**. Zed has no TCP connect option for agent servers.

## 2. Protocol surface (what we must implement)

### Initialize handshake (mandatory first)
Client -> agent `initialize`:
```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocolVersion":1,
  "clientCapabilities":{"fs":{"readTextFile":true,"writeTextFile":true},
                        "terminal":true}}}
```
Agent replies:
```json
{"protocolVersion":1,
 "agentCapabilities":{"loadSession":false,
   "promptCapabilities":{"image":false,"audio":false,"embeddedContext":false}},
 "authMethods":[]}
```
Negotiation: respond with client's version or our latest; mismatch ->
client closes.

### Session lifecycle
- `session/new` {cwd, mcpServers[]} -> {sessionId, modes[]}.
  cwd is absolute workspace root. modes optional (primary/read-only/plan).
- `session/prompt` {sessionId, prompt:[content blocks]} ->
  streamed `session/update` notifications, then final response
  {stopReason:"end_turn"|"max_tokens"|"max_turn_requests"|"refusal"|"cancelled"}.
- `session/update` notification kinds: `user_message_chunk`,
  `agent_message_chunk` (streamed text), `agent_thought_chunk` (thinking),
  `tool_call` {toolCallId,title,kind(read|edit|execute|fetch|think|other),
  status:pending|in_progress|completed|failed, content},
  `tool_call_update`, `plan`, `available_commands_update`.
- `session/cancel` {sessionId}: stop LLM calls + tools, flush updates,
  answer prompt with stopReason `cancelled`.
- `session/load` (optional, we advertise loadSession:false -> skip).
- `session/request_permission` (agent -> client request, optional).

### Client-side services (optional, only if client advertises)
- `fs/read_text_file` {path} -> {content}
- `fs/write_text_file` {path, content}
Not required: llmkit builtin tools do own file I/O; we never call these.

## 3. Zed integration facts

- Zed config: `settings.json` -> `agent_servers`:
```json
{"agent_servers": {
  "llmkit": {
    "command": {"path": "/usr/bin/llmkit",
                "args": ["acp", "--anthropic",
                         "--api-base", "https://api.anthropic.com",
                         "--key", "sk-...",
                         "--model", "claude-..."],
                "env": {}}
  }
}}
```
- Zed spawns process, talks ndjson JSON-RPC on its stdio.
- **`--listen` is useless for Zed**: it will not connect over TCP.
  Value is for other tooling / remote use: accept TCP connections that
  speak the same ndjson JSON-RPC per connection.

## 4. Mapping onto llmkit

| ACP concept | llmkit piece |
|---|---|
| turn loop, streaming | `engine.c` (`engine_new/apply_config_record/input_record/start/run`) |
| stream text + tool calls | engine `emit_fn` sink -> translate records (T_RESPONSE text -> `agent_message_chunk`, T_THINKING -> `agent_thought_chunk`, T_TREQ -> `tool_call`/`tool_call_update`) |
| tools | builtin mcp server (`builtin.c`), in-process; agent runs its own file/process tools, no client fs delegation |
| session mcpServers[] | `mcp.c` client + tools config record (same as `--mcp-proxy`) |
| cwd | set builtin tool root (like `agent` repl cwd) |
| cancel | `engine_stop_orderly()` path (same as repl SIGINT) |
| config | no config file exists; flags build JSONL records. Reuse `call_cfg_t`/`call_build_llm()` shape from `src/call.c` |

Suggested flags (mirror proxy/call conventions):
`llmkit acp (--anthropic|--openai|--openai-responses) <api_base> --key
<token> --model <id> [--listen <host:port>] [--mcp-proxy <cfg>]`
Default when no `--listen`: stdio mode (Zed-compatible). With `--listen`:
TCP mode, one JSON-RPC ndjson stream per connection, same message loop.

## 5. Work plan

1. `src/acp.c`: ndjson JSON-RPC reader/writer + dispatch (initialize,
   session/new, session/prompt, session/cancel); stdio loop.
2. Engine bridge: per-session `engine_t`, config snapshot from flags,
   emit_fn -> `session/update` translation, stopReason mapping.
3. `main.c` dispatch entry + usage line.
4. Optional `--listen host:port` TCP accept loop (reuse `listen_tcp`
   pattern from `src/llmproxy.c:1666`).
5. `Makefile` object; selfcheck tests for framing + handshake.
6. Docs: `docs/` page + Zed `agent_servers` example.

## References
- https://agentclientprotocol.com/protocol/v1/overview
- https://github.com/zed-industries/agent-client-protocol (spec + rust/ts impls)
- Zed agent_servers: zed.dev/docs/agent-servers / deepwiki zed-industries/zed sec 8.2
