#!/usr/bin/env bash
# The interactive chat: same flags as call, no --prompt - the turns are
# typed. Thinking renders italic, tool calls bold, ascii rules frame every
# block (on a terminal that supports them). Ctrl-C stops the running turn
# or clears the input line; a second Ctrl-C at a clear prompt exits 8;
# Ctrl-D exits cleanly.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

exec "$LLMKIT" repl --openai http://localhost:11434/v1 \
    --model llama3.1 \
    --system-prompt "You are a helpful assistant"
