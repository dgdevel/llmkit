# llmkit

A lightweight CLI tool to interact with LLMs and MCP from shell applications, for Windows, Linux and MacOS.

A quick example:
```
$ cat config.yml
llm:
  api_base: "http://127.0.0.1:8000/v1"
agent:
  system_prompt: "You are a helpful assistant. Use the calculator to do your math."
mcps:
  - name: "calculator"
    cmdline: "uvx mcp-server-calculator"

$ llmkit agent -c config.yml --conversation convo.jsonl -p "How much is 3 + 3?"
3 + 3 is 6.
$ llmkit agent -c config.yml --conversation convo.jsonl -p "And if you add 4?"
If you add 4, the total is 10.

```

## Modes of operation

- **`llmkit agent`** -- Runs an LLM conversation loop with MCP tool support.
  Reads a YAML config, loads conversation history from JSONL, calls the LLM
  API, executes MCP tool calls, and writes results back to the JSONL file.
- **`llmkit proxy`** -- Runs an MCP proxy server that fronts one or more
  backend MCP servers, providing namespace isolation, rename/redefine, and
  whitelist/blacklist filtering over a single MCP endpoint (stdio or HTTP).
- **`llmkit response`** -- Reads a conversation JSONL file and prints the
  last LLM assistant response to stdout. Useful for extracting the final
  answer from a completed conversation.

The binary is statically linkable, has zero runtime language dependencies,
and targets Linux, macOS, and Windows (via MinGW-w64 cross-compilation).

## Usage

```
llmkit agent -c <agent_config.yml> --conversation <convo.jsonl> -p <prompt|prompt_file> [--mode <type>] [--steer]
llmkit proxy -c <proxy_config.yml> [-l <host:port>]
llmkit response --conversation <conversation.jsonl>
```

If `-l` (or `--listen`) is omitted from `proxy`, it runs as a stdio MCP server (reads
JSON-RPC from stdin, writes to stdout). With `-l host:port` it serves HTTP.

The `response` command reads the given `--conversation` JSONL file, finds the last
`"type":"assistant"` entry, and prints its `content` field to stdout.
Returns empty output if no assistant entry exists.

### Prompt resolution (`agent`)

The `-p` (or `--prompt`) argument is treated as a file path if the file exists; otherwise it
is used as the literal prompt text. An empty or whitespace-only prompt exits
with code 2.

### Stdout output modes

The `--mode <type>` flag controls stdout output:

- **`quiet`** (default): only the final assistant response text is printed to
  stdout (plus errors). Nothing else. Ideal for scripting.
- **`debug`**: timestamped, human-readable lines for each event (`begin`,
  `turn_start`, `assistant`, `tool_call`, `tool_result`, `done`, `error`).
- **`stream`**: structured JSONL events emitted to stdout. Each line is a
  JSON event (`turn_start`, `assistant`, `tool_call`, `tool_result`, `done`,
  `error`). See [docs/conversation-format.md](docs/conversation-format.md)
  for the full schema.

### Steering (`agent`)

