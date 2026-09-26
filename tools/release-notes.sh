#!/bin/sh
# release-notes.sh - compose the GitHub release body: which artifact is for
# which system, then the auto-generated changelog appended.
#
# usage: tools/release-notes.sh v1.2.3 > notes.md     (needs dist/ populated)
# also handy to fix an existing release:
#   tools/release-notes.sh v1.0.0 > /tmp/n.md && gh release edit v1.0.0 --notes-file /tmp/n.md
set -eu
TAG=${1:?usage: tools/release-notes.sh v1.2.3}
cd "$(dirname "$0")/.."
[ -d dist ] || { echo "release-notes.sh: no dist/ here - run tools/package.sh first" >&2; exit 1; }

echo "one binary for llm interaction from the shell: \`repl\`, \`call\`,"
echo "\`runner\`, \`agent-as-tool\` and \`mcp-proxy\` - see the README."
echo
echo "## pick your file"
echo
echo "| file | system | install |"
echo "| --- | --- | --- |"
for f in dist/*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    case "$b" in
        checksums-*) continue ;;
        *.deb)
            sys="debian 12+, ubuntu 22.04+"
            ins="sudo apt install ./$b" ;;
        *.el9.*.rpm)
            sys="rhel, alma, rocky 9+"
            ins="sudo dnf install ./$b" ;;
        *.fc*.rpm)
            sys="fedora 41+"
            ins="sudo dnf install ./$b" ;;
        *.rpm)
            sys="other rpm-based linux"
            ins="sudo dnf install ./$b" ;;
        *.pkg.tar.zst)
            sys="arch linux"
            ins="sudo pacman -U $b" ;;
        *windows*.zip)
            sys="windows 10+ 64-bit, no install"
            ins="unzip, put llmkit.exe on PATH" ;;
        *)
            sys="any linux, release host's glibc or newer, needs libcurl + libcjson - prefer a package above"
            ins="unpack, put llmkit on PATH" ;;
    esac
    echo "| \`$b\` | $sys | \`$ins\` |"
done
echo
echo "verify downloads: \`sha256sum -c --ignore-missing checksums-$TAG.txt\`"

# the generated changelog (commit list since the previous release), appended
# when gh is around; without it the notes above still stand alone
if command -v gh >/dev/null 2>&1; then
    body=$(gh api repos/{owner}/{repo}/releases/generate-notes \
               -f tag_name="$TAG" --jq .body 2>/dev/null || true)
    if [ -n "$body" ]; then
        echo
        echo "---"
        echo
        printf '%s\n' "$body"
    fi
fi
