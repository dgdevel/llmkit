#!/bin/sh
# package.sh - build distro packages via docker; artifacts land in dist/.
#
# usage: tools/package.sh v1.2.3 [targets]
#   targets: deb rpm-fc rpm-el9 arch win mac (default: all six)
#
# each target builds in its distro's own container with that distro's own
# packaging tools (dpkg-deb / rpmbuild / makepkg / zip). the linux binary
# embeds a pinned static cjson (tools/pkg/cjson.sh) and links the system
# libcurl, so those packages depend on libcurl only. the windows build
# cross-builds pinned static curl (schannel) and cjson with the mingw
# toolchain instead: the zip holds the binary, license and readme only.
#
# build-host choice matters: a binary runs on the build host's glibc or
# newer, so each target builds on the oldest distro it should support:
#   deb      ubuntu:22.04          glibc 2.35 -> ubuntu 22.04/24.04+, debian 12+
#   rpm-fc   fedora:41             -> fedora 41+
#   rpm-el9  rockylinux:9          -> rhel/alma/rocky 9+
#   arch     archlinux:base-devel  -> rolling, always current
#   win      africanfuture/msys2-base:linux-latest
#                                  arch linux + mingw-w64 cross toolchain;
#                                  curl and cjson are pinned and
#                                  cross-built from source, fully static
#                                  -> any 64-bit windows 10+
#   mac      local build of tools/pkg/mac.Dockerfile
#                                  ubuntu + osxcross (crazymax/osxcross,
#                                  MacOSX 11.3 SDK, min 10.13); openssl,
#                                  curl and cjson are pinned and
#                                  cross-built from source, static curl
#                                  against openssl with the CA trust
#                                  anchored at /etc/ssl/cert.pem (the
#                                  store macOS ships)
#                                  -> macOS 10.13+ x86_64 (rosetta 2 on
#                                  apple silicon)
#
# the win cross-build of curl takes minutes, so its installed prefix is
# cached on the host across runs in ${XDG_CACHE_HOME:-$HOME/.cache}/
# llmkit-pkg/win and bind-mounted at /pkgcache; win.sh reuses it while
# the curl version, triplet and configure flags are unchanged (the mac
# target does the same under llmkit-pkg/mac). delete either dir to
# force a from-scratch curl build.
set -eu
die() { echo "package.sh: $*" >&2; exit 1; }

TAG=${1:-}
[ -n "$TAG" ] || die "usage: tools/package.sh v1.2.3 [deb rpm-fc rpm-el9 arch win mac]"
printf '%s' "$TAG" | grep -Eq '^v?[0-9]+\.[0-9]+(\.[0-9]+)?$' \
    || die "version '$TAG' must look like v1.2.3"
VER=${TAG#v}
shift
targets=${*:-"deb rpm-fc rpm-el9 arch win mac"}

cd "$(dirname "$0")/.."
repo=$PWD

command -v docker >/dev/null 2>&1 || die "docker is not installed"
docker info >/dev/null 2>&1 || die "docker daemon is not running"

mkdir -p dist
for t in $targets; do
    extra=
    case "$t" in
        deb)     image=ubuntu:22.04;         script=deb.sh  ;;
        rpm-fc)  image=fedora:41;            script=rpm.sh  ;;
        rpm-el9) image=rockylinux:9;         script=rpm.sh  ;;
        arch)    image=archlinux:base-devel; script=arch.sh ;;
        win)     image=africanfuture/msys2-base:linux-latest; script=win.sh
                 cache=${XDG_CACHE_HOME:-$HOME/.cache}/llmkit-pkg/win
                 mkdir -p "$cache"
                 extra="-v $cache:/pkgcache" ;;
        mac)
                 # no runnable upstream image: mac.Dockerfile wraps the
                 # FROM-scratch crazymax/osxcross toolchain in ubuntu.
                 # built once locally as llmkit-osxcross:11.3; the docker
                 # layer cache makes later rebuilds a no-op.
                 image=llmkit-osxcross:11.3
                 docker build -q -t "$image" -f tools/pkg/mac.Dockerfile . \
                     >/dev/null || die "mac: building $image failed"
                 script=mac.sh
                 cache=${XDG_CACHE_HOME:-$HOME/.cache}/llmkit-pkg/mac
                 mkdir -p "$cache"
                 extra="-v $cache:/pkgcache" ;;
        *) die "unknown target '$t' (want: deb rpm-fc rpm-el9 arch win mac)" ;;
    esac
    echo "==> $t ($image)"
    # $extra stays unquoted on purpose: one -v flag, space-free cache path
    docker run --rm -e VER="$VER" $extra -v "$repo":/src -w /src \
        "$image" sh "tools/pkg/$script"
done

echo "packages in dist/:"
ls -l dist/*.deb dist/*.rpm dist/*.pkg.tar.zst dist/*.zip 2>/dev/null || true
