# llmkit

A lightweight CLI tool to interact with LLMs and MCP from shell applications, for Windows, Linux and MacOS.

A quick example `config.yml`:

```yaml
llm:
  api_base: "http://127.0.0.1:8000/v1"
agent:
  system_prompt: "You are a helpful assistant. Use the calculator to do your math."
mcps:
  - name: "calculator"
    cmdline: "uvx mcp-server-calculator"
```

Usage:

```bash
$ llmkit agent -c config.yml --conversation convo.jsonl -p "How much is 3 + 3?"
3 + 3 is 6.
$ llmkit agent -c config.yml --conversation convo.jsonl -p "And if you add 4?"
If you add 4, the total is 10.

# Or run a one-shot conversation with no persisted file (discarded at the end):
$ llmkit agent -c config.yml -p "How much is 3 + 3?"
3 + 3 is 6.

```

## Modes of operation

- **`llmkit agent`** -- Runs an LLM conversation loop with MCP tool support.
  Reads a YAML config, loads conversation history from JSONL (if existing, otherwise it will create it), calls the LLM
  API, executes MCP tool calls, and writes results back to the JSONL file.
- **`llmkit proxy`** -- Runs an MCP proxy server that fronts one or more
  backend MCP servers, providing namespace isolation, rename/redefine, and
  whitelist/blacklist filtering over a single MCP endpoint (stdio or HTTP).
- **`llmkit gateway`** -- Runs an MCP gateway server that fronts one or more
  backend MCP servers but exposes only two tools: `discover` (asks the LLM to
  select the backend tools matching a natural-language query and returns
  their full specs, with a keyword fallback if the LLM is unreachable) and
  `invoke` (forwards a call to a backend tool by namespaced name). Serves
  stdio or HTTP like the proxy.
- **`llmkit mcp`** -- Runs an MCP server exposing llmkit's built-in tools
  (currently `online_search`, `online_fetch` and `file_scan`), selected
  with a comma-separated list on the command line. No config file or
  backend servers are needed. Serves stdio or HTTP like proxy/gateway.
- **`llmkit response`** -- Reads a conversation JSONL file and prints the
  last LLM assistant response to stdout. Useful for extracting the final
  answer from a completed conversation.

The binary is statically linkable on Linux and Windows (zero runtime language
dependencies), and dynamically linked on macOS (static linking is not
idiomatic on macOS). It targets Linux, macOS, and Windows (Windows via
MinGW-w64 cross-compilation).

## Usage

```bash
llmkit agent -c <agent_config.yml> [--conversation <convo.jsonl>] -p <prompt|prompt_file> [--mode <type>] [--steer] [--max-retries <n>]
llmkit proxy -c <proxy_config.yml> [-l <host:port>]
llmkit gateway -c <gateway_config.yml> [-l <host:port>]
llmkit mcp <tool[,tool...]> [-l <host:port>]
llmkit response --conversation <conversation.jsonl>
```

If `-l` (or `--listen`) is omitted from `proxy`, `gateway` or `mcp`, it runs
as a stdio MCP server (reads JSON-RPC from stdin, writes to stdout). With
`-l host:port` it serves HTTP.

### Gateway (`gateway`)

The gateway is an MCP server for clients (agents, IDEs) that should not see
every backend tool up front -- for example when the combined tool catalog is
too large to fit in the model context. It exposes exactly two tools:

- **`discover`** -- takes a natural-language `query` (e.g. `"write a file"`),
  sends it to the configured LLM together with the catalog of backend tools,
  and returns the full specs (`name`, `description`, `inputSchema`, `server`)
  of the matching tools. If the LLM call fails, it falls back to keyword
  matching (stopwords ignored) and reports `"matched_by": "keyword"`.
- **`invoke`** -- takes a namespaced tool `name` (as returned by
  `discover`) plus an `arguments` object, and passes them through to the
  real backend MCP tool unchanged.

The gateway config requires the `llm` section (used by `discover`) and at
least one entry under `mcps`. Proxy per-server controls (`namespace`,
`rename`, `redefine`, `whitelist`, `blacklist`, `hide`) apply to what
`discover` can see. See `examples/gateway-complete.yml`.

### Built-in tools (`mcp`)

`llmkit mcp` exposes tools implemented inside llmkit itself -- no config
file, backend servers, API keys or LLM endpoint are involved:

```bash
$ llmkit mcp online_search,online_fetch            # stdio MCP server
$ llmkit mcp online_search -l 127.0.0.1:8080       # or HTTP
```

The tool list is a comma-separated subset of the available tools (unknown
or duplicate names exit with code 2). Available tools:

- **`online_search`** -- Web search via DuckDuckGo's free, unauthenticated
  HTML endpoint. Takes a `query` string and returns every result as
  entries separated by blank lines, each in the form:
  `Title: [title]`, `URL: [url]`, `Description: [description]`.
  DuckDuckGo redirect links are unwrapped to the real target URLs.
