#!/bin/sh
# publish.sh - tag and publish an already-staged release to GitHub Releases.
#
# usage: tools/publish.sh v1.2.3     (needs gh, run outside the build container)
#
# what it does, in order:
#   1. sanity checks: tag format, tag not taken, clean tree, gh present+authed,
#      dist/ holding staged artifacts for this tag
#   2. verifies checksums still match (nothing corrupted in transfer)
#   3. verifies the tree is still the commit the artifacts were built from
#   4. tags, pushes the tag, creates the GitHub release and uploads dist/*
#
# the artifacts are produced by tools/stage.sh inside the build container.
set -eu

die() { echo "publish.sh: $*" >&2; exit 1; }

TAG=${1:-}
[ -n "$TAG" ] || die "usage: tools/publish.sh v1.2.3"
printf '%s' "$TAG" | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$' \
    || die "tag '$TAG' must look like v1.2 or v1.2.3"

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

[ -d dist ] || die "no dist/ here - run tools/stage.sh $TAG (in the build container) first"
[ -f "dist/checksums-$TAG.txt" ] \
    || die "no dist/checksums-$TAG.txt - dist/ was not staged for $TAG"

# ---- 2. checksums --------------------------------------------------------------
cd dist
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c --ignore-missing "checksums-$TAG.txt"
else
    shasum -a 256 -c --ignore-missing "checksums-$TAG.txt"
fi
cd ..

# ---- 3. release tree matches the tree that staged -----------------------------
# dist/ was built from some checkout of this repo; the tag goes to HEAD.
# refuse to mix the two when they drifted apart between staging and
# publishing. (staging hosts without git would lack the stamp file, but
# stage.sh runs from a git checkout, so a missing stamp means a hand-made
# or tampered dist/ - that dies here too.)
[ -f dist/.staged-commit ] \
    || die "dist/.staged-commit missing - dist/ was not staged by tools/stage.sh"
staged=$(cat dist/.staged-commit)
head=$(git rev-parse HEAD)
[ "$staged" = "$head" ] \
    || die "dist/ was staged from $staged, HEAD is $head - checkout the staged commit or re-stage"

# ---- 4. tag, push, release ---------------------------------------------------
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
