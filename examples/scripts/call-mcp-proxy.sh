#!/usr/bin/env bash
# Tools in a one-shot call: every --mcp-proxy compiles to one stdio server
# of a single tools record - this same executable spawned as
# `llmkit mcp-proxy <config>` - and the tool rounds run to completion
# before the final answer text. The model sees the config's exposed tools
# prefixed with the config basename: examples/mcp-proxy/expose.jsonl
# yields expose.list_dir and expose.read_file (the renames are the
# config's, see records.md, the expose record). The flag is repeatable,
# one config per server, each basename a unique server name. Spawning
# the upstream needs node/npx; the endpoint is the usual local ollama.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

"$LLMKIT" call --openai http://localhost:11434/v1 \
    --model llama3.1 \
    --system-prompt "When tools can answer a question, use them instead of guessing." \
    --mcp-proxy "$ROOT/examples/mcp-proxy/expose.jsonl" \
    --prompt "Which files and directories are in /tmp?"
