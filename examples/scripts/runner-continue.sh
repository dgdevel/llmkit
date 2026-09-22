#!/usr/bin/env bash
# Continuation / replay: the stdout of a run appended to its input is a
# valid new input. Run once, then feed input + output + one new user
# record back into the runner; the serialized prefix is stable, so
# endpoint prefix caches still hit on the second run.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found — build it first: make" >&2; exit 1; }

IN="$ROOT/examples/runner/minimal.jsonl"
PREV="$(mktemp)"
trap 'rm -f "$PREV"' EXIT

"$LLMKIT" runner < "$IN" > "$PREV"

{
    cat "$IN" "$PREV"
    printf '%s\n' '{"type":"user","content":[{"type":"text","text":"Now say it in exactly five words."}]}'
} | "$LLMKIT" runner
