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

$ llmkit agent -c config.yml -o convo.jsonl -p "How much is 3 + 3?"
[...]
$ llmkit response -f convo.jsonl
3 + 3 is 6.
$ llmkit agent -c config.yml -o convo.jsonl -p "And if you add 4?"
[...]
$ llmkit response -f convo.jsonl
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
llmkit agent -c <agent_config.yml> -o <convo.jsonl> -p <prompt|prompt_file> [--stream]
llmkit proxy -c <proxy_config.yml> [-l <host:port>]
llmkit response -f <conversation.jsonl>
```

If `-l` is omitted from `proxy`, it runs as a stdio MCP server (reads
JSON-RPC from stdin, writes to stdout). With `-l host:port` it serves HTTP.

The `response` command reads the given JSONL file, finds the last
`"type":"assistant"` entry, and prints its `content` field to stdout.
Returns empty output if no assistant entry exists.

### Prompt resolution (`agent`)

The `-p` argument is treated as a file path if the file exists; otherwise it
is used as the literal prompt text. An empty or whitespace-only prompt exits
with code 2.

### Stdout streaming

The `--stream` (or `-s`) flag enables real-time output on stdout:

- **Without `--stream`** (default): assistant text content is printed to stdout
  as plain text, one block per turn. Useful for simple piping.
- **With `--stream`**: structured JSONL events are emitted to stdout in addition
  to the conversation file. Each line is a JSON event (`turn_start`, `assistant`,
  `tool_call`, `tool_result`, `done`, `error`). See
  [docs/conversation-format.md](docs/conversation-format.md) for the full schema.

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
