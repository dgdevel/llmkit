# LLMKIT — Technical Specification

## 1. Project Overview

LLMKIT is a lightweight C CLI tool with three modes of operation:

- **`llmkit agent`** — Runs an LLM conversation loop with MCP tool support. Reads a YAML config, loads conversation history from JSONL, calls the LLM API, executes MCP tool calls, and writes results back to the JSONL file.
- **`llmkit proxy`** — Runs an MCP proxy server that fronts one or more backend MCP servers, providing namespace isolation, rename/redefine, and whitelist/blacklist filtering over a single MCP endpoint (stdio or HTTP).
- **`llmkit response`** — Reads a conversation JSONL file and prints the last assistant response content to stdout. Used to extract the final LLM answer from a completed conversation.

The binary is statically linked, has zero runtime language dependencies, and targets Linux, macOS, and Windows (via MinGW-w64 cross-compilation).

---

## 2. File Layout and Function Inventory

```
src/
├── main.c              — Entry point, CLI dispatch
├── agent.c             — Agent conversation loop
├── agent.h
├── proxy.c             — MCP proxy server (stdio/HTTP listeners)
├── proxy.h
├── config.c            — YAML configuration loading & validation
├── config.h
├── llm.c               — LLM API client (OpenAI-compatible chat completions)
├── llm.h
├── mcp.c               — MCP server lifecycle & transport abstraction
├── mcp.h
├── jsonrpc.c           — JSON-RPC 2.0 message construction/parsing
├── jsonrpc.h
├── conversation.c      — JSONL conversation file read/write/reconstruct
├── conversation.h
├── mcp_transport.c     — Transport implementations (stdio, http, sse)
├── mcp_transport.h
├── utf8.c              — UTF-8 validation
├── utf8.h
├── util.c              — Timestamp, UUID v4, SHA256, duration parsing
├── util.h
├── platform.c          — Platform abstraction (process spawn, pipe, signals)
├── platform.h
└── llmkit.h            — Shared types, enums, constants
```

### 2.1 `llmkit.h` — Shared Types and Macros

```c
// Exit codes
#define EXIT_SUCCESS        0
#define EXIT_CONFIG_ERR     1
#define EXIT_ARGS_ERR       2
#define EXIT_FILE_ERR       3
#define EXIT_LLM_ERR        4
#define EXIT_MCP_ERR        5
#define EXIT_MCP_INIT_ERR   6
#define EXIT_INTERNAL_ERR   7

// Transport type
typedef enum { MCP_STDIO, MCP_HTTP, MCP_SSE } mcp_transport_type;

// Call timeout behavior
typedef enum { TIMEOUT_FAIL, TIMEOUT_CONTINUE } timeout_behavior;

// Conversation entry types
typedef enum { ENTRY_META, ENTRY_USER, ENTRY_ASSISTANT, ENTRY_TOOL_CALL,
               ENTRY_TOOL_RESULT, ENTRY_ERROR } entry_type;

// MCP server config (parsed from YAML)
typedef struct {
    char            *name;
    mcp_transport_type transport;
    char            *cmdline;       // stdio
    char            *url;           // http/sse
    char            **headers;      // key=value pairs, NULL-terminated
    int64_t         init_timeout_ms;
    int64_t         call_timeout_ms;
    timeout_behavior call_timeout_beh;
    bool            hide;
    char            *namespace;
    char            **rename_keys;  // NULL-terminated, "orig\0new"
    char            **redefine_keys;
    char            **whitelist;
    char            **blacklist;
    int             max_reconnect;  // sse only
    int64_t         reconnect_delay_ms;
} mcp_server_cfg;

// LLM config
typedef struct {
    char *api_base;
    char *api_key;
    char *model;
    char **headers;
} llm_cfg;

// Agent config
typedef struct {
    char *system_prompt;
} agent_cfg;

// Tool definition (cached from tools/list)
typedef struct {
    char *name;         // namespaced
    char *original;     // backend name
    char *description;
    char *input_schema; // JSON string
    char *mcp_server;   // backend server name
} tool_def;

// Runtime context
typedef struct {
    llm_cfg         llm;
    mcp_server_cfg  *mcps;
    int             mcp_count;
    agent_cfg       agent;
    tool_def        *tools;
    int             tool_count;
    char            *convo_path;
    char            *prompt;
    char            *prompt_source; // "cli" or "file"
    char            config_hash[65];
    char            run_id[37];     // UUID v4 string
} runtime_ctx;
```

### 2.2 `main.c` — Entry Point

| Function | Purpose |
|----------|---------|
| `main(int argc, char **argv)` | Parse subcommand and flags. Dispatch to `agent_run()`, `proxy_run()`, or `response_run()`. |

