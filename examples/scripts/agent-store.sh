#!/usr/bin/env bash
# An agent session with a conversation store: the repl with the built-in
# mcp tools attached and AGENTS.md injected after the system prompt.
# The store persists the conversation as jsonl; run the script twice -
# the second run replays the first session in full before prompting,
# then continues it. Works piped: one turn per line, plain rendering.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

STORE="${STORE:-/tmp/llmkit-agent-demo.jsonl}"

printf 'what is in this directory?\nsummarize the README in one line\n' |
"$LLMKIT" agent --openai http://localhost:9931/v1 \
    --model llama3.1 \
    --conversation-store "$STORE"
