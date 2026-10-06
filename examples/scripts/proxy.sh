#!/usr/bin/env bash
# Watch a live conversation through `llmkit proxy`: start it in front of
# an openai-compatible endpoint, fire one request at it with curl, and
# see both directions render prettyprint-style on the proxy's stdout
# while curl receives the untouched response.
#
# Needs an openai-compatible endpoint at http://localhost:11434
# (ollama serve; ollama pull llama3.1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }
BASE="${BASE:-http://localhost:11434/v1}"
LISTEN="${LISTEN:-127.0.0.1:18080}"
MODEL="${MODEL:-llama3.1}"

"$LLMKIT" proxy --openai "$BASE" --listen "$LISTEN" &
PROXY=$!
trap 'kill $PROXY 2>/dev/null || true' EXIT
sleep 0.3

curl -sN "http://$LISTEN/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"$MODEL\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hello, how are you?\"}]}" \
    > /dev/null

wait "$PROXY"
