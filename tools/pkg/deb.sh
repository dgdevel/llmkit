#!/bin/sh
# deb.sh - build the .deb; runs inside an ubuntu/debian container as root
# (tools/package.sh invokes it; can also run natively with dpkg-deb present).
#
# built on ubuntu:22.04 (glibc 2.35): installs on ubuntu 22.04/24.04+ and
# debian 12+ - newer glibc runs older binaries, so build on the oldest
# distro you want to support.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

export DEBIAN_FRONTEND=noninteractive
if [ "$(id -u)" = 0 ]; then
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        build-essential libcurl4-openssl-dev curl ca-certificates >/dev/null
fi

sh tools/pkg/cjson.sh
rm -f llmkit test/selfcheck   # no make clean: must not wipe dist/
make "VERSION=$VER" "CJSON=/tmp/cj/libcjson.a" "EXTRA_CFLAGS=-I/tmp/cj/include"
./llmkit version | grep -q "$VER" || { echo "deb.sh: version stamp not picked up" >&2; exit 1; }

arch=$(dpkg --print-architecture)
root=/tmp/debroot
rm -rf "$root"
mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/llmkit"
install -m 755 llmkit "$root/usr/bin/llmkit"
install -m 644 LICENSE.md "$root/usr/share/doc/llmkit/copyright"
install -m 644 README.md "$root/usr/share/doc/llmkit/README.md"
cat > "$root/DEBIAN/control" <<EOF
Package: llmkit
Version: $VER-1
Architecture: $arch
Maintainer: dgdevel <dgdevel@users.noreply.github.com>
Installed-Size: $(du -sk "$root/usr" | cut -f1)
Depends: libcurl4
Section: utils
Priority: optional
Homepage: https://github.com/dgdevel/llmkit
Description: one binary for llm interaction from the shell
 llmkit is a single executable for llm interaction from the shell and from
 other programs: repl, call, runner, agent-as-tool and mcp-proxy commands,
 talking to any openai- or anthropic-compatible endpoint, with mcp servers
 as tools.
EOF

mkdir -p dist
dpkg-deb --build --root-owner-group "$root" "dist/llmkit_${VER}-1_${arch}.deb" >/dev/null
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "deb: dist/llmkit_${VER}-1_${arch}.deb"
