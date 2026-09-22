#!/usr/bin/env bash
# Every input record type in one file: header, llm (headers, full
# inference_options), tools (stdio + http servers), options, system, user,
# and a trailing flush — the flush starts the conversation while stdin is
# still open; stdin closing right after has the same effect.
#
# Targets the real anthropic api: replace the x-api-key placeholder in
# examples/runner/complete.jsonl first. The non-required http server is
# skipped (with a non-fatal connect_failed record) when unreachable.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found — build it first: make" >&2; exit 1; }

"$LLMKIT" runner < "$ROOT/examples/runner/complete.jsonl"
