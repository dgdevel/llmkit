#!/bin/sh
# fuzz/docker-fuzz.sh - run the docker-only harness (fuzz_mcp_tools)
# inside a throwaway container. docker replaces the "never fuzz a
# spawner unsandboxed" caveat: the container has no network, a
# read-only root filesystem, and no capabilities.
#
# usage:
#   fuzz/docker-fuzz.sh [extra libFuzzer args...]
#     e.g. fuzz/docker-fuzz.sh -runs=100000 -max_len=2048
# seeds: fuzz/seeds/mcp_tools (mounted read-only)
# env:   RUNS MAXLEN TIMEOUT RSS CORPUSDIR
#   CORPUSDIR (optional) host dir to grow the corpus into; artifacts
#   (crash-*/oom-*/timeout-*) land there too instead of dying with
#   the container.
set -eu
cd "$(dirname "$0")/.."

docker build -f fuzz/Dockerfile -t llmkit-fuzz . >&2

# scratch tree the seeds point at (paths baked into fuzz/seeds/mcp_tools)
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
echo 'seed file' > "$SCRATCH/seed.txt"

# note: no --memory/--pids-limit - this host's docker runs cgroup v2 in
# threaded mode and rejects them; --network none + --read-only +
# cap-drop is the isolation that matters for a fuzzer process.
CORPUS_ARGS=""
ARTIFACT_PREFIX=/tmp/
[ -n "${CORPUSDIR:-}" ] && {
    CORPUSDIR=$(cd "$CORPUSDIR" && pwd)   # docker -v needs an absolute host path
    CORPUS_ARGS="-v ${CORPUSDIR}:/corpus"
    ARTIFACT_PREFIX=/corpus/               # keep crash-*/oom-* artifacts on host
}

docker run --rm -i \
    --cap-drop ALL \
    --security-opt no-new-privileges \
    --read-only \
    --network none \
    --tmpfs /tmp:rw,size=512m,exec \
    -v "$(pwd)/fuzz/seeds/mcp_tools:/seeds:ro" \
    $CORPUS_ARGS \
    -v "$SCRATCH:/scratch:ro" \
    llmkit-fuzz \
    /bin/sh -c 'mkdir -p /tmp/corpus && exec "$0" "$@"' \
    /src/fuzz/fuzz_mcp_tools \
    -runs="${RUNS:-100000}" -max_len="${MAXLEN:-4096}" \
    -timeout="${TIMEOUT:-15}" -rss_limit_mb="${RSS:-2560}" \
    -artifact_prefix="${ARTIFACT_PREFIX}" \
    ${CORPUSDIR:+/corpus} /tmp/corpus /seeds "$@"
