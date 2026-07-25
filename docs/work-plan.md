# LLMKIT — Work Plan

## Overview

Build a lightweight C CLI binary with two modes: `agent` (LLM conversation loop with MCP tool support) and `proxy` (MCP proxy with namespace isolation, filtering, rename/redefine). Single-threaded, blocking I/O, static linking, zero runtime dependencies beyond libyaml + libcurl + cJSON + OpenSSL.

---

## Phase 1 — Project Scaffold & Build System (Day 1)

| Task | File(s) | Detail |
|------|---------|--------|
| 1.1 Directory layout | `Makefile`, `src/`, `tests/`, `scripts/`, `vendor/` | Create full directory structure per spec §4.5 |
| 1.2 Makefile | `Makefile` | Targets: `all`, `debug`, `profile`, `test`, `clean`, `install`, `dist`, `windows`, `windows32`, `vendors`, `check-deps`. Auto-detect libs via pkg-config. Static linking flags. |
| 1.3 Cross-compile Makefile | `Makefile.cross` | MinGW-w64 targets: `x86_64-w64-mingw32` and `i686-w64-mingw32`. Flags: `-static -static-libgcc -static-libstdc++ -lws2_32 -lbcrypt`. |
| 1.4 ASCII checker | `scripts/check-ascii.sh` | Scan source files for bytes >0x7F. GCC-format error output. Aborts build on violation. |
| 1.5 Vendorfetch | `Makefile` (vendors target) | Download cJSON single-file distribution into `src/vendor/` if not found via pkg-config. |

**Deliverable:** `make` produces `llmkit` binary (stub main), `make windows` produces `llmkit.exe`.

---

## Phase 2 — Shared Types & Utilities (Day 1–2)

| Task | File(s) | Detail |
|------|---------|--------|
| 2.1 Shared types | `src/llmkit.h` | Exit code macros, enums (`mcp_transport_type`, `timeout_behavior`, `entry_type`), structs (`mcp_server_cfg`, `llm_cfg`, `agent_cfg`, `tool_def`, `runtime_ctx`). Per spec §2.1. |
| 2.2 UTF-8 validation | `src/utf8.c`, `src/utf8.h` | `utf8_validate()` and `utf8_validate_c_string()`. Manual byte walk per RFC 3629. Reject overlong sequences, surrogates, codepoints >U+10FFFF. |
| 2.3 Duration parser | `src/util.c`, `src/util.h` | `util_parse_duration()`. Parse `"30s"` → 30000, `"10m"` → 600000, `"1h"` → 3600000. Return -1 on error. |
| 2.4 Timestamp | `src/util.c` | `util_timestamp_now()`. ISO 8601 UTC via `gmtime_r()`/`gmtime_s()`. |
| 2.5 UUID v4 | `src/util.c` | `util_uuid_v4()`. 16 random bytes → hex with dashes. `/dev/urandom` / `arc4random_buf()` / `CryptGenRandom()`. |
| 2.6 SHA256 | `src/util.c` | `util_sha256()`. OpenSSL `SHA256()` → hex string `"sha256:..."`. |
| 2.7 File reader | `src/util.c` | `util_read_file()`. Read entire file into malloc'd buffer. |
| 2.8 Safe strdup | `src/util.c` | `util_strdup()`. `strdup` with OOM → exit(7). |
| 2.9 Activity logger | `src/util.c` | `log_activity()`. Write `[tag] message\n` to stderr only when TTY. Tags: `init`, `progress`, `tool`, `done`. |

**Deliverable:** All utility functions unit-testable. Tests in `tests/test_utf8.c`, `tests/test_util.c`.

---

## Phase 3 — Platform Abstraction Layer (Day 2–3)

| Task | File(s) | Detail |
|------|---------|--------|
| 3.1 Platform header | `src/platform.h` | Opaque types `platform_process`, `platform_pipe`. Function declarations for spawn, kill, wait, pipe I/O, randomness, timestamps, TCP listen/accept, TTY check. Single `#ifdef _WIN32` boundary. |
| 3.2 POSIX implementation | `src/platform.c` | `fork()`+`execvp()`, `pipe()`+`poll()`, `waitpid()`, `kill(SIGTERM)`, `/dev/urandom`, `gmtime_r()`, POSIX sockets, `isatty()`. |
| 3.3 Windows implementation | `src/platform.c` | `CreateProcess()` with `STARTF_USESTDHANDLES`, `CreatePipe()`+`WaitForMultipleObjects()`, `WaitForSingleObject()`, `TerminateProcess()`, `CryptGenRandom()`, `gmtime_s()`, Winsock2 with `WSAStartup()`, `_isatty()`. |