- **`online_fetch`** -- Fetches an `http(s)` URL and returns its main
  content as markdown. Boilerplate (scripts, styles, navigation, footers)
  is stripped with a simplified readability pass (preferring
  `<article>`/`<main>` content) and the HTML is converted to markdown
  (headings, lists, links, images, fenced code, blockquotes, tables).
  Non-200 responses return `HTTP Status <code>`; output over 100000
  characters is truncated with a note.
- **`file_scan`** -- Searches local files, resolving the mandatory
  `filenames_glob` against the server process's working directory
  (`**` matches any number of directories, `*` and `?` match within a
  single name). Absolute paths and any `..` segment are rejected and
  symlinks are never followed, so the scan cannot leave the working
  directory. The optional `content_lines_regex` (POSIX extended) keeps
  only files with at least one matching line. Each record reports the
  relative path, the size (`b`/`Kb`/`Mb`/`Gb`), the line count for text
  files, and -- when the regex is given -- the matching line numbers
  (up to 50, a trailing `+` marks more). At most 20 records are
  returned, followed by an `<N> more files matching` note; binary files
  are listed without line counts and never match a content filter.
- **`exec`** -- Runs a shell command line in a subshell (`/bin/sh -c`,
  `cmd.exe /c` on Windows) with this server's privileges and in its
  working directory; stdin is detached and stdout+stderr are captured
  together. Once the command finishes within 10 seconds, the reply is
  an `Exit code:` / `Duration:` / `Output:` block holding the last 3KB
  of output, cut at the first newline. Longer output is kept in full
  under `.output/` and the reply gains an `Output truncated, full
  output in file .output/... (size ..., N lines)` note (no line count
  for binary output). A command still running after 10 seconds keeps
  running in the background; the reply names its pid for polling, and
  enabling `exec` automatically enables `exec_status` with it.
- **`exec_status`** -- Polls a background command by `pid`. While it is
  still running the reply is `PID <pid> still running, started
  <duration> ago.`; once it has terminated (a signal death is reported
  as 128+signal), the reply is the same report as `exec`: exit code,
  total duration and the output tail. A pid this server never started
  gets a plain `No exec process with pid <pid>.` answer.

Example stdio session:

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"online_search","arguments":{"query":"c17 standard pdf"}}}
```

The `--conversation` flag is **optional** for `agent`. When given, the agent
appends to the JSONL file and continues prior turns; when omitted, the run uses
a temporary file that is deleted at the end, so the conversation is discarded.
This is handy for one-shot prompts where you do not need to persist history.
The `response` command always requires `--conversation` (it reads an existing
file). It finds the last `"type":"assistant"` entry and prints its `content`
field to stdout. Returns empty output if no assistant entry exists.

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
conversation at the next turn boundary - the earliest point the LLM can
legally see them. This lets you course-correct a running agent (e.g. "stop
searching, just summarize what you have").

**Wire format:** messages are separated by a **blank line** (`\n\n`). A
message may span multiple lines. Carriage returns (`\r`) are stripped, so
both `\n\n` and `\r\n\r\n` work as delimiters. Anything after the last
delimiter is held until more input arrives, or flushed when stdin closes.

```bash
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
not possible - the turn boundary *is* the earliest legal and practical
delivery point.

Injected messages are written to the conversation JSONL as `"user"` entries
with `"source":"steer"`. In `debug` mode a `steer:` line is emitted; in
`stream` mode a `{"type":"steer",...}` JSONL event is emitted. Steering is
silent in `quiet` mode.

### Retries (`agent`)

The `--max-retries <n>` flag controls how many times the agent retries a
failed LLM request before giving up (default: **5**). `n` is a
non-negative integer; `0` disables retries entirely and fails immediately,
while an invalid value (non-numeric or negative) exits with code 2.

Between retries the agent waits a number of seconds that follows the
**Fibonacci sequence** — the *k*-th retry waits `fib(k)` seconds:

| Retry | 1 | 2 | 3 | 4 | 5 | 6  | 7  | 8  | 9  | 10 |
|-------|---|---|---|---|---|----|----|----|----|-----|
| Delay | 1 | 1 | 2 | 3 | 5 | 8  | 13 | 21 | 34 | 55  |

so the cumulative wait grows as 1, 2, 4, 7, 12, 20, ... seconds. A request
that ultimately fails after exhausting all retries returns exit code 4
(LLM error). In `debug` mode each attempt emits a
`retry: attempt k/n, waiting Ns` line; in `stream` mode a
`{"type":"retry",...}` JSONL event is emitted. Retries are silent in
`quiet` mode.

### Subagents (`agent`, agent-as-tool)

The optional `subagents` config key exposes nested agents to the main agent
as ordinary tools. When the LLM calls such a tool, llmkit runs a fresh
conversation with the subagent's own system/user prompts and MCP servers and
returns the subagent's final answer as the tool result:

