#!/usr/bin/env bash
# The terminal tool ending: --terminal-tool marks an exposed tool so the
# conversation stops as soon as the model has used it - no further llm
# turn, and the answer printed is the tool's own text (here the directory
# listing, not a summary of it). Requested, not an error - but the exit
# code is 9 (design sec.12), so a set -e script has to accept it
# explicitly. 0 means the model answered in plain text without calling
# the tool; anything else is a real failure. Spawning the upstream needs
# node/npx; the endpoint is the usual local ollama.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

set +e
"$LLMKIT" call --openai http://localhost:11434/v1 \
    --model llama3.1 \
    --mcp-proxy "$ROOT/examples/mcp-proxy/expose.jsonl" \
    --terminal-tool expose.list_dir \
    --prompt "List the files in /tmp."
rc=$?
set -e

case $rc in
    0) ;;    # answered in plain text, the tool was never called
    9) ;;    # the terminal tool ended it - the listing above is the answer
    *) exit "$rc" ;;
esac