Logic:
1. Check `argv[1]` for subcommand (`agent` or `proxy` or `response`)
2. Parse remaining args with manual loop (no getopt dependency)
3. For `agent`: load YAML config via `config_load()`. If `--conversation` is omitted, synthesize a temporary JSONL path (`<tmpdir>/llmkit-<run_id>.jsonl`) and delete it after the run, so the conversation is discarded. Call `agent_run(ctx, convo_path, prompt)`
4. For `proxy`: load YAML config via `config_load()`, call `proxy_run(ctx, listen_addr)`
5. For `response`: parse `-f` flag, call `conversation_read_last_assistant()`, print to stdout

### 2.3 `config.c` / `config.h` — YAML Config

| Function | Purpose |
|----------|---------|
| `int config_load(const char *path, runtime_ctx *ctx)` | Parse YAML file, populate ctx. Validate required fields. Validate UTF-8. Returns 0 on success, exit code on error (1 config err, 7 internal). |
| `void config_free(runtime_ctx *ctx)` | Free all allocated config memory. |

Internally uses `libyaml` to parse. Walks the YAML document tree extracting:
- `llm.api_base`, `llm.api_key`, `llm.model`, `llm.headers`
- `mcps[]` array entries → `mcp_server_cfg` structs
- `agent.system_prompt`

Validation rules:
- `llm.api_base` must be present for agent mode (non-empty)
- `mcps` must be present for proxy mode (at least one entry)
- Each mcp entry must have `name`, and one of `cmdline` (stdio) or `url` (http/sse)
- Duration fields parsed via `util_parse_duration()`
- UTF-8 validated on all string values via `utf8_validate()`

### 2.4 `agent.c` / `agent.h` — Agent Loop

| Function | Purpose |
|----------|---------|
| `int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt)` | Main agent entry point. |
| `static int startup_sequence(runtime_ctx *ctx, const char *convo_path, const char *prompt)` | Steps 4.1.1–4.1.7. |
| `static int conversation_loop(runtime_ctx *ctx)` | Steps 4.2.1–4.2.6. |

**`agent_run` flow:**
1. `startup_sequence()` — open convo file, write meta entry, write user entry
2. `conversation_loop()` — repeat until LLM returns no tool_calls or error
3. Cleanup — close MCP connections, close convo file

**`conversation_loop` flow:**
1. Read JSONL → build message array via `conversation_reconstruct()`
2. Discover tools via `mcp_discover_tools()`
3. Call `llm_chat_complete()` with messages + tools
4. Write assistant entry via `conversation_write_entry()`
5. If tool_calls in response:
   - For each: `mcp_call_tool()` sequentially
   - Write tool_call + tool_result entries
   - Loop back to 1
6. Else: exit 0

### 2.5 `proxy.c` / `proxy.h` — MCP Proxy

| Function | Purpose |
|----------|---------|
| `int proxy_run(runtime_ctx *ctx, const char *listen_addr)` | Main proxy entry point. |
| `static int proxy_loop_stdio(runtime_ctx *ctx)` | stdio MCP server loop (read JSON-RPC from stdin, write to stdout). |
| `static int proxy_loop_http(runtime_ctx *ctx, const char *addr)` | HTTP server loop (listen on addr, handle POST /mcp and SSE GET). |
| `static void handle_mcp_request(runtime_ctx *ctx, const char *req_json, char **out_resp)` | Route incoming MCP request to backend(s) with name translation. |
| `static void translate_names_forward(mcp_server_cfg *cfg, jsonrpc_request *req)` | Strip namespace prefix from tool/resource/prompt names. |
| `static void translate_names_reverse(mcp_server_cfg *cfg, jsonrpc_response *resp)` | Prepend namespace prefix in list responses. |
| `static bool apply_filters(mcp_server_cfg *cfg, const char *namespaced_name)` | Check whitelist/blacklist. |

**Namespacing translation:**
- Forward path (incoming client → proxy → backend):
  - For `tools/call`: strip `{namespace}.` prefix from `params.name`
  - For `resources/read`: strip `{namespace}.` prefix from `params.uri`
  - For `prompts/get`: strip `{namespace}.` prefix from `params.name`
- Reverse path (backend → proxy → client):
  - For `tools/list` response: prepend `{namespace}.` to each `name` in `result.tools[]`
  - For `resources/list`: prepend to `name` and `uri`
  - For `prompts/list`: prepend to `name`

**Whitelist/Blacklist:** Applied after namespacing. If whitelist non-empty, only items in whitelist pass. If blacklist non-empty, items in blacklist are removed.

**Rename:** Map `redefine_keys` entries: key = namespaced name, value = new exposed name. Applied after namespacing and filtering.

**Redefine:** Map `redefine_keys` entries: key = namespaced name, value = new description. Applied after namespacing and filtering, after rename.

### 2.6 `llm.c` / `llm.h` — LLM API Client

| Function | Purpose |
|----------|---------|
| `int llm_chat_complete(runtime_ctx *ctx, json_message *messages, int msg_count, tool_def *tools, int tool_count, char **out_content, char **out_model, tool_call **out_calls, int *out_call_count, usage_info *usage)` | POST to `{api_base}/chat/completions`. Parse response. |
| `static char *build_request_body(json_message *msgs, int msg_count, tool_def *tools, int tool_count, llm_cfg *cfg)` | Build JSON request body. |
| `static int parse_response(const char *body, char **content, char **model, tool_call **calls, int *call_count, usage_info *usage)` | Parse JSON response. |