**Deliverable:** `tests/test_platform.c` can exercise all APIs (may need platform-specific test harness). No `#ifdef _WIN32` outside `platform.c`.

---

## Phase 4 — YAML Configuration (Day 3–4)

| Task | File(s) | Detail |
|------|---------|--------|
| 4.1 Config loader | `src/config.c`, `src/config.h` | `config_load(path, ctx)`. Parse YAML via libyaml, walk document tree, populate `runtime_ctx`. |
| 4.2 Field extraction | `src/config.c` | Extract `llm.*`, `mcps[]` entries, `agent.system_prompt`. Handle defaults (model="gpt-4o-mini", init_timeout="30s", call_timeout="10m", etc.). |
| 4.3 Validation | `src/config.c` | `llm.api_base` required for agent mode. `mcps` required for proxy mode. Each mcp needs `name` + `cmdline`/`url`. Duration fields parsed. UTF-8 validation on all strings. |
| 4.4 Config free | `src/config.c` | `config_free()`. Free all allocated strings and arrays. |
| 4.5 Header key-value parsing | `src/config.c` | Parse `headers` maps into `key=value` NULL-terminated arrays. Same for `rename`, `redefine`, whitelist, blacklist. |

**Deliverable:** `tests/test_config.c` with YAML fixtures. `config_load()` returns appropriate exit codes on invalid input.

---

## Phase 5 — JSON-RPC 2.0 (Day 4)

| Task | File(s) | Detail |
|------|---------|--------|
| 5.1 Request builder | `src/jsonrpc.c`, `src/jsonrpc.h` | `jsonrpc_build_request(method, params_json, id)`. Build `{"jsonrpc":"2.0","id":"...","method":"...","params":...}` via cJSON. |
| 5.2 Response parser | `src/jsonrpc.c` | `jsonrpc_parse_response()`. Extract `result` or `error.message`. |
| 5.3 MCP helpers | `src/jsonrpc.c` | `jsonrpc_build_initialize()`, `jsonrpc_build_list_tools()`, `jsonrpc_build_call_tool()`, `jsonrpc_build_list_resources()`, `jsonrpc_build_list_prompts()`. |
| 5.4 ID generation | `src/jsonrpc.c` | Incrementing numeric ID or UUID-based ID for requests. |

**Deliverable:** `tests/test_jsonrpc.c` validates request/response round-trips.

---

## Phase 6 — MCP Transports (Day 4–6)

| Task | File(s) | Detail |
|------|---------|--------|
| 6.1 Transport header | `src/mcp_transport.h` | Declare `transport_open()`, `transport_send()`, `transport_close()`. |
| 6.2 stdio transport | `src/mcp_transport.c` | Spawn process via `platform_process_spawn()`, communicate via pipes. init_timeout applied on open. call_timeout on send. |
| 6.3 HTTP transport | `src/mcp_transport.c` | POST via libcurl easy. Headers merged from config. Timeout via `CURLOPT_TIMEOUT_MS`. |
| 6.4 SSE transport | `src/mcp_transport.c` | SSE connect via libcurl with write callback. POST requests base URL. Reconnect on disconnect (up to max_reconnect_attempts). Session ID tracking. |
| 6.5 Connection struct | `src/mcp_transport.h` | `mcp_connection` struct with union of stdio/http/sse state. |

**Deliverable:** Each transport implementation testable with a mock MCP server.

---

## Phase 7 — MCP Server Lifecycle (Day 6–7)

| Task | File(s) | Detail |
|------|---------|--------|
| 7.1 Connect all | `src/mcp.c`, `src/mcp.h` | `mcp_connect_all()`. Open transport + initialize handshake for each configured server. Fail with exit code 6 on init_timeout. |
| 7.2 Initialize handshake | `src/mcp.c` | `mcp_initialize()`. Send `initialize` request, validate protocol version in response. |
| 7.3 Discover tools | `src/mcp.c` | `mcp_discover_tools()`. Call `tools/list` on each server, build `tool_def` array with namespaced names (`{server}.{name}`). |
| 7.4 Call tool | `src/mcp.c` | `mcp_call_tool()`. Dispatch `tools/call` to specific server. Handle timeout per `call_timeout_behavior`. |
| 7.5 Disconnect all | `src/mcp.c` | `mcp_disconnect_all()`. Kill processes, close connections, free resources. SIGTERM + wait. |

