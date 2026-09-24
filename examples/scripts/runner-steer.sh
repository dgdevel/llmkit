#!/usr/bin/env bash
# Steering: keep stdin open and feed records plus a flush while a turn is
# in flight. The steered records apply at the next turn boundary:
#  - records before the first flush start the conversation (the runner
#    answers each consumed flush with a {"type":"start"} marker on stdout);
#  - the user record sent mid-turn steers the answer of the NEXT turn.
#
# Tune SLEEP to something shorter than the model's first turn:
#   SLEEP=5 ./examples/scripts/runner-steer.sh
# If the first turn already finished, the second flush simply resumes the
# conversation with the new user record instead of steering it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }
SLEEP="${SLEEP:-2}"

{
    cat "$ROOT/examples/runner/minimal.jsonl"
    printf '%s\n' '{"type":"flush"}'
    sleep "$SLEEP"
    printf '%s\n' '{"type":"user","content":[{"type":"text","text":"Ignore that. Answer with the single word: steered."}]}'
    printf '%s\n' '{"type":"flush"}'
} | "$LLMKIT" runner
