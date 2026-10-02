#!/usr/bin/env bash
# A scripted mcp-repl session against the built-in mcp server: stdin is
# not a terminal, so there is no editor, no tab completion and no styled
# prompt - each line is one action (call, command), output renders plain,
# EOF ends the session, exit 0. The interactive form of the same session
# (tab completion included) is the same command without the pipe.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

printf 'file_create("mcp-repl-demo.txt", "hello from mcp-repl\\n")\nfile_read("mcp-repl-demo.txt", 0, 10)\ntools\nquit\n' |
"$LLMKIT" mcp-repl --stdio "$LLMKIT builtin-mcp"
rm -f mcp-repl-demo.txt
