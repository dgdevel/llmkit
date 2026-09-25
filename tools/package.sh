#!/bin/sh
# package.sh - build distro packages via docker; artifacts land in dist/.
#
# usage: tools/package.sh v1.2.3 [targets]
#   targets: deb rpm-fc rpm-el9 arch (default: all four)
#
# each target builds in its distro's own container with that distro's own
# packaging tools (dpkg-deb / rpmbuild / makepkg). the binary embeds a
# pinned static cjson (tools/pkg/cjson.sh) and links the system libcurl,
# so packages depend on libcurl only.
#
# build-host choice matters: a binary runs on the build host's glibc or
# newer, so each target builds on the oldest distro it should support:
#   deb      ubuntu:22.04          glibc 2.35 -> ubuntu 22.04/24.04+, debian 12+
#   rpm-fc   fedora:41             -> fedora 41+
#   rpm-el9  rockylinux:9          -> rhel/alma/rocky 9+
#   arch     archlinux:base-devel  -> rolling, always current
set -eu
die() { echo "package.sh: $*" >&2; exit 1; }

TAG=${1:-}
[ -n "$TAG" ] || die "usage: tools/package.sh v1.2.3 [deb rpm-fc rpm-el9 arch]"
printf '%s' "$TAG" | grep -Eq '^v?[0-9]+\.[0-9]+(\.[0-9]+)?$' \
    || die "version '$TAG' must look like v1.2.3"
VER=${TAG#v}
shift
targets=${*:-"deb rpm-fc rpm-el9 arch"}

cd "$(dirname "$0")/.."
repo=$PWD

command -v docker >/dev/null 2>&1 || die "docker is not installed"
docker info >/dev/null 2>&1 || die "docker daemon is not running"

mkdir -p dist
for t in $targets; do
    case "$t" in
        deb)     image=ubuntu:22.04;         script=deb.sh  ;;
        rpm-fc)  image=fedora:41;            script=rpm.sh  ;;
        rpm-el9) image=rockylinux:9;         script=rpm.sh  ;;
        arch)    image=archlinux:base-devel; script=arch.sh ;;
        *) die "unknown target '$t' (want: deb rpm-fc rpm-el9 arch)" ;;
    esac
    echo "==> $t ($image)"
    docker run --rm -e VER="$VER" -v "$repo":/src -w /src \
        "$image" sh "tools/pkg/$script"
done

echo "packages in dist/:"
ls -l dist/*.deb dist/*.rpm dist/*.pkg.tar.zst 2>/dev/null || true
