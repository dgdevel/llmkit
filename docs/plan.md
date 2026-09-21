# llmkit implementation plan

Build order for [design.md](design.md), which implements
[requirements.md](requirements.md). Bottom-up: pure parsing and
serialization first — cheapest to test, everything depends on them — then
the wire clients, then the engine, then the subprocess and network edge
features, and the two extra entry points last: they are thin compositions
of the finished core. Every phase lands with its tests, so `make check`
grows with the build instead of arriving at the end; design §13's selfcheck
categories map onto the phases one to one.

1. **scaffold** — makefile, subcommand dispatch, help/version, exit code
   plumbing. Dependencies (`libcjson`, `libcurl`) verified present.
   Test: the binary runs; usage errors exit 1.
2. **foundations** — byte buffers (the append-only prefix buffer among
   them), utf-8 validation, the input pipeline: byte rules, json parsing,
   record validation, dispatch. Tests: byte-level vectors — bom, cr
   dropping, invalid utf-8, malformed json, unknown type, missing fields,
   duplicate server name.
3. **sse parser** — the one shared event parser behind every wire dialect.
   Tests: fixed vectors for both openai apis, anthropic and mcp, split at
   awkward byte boundaries.
4. **serialization** — record→wire mapping per protocol, deterministic body
   order, the prefix buffer, anthropic cache_control overlay. Tests: the
   prefix invariant directly — turn N's message bytes byte-compared against
   turn N−1's request, across protocols, with tools, steering and an llm
   switch; sampling-only changes leave the prefix untouched.
5. **endpoint clients** — the two wire modules on curl, streaming and
   non-streaming. Tests: stream parity (the same scripted response both
   ways → identical final records), finish_reason normalization,
   api_error/http_error mapping, the timeout knobs.
6. **platform + engine** — threads, queues, signals, spawn; the state
   machine and turn loop: flush consumption, steering, the drop rule,
   pre-send validations, stop conditions, exit codes. Tests: the state
   machine against an in-process fake endpoint — the flush / steering /
   no-op flush / drop rule matrix, max_tool_rounds, the anthropic
   validations — plus the sigint scenarios: mid stream, mid tool, before
   start, double sigint.
7. **mcp client** — stdio, streamable http and legacy sse transports, the
   v1 and v2 flows, tools/call, timeouts, `required` semantics. Tests:
   scripted fake servers as child processes.
8. **mcp-proxy** — config pipeline, expose/hide resolution, schema rewrite,
   the stdio server loop. Tests: selection and validation rules, rewrite
   determinism, inverse argument mapping on forwarding.
9. **agent-as-tool** — seed bootstrap, the invoke loop over the engine,
   retain_context in both modes with rollback. Tests: seed validation,
   reply shape, rollback on fatal error.
10. **endgame** — full `make check`, the exit code table verified end to
    end, manual smoke pass of the documented flows: piped one-shot input,
    continuation replay, steering from a kept-open stdin.

Out of scope per design §13: network integration tests in-tree; a
`test/live.sh` against a real endpoint appears when the first endpoint bug
shows up.
