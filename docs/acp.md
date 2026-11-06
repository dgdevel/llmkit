# llmkit acp

`llmkit acp` serves the Agent Client Protocol (ACP, version 1) on
stdio: newline-delimited JSON-RPC 2.0 in, newline-delimited JSON-RPC
2.0 out. ACP editors (Zed, JetBrains) spawn it as a child process and
drive llmkit as their coding agent. There is no `--listen`: the
protocol's consumers speak stdio only.

## Usage

```
usage: llmkit acp (--anthropic|--openai|--openai-responses) <api_base>
              [--key <token>] [--model <name>] [--max-tokens <n>]
              [--reasoning-effort <value>] [--system-prompt <text>]
              [--header <name=value>]...
              [--mcp-proxy <config>]... [--bundle full|read-only]
```

The flags match `llmkit call`: one protocol flag names the endpoint's
wire, `<api_base>` is the positional upstream base URL.

## Methods

| method | behavior |
|---|---|
| `initialize` | answers `protocolVersion:1`, `loadSession:false`, all `promptCapabilities` false, `authMethods:[]` |
| `session/new` | `{cwd, mcpServers[]}` -> `{sessionId, modes:[]}`; one engine per session. The workspace rides the session: the built-in server child spawns with its own `--cwd` and `AGENTS.md` is read from the explicit dir, so sessions may carry different cwds in one process (Zed reuses the process across conversations). `mcpServers` is ignored (attach servers with `--mcp-proxy`) |
| `session/prompt` | text blocks joined with `\n` into one user record, the turn runs to completion streaming `session/update` notifications (`agent_message_chunk`, `agent_thought_chunk`, `tool_call`, `tool_call_update`), the answer carries the mapped `stopReason` (`end_turn`, `max_tokens`, `refusal`, `cancelled`) |
| `session/cancel` | raises the engine's stop flag; the in-flight turn ends at its next boundary and its prompt answers `stopReason:"cancelled"`; no-op with no turn running |

Unknown methods answer `-32601`, malformed lines `-32700`. Batches are
rejected. Notifications are never answered.

Only the protocol rides stdout; every log line goes to stderr, so
piped transcripts stay clean JSON Lines.

## --bundle full|read-only

The agent package attached at `session/new`:

- `full`: the bundled agent system prompt, `AGENTS.md` from the
  session cwd appended, the built-in mcp server (`llmkit builtin-mcp`)
  and the bundled deepwiki server (an `mcp-proxy` wrapper renaming its
  tools `repo_topics`/`repo_info`/`repo_ask`).
- `read-only`: the same, but the built-in server hides its
  file-writing tools (`--read-only`); deepwiki stays, it is read-only
  by nature.
- no bundle: a plain llm conversation - no default prompt (only
  `--system-prompt`), no built-in tools, no deepwiki; `--mcp-proxy`
  configs still apply.

A `--mcp-proxy` server named `deepwiki` wins over the bundled one.

## Zed

`settings.json`:

```json
{
  "agent_servers": {
    "llmkit": {
      "command": {
        "path": "/usr/local/bin/llmkit",
        "args": ["acp", "--anthropic", "https://api.anthropic.com",
                 "--key", "sk-...", "--model", "claude-sonnet-4-5",
                 "--bundle", "full"]
      }
    }
  }
}
```

Notes:
- the workspace is per session: Zed reuses one `llmkit acp` process
  across conversations, and concurrent conversations can carry
  different cwds (multi-root projects, explicit work dirs). Each
  session's built-in server child gets its own `--cwd`, `AGENTS.md`
  is read from the session's dir, and the parent never chdirs - so
  mixed-cwd sessions stay isolated.
- `session/prompt` accepts text blocks only (the advertised
  `promptCapabilities` are all false); anything else answers
  `-32602`.

## Implementation

`src/acp.c`. The framing loop owns stdin/stdout; a detached reader
thread parses lines into a queue so `session/cancel` lands while a
turn runs. The engine bridge reuses the `call`/`repl` machinery
(`call_compile`, `call_build_agent_tools`, `repl_deepwiki_build`,
`inject_agents_md`); the engine's emit sink translates records into
`session/update` notifications (`acp_record_to_update`).
