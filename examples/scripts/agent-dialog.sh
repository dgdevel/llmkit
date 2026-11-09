#!/usr/bin/env bash
# A dialog wrapper around `llmkit agent`: the menu lets you start a new
# conversation (the store file is named after its timestamp) or resume a
# previous one from the store directory - the store replays the saved
# transcript and continues it. All llm flags live in LLM_FLAGS, the
# stores in STORE_DIR; both are environment-overridable.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLMKIT="${LLMKIT:-$ROOT/llmkit}"
[ -x "$LLMKIT" ] || { echo "$LLMKIT not found - build it first: make" >&2; exit 1; }

# every flag passed to `llmkit agent` except --conversation-store;
# word-split on purpose, so do not quote it at the call site
LLM_FLAGS="${LLM_FLAGS:---openai http://localhost:9931/v}"
# where the conversation jsonl files are kept
STORE_DIR="${STORE_DIR:-$HOME/.local/share/llmkit/agent}"

command -v dialog >/dev/null ||
    { echo "dialog not found - install it first" >&2; exit 1; }
mkdir -p "$STORE_DIR"

pick() { # $@: dialog args, prints the selection, returns dialog's status
    dialog --stdout "$@"
}

action=$(pick --menu "llmkit agent - pick a conversation" 12 60 2 \
    "New"    "Start a new conversation" \
    "Resume" "Continue a stored conversation") || exit 0   # cancelled

# the stored conversations, newest first, with their last-modified time
mapfile -t sessions < <(ls -t "$STORE_DIR"/*.jsonl 2>/dev/null || true)

case "$action" in
New)
    store="$STORE_DIR/$(date +%Y%m%d-%H%M%S).jsonl"
    while [ -e "$store" ]; do          # same-second collision: add a suffix
        store="${store%.jsonl}-$$.jsonl"
    done
    ;;
Resume)
    if [ "${#sessions[@]}" -eq 0 ]; then
        dialog --msgbox "No stored conversations in $STORE_DIR" 8 60
        exit 1
    fi
    items=()
    for f in "${sessions[@]}"; do
        items+=("${f##*/}" "$(date -r "$f" '+%Y-%m-%d %H:%M:%S')")
    done
    name=$(pick --menu "Resume a conversation from $STORE_DIR" 0 0 0 \
        "${items[@]}") || exit 0       # cancelled
    store="$STORE_DIR/$name"
    ;;
*)  exit 1 ;;
esac

echo "$(basename "$store") -> llmkit agent"
# shellcheck disable=SC2086
exec "$LLMKIT" agent $LLM_FLAGS --conversation-store "$store"
