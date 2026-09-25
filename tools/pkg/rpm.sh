#!/bin/sh
# rpm.sh - build the .rpm; runs inside a fedora/rockylinux container as root
# (tools/package.sh invokes it; fedora:41 -> .fc41, rockylinux:9 -> .el9).
#
# build on the oldest distro you support: a binary needs the glibc of the
# build host or newer, so the el9 build covers every rhel-9+ derivative and
# the fc41 build covers fedora 41+.
set -eu
VER=${VER:?}
cd "$(dirname "$0")/../.."

if [ "$(id -u)" = 0 ]; then
    # --allowerasing: el9 images ship curl-minimal, which conflicts with the
    # full curl package; this lets dnf swap it (no-op on plain fedora)
    dnf install -y -q --allowerasing \
        gcc make curl ca-certificates libcurl-devel rpm-build >/dev/null
fi

sh tools/pkg/cjson.sh
rm -f llmkit test/selfcheck   # no make clean: must not wipe dist/
make "VERSION=$VER" "CJSON=/tmp/cj/libcjson.a" "EXTRA_CFLAGS=-I/tmp/cj/include"
./llmkit version | grep -q "$VER" || { echo "rpm.sh: version stamp not picked up" >&2; exit 1; }

top=/tmp/rpmbuild
rm -rf "$top"
mkdir -p "$top/BUILD" "$top/BUILDROOT" "$top/RPMS" "$top/SOURCES" "$top/SPECS" "$top/SRPMS"
cat > "$top/SPECS/llmkit.spec" <<EOF
Name: llmkit
Version: $VER
Release: 1%{?dist}
Summary: one binary for llm interaction from the shell
License: EUPL-1.2
URL: https://github.com/dgdevel/llmkit
Requires: libcurl

%description
llmkit is a single executable for llm interaction from the shell and from
other programs: repl, call, runner, agent-as-tool and mcp-proxy commands,
talking to any openai- or anthropic-compatible endpoint, with mcp servers
as tools.

%files
%{_bindir}/llmkit
/usr/share/licenses/llmkit/LICENSE.md
/usr/share/doc/llmkit/README.md

%install
install -Dm755 '%{llmsrc}/llmkit' '%{buildroot}%{_bindir}/llmkit'
install -Dm644 '%{llmsrc}/LICENSE.md' '%{buildroot}/usr/share/licenses/llmkit/LICENSE.md'
install -Dm644 '%{llmsrc}/README.md' '%{buildroot}/usr/share/doc/llmkit/README.md'
EOF

rpmbuild --define "_topdir $top" --define "llmsrc $PWD" \
    -bb "$top/SPECS/llmkit.spec" >/dev/null
mkdir -p dist
mv "$top"/RPMS/*/*.rpm dist/
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "rpm: $(ls dist/llmkit-*.rpm)"