**Deliverable:** Agent mode can connect to MCP servers and discover tools.

---

## Phase 8 — Conversation JSONL (Day 7–8)

| Task | File(s) | Detail |
|------|---------|--------|
| 8.1 File open | `src/conversation.c`, `src/conversation.h` | `conversation_open()`. Open for append, create if not exists. Validate UTF-8 of existing content. |
| 8.2 Write meta | `src/conversation.c` | `conversation_write_meta()`. Write meta entry with version, timestamp, config_hash, run_id. |
| 8.3 Write entries | `src/conversation.c` | `conversation_write_entry()`. Write type+timestamp+fields per spec §3.2–3.6. Flush after each write. |
| 8.4 Reconstruct messages | `src/conversation.c` | `conversation_reconstruct()`. Read all lines, skip meta/error, build message array for LLM API. Group tool_call with preceding assistant. |
| 8.5 Free messages | `src/conversation.c` | `conversation_free_messages()`. Free reconstructed array. |

**Deliverable:** `tests/test_conversation.c` validates round-trip: write entries → reconstruct → correct message order.

---

## Phase 9 — LLM API Client (Day 8–9)

| Task | File(s) | Detail |
|------|---------|--------|
| 9.1 Chat complete | `src/llm.c`, `src/llm.h` | `llm_chat_complete()`. POST to `{api_base}/chat/completions` via libcurl. Build request body JSON with messages + tools. Parse response for content, tool_calls, model, usage. |
| 9.2 Request builder | `src/llm.c` | Build JSON body: `model`, `messages[]`, `tools[]` (with namespaced names + schemas), `tool_choice="auto"`. |
| 9.3 Response parser | `src/llm.c` | Parse `choices[0].message`. Extract `content` (nullable), `tool_calls[]` (id, function.name, function.arguments), `model`, `usage`. |
| 9.4 Error handling | `src/llm.c` | Non-2xx → parse error body, return exit code 4. Network errors → exit code 4. Invalid UTF-8 in response → exit code 4. |

**Deliverable:** Agent mode can call LLM API and receive responses.

---

## Phase 10 — Agent Mode (Day 9–11)

| Task | File(s) | Detail |
|------|---------|--------|
| 10.1 Main CLI dispatch | `src/main.c` | Parse `agent`/`proxy` subcommand, flags `-c`, `-o`, `-p`, `-l`. Dispatch to `agent_run()` or `proxy_run()`. Manual loop, no getopt. |
| 10.2 Agent entry | `src/agent.c`, `src/agent.h` | `agent_run()`. Call startup_sequence → conversation_loop → cleanup. |
| 10.3 Startup sequence | `src/agent.c` | Parse CLI args → load config → open convo file → write meta entry → resolve prompt (file or literal) → write user entry. |
| 10.4 Conversation loop | `src/agent.c` | reconstruct messages → discover tools → call LLM → write assistant entry → if tool_calls: sequentially execute each via mcp_call_tool, write tool_call + tool_result entries, loop → else: exit 0. |
| 10.5 Prompt resolution | `src/agent.c` | Check if `-p` is existing file → read it. Otherwise use as literal. Trim trailing newline. Empty → exit code 2. |
| 10.6 Error handling | `src/agent.c` | On error at any step: write error entry to JSONL, cleanup (close MCP connections, close file), return appropriate exit code. |

**Deliverable:** `llmkit agent -c config.yml -o convo.jsonl -p "hello"` runs end-to-end.

---

## Phase 11 — Proxy Mode (Day 11–14)

