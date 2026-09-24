#!/usr/bin/env bash
# The smallest conversation that runs: one llm record, one user record.
# stdin closes, so the conversation starts without a flush record.
#
# Needs an openai-compatible endpoint at http://localhost:11434
# (ollama serve; ollama pull llama3.1) - edit examples/runner/minimal.jsonl
# to point elsewhere.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

"$LLMKIT" runner < "$ROOT/examples/runner/minimal.jsonl"