Uses libcurl for HTTP POST with:
- URL: `{api_base}/chat/completions`
- Headers: `Authorization: Bearer {api_key}`, `Content-Type: application/json`, plus user headers
- Body: JSON with `model`, `messages[]`, `tools[]`, `tool_choice="auto"`
- Response: parsed for `choices[0].message.content`, `.tool_calls`, `.model`, `.usage`

Error handling: non-2xx → write error entry, exit code 4.

### 2.7 `mcp.c` / `mcp.h` — MCP Server Lifecycle

| Function | Purpose |
|----------|---------|
| `int mcp_connect_all(runtime_ctx *ctx)` | Spawn/connect to all MCP servers. |
| `int mcp_discover_tools(runtime_ctx *ctx)` | Call `tools/list` on each server, build tool_def array with namespaced names. |
| `int mcp_call_tool(runtime_ctx *ctx, const char *server_name, const char *tool_name, const char *args_json, char **out_result, bool *out_is_error)` | Call `tools/call` on specific server. |
| `void mcp_disconnect_all(runtime_ctx *ctx)` | Kill processes, close connections. |
| `int mcp_initialize(mcp_server_cfg *cfg, mcp_connection *conn)` | Perform MCP initialize handshake. |

Manages `mcp_connection` array — one per configured server:
```c
typedef struct {
    mcp_server_cfg   *cfg;
    mcp_transport_type transport;
    // stdio
    platform_process  proc;
    platform_pipe     pipe_in;   // write to child stdin
    platform_pipe     pipe_out;  // read from child stdout
    // http/sse
    CURL             *curl;
    char             *base_url;
    // sse
    char             *session_id;
    // common
    bool              initialized;
} mcp_connection;
```

### 2.8 `mcp_transport.c` / `mcp_transport.h` — Transports

| Function | Purpose |
|----------|---------|
| `int transport_open(mcp_server_cfg *cfg, mcp_connection *conn)` | Open transport (spawn process, connect HTTP/SSE). |
| `int transport_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms, char **out_resp)` | Send JSON-RPC request, read response. |
| `void transport_close(mcp_connection *conn)` | Close transport. |
| `static int transport_stdio_open(mcp_server_cfg *cfg, mcp_connection *conn)` | `fork()` + `execvp()`, pipe stdin/stdout. |
| `static int transport_http_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms, char **out_resp)` | `curl_easy_perform()` POST. |
| `static int transport_sse_open(mcp_connection *conn)` | `curl_easy_perform()` with SSE callback. |
| `static int transport_sse_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms, char **out_resp)` | POST to base URL using stored session. |

**stdio transport:**
- `transport_open`: `platform_process_spawn(cfg->cmdline, &proc, &pipe_in, &pipe_out)`
- `transport_send`: `platform_pipe_write(&pipe_in, req_json)` + `platform_pipe_read(&pipe_out)` with timeout
- `transport_close`: `platform_process_kill(&proc)`, `platform_process_wait(&proc)`, close pipes

**http transport:**
- `transport_open`: just store URL (lazy init)
- `transport_send`: `curl_easy_setopt()` with URL, POST body, headers, timeout. Sync call.

**sse transport:**
- `transport_open`: start SSE connection with curl, register session callback
- `transport_send`: POST JSON-RPC to base URL, receive response
- Reconnect logic: on disconnect, retry up to `max_reconnect_attempts` with `reconnect_delay`

### 2.9 `jsonrpc.c` / `jsonrpc.h` — JSON-RPC 2.0

| Function | Purpose |
|----------|---------|
| `char *jsonrpc_build_request(const char *method, const char *params_json, const char *id)` | Build `{"jsonrpc":"2.0","id":"...","method":"...","params":...}`. |
| `int jsonrpc_parse_response(const char *resp_json, char **out_result, char **out_error)` | Parse response, extract `result` or `error.message`. |
| `int jsonrpc_build_initialize(char **out_json)` | Build `initialize` request with protocol version + client capabilities. |
| `int jsonrpc_build_list_tools(char **out_json)` | Build `tools/list` request. |
| `int jsonrpc_build_call_tool(const char *name, const char *args_json, char **out_json)` | Build `tools/call` request. |
| `int jsonrpc_build_list_resources(char **out_json)` | Build `resources/list` request. |
| `int jsonrpc_build_list_prompts(char **out_json)` | Build `prompts/list` request. |

All functions use cJSON for JSON construction/parsing. Return value indicates success (0) or memory error (non-zero).

### 2.10 `conversation.c` / `conversation.h` — JSONL File

