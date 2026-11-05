#!/bin/sh
# stage.sh - build everything a release needs into dist/: tests, version-
# stamped binary, tarball, distro packages, checksums. no github access
# needed - tagging and uploading happen outside the build container with
# tools/publish.sh.
#
# usage: tools/stage.sh v1.2.3        (run inside the build container)
#
# what it does, in order:
#   1. sanity checks: tag format, tag not taken, clean tree
#   2. runs the test suite (make check)
#   3. builds llmkit with the version stamped into `llmkit version`
#   4. packs dist/llmkit-<tag>-<os>-<arch>.tar.gz
#   4b. distro packages in docker, when docker is available (PACKAGES=0 skips)
#   4c. sha256 checksums over everything in dist/
#   4d. records the commit the artifacts were built from (dist/.staged-commit,
#       a dotfile: the dist/* globs skip it, so it never gets checksummed
#       or uploaded; tools/publish.sh verifies it)
# afterwards: copy dist/ out of the container and run tools/publish.sh <tag>
set -eu

die() { echo "stage.sh: $*" >&2; exit 1; }

TAG=${1:-}
[ -n "$TAG" ] || die "usage: tools/stage.sh v1.2.3"
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

# ---- 2. tests ---------------------------------------------------------------
make check

# ---- 3. build with the version stamp ----------------------------------------
make clean
make "VERSION=$VER"
./llmkit version | grep -q "$VER" || die "version stamp not picked up"

# ---- 4. pack -----------------------------------------------------------------
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
staging="dist/llmkit-$TAG-$os-$arch"
rm -rf dist
mkdir -p "$staging"
cp llmkit LICENSE.md README.md "$staging/"
tar -C dist -czf "$staging.tar.gz" "${staging#dist/}"
rm -rf "$staging"

# ---- 4b. distro packages (docker; set PACKAGES=0 to skip) --------------------
if [ "${PACKAGES:-1}" = 0 ]; then
    echo "stage.sh: PACKAGES=0 - skipping distro packages"
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    tools/package.sh "$TAG"
else
    echo "stage.sh: docker not available - skipping distro packages"
fi

# ---- 4c. checksums over everything in dist/ ----------------------------------
: > "dist/checksums-$TAG.txt"
for f in dist/*; do
    case "$f" in dist/checksums-*) continue ;; esac
    [ -f "$f" ] || continue
    (cd dist && { sha256sum "$(basename "$f")" >> "checksums-$TAG.txt" 2>/dev/null \
                  || shasum -a 256 "$(basename "$f")" >> "checksums-$TAG.txt"; })
done

# ---- 4d. record the commit these artifacts were built from -------------------
# dotfile: the dist/* globs above and in publish.sh skip it, so it never
# gets checksummed or uploaded; tools/publish.sh verifies it
git rev-parse HEAD > dist/.staged-commit

echo "staged $TAG in dist/:"
ls -l dist/*
echo
echo "next: copy dist/ out of the container, then on a machine with gh run:"
echo "  tools/publish.sh $TAG"
