#!/usr/bin/env bash
# Drive `llmkit mcp-proxy` by hand over its stdio mcp interface:
# newline-delimited JSON-RPC 2.0. The proxy never talks to an llm
# endpoint; it only relays tools.
#
# With no expose/hide records every upstream tool is exposed under its
# server-qualified name. Spawning the upstream server needs node/npx
# (first run downloads the package). Try examples/mcp-proxy/expose.jsonl
# or hide.jsonl to see whitelist / blacklist modes - call names and
# arguments are the renamed ones there, e.g.:
#   params:{"name":"list_dir","arguments":{"folder":"/tmp"}}
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }
CONFIG="${CONFIG:-$ROOT/examples/mcp-proxy/minimal.jsonl}"

{
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"llmkit-examples","version":"1.0"}}}'
    printf '%s\n' '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fs.list_directory","arguments":{"path":"/tmp"}}}'
} | "$LLMKIT" mcp-proxy "$CONFIG"