| Function | Purpose |
|----------|---------|
| `int conversation_open(const char *path, FILE **out_fp)` | Open file for append ("a"), create if not exists. Validate UTF-8 of existing content. |
| `int conversation_write_meta(FILE *fp, const char *config_hash, const char *run_id)` | Write meta JSON entry. |
| `int conversation_write_entry(FILE *fp, entry_type type, ...)` | Write typed entry to JSONL. |
| `int conversation_reconstruct(const char *path, json_message **out_msgs, int *out_count)` | Read all lines, skip meta/error, build message array for LLM API. |
| `void conversation_free_messages(json_message *msgs, int count)` | Free reconstructed message array. |
| `int conversation_read_last_assistant(const char *path, char **out_content)` | Read file line by line, find the last `"type":"assistant"` entry, return its `content`. |

**`conversation_reconstruct` algorithm:**
1. Open file for reading
2. Read line by line
3. Parse JSON, check `type` field
4. Skip `meta` and `error` entries
5. `user` → `{role: "user", content: content}`
6. `assistant` → `{role: "assistant", content: content}`
7. Group `tool_call` with preceding assistant. Collect tool_calls, then append tool role messages:
   - `tool_call` entry → assistant message gets `tool_calls` array entry
   - `tool_result` entry → `{role: "tool", tool_call_id: call_id, content: result}`

### 2.11 `utf8.c` / `utf8.h` — UTF-8 Validation

| Function | Purpose |
|----------|---------|
| `bool utf8_validate(const char *s, size_t len)` | Validate UTF-8 byte sequence per RFC 3629. Reject overlong sequences, surrogate halves, codepoints > U+10FFFF. |
| `bool utf8_validate_c_string(const char *s)` | Wrapper that calls `strlen()` first. |

Implementation manually walks bytes checking continuation byte validity without any dependencies.

### 2.12 `util.c` / `util.h` — Utilities

| Function | Purpose |
|----------|---------|
| `int64_t util_parse_duration(const char *s)` | Parse duration strings like `"30s"`, `"10m"`, `"1h"` to milliseconds. Returns -1 on error. |
| `void util_timestamp_now(char *buf, size_t len)` | Write ISO 8601 UTC timestamp to buffer via `gmtime_r()`. |
| `void util_uuid_v4(char *buf)` | Generate UUID v4 string. Uses `/dev/urandom` on Linux, `arc4random()` on macOS. |
| `void util_sha256(const char *data, size_t len, char *hex_out)` | Compute SHA256 hex digest. Uses OpenSSL's `SHA256()` or libcrypto. |
| `char *util_read_file(const char *path)` | Read entire file into malloc'd string. |
| `char *util_strdup(const char *s)` | Safe strdup with OOM check. |

### 2.13 `platform.c` / `platform.h` — Platform Abstraction

Hides all OS-specific APIs behind a uniform interface. The rest of the code never calls POSIX or Win32 directly.

```c
// Process management
typedef struct { ... } platform_process;  // opaque
typedef struct { ... } platform_pipe;     // opaque

int  platform_process_spawn(const char *cmdline, platform_process *out_proc,
                            platform_pipe *in_pipe, platform_pipe *out_pipe);
int  platform_process_kill(platform_process *proc);
int  platform_process_wait(platform_process *proc, int64_t timeout_ms);
void platform_process_close(platform_process *proc);

// Pipe I/O
int  platform_pipe_read(platform_pipe *p, char *buf, size_t size, int64_t timeout_ms);
int  platform_pipe_write(platform_pipe *p, const char *data, size_t len);
void platform_pipe_close(platform_pipe *p);

// UUID entropy
void platform_random_bytes(void *buf, size_t len);

// Timestamps
void platform_timestamp_now(char *buf, size_t len);

// Socket helpers (HTTP proxy listener)
int  platform_tcp_listen(const char *addr, int port);
int  platform_tcp_accept(int fd, int64_t timeout_ms);

// TTY check
bool platform_stderr_is_tty(void);

// Temp filesystem helpers (used by the agent when --conversation is omitted)
const char *platform_temp_dir(void);   // $TMPDIR or /tmp on POSIX, GetTempPath on Windows
int platform_delete_file(const char *path);  // unlink()/DeleteFile(); ENOENT is success
```

| Function | Linux Implementation | Windows Implementation |
|----------|----------------------|------------------------|
| `platform_process_spawn` | `fork()` + `execvp()` with `pipe()` × 2 | `CreateProcess()` with `STARTF_USESTDHANDLES` + `CreatePipe()` |
| `platform_process_kill` | `kill(pid, SIGTERM)` | `TerminateProcess()` |
| `platform_process_wait` | `waitpid()` with `WNOHANG` loop + `poll()` | `WaitForSingleObject()` with timeout |
| `platform_process_close` | `close()` fds | `CloseHandle()` |
| `platform_pipe_read` | `poll()` on fd, then `read()` | `PeekNamedPipe()` + `ReadFile()` with overlapped I/O |
| `platform_pipe_write` | `write()` | `WriteFile()` |
| `platform_pipe_close` | `close()` | `CloseHandle()` |
| `platform_random_bytes` | `read(/dev/urandom)` | `CryptGenRandom()` |
| `platform_timestamp_now` | `gmtime_r()` + `strftime()` | `gmtime_s()` + `strftime()` |
| `platform_tcp_listen` | `socket()` + `bind()` + `listen()` | `WSASocket()` + `bind()` + `listen()` (WSAStartup on first call) |
| `platform_tcp_accept` | `accept()` with `poll()` timeout | `WSAAccept()` with `WSAEventSelect()` timeout |
| `platform_stderr_is_tty` | `isatty(STDERR_FILENO)` | `_isatty(_fileno(stderr))` |
| `platform_temp_dir` | `getenv("TMPDIR")` else `/tmp` | `GetTempPathA()` |
| `platform_delete_file` | `unlink()` | `DeleteFileA()` |

