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
    - `main.c`: Entry point and command dispatch.
    - `agent.c`: Agent logic and workflow.
    - `engine.c`: Core LLM orchestration engine.
    - `llmproxy.c`: Interface for communicating with LLM providers.
    - `mcp.c`: MCP protocol implementation.
    - `builtin.c`: Implementation of built-in MCP tools (file system, process, web).
    - `repl.c`: Interactive terminal interface.
    - `prompts/`: System and tool prompts (bundled into binary via `tools/gen-prompts.sh`).
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

