#!/usr/bin/env bash
# The real anthropic api recipe: --key sends Authorization: Bearer, which
# anthropic does not take - the headers record carries x-api-key and
# anthropic-version instead (see docs/requirements.md, the llm record).
# --max-tokens is mandatory under anthropic.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

ANTHROPIC_API_KEY="${ANTHROPIC_API_KEY:-replace-me}"

"$LLMKIT" call --anthropic https://api.anthropic.com/v1 \
    --header "x-api-key=$ANTHROPIC_API_KEY" \
    --header "anthropic-version=2023-06-01" \
    --model claude-sonnet-4-5 \
    --max-tokens 1024 \
    --system-prompt "You are a helpful assistant" \
    --prompt "hello, how are you?"
