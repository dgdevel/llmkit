#!/bin/sh
# check-ascii.sh - fail if any file in the repo contains a non-ASCII byte.
#
# Scope: every file git knows about plus untracked files, minus gitignored
# ones (git ls-files --cached --others --exclude-standard) - exactly the set
# of files that would enter a commit - except src/prompts (the prompt texts
# are prose the owner may shape freely, non-ASCII included) and
# src/banner.txt (the greeting art's UTF-8 box characters).
#
# Detection: with LC_ALL=C, grep works on raw bytes, and any byte outside
# ASCII is neither [:print:] nor [:space:], so one bracket class flags them
# all. Control bytes other than tab/newline are caught by the same class and
# rejected too.
#
# Output: one "file:line:content" line per offending line, exit status 1.
set -u
cd "$(dirname "$0")/.." || exit 2

hits=$(git ls-files --cached --others --exclude-standard |
    grep -v '^src/prompts/' |
    grep -v '^src/banner\.txt$' | # the greeting art: UTF-8 box characters
    grep -v '^src/vendor/md4c/' | # vendored md4c: upstream code stays pristine
    grep -v -E '^fuzz/(fuzz_[a-z_]+|out/|seeds/mcp_tools/.*\.bin|seeds/regex/.*\.bin)$' |
    while IFS= read -r f; do
        LC_ALL=C grep -Hn '[^[:print:][:space:]]' "$f" || true
    done)

if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "check-ascii: FAIL: non-ASCII byte(s) found, see file:line above" >&2
    exit 1
fi

echo "check-ascii: ok"
