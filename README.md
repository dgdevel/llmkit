# llmkit

`llmkit` is a single tool for interacting with Large Language Models (LLMs) and Model Context Protocol (MCP) servers directly from your shell.

## Getting Started

### Interactive Chat (REPL)
The easiest way to use `llmkit` is through the `repl` command. This provides a live chat interface where you can see thinking and tool calls as they happen.

```sh
$ llmkit repl --openai http://localhost:9931/v1
```

- **Navigation**: Use `/help` for commands, `/models` to change the model, and `/new` to start a fresh conversation.
- **Files**: Start a word with `@` and press Tab to autocomplete file or directory paths.
- **Controls**: Ctrl-C stops the current turn; Ctrl-D exits.

### AI Agent with Tools
The `agent` command is a powerful version of the chat that includes built-in MCP tools (web search, file management, process execution, and skills). It also reads instructions from an `AGENTS.md` file in your current directory.

```sh
$ llmkit agent --openai http://localhost:9931/v1 --model llama3.1 \
      --conversation-store ~/chats/project.jsonl
```

- **Persistence**: Use `--conversation-store <file>` to save your chat history and resume it later.

### One-off Calls
For scripts or simple questions, use the `call` command. It takes one prompt and returns one answer.

```sh
$ llmkit call --openai http://localhost:9931/v1 --model llama3.1 \
      --system-prompt "You are a helpful assistant" \
      --prompt "hello, how are you?"
```

- **Piping**: Use `--prompt -` to read from stdin:
  `ANSWER=$(llmkit call --openai "$API_BASE" --prompt - < input.txt)`

## Advanced Tools

### Integration and Automation
- **`runner`**: For external applications. It reads JSONL records from stdin and writes JSONL records to stdout.
- **`agent-as-tool`**: Exposes an agent conversation as a single tool for other MCP servers.
- **`mcp-proxy`**: Curates and re-describes upstream MCP servers.

### MCP Server Management
- **`builtin-mcp`**: A built-in server providing tools for web search, file operations, process control, and skills.
- **`mcp-repl`**: A debugging console to test tools from a specific MCP server.

```sh
$ llmkit mcp-repl --stdio './calculator-mcp'
> add(1, 1)
2
```

### Monitoring and Debugging
- **`prettyprint`**: Renders a recorded JSONL conversation file into a human-readable format.
- **`proxy`**: A local HTTP server that forwards traffic to an LLM and prints the conversation to your terminal in real-time.
- **`models`**: Lists available models from an endpoint.

## Command Line Reference

### General Commands
| Command | Description |
|---|---|
| `runner` | Process JSONL conversation streams |
| `agent-as-tool <seed.jsonl>` | Wrap a conversation as an MCP tool |
| `mcp-proxy <config.jsonl>` | Proxy and filter MCP servers |
| `builtin-mcp` | Start built-in MCP tools server |
| `call <flags>` | One-shot prompt/response |
| `repl <flags>` | Interactive chat |
| `mcp-repl <flags>` | Interactive MCP tool console |
| `prettyprint [file.jsonl]` | Human-readable conversation viewer |
| `proxy <flags>` | HTTP LLM proxy with live terminal output |
| `models <flags>` | List models of an endpoint |
| `help` | Show usage |
| `version` | Show version |
| `update` | Check and apply updates |

### Call and REPL Flags
Requires one protocol flag (`--anthropic`, `--openai`, `--openai-responses`) and the API base URL.

- `<api_base>`: The endpoint base URL (e.g., `http://localhost:9931/v1`).
- `--key <token>`: API key.
- `--model <name>`: Model name.
- `--max-tokens <n>`: Limit output tokens.
- `--reasoning-effort <value>`: Control reasoning depth (e.g., `high`, `medium`, `low`).
- `--system-prompt <text>`: Set the system instructions.
- `--prompt <text|->`: (Call only) The prompt to send.
- `--header <name>=<value>`: Custom HTTP header.
- `--mcp-proxy <config.jsonl>`: Attach an MCP proxy configuration.
- `--terminal-tool <server.tool>`: Mark a tool as the final output.
- `--verbose`, `--conversation-store <file>`, `--no-helper`: Agent-specific options for logging, persistence, and subagent control.
- `--no-deepwiki`: Disable the bundled DeepWiki MCP server.

### MCP-REPL Flags
Requires one transport flag.

- `--stdio <command>`: Run a command as the MCP server.
- `--http <url>`: Use HTTP transport.
- `--sse <url>`: Use SSE transport.
- `--protocol <revision>`: MCP version to use.
- `--header <name>=<value>`: Custom HTTP header.

### Proxy and Model Flags
- `--listen <host:port>`: (Proxy only) Local endpoint. Default `127.0.0.1:8080`.
- `--json`: (Models only) Output model list as JSON.

## Building

### Dependencies
- Linux environment with C11 compiler.
- **cJSON** (`libcjson-dev`).
- **libcurl** (`libcurl4-openssl-dev`).
- pthreads.

```sh
make          # Build binary
make check    # Run self-check tests
```

Windows builds are performed via msys2. Mac builds are performed via osxcross. See `tools/package.sh`.

### License
Distributed under the European Union Public Licence v1.2 (EUPL-1.2).
