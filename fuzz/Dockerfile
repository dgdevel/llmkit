# fuzz/Dockerfile - the sandbox for fuzz_mcp_tools (tools/call with
# real file/process/web tool functions on fuzzer bytes).
#
# build context is the repo root (see fuzz/docker-fuzz.sh):
#   docker build -f fuzz/Dockerfile -t llmkit-fuzz .
#
# the harnesses are built here, at image time, on a writable rootfs;
# at runtime docker mounts the rootfs read-only and /tmp becomes a
# throwaway tmpfs. the seeds are mounted from the host at runtime, so
# a rebuilt image never masks seed edits.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang libclang-rt-14-dev make git libcjson-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# a skill matching the skills_* seeds: SKILL.md with frontmatter
RUN mkdir -p /tmp/skills/probe && \
    printf -- '---\nname: probe\ndescription: fixture skill for the mcp_tools fuzzer\n---\nprobe body\n' \
        > /tmp/skills/probe/SKILL.md

# build once here; the artifacts are owned by root in /src/fuzz, and
# the runtime user only reads them
RUN make -C /src fuzz

# non-root, no matter what the host user id is
RUN useradd -m fuzzer
USER fuzzer
ENV HOME=/tmp

# run as: docker run ... llmkit-fuzz /src/fuzz/fuzz_mcp_tools <libFuzzer args> /seeds
CMD ["/bin/sh", "-c"]
