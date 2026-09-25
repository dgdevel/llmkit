#!/usr/bin/env bash
# A scripted session: stdin is not a terminal, so there is no editor and
# no styling - each line is one turn, the transcript renders plain, EOF
# ends the session with the last ending's exit code. Works for piping a
# fixed conversation through the same rendering as the interactive chat.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

printf 'hello, how are you?\nand now a second turn\n' |
"$LLMKIT" repl --openai http://localhost:11434/v1 \
    --model llama3.1
