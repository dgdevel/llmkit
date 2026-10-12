# mac.Dockerfile - thin runnable image around crazymax/osxcross.
#
# the upstream image is FROM scratch: it holds only /osxcross (clang
# wrappers o64-clang/oa64-clang..., cctools ld64/ar/strip, the packaged
# SDK) and cannot be docker-run itself. this stage copies that directory
# into a stock ubuntu with a host clang (the wrappers wrap the host
# compiler), which tools/package.sh builds once and layer-caches as
# llmkit-osxcross:11.3 before running tools/pkg/mac.sh in it.
#
# tag 11.3: MacOSX 11.3 SDK, osxcross built with OSX_VERSION_MIN=10.13,
# so binaries target macOS 10.13+ x86_64 (apple silicon runs them via
# rosetta 2). the arm64 wrappers are not part of that osxcross build -
# a lipo universal binary would need a newer image and is future work.
FROM crazymax/osxcross:11.3 AS osxcross

FROM ubuntu:24.04
COPY --from=osxcross /osxcross /osxcross
RUN export DEBIAN_FRONTEND=noninteractive && apt-get update \
    && apt-get install -y --no-install-recommends \
        bash make clang lld libc6-dev git \
        ca-certificates curl zip python3 file xz-utils perl
ENV PATH="/osxcross/bin:$PATH"
ENV LD_LIBRARY_PATH="/osxcross/lib:$LD_LIBRARY_PATH"
# the repo bind-mounts in as root while the image user is host-uid; git
# refuses to look at it otherwise (check-ascii uses git ls-files)
RUN git config --global --add safe.directory /src
