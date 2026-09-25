#!/usr/bin/env bash
# The responses api flavor: --openai-responses talks to <api_base>/responses
# instead of /chat/completions - the flag form of endpoint_protocol
# openai_responses (docs/requirements.md, the llm record). --key is the
# token channel for openai-compatible endpoints: sent as
# Authorization: Bearer, which openai takes and anthropic does not
# (call-anthropic.sh shows the --header recipe there). The model name is
# editable in place; --max-tokens stays optional (anthropic is the
# protocol that requires it).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

OPENAI_API_KEY="${OPENAI_API_KEY:-replace-me}"

"$LLMKIT" call --openai-responses https://api.openai.com/v1 \
    --key "$OPENAI_API_KEY" \
    --model gpt-4o-mini \
    --prompt "hello, how are you?"
