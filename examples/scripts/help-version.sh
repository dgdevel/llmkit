#!/usr/bin/env bash
# llmkit built-ins. There are no --help / --version flags: help and version
# are subcommands like everything else.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

"$LLMKIT" help
"$LLMKIT" version
