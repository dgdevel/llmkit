#!/usr/bin/env bash
# Drive `llmkit agent-as-tool` by hand over its stdio mcp interface:
# newline-delimited JSON-RPC 2.0. Normally the process is spawned by a
# parent runner through a tools record command_line, e.g.:
#   {"type":"tools","tools":[{"type":"stdio","name":"helper",\
# "command_line":"llmkit agent-as-tool examples/agent-as-tool/minimal.jsonl"}]}
#
# initialize + tools/list need no llm endpoint. tools/call runs a whole
# conversation (seed + one user record), so it needs the endpoint configured
# in the seed's llm record; a failed conversation comes back as a failed
# tool call, the server stays up.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

{
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"llmkit-examples","version":"1.0"}}}'
    printf '%s\n' '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"invoke","arguments":{"input":"Reply with the single word: pong"}}}'
} | "$LLMKIT" agent-as-tool "$ROOT/examples/agent-as-tool/minimal.jsonl"