The header selects implementation via `#ifdef _WIN32`:

```c
#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
  struct platform_process { HANDLE hProcess; DWORD pid; };
  struct platform_pipe    { HANDLE hRead, hWrite; OVERLAPPED ov; };
#else
  #include <unistd.h>
  #include <sys/wait.h>
  struct platform_process { pid_t pid; int stdin_fd, stdout_fd; };
  struct platform_pipe    { int fd; };
#endif
```

### 2.14 Stderr Activity Logger (inline in `util.c`)

```c
void log_activity(const char *fmt, ...);
```

Writes `[tag] message\n` to stderr only when `platform_stderr_is_tty()` returns true. Tags: `init`, `progress`, `tool`, `done`.

---

## 3. Libraries

| Library | Version | Use | Header | MinGW Package |
|---------|---------|-----|--------|---------------|
| libyaml | ≥ 0.2.5 | YAML config parsing | `<yaml.h>` | `mingw-w64-x86_64-libyaml` |
| libcurl | ≥ 7.68 | HTTP client (LLM API, MCP http/sse) | `<curl/curl.h>` | `mingw-w64-x86_64-curl` |
| cJSON | ≥ 1.7.15 | JSON construction/parsing (vendored if unavailable) | `<cjson/cJSON.h>` | `mingw-w64-x86_64-cjson` (or vendor) |
| OpenSSL/libcrypto | ≥ 1.1.1 | SHA256 hashing | `<openssl/sha.h>` | `mingw-w64-x86_64-openssl` |

**No external dependencies beyond these.** The standard C library (libc) covers string/memory/stdio. On Windows, `-lws2_32` (Winsock) and `-lbcrypt` (CryptGenRandom) are added automatically.

### Static Linking Strategy

- `libyaml.a`, `libcurl.a`, `libcrypto.a`, `libssl.a` are linked statically
- cJSON is compiled from source (vendored or fetched) — single .c/.h pair
- Linux: linked with `-static` flag
- macOS: dynamic linking acceptable (static not idiomatic on macOS); `-static-libgcc -static-libstdc++` if GCC
- Windows (MinGW): linked with `-static -static-libgcc -static-libstdc++ -lws2_32 -lbcrypt`. The `-lws2_32` provides Winsock APIs; `-lbcrypt` provides `CryptGenRandom`.

---

## 4. Build System — GNU Make

### 4.1 Top-level `Makefile`

```make
CC       ?= gcc
CFLAGS   ?= -Os -g0 -Wall -Wextra -Wpedantic -std=c17 -D_DEFAULT_SOURCE
LDFLAGS  ?= -static
LIBS     := -lyaml -lcurl -lcrypto -lssl

SRCDIR   := src
OBJDIR   := build
TARGET   := llmkit

SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

SCRIPTS  := scripts
SOURCES  := $(SRCS) $(wildcard $(SRCDIR)/*.h)

.PHONY: all check-ascii clean install dist

all: check-ascii $(TARGET)

check-ascii:
	@$(SCRIPTS)/check-ascii.py $(SOURCES)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

dist: $(TARGET)
	strip $(TARGET)
	tar czf llmkit-$(shell git describe --tags 2>/dev/null || echo "dev").tar.gz $(TARGET)
```

### 4.2 Cross-Compilation (Linux → Windows)

A separate Makefile or `Makefile.cross` adds MinGW-w64 targets. These can also be included conditionally in the main Makefile.

