#!/bin/sh
# release.sh - tag a version, build the binary, publish it to GitHub Releases.
#
# usage: tools/release.sh v1.2.3
#
# what it does, in order:
#   1. sanity checks: tag format, tag not taken, clean tree, gh present+authed
#   2. runs the test suite (make check)
#   3. builds llmkit with the version stamped into `llmkit version`
#   4. packs dist/llmkit-<tag>-<os>-<arch>.tar.gz
#   4b. distro packages in docker, when docker is available (PACKAGES=0 skips)
#   4c. sha256 checksums over everything in dist/
#   5. tags, pushes the tag, creates the GitHub release and uploads dist/*
set -eu

die() { echo "release.sh: $*" >&2; exit 1; }

TAG=${1:-}
[ -n "$TAG" ] || die "usage: tools/release.sh v1.2.3"
printf '%s' "$TAG" | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$' \
    || die "tag '$TAG' must look like v1.2 or v1.2.3"
VER=${TAG#v}

cd "$(dirname "$0")/.."

# ---- 1. sanity checks -------------------------------------------------------
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null \
    && die "tag $TAG already exists"
git diff --quiet HEAD || die "working tree has uncommitted changes"
[ -z "$(git ls-files --others --exclude-standard)" ] \
    || die "working tree has untracked files"
upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)
[ -z "$upstream" ] || git merge-base --is-ancestor HEAD "$upstream" \
    || die "HEAD is ahead of $upstream - push first"
command -v gh >/dev/null 2>&1 || die "gh is not installed"
gh auth status >/dev/null 2>&1 || die "gh is not authenticated (gh auth login)"

# ---- 2. tests ---------------------------------------------------------------
make check

# ---- 3. build with the version stamp ----------------------------------------
make clean
make "VERSION=$VER"
./llmkit version | grep -q "$VER" || die "version stamp not picked up"

# ---- 4. pack -----------------------------------------------------------------
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
stage="dist/llmkit-$TAG-$os-$arch"
rm -rf dist
mkdir -p "$stage"
cp llmkit LICENSE.md README.md "$stage/"
tar -C dist -czf "$stage.tar.gz" "${stage#dist/}"
rm -rf "$stage"

# ---- 4b. distro packages (docker; set PACKAGES=0 to skip) --------------------
if [ "${PACKAGES:-1}" = 0 ]; then
    echo "release.sh: PACKAGES=0 - skipping distro packages"
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    tools/package.sh "$TAG"
else
    echo "release.sh: docker not available - skipping distro packages"
fi

# ---- 4c. checksums over everything in dist/ ----------------------------------
: > "dist/checksums-$TAG.txt"
for f in dist/*; do
    case "$f" in dist/checksums-*) continue ;; esac
    [ -f "$f" ] || continue
    (cd dist && { sha256sum "$(basename "$f")" >> "checksums-$TAG.txt" 2>/dev/null \
                  || shasum -a 256 "$(basename "$f")" >> "checksums-$TAG.txt"; })
done

# ---- 5. tag, push, release ---------------------------------------------------
# notes: artifact-to-system table + the generated changelog appended
# (tools/release-notes.sh); --notes-file instead of --generate-notes
notes=$(mktemp /tmp/llmkit-notes.XXXXXX)
tools/release-notes.sh "$TAG" > "$notes"
git tag -a "$TAG" -m "llmkit $TAG"
git push origin "refs/tags/$TAG"
gh release create "$TAG" dist/* \
    --title "llmkit $TAG" --notes-file "$notes"
rm -f "$notes"

echo "released $TAG:"
ls -l dist/*