| Task | File(s) | Detail |
|------|---------|--------|
| 11.1 Proxy entry | `src/proxy.c`, `src/proxy.h` | `proxy_run()`. Start stdio loop (if no `-l`) or HTTP listener (if `-l` given). |
| 11.2 stdio proxy loop | `src/proxy.c` | Read JSON-RPC from stdin line by line → `handle_mcp_request()` → write response to stdout. |
| 11.3 HTTP proxy loop | `src/proxy.c` | TCP listen on `host:port`. Accept connections. Handle POST `/mcp` and optional SSE GET. libcurl for backend calls. |
| 11.4 Request router | `src/proxy.c` | `handle_mcp_request()`. Parse incoming JSON-RPC, identify method, route to correct backend with name translation. |
| 11.5 Namespace translation | `src/proxy.c` | `translate_names_forward()`: strip `{namespace}.` prefix from tool/resource/prompt names. `translate_names_reverse()`: prepend namespace in list responses. |
| 11.6 Whitelist/Blacklist | `src/proxy.c` | `apply_filters()`. Check namespaced names against whitelist/blacklist arrays. |
| 11.7 Rename | `src/proxy.c` | Apply `rename` map after filtering. |
| 11.8 Redefine | `src/proxy.c` | Apply `redefine` map (description override) after rename. |
| 11.9 Hide | `src/proxy.c` | If `hide: true`, server not exposed (initialization still happens for agent mode, but tools not listed). |

**Deliverable:** `llmkit proxy -c config.yml -l 0.0.0.0:8080` serves proxy endpoint.

---

## Phase 12 — Integration & Testing (Day 14–16)

| Task | Detail |
|------|--------|
| 12.1 Unit tests | Complete `tests/test_utf8.c`, `tests/test_jsonrpc.c`, `tests/test_config.c`, `tests/test_conversation.c`, `tests/test_util.c`. Test Makefile target compiles and runs them. |
| 12.2 Integration test: agent | Script that starts a stdio MCP server, runs `llmkit agent` with a mock LLM endpoint, and validates JSONL output. |
| 12.3 Integration test: proxy | Script that starts an `llmkit proxy` with multiple backend MCP servers (stdio + http), connects a client, and validates namespace translation. |
| 12.4 Cross-compile test | `make windows` succeeds on Linux. Smoke test the .exe under Wine (if available). |
| 12.5 Edge cases | Config with all optional fields omitted. UTF-8 rejection cases. Empty prompt. MCP init timeout. Tool call timeout with `fail` vs `continue`. SSE disconnect/reconnect. Large tool responses. |
| 12.6 Memory leak check | Run agent loop under valgrind (or ASAN) with multiple turns. |

**Deliverable:** `make test` passes. `make windows` produces .exe.

---

## Phase 13 — Polish & Release (Day 16–17)

| Task | Detail |
|------|--------|
| 13.1 Stderr activity output | Verify all log_activity() calls per spec §4.6. Suppress when stderr not a TTY. |
| 13.2 Exit code audit | Verify every error path returns correct exit code per spec. |
| 13.3 Config hash correctness | Verify SHA256 of raw YAML content (not parsed/formatted). |
| 13.4 Binary size check | Target < 2 MB statically linked. Strip binary. |
| 13.5 Startup time check | Target < 50 ms (excluding MCP spawn). |
| 13.6 README | Brief usage doc. |
| 13.7 `make dist` | Produces release tarball. |

**Deliverable:** `make dist` produces `llmkit-<version>.tar.gz`.

---

## Dependencies Between Phases

```
Phase 1 (Scaffold)
   ↓
Phase 2 (Types/Utils) ──────────────────────────┐
   ↓                                               ↓
Phase 3 (Platform) → Phase 4 (Config) → Phase 5 (JSON-RPC)
   ↓                       ↓               ↓
   ↓                   Phase 6 (Transports) ←┘
   ↓                       ↓
   ↓                  Phase 7 (MCP Lifecycle)
   ↓                       ↓
   ↓                  Phase 8 (Conversation JSONL)
   ↓                       ↓
   ↓                  Phase 9 (LLM Client)
   ↓                       ↓
   ↓                  Phase 10 (Agent Mode) ←── Phase 8 + Phase 9 + Phase 7
   ↓                       ↓
   ↓                  Phase 11 (Proxy Mode) ←── Phase 7 + Phase 5 + Phase 4
   ↓                       ↓
   └────────────────── Phase 12 (Testing)
                           ↓
                      Phase 13 (Polish)
```

Phases 2–3 can parallelize partially (types/header first, then util + platform concurrently). Phases 5–6 can be started once Phase 2 is complete. Phases 8–9 are independent of each other (both depend on Phase 2 only). Phase 10 depends on Phases 7, 8, 9. Phase 11 depends on Phases 5, 7.