```make
# Cross-compilation prefix — override per target arch
MINGW_W64   ?= x86_64-w64-mingw32
MINGW_CC     = $(MINGW_W64)-gcc
MINGW_CFLAGS  = -Os -g0 -Wall -Wextra -Wpedantic -std=c17 -D_DEFAULT_SOURCE
MINGW_LDFLAGS = -static -static-libgcc -static-libstdc++
MINGW_LIBS    = -lyaml -lcurl -lcrypto -lssl -lws2_32 -lbcrypt
MINGW_TARGET  = llmkit.exe

# pkg-config for MinGW (may need PKG_CONFIG_LIBDIR override)
MINGW_PKG_CONFIG := PKG_CONFIG_LIBDIR=$(MINGW_W64)/lib/pkgconfig pkg-config

MINGW_OBJDIR := build-win

$(MINGW_OBJDIR)/%.o: $(SRCDIR)/%.c | $(MINGW_OBJDIR)
	$(MINGW_CC) $(MINGW_CFLAGS) -c -o $@ $<

$(MINGW_TARGET): check-ascii $(MINGW_OBJS)
	$(MINGW_CC) $(MINGW_CFLAGS) $(MINGW_LDFLAGS) -o $@ $^ $(MINGW_LIBS)

.PHONY: windows windows32 windows64 clean-win

windows: windows64

windows64:
	$(MAKE) $(MINGW_TARGET) MINGW_W64=x86_64-w64-mingw32

windows32:
	$(MAKE) $(MINGW_TARGET) MINGW_W64=i686-w64-mingw32

clean-win:
	rm -rf $(MINGW_OBJDIR) $(MINGW_TARGET)
```

**Prerequisites for cross-compilation:**

```bash
# Debian/Ubuntu
sudo apt install mingw-w64 mingw-w64-tools \
  libyaml-dev:windows libcurl4-openssl-dev:windows \
  libssl-dev:windows

# Arch Linux
sudo pacman -S mingw-w64-gcc mingw-w64-libyaml \
  mingw-w64-curl mingw-w64-openssl
```

If distro MinGW packages are unavailable, the Makefile falls back to a vendored dependency tree under `vendor/mingw/` populated by `make vendors`.

### 4.3 Windows-Specific Build Notes

- **Native Windows builds** (MSVC, clang-cl) are not a primary target. MinGW-w64 cross-compilation from Linux is the supported path.
- **Binary name:** Automatically appends `.exe` extension when cross-compiling (`MINGW_TARGET := llmkit.exe`).
- **Resource file:** An optional `llmkit.rc` (VersionInfo resource) can be compiled with `windres` for proper Windows metadata:

```make
$(MINGW_OBJDIR)/llmkit.res: src/llmkit.rc
	$(MINGW_W64)-windres $< -O coff -o $@
```

- **Winsock init:** `WSAStartup()` is called once at program start on the first `platform_tcp_listen()` call. `WSACleanup()` on shutdown.
- **Unicode:** All external interfaces use UTF-8. On Windows, `CreateProcessA` (ANSI) is used; command-line strings must be UTF-8. If the MCP server command line contains non-ASCII characters, `CreateProcessW` with `MultiByteToWideChar(CP_UTF8)` conversion is used instead.

### 4.4 Build Variants

| Target | Flags | Description |
|--------|-------|-------------|
| `make` | `-Os -g0` | Optimized release build |
| `make debug` | `-Og -g3 -DLLMKIT_DEBUG` | Debug build with address sanitizer |
| `make profile` | `-O2 -g -pg` | Profiling build |
| `make windows` | MinGW cross | Windows .exe (64-bit) |
| `make windows32` | MinGW cross | Windows .exe (32-bit) |

### 4.5 Dependency Detection

The Makefile auto-detects library availability for native and cross-compilation builds:

```make
# Auto-detect library paths for static linking
YAML_CFLAGS  := $(shell pkg-config --cflags yaml-0.1 2>/dev/null)
YAML_LIBS    := $(shell pkg-config --libs --static yaml-0.1 2>/dev/null || echo "-lyaml")
CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS    := $(shell pkg-config --libs --static libcurl 2>/dev/null || echo "-lcurl")
CRYPTO_LIBS  := $(shell pkg-config --libs --static libcrypto 2>/dev/null || echo "-lcrypto")

# cJSON vendored fallback
ifeq ($(wildcard $(SRCDIR)/cJSON.c),)
  CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null)
  CJSON_LIBS   := $(shell pkg-config --libs --static libcjson 2>/dev/null || echo "-lcjson")
else
  CJSON_CFLAGS := -I$(SRCDIR)
  CJSON_LIBS   :=
endif
```

### 4.6 `Makefile` Targets Summary

| Target | Action |
|--------|--------|
| `all` | Run `check-ascii`, then build release binary (native) |
| `debug` | Run `check-ascii`, then build with debug symbols and sanitizers |
| `profile` | Run `check-ascii`, then build with profiling instrumentation |
| `check-ascii` | Scan all source files for non-ASCII characters |
| `windows` | Run `check-ascii`, then cross-compile 64-bit Windows .exe |
| `windows32` | Run `check-ascii`, then cross-compile 32-bit Windows .exe |
| `test` | Compile and run unit tests under `tests/` (if present) |
| `clean` | Remove native build artifacts |
| `clean-win` | Remove Windows build artifacts |
| `install` | Copy binary to `/usr/local/bin` |
| `uninstall` | Remove binary from install prefix |
| `dist` | Run `check-ascii`, create release tarball (includes both native binary and Windows .exe if present) |
| `dist-win` | Create Windows-only zip archive of .exe |
| `vendors` | Fetch and prepare vendored dependencies |
| `check-deps` | Verify required libraries are available |

### 4.5 Directory Layout After Build