The `--steer` flag enables **steering**: while the agent runs, it reads
additional user messages from **stdin** and injects them into the
conversation at the next turn boundary — the earliest point the LLM can
legally see them. This lets you course-correct a running agent (e.g. "stop
searching, just summarize what you have").

**Wire format:** messages are separated by a **blank line** (`\n\n`). A
message may span multiple lines. Carriage returns (`\r`) are stripped, so
both `\n\n` and `\r\n\r\n` work as delimiters. Anything after the last
delimiter is held until more input arrives, or flushed when stdin closes.

```
# In one shell: pipe steering messages to the running agent
(echo "Focus only on the pricing section."; echo) | llmkit agent --steer -c cfg.yml ...
```

```python
# Programmatically: write to the agent's stdin
proc = subprocess.Popen(["llmkit", "agent", "--steer", ...], stdin=PIPE)
proc.stdin.write(b"Change direction: only summarize.\n\n")
proc.stdin.flush()
```

**Delivery semantics:** the agent polls stdin once at the top of every turn
(before reconstructing the conversation and calling the LLM) and again just
before the run would complete. Messages typed during a blocking LLM call or
a tool call simply queue in the stdin buffer and are delivered at the next
turn. The OpenAI API forbids interleaving a user message between an
assistant's `tool_calls` and their `tool_results`, so mid-turn injection is
not possible — the turn boundary *is* the earliest legal and practical
delivery point.

Injected messages are written to the conversation JSONL as `"user"` entries
with `"source":"steer"`. In `debug` mode a `steer:` line is emitted; in
`stream` mode a `{"type":"steer",...}` JSONL event is emitted. Steering is
silent in `quiet` mode.

## Configuration

Both modes share the same YAML schema; the difference is which root keys
are accepted.

- **Agent config:** `llm`, `mcps`, `agent` (all optional except `llm.api_base`)
- **Proxy config:** only `mcps` (required)

### LLM (`llm`)

| Field      | Required | Default        | Description                          |
|------------|----------|----------------|--------------------------------------|
| `api_base` | Yes      | -              | OpenAI-compatible base URL           |
| `api_key`  | No       | `""`           | API key for Authorization header     |
| `model`    | No       | `gpt-4o-mini`  | Model identifier                     |
| `headers`  | No       | `{}`           | Additional HTTP headers              |

### MCP servers (`mcps`)

Array of server entries:

| Field                  | Default   | Description                                            |
|------------------------|-----------|--------------------------------------------------------|
| `name`                 | -         | Unique identifier                                     |
| `type`                 | `stdio`   | `stdio`, `http`, or `sse`                             |
| `cmdline`              | -         | Command line (stdio)                                  |
| `url`                  | -         | Endpoint URL (http/sse)                               |
| `init_timeout`         | `30s`     | Max time for full MCP initialization                  |
| `call_timeout`         | `10m`     | Max time for a single tools/call                      |
| `call_timeout_behavior`| `fail`    | `fail` (exit 5) or `continue` (return error to LLM)   |
| `namespace`            | `name`    | Proxy: prefix for tools/resources/prompts             |
| `hide`                 | `false`   | Proxy: hide all items from this server                |
| `rename`               | `{}`      | Proxy: map namespaced name -> new exposed name        |
| `redefine`             | `{}`      | Proxy: override descriptions                          |
| `whitelist`            | `[]`      | Proxy: only expose these namespaced names             |
| `blacklist`            | `[]`      | Proxy: exclude these namespaced names                 |

### Example agent config

```yaml
llm:
  api_base: "http://localhost:11434/v1"
  api_key: "sk-..."
  model: "llama3"
agent:
  system_prompt: "You are a helpful assistant."
mcps:
  - name: fs
    cmdline: "npx -y @modelcontextprotocol/server-filesystem /tmp"
    init_timeout: "30s"
    call_timeout: "10m"
```

### Example proxy config

```yaml
mcps:
  - name: fs
    cmdline: "npx -y @modelcontextprotocol/server-filesystem /tmp"
    namespace: fs
    whitelist:
      - "fs.read_file"
  - name: internal
    type: http
    url: "http://localhost:9000/mcp"
    hide: true
```

## Exit codes

Both commands use the same numeric scheme:

| Code | Meaning (agent)              | Meaning (proxy)                          | Meaning (response)                   |
|------|------------------------------|------------------------------------------|--------------------------------------|
| 0    | Success                      | Success                                  | Success (content printed, may be empty) |
| 1    | Configuration error          | Configuration error                      | -                                    |
| 2    | Invalid arguments            | Invalid arguments / listen address       | Invalid argument (missing `-f`)      |
| 3    | Conversation file error      | Server error (bind failure)              | File error (cannot read, invalid JSON) |
| 4    | LLM API error                | MCP connection error                     | -                                    |
| 5    | MCP error                    | MCP error                                | -                                    |
| 6    | MCP init timeout             | MCP init timeout                         | -                                    |
| 7    | Internal error               | Internal error                           | Internal error                       |

## Building

### Dependencies

- C compiler (gcc or clang)
- libyaml, libcurl, OpenSSL (libcrypto/libssl)
- cJSON (system libcjson, or vendored automatically via `make vendors`)
- MinGW-w64 cross-compiler (for Windows builds only)

Run `make check-deps` to verify required libraries are detected.

### Build

```sh
make            # native binary -> ./llmkit
make debug      # ASAN + debug symbols
make test       # all unit + integration tests
make windows    # cross-compile 64-bit Windows .exe -> llmkit.exe
make windows32  # cross-compile 32-bit Windows .exe
make dist       # release tarball (stripped binary + sources)
make install    # copy binary to /usr/local/bin
```

The build runs an ASCII-source check, clang-format verification, and
clang-tidy linting before compiling. These can be skipped by building the
target directly: `make llmkit`.

## Activity logging

Progress lines (`[init]`, `[progress]`, `[tool]`, `[done]`, `[error]`) are
written to stderr and are automatically suppressed when stderr is not a TTY,
keeping stdout/stderr pipes clean for the JSONL file and exit codes.

In **quiet mode** (`--mode quiet`, the default), these progress lines are
suppressed entirely — even when stderr is a TTY — so only the final assistant
response appears on stdout and nothing else is printed. The `[stats]` token
summary is likewise silenced. Use `--mode debug` or `--mode stream` if you want
the progress lines while the agent runs.

## Project layout

```
llmkit/
+-- src/          # C sources (no #ifdef _WIN32 outside platform.c)
+-- tests/        # unit + integration tests + fixtures
+-- docs/         # requirements, technical spec, work plan
+-- scripts/      # build helpers (check-ascii)
+-- Makefile      # native build
`-- Makefile.cross# Windows cross-compile
```

See `docs/` for the full requirements document, technical specification,
and phased work plan.

## License

See the project repository for license information.
