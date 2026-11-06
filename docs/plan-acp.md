# Plan: `llmkit acp` -- ACP server over stdio

Feature: `llmkit acp` serves Agent Client Protocol v1 (ndjson JSON-RPC 2.0
on stdio) so Zed (and other ACP editors) can drive llmkit as coding agent.
Flags mirror `call`/`agent` conventions. `--bundle full|read-only` adds
the whole agent package (builtin mcp + deepwiki); read-only hides
file-writing tools, deepwiki included. NO `--listen` (user dropped TCP).

Research facts: see docs/acp-research.md.

## Files

- NEW `src/acp.c` -- the whole subcommand (~600-800 lines)
- `src/llmkit.h` -- `cmd_acp()` prototype + usage in main.c
- `src/main.c` -- dispatch line + usage text
- `Makefile` -- add `src/acp.c` to SRC (after `src/repl.c`)
- `test/selfcheck.c` -- framing + handshake tests
- `docs/` -- acp doc page

## src/acp.c design

### Flags (mirror call_parse + agent extras)

`llmkit acp (--anthropic|--openai|--openai-responses) <api_base>`
`  --key --model --reasoning-effort --max-tokens --system-prompt`
`  --header <n=v>... --mcp-proxy <config>... [--bundle full|read-only]`

Parse pattern: copy `cmd_agent_repl` extractors approach -- but simpler:
single pass, own parser modeled on `llm_proxy_parse` (llmproxy.c:57).
Default `--bundle`: none (no builtin tools; plain llm, mcp-proxies only).

### Bundle semantics

- `full`: builtin_mcp=true, deepwiki_tool=true, agents_md=true
  (reuse repl_opts_t semantics; AGENTS.md from session cwd injected).
- `read-only`: same but `call_build_agent_tools(..., read_only=true)`
  (builtin hides file-writing tools) AND deepwiki still attached
  (deepwiki is read-only by nature: repo_topics/repo_info/repo_ask).
- Reuse: `call_build_agent_tools` (call.c:389), `repl_deepwiki_build`
  (repl.c) -- check exact fn name; and `inject_agents_md` (repl.c).

### JSON-RPC framing

- ndjson: read line -> cJSON_Parse; write cJSON_PrintUnformatted + "\n",
  fflush after every message. stdout ONLY protocol; all logs to stderr.
- Reader: background thread (copy `stdin_reader_thread` pattern,
  engine.c:897) pushing raw lines to queue; main loop pops.
  Alternatively single-thread blocking read between turns + special
  handling during turn. Prefer thread: enables cancel mid-turn.
- Threading contract: reader thread only parses+enqueues; main thread
  owns engine. Writes serialized by main thread only.

### Message loop / dispatch

Methods (agent role):
- `initialize` -> respond `protocolVersion:1`,
  `agentCapabilities:{loadSession:false,
  promptCapabilities:{image:false,audio:false,embeddedContext:false}}`,
  `authMethods:[]`.
- `session/new` {cwd, mcpServers[]} -> build engine (per session):
  - `engine_new(emit_fn -> acp_sink, ctx)`
  - config records: llm from flags (`call_build_llm`), options
    (stream interval 1), system prompt (default agent prompt +
    AGENTS.md from cwd if bundle), tools (`call_build_tools` +
    builtin server + deepwiki when bundle).
  - respond `{sessionId:"<uuid-ish>", modes:[]}`.
- `session/prompt` {sessionId, prompt:[blocks]} -> append user record
  (`engine_input_record` with T_USER built from text blocks),
  `engine_start` if needed, run turn loop with engine driven until turn
  ends; emit_fn translates records to `session/update` notifications
  (see mapping); final response `{stopReason:"end_turn"}` (map engine
  endings: EXIT_OK->end_turn, max_tokens->max_tokens, refusal-ish ->
  refusal, interrupted/cancelled->cancelled).
- `session/cancel` {sessionId}: set engine stop; the turn's prompt
  response gets stopReason cancelled.
- Unknown method -> JSON-RPC error -32601.
- Ignore notifications (no id): log to stderr.
- Batch/array messages: ACP does not use batches; reject with error.

### Record -> session/update translation (emit_fn sink)

Records from engine emit (repl.c sink translates display; acp sink
translates to ACP notifications):
- partial/final T_TEXT (response) -> `agent_message_chunk`
  {sessionId, update:{sessionUpdate:"agent_message_chunk",
  content:{type:"text",text:...}}}
- partial/final T_THINK -> `agent_thought_chunk` {content:
  {type:"text",text}} (thinking key optional; use type "text").
- T_TREQ (tool request record) -> `tool_call` update with toolCallId
  = record tool id, title = tool name + short args summary, kind:
  map builtin tool names to ACP kinds:
  files family->"edit" (except read family->"read"), process->"execute",
  web->"fetch", analyze->"read", skills->"read", now->"other",
  deepwiki (repo_*)->"read", helper->"think"? per spec "think" is kind
  value; use helper->"think".
- T_TRESP (tool response) -> `tool_call_update` status completed
  (or failed when is_error), content from record text.
- turn start: optional `user_message_chunk` echo -- spec allows, skip.

Title generation: tool name + first string arg elided ~60 chars.

### Cancel path

session/cancel arrives on queue while turn running. Main loop: pop
queue inside turn wait (engine_run is blocking? verify) -- if blocking,
need engine steering: engine has queue inq + `engine_drain_input`;
runner mode feeds steering records mid-turn. Simplest correct path:
dedicated reader thread pushes Q_CANCEL custom; but engine_run blocks
main thread. CHECK: engine.c turn loop -- if it blocks on wire curl
call, cancel requires wire-level stop. Investigate `engine_stop_orderly`
+ how repl interrupts turn (SIGINT handler sets flag + breaks curl?).
Follow same mechanism as repl SIGINT: set flag, engine returns
EXIT_INTERRUPTED at next wire boundary; ACP stopReason cancelled.

### Session store

sessions[] array; multiple sessions per process allowed (Zed opens
many). Each: id, engine, emit sink state (sessionUpdate counters),
deepwiki cfg buffer (like repl's deepwiki_cfg).

### Prompts

Default system prompt: `prompt_system_prompts_agent` (prompts.gen.h)
when bundle; else `prompt_system_prompts_repl`? Check what `call` uses
-- call default system none. For acp no-bundle: no default prompt,
only --system-prompt flag. Bundle: agent prompt + AGENTS.md.

## Implementation steps

1. Scaffolding: acp.c skeleton, main.c dispatch, Makefile, usage. [done]
2. Framing + initialize/session/new handshake loop. [done]
3. session/prompt turn run + record->update translation sink. [done]
4. Bundle: attach builtin + deepwiki + agents_md per mode. [done]
5. Cancel handling. [done: reader thread + g_stop_flag, stopReason cancelled]
6. Tests: handshake framing, unknown method error, prompt turn with
   fake wire (like existing tests use wire_factory), translation
   correctness (assert emitted ndjson lines). [done: 41 acp checks]
7. Docs page + Zed example. [done: docs/acp.md]

## Test approach

test/selfcheck.c has wire_factory seam (llmkit.h:577 `wire_t
*(\*wire_factory)(engine_t *)`). Model tests on existing repl/runner
tests (selfcheck.c around line 5351+ uses repl opts). Drive cmd via
fd? Tests call engine directly with acp sink functions if exported;
prefer exporting `acp_record_to_update()` pure fn for tests.