```
llmkit/
├── Makefile
├── src/
│   ├── main.c
│   ├── agent.c / agent.h
│   ├── proxy.c / proxy.h
│   ├── config.c / config.h
│   ├── llm.c / llm.h
│   ├── mcp.c / mcp.h
│   ├── mcp_transport.c / mcp_transport.h
│   ├── jsonrpc.c / jsonrpc.h
│   ├── conversation.c / conversation.h
│   ├── utf8.c / utf8.h
│   ├── util.c / util.h
│   ├── llmkit.h
│   └── vendor/            # vendored cJSON
│       ├── cJSON.c
│       └── cJSON.h
├── tests/
│   ├── test_utf8.c
│   ├── test_jsonrpc.c
│   ├── test_config.c
│   ├── test_conversation.c
│   └── test_util.c
├── build/                  # native object files
├── build-win/              # MinGW-cross object files
├── llmkit                  # native binary (Linux/macOS)
├── llmkit.exe              # Windows binary (cross-compiled)
├── vendor/
│   └── mingw/              # vendored MinGW .a / .h
├── scripts/
│   └── check-ascii.py      # ASCII-only source checker
└── docs/
    ├── requirements.md
    └── technical-spec.md
```

---

## 5. Data Flow Diagrams

### 5.1 Agent Mode

```
CLI args → main.c
    ↓
config_load() → runtime_ctx
    ↓
conversation_open() → FILE*
    ↓
conversation_write_meta()  ──→ JSONL file
conversation_write_entry(user)
    ↓
┌─→ conversation_reconstruct() ──→ JSONL file
│   ↓
│   mcp_discover_tools() ──→ mcp servers (tools/list)
│   ↓
│   llm_chat_complete() ──→ LLM API
│   ↓
│   conversation_write_entry(assistant) ──→ JSONL file
│   ↓
│   if tool_calls:
│     for each:
│       mcp_call_tool() ──→ mcp server (tools/call)
│       conversation_write_entry(tool_call) ──→ JSONL file
│       conversation_write_entry(tool_result) ──→ JSONL file
│     └─────────────────────────────────────────→ back to top
│   else:
│     exit 0
└──
```

### 5.2 Proxy Mode

```
Client (MCP) ←→ proxy (stdio/HTTP)
                    ↓
         handle_mcp_request()
            ↓          ↓
     translate_names_forward()
            ↓          ↓
     dispatch to backend MCP server
            ↓          ↓
     translate_names_reverse()
     apply_filters()
     apply_rename()
     apply_redefine()
            ↓          ↓
         Response to Client
```

### 5.3 Response Mode

```
CLI args → main.c
    ↓
conversation_read_last_assistant() ──→ JSONL file
    ↓
printf("%s", content) ──→ stdout
    ↓
exit 0
```

---

## 6. Memory Management Convention

- All heap allocations use `malloc()`/`calloc()` with immediate NULL check → `log_activity("[error] OOM")` → `exit(EXIT_INTERNAL_ERR)`
- Ownership is explicit: functions that allocate return the pointer; callers free
- `config_free()` frees all config fields
- `conversation_free_messages()` frees reconstructed message array
- `mcp_disconnect_all()` closes connections and frees connection structs
- No global state — all state in `runtime_ctx` (stack-allocated in `main()`)

---

## 7. Error Handling Strategy

| Layer | Strategy |
|-------|----------|
| malloc failures | Immediate exit with code 7 |
| file I/O | Return exit code 3 with stderr message |
| YAML parse | Return exit code 1 with detailed parse error |
| LLM API HTTP | Parse response body for error, return exit code 4 |
| MCP connection | Return exit code 5, write error entry to JSONL |
| MCP init timeout | exit code 6 after killing process |
| UTF-8 validation failure | Immediate exit with code 1 (config) or 7 (other) |
| Partial data written | Keep JSONL file open, flush on each write; on error, close preserving written entries |

---

## 8. Threading / Concurrency Model

- **Single-threaded, blocking I/O**
- No threads, no async, no signal handlers (except SIGCHLD for process reaping on POSIX)
- On Linux/macOS: `platform_pipe_read()` uses `poll()` for stdio timeouts
- On Windows: `platform_pipe_read()` uses `WaitForMultipleObjects()` on pipe handles with a timeout
- libcurl used in blocking (easy) mode — no multi interface
- Tool calls execute sequentially in order

---

## 9. Portability — Cross-Platform Matrix

