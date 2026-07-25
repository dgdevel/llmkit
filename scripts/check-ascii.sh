#!/usr/bin/env bash
# scripts/check-ascii.sh -- verify all given files contain only ASCII (0x00-0x7F)
# Usage: scripts/check-ascii.sh <file> [file...]
# Exit code: 0 if all files are pure ASCII, 1 if any non-ASCII found

set -euo pipefail

has_error=0

for path in "$@"; do
  case "$path" in
    vendor/*) continue ;;
  esac

  lineno=1
  while IFS= read -r line; do
    col=0
    for (( i=0; i<${#line}; i++ )); do
      byte=$(printf '%d' "'${line:$i:1}")
      if [ "$byte" -gt 127 ]; then
        char="${line:$i:1}"
        hex=$(printf '0x%02X' "$byte")
        printf '%s:%d:%d: error: non-ASCII character %s (%s)\n' \
          "$path" "$lineno" "$col" "$hex" "$char" >&2
        has_error=1
      fi
      col=$((col + 1))
    done
    lineno=$((lineno + 1))
  done < "$path"
done

exit "$has_error"
