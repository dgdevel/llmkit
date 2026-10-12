# llmkit Agents Guide

This document provides context and instructions for agents working on the `llmkit` project.

## Project Overview
`llmkit` is a single static-purpose executable in C designed for LLM interaction from the shell. It provides various interfaces including a REPL, a one-off call utility, an agent runner, and MCP (Model Context Protocol) proxy/server capabilities.

## Technical Stack
- **Language:** C (C11)
- **Dependencies:** `libcjson`, `libcurl`, `pthread`
- **Build System:** GNU Make
- **Build Targets:** Linux (arch, debian, rhel/fedora via prebuilt packages) and Windows, uses `tools/package.sh`

## Project Structure
- `src/`: Core source code.
    - `main.c`: Entry point and subcommand dispatch (runner, call, repl, agent, mcp-repl, prettyprint, proxy, mcp servers).
    - `engine.c`: Core conversation engine (turn loop, tool calls, steering).
    - `agent.c`: agent-as-tool runner (`llmkit agent-as-tool`); the `llmkit agent` chat lives in `repl.c`.
    - `wire_openai.c`, `wire_anthropic.c`: LLM provider wire clients (chat completions, responses, anthropic).
    - `llmproxy.c`: `llmkit proxy`, a plain-http endpoint forwarding to one upstream LLM.
    - `mcp.c`: MCP client protocol implementation.
    - `proxy.c`: `llmkit mcp-proxy` config pipeline and stdio server.
    - `builtin.c` + `builtin/`: `llmkit builtin-mcp` server: `builtin.c`
      holds the tool table and json-rpc dispatch, `builtin/` one file
      per tool family (`files.c`, `analyze.c`, `process.c`, `web.c`,
      `skills.c`) plus shared helpers (`util.c`, `internal.h`).
    - `call.c`, `repl.c`, `mcprepl.c`, `pretty.c`: Command front-ends (one-off call, interactive repl, mcp console, transcript rendering).
    - `editor.c` + `vendor/linenoise/`: interactive line editing behind the repl.
    - `platform.c`, `sse.c`, `buf.c`, `jsonl.c`: platform tty/pipes, SSE parsing, buffers, JSON Lines.
    - `prompts/`: System and tool prompts (bundled into binary via `tools/gen-prompts.sh`).
    - `vendor/regex/`: vendored regex for Windows (MinGW) builds.
- `docs/`: Design, requirements, and release documentation.
- `examples/`: Sample usage and scripts.
- `test/`: Self-check tests.
- `tools/`: Build, release, and packaging utilities.

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
- **Prompt Changes:** Modify `.txt` files in `src/prompts/`. These are automatically compiled into the binary during the build process.
- **Code Style:** Follow existing C11 patterns. Use `src/buf.c` for buffer management and `src/jsonl.c` for JSON Lines processing.
- **Adding Tools:** New built-in tools should be added to `src/builtin.c` and have corresponding prompt definitions in `src/prompts/mcp/`.
- **Cross-Platform:** Note that `src/vendor/regex` is used for Windows compatibility (MinGW).