| Concern | Linux | macOS | Windows (MinGW) |
|---------|-------|-------|-----------------|
| Process spawn | `fork()` + `execvp()` | `fork()` + `execvp()` | `CreateProcess()` with `STARTF_USESTDHANDLES` |
| Pipe I/O | `pipe()` + `poll()` / `read()` / `write()` | Same as Linux | `CreatePipe()` + `WaitForMultipleObjects()` / `ReadFile()` / `WriteFile()` |
| Process reap | `waitpid()` with `WNOHANG` loop | Same as Linux | `WaitForSingleObject()` on process handle |
| Process kill | `kill(pid, SIGTERM)` | `kill(pid, SIGTERM)` | `TerminateProcess()` |
| UUID entropy | `/dev/urandom` | `arc4random_buf()` (detected via `__APPLE__`) | `CryptGenRandom()` via `<bcrypt.h>` (`-lbcrypt`) |
| Timestamps | `gmtime_r()` + `strftime()` | Same as Linux | `gmtime_s()` (standard C11) |
| Socket API | POSIX `<sys/socket.h>` | Same as Linux | Winsock2 `<winsock2.h>` (`-lws2_32`); `WSAStartup()` on first use |
| TTY detection | `isatty(STDERR_FILENO)` | Same as Linux | `_isatty(_fileno(stderr))` |
| Static linking | `-static` | Dynamic OK (static not idiomatic) | `-static -static-libgcc -static-libstdc++` |
| Binary extension | none | none | `.exe` — handled by `MINGW_TARGET` in Makefile |
| Unicode filesystem | UTF-8 native | UTF-8 native (normalized) | `CreateProcessA` for ASCII, `CreateProcessW` + `MultiByteToWideChar(CP_UTF8)` for non-ASCII cmdlines |
| Signals | `sigaction()` with `SA_NOCLDWAIT` for SIGCHLD | Same as Linux | Not needed — `WaitForSingleObject` replaces SIGCHLD |
| libyaml | Fully compatible | Fully compatible | Fully compatible (MinGW builds) |
| libcurl | Fully compatible | Fully compatible | Fully compatible (MinGW builds, `-DCURL_STATICLIB`) |
| OpenSSL | Fully compatible | Fully compatible | Fully compatible (MinGW builds) |

### 9.1 Preprocessor Guard Strategy

All platform-specific code in `platform.c` uses a single header guard pattern:

```c
#include "platform.h"

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
  #include <bcrypt.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <sys/socket.h>
  #include <poll.h>
  #ifdef __APPLE__
    #include <sys/random.h>   // for getentropy() / arc4random_buf()
  #endif
#endif
```

No other `.c` file contains `#ifdef _WIN32`. They call `platform_*()` functions exclusively.

---

## 10. Source Code Encoding — ASCII Only

### 10.1 Requirement

All source code files (`.c`, `.h`, `.sh`, `Makefile`, and any other source artifacts under `src/`, `scripts/`, `tests/`) **MUST contain only ASCII characters (0x00–0x7F)**. Non-ASCII Unicode characters in string literals, comments, or identifiers are forbidden. The compiler does not accept source files containing bytes above 0x7F.

**Rationale:** Guarantees identical source interpretation across all toolchains and editors regardless of locale, encoding guess, or BOM handling. Eliminates an entire class of portability bugs.

**Exceptions:**
- vendored third-party source files (under `vendor/`) are exempt, though they SHOULD be ASCII too
- generated binary files are not scanned

### 10.2 Checker Script — `scripts/check-ascii.py`

The script scans each source file byte-by-byte and reports every non-ASCII character with its filename, line number, byte offset, hex code, and the actual character.

```python
#!/usr/bin/env python3
"""check-ascii -- Verify all given files contain only ASCII (0x00-0x7F)."""
import sys
import os

VENDOR_MARKER = os.sep + "vendor" + os.sep


def check_file(path: str) -> bool:
    if VENDOR_MARKER in path:
        return True
    ok = True
    with open(path, "rb") as f:
        data = f.read()
    lineno = 1
    col = 0
    for byte in data:
        if byte == ord("\n"):
            lineno += 1
            col = 0
            continue
        if byte > 127:
            print(
                f"{path}:{lineno}:{col}: error: non-ASCII character 0x{byte:02X} ({chr(byte)})",
                file=sys.stderr,
            )
            ok = False
        col += 1
    return ok


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: check-ascii.py <file> [file ...]", file=sys.stderr)
        return 1
    ok = True
    for path in sys.argv[1:]:
        if not check_file(path):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

**Output format:** `<filename>:<line>:<column>: error: non-ASCII character 0x<HEX> (<char>)`

This is the standard GCC/Clang error format, so editors and CI systems that parse compiler output will automatically pick up violations.

**Example output:**

```
src/llm.c:42:18: error: non-ASCII character 0xE2 (â)
src/llm.h:7:31: error: non-ASCII character 0x201C (")
```

### 10.3 Integration

- The script runs **before every build target** (`all`, `debug`, `profile`, `windows`, `windows32`).
- A failed check (non-zero exit) **aborts the build immediately** — no object files or binaries are produced.
- The script is also suitable for running in CI as a standalone check: `scripts/check-ascii.py src/*.c src/*.h`.

### 10.4 Pre-Commit Hook (Optional)

A pre-commit hook at `.git/hooks/pre-commit` can invoke the checker:

```bash
#!/bin/sh
exec scripts/check-ascii.py $(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(c|h)$')
```

This catches non-ASCII characters before they reach the repository.