```yaml
llm:
  api_base: "http://127.0.0.1:8000/v1"
agent:
  system_prompt: "You are a helpful assistant. Use the calculator to do your math."
subagents:
  - tool_definition:
      name: calculator
      description: A mathematic helper
      attributes:
        expression:
          type: string
          description: the expression to be evaluated
    system_prompt: "You help doing math, use the real_calculator tool to resolve expressions."
    user_prompt: "Resolve the expression {expression} using the calculator tool"
    mcps:
      - name: real_calculator
        cmdline: "uvx mcp-server-calculator"
```

Highlights:

- `tool_definition.attributes` become the tool's JSON schema, and their
  values are interpolated into `system_prompt`/`user_prompt` via
  `{attribute}` placeholders.
- Subagents always use the same `llm` configuration as the main agent.
- A subagent's `mcps` entries use the main `mcps` format; an entry with only
  a `name` is a *reference* that reuses the already-running top-level
  server, while fully-specified entries are private and started lazily on
  first use.
- Subagents can have their own `subagents` (recursive, max depth 8).
- The **full sub-conversation of every subagent run is retained in the
  conversation JSONL** as a nested trace: bracketed by `subagent_start` /
  `subagent_end` entries and stamped with `depth`, `subagent` and `run_id`
  fields, so sublevels stay inspectable and attributable. The traces are
  excluded from the LLM's own history — the main agent and each subagent
  still see exactly their own messages. See
  [docs/conversation-format.md](docs/conversation-format.md#subagent-traces-sublevels).

See [docs/configuration.md](docs/configuration.md#subagents-subagents--agent-as-tool)
for the full reference and `examples/agent-subagent.yml` for a complete
example.

## Configuration

llmkit is driven by a single YAML config file passed via `-c <config.yml>`.
Both modes (`agent` and `proxy`) share the same schema; the difference is which
root keys are accepted.

LLM endpoints support two wire protocols, selected by `llm.provider`:

- **`openai`** (default) -- any OpenAI-compatible chat-completions endpoint
  (vLLM, Ollama, LiteLLM, DeepSeek, ...): `{api_base}/chat/completions`.
- **`anthropic`** -- the Anthropic Messages API: `{api_base}/messages` with
  `x-api-key` auth and a required `max_tokens`.

```yaml
llm:
  provider: "anthropic"
  api_base: "https://api.anthropic.com/v1"
  model: "claude-sonnet-4-5"
  max_tokens: 8192
```

See [docs/configuration.md](docs/configuration.md#providers-provider) for the
full provider reference.

See [docs/configuration.md](docs/configuration.md) for the full reference:
the `llm`, `mcps`, `agent`, and `subagents` fields, all MCP server options,
and example agent/proxy configs.

## Exit codes

All commands use the same numeric scheme:

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

```bash
make            # native binary -> ./llmkit
make debug      # ASAN + debug symbols
make test       # all unit + integration tests
make windows    # cross-compile 64-bit Windows .exe -> llmkit.exe
make windows32  # cross-compile 32-bit Windows .exe
make macos      # native build on macOS (must run on a Mac)
make dist       # release archives for all platforms buildable on this host
make install    # copy binary to /usr/local/bin
```

The build runs an ASCII-source check, clang-format verification, and
clang-tidy linting before compiling. These can be skipped by building the
target directly: `make llmkit`.

#### macOS notes

macOS cannot be cross-compiled from Linux (no standard toolchain like
MinGW exists for it), so `make macos` runs a **native** build and refuses to
run anywhere else. To build on a Mac:

```bash
brew install pkg-config libyaml openssl@3
make macos          # sets the Homebrew openssl pkg-config path automatically
```

macOS ships **LibreSSL**, not OpenSSL, and Homebrew's `openssl@3` is
keg-only, so `<openssl/sha.h>` is only found via `pkg-config`. `make macos`
handles this for you. Note that the format/lint gates require `clang-format`
and `clang-tidy` (not in the Xcode Command Line Tools); install LLVM
(`brew install llvm`) to run the full `make`, or use `make macos` which builds
the binary directly.

#### Release archives (`make dist`)

`make dist` produces one archive per platform that can be built on the
current host:

- **Linux host**: `llmkit-linux-x86_64-<ver>.tar.gz` (native) and
  `llmkit-windows-x86_64-<ver>.zip` (cross-compiled) in `dist/`.
- **macOS host**: `llmkit-macos-x86_64-<ver>.tar.gz` (native) in `dist/`.

Because the macOS binary can only be built on macOS, produce it from a
macOS host (`make dist`) or a macOS CI runner and attach it to the same
release. Archives contain the stripped binary plus `README.md`,
`LICENSE.md`, and `docs/`.

## Activity logging

Progress lines (`[init]`, `[progress]`, `[tool]`, `[done]`, `[error]`) are
written to stderr and are automatically suppressed when stderr is not a TTY,
keeping stdout/stderr pipes clean for the JSONL file and exit codes.

In **quiet mode** (`--mode quiet`, the default), these progress lines are
suppressed entirely - even when stderr is a TTY - so only the final assistant
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

This project is licensed under the GNU General Public License v3.0 or later - see the [LICENSE](LICENSE.md) file for details.

Copyright (C) 2026 Daniele Guttuso
