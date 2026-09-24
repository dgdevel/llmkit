#!/usr/bin/env bash
# The prompt from stdin: --prompt - reads the whole of stdin, so call
# composes with pipes. Exit code and stderr behave like every llmkit
# command, and the answer is plain stdout - capturable with $(...).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

echo "Summarize this in one sentence:" |
"$LLMKIT" call --openai http://localhost:11434/v1 \
    --model llama3.1 \
    --prompt -

# capture pattern:
#   ANSWER=$("$LLMKIT" call --openai "$API_BASE" --model "$MODEL" --prompt - < input.txt)
