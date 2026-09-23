#!/usr/bin/env bash
# The smallest call: one prompt in, one answer out, plain text on stdout.
#
# Needs an openai-compatible endpoint at http://localhost:11434
# (ollama serve; ollama pull llama3.1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found — build it first: make" >&2; exit 1; }

"$LLMKIT" call --openai http://localhost:11434/v1 \
    --model llama3.1 \
    --prompt "hello, how are you?"
