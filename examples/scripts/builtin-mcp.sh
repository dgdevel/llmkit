#!/usr/bin/env bash
# Drive `llmkit builtin-mcp` by hand over its stdio mcp interface:
# newline-delimited JSON-RPC 2.0. The server never talks to an llm
# endpoint; the file tools work on the current directory, the web tools
# go out to duckduckgo / the fetched site. Every tool and argument
# description starts empty; they are editable in one block at the top of
# src/builtin.c (docs/builtin-mcp.md has the contracts).
#
# Swap the last line for any other tool, e.g.:
#   {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"file_edit","arguments":{"path":"editme.txt","oldString":"old","newString":"new","line_number":1}}}
#   {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"web_fetch","arguments":{"url":"https://example.com"}}}
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

printf 'one\ntwo with old text\nthree\n' > builtin-example.txt

{
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"llmkit-examples","version":"1.0"}}}'
    printf '%s\n' '{"jsonrpc":"2.0","method":"notifications/initialized"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"file_edit","arguments":{"path":"builtin-example.txt","oldString":"two with old text","newString":"two, edited","line_number":1}}}'
    printf '%s\n' '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"file_read","arguments":{"path":"builtin-example.txt","lines_offset":0,"lines_length":3}}}'
} | "$LLMKIT" builtin-mcp
