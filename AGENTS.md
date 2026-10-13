# llmkit Agents Guide

This document gives context and instructions for agents that work on the `llmkit` project.

## Project Overview
`llmkit` is a single static-purpose executable in C for LLM interaction from the shell. It provides several interfaces: a repl, a one-off call utility, an agent runner, and MCP (Model Context Protocol) proxy and server capabilities.

## Technical Stack
- Language: C (C11)
- Dependencies: `libcjson`, `libcurl`, `pthread`
- Build system: GNU Make
- Build targets: Linux (arch, debian, rhel/fedora via prebuilt packages) and Windows. Windows builds use `tools/package.sh`.

## Project Structure
- `src/`: the source code.
    - `main.c`: entry point and subcommand dispatch (runner, call, repl, agent, mcp-repl, prettyprint, proxy, mcp servers).
    - `engine.c`: the conversation engine (turn loop, tool calls, steering).
    - `agent.c`: the agent-as-tool runner (`llmkit agent-as-tool`). The `llmkit agent` chat lives in `repl.c`.
    - `wire_openai.c`, `wire_anthropic.c`: the LLM provider wire clients (chat completions, responses, anthropic).
    - `llmproxy.c`: `llmkit proxy`, a plain-http endpoint that forwards to one upstream LLM.
    - `mcp.c`: the MCP client protocol implementation.
    - `proxy.c`: the `llmkit mcp-proxy` configuration pipeline and the stdio server.
    - `builtin.c` + `builtin/`: the `llmkit builtin-mcp` server. `builtin.c`
      holds the tool table and the json-rpc dispatch. `builtin/` has one file
      per tool family (`files.c`, `analyze.c`, `process.c`, `web.c`,
      `skills.c`) plus shared helpers (`util.c`, `internal.h`).
    - `call.c`, `repl.c`, `mcprepl.c`, `pretty.c`: the command front-ends (one-off call, interactive repl, mcp console, transcript rendering).
    - `editor.c` + `vendor/linenoise/`: interactive line editing behind the repl.
    - `platform.c`, `sse.c`, `buf.c`, `jsonl.c`: platform tty and pipes, SSE parsing, buffers, JSON Lines.
    - `prompts/`: system and tool prompts. `tools/gen-prompts.sh` bundles them into the binary at build time.
    - `vendor/regex/`: vendored regex for Windows (MinGW) builds.
- `docs/`: design, requirements, and release documentation.
- `examples/`: sample usage and scripts.
- `test/`: self-check tests.
- `tools/`: build, release, and packaging utilities.

## Build and Test
### Building
Run `make` to build the `llmkit` binary.
```bash
make
```

### Testing
Run the self-check test suite.
```bash
make check
```

## Development Guidelines
- Prompt changes: edit the `.txt` files in `src/prompts/`. The build compiles them into the binary automatically.
- Code style: follow the existing C11 patterns. Use `src/buf.c` for buffer management and `src/jsonl.c` for JSON Lines processing.
- Adding tools: add new built-in tools to `src/builtin.c` and add the matching prompt definitions in `src/prompts/mcp/`.
- Cross-platform: `src/vendor/regex` serves the Windows (MinGW) builds.
