#!/usr/bin/env bash
# prettyprint: render a recorded conversation with the repl's display -
# user blocks under heavy rules, thinking italic, tool traffic bold, one
# usage line per turn that reports one. No endpoint needed: the file is
# only read and drawn.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

# a full transcript: thinking, a tool round, usage, a pending user turn
"$LLMKIT" prettyprint "$ROOT/examples/runner/continuation.jsonl"

# stdin reads the same: llmkit runner < input | tee session.jsonl, then
# llmkit prettyprint < session.jsonl
