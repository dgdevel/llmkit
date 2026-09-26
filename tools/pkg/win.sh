#!/bin/sh
# win.sh - build llmkit.exe; runs inside the mingw cross container
# (africanfuture/msys2-base:linux-latest, tools/package.sh invokes it).
#
# that image is arch linux with the mingw-w64 cross toolchain
# (/usr/bin/x86_64-w64-mingw32-gcc, sysroot /usr/x86_64-w64-mingw32,
# static winpthreads) - a linux-hosted cross compiler, not an msys2
# runtime. it ships no libcurl and no libcjson for the target, so both
# are pinned and cross-built from source: libcurl static against
# schannel (windows cert store, no ca bundle) with every optional
# third-party dep off - the .a needs nothing but ws2_32/crypt32 and
# friends - and cjson via tools/pkg/cjson.sh with CC/AR pointed at the
# cross tools. the exe is fully static: no dlls next to it.
set -eu
VER=${VER:?}
CURL_VER=${CURL_VER:-8.22.0}
cd "$(dirname "$0")/../.."

fetch() { # url out: curl when installed, python3 otherwise (image has no curl cli)
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    else
        python3 -c "import sys, urllib.request; \
                    urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$1" "$2"
    fi
}

# ---- toolchain ---------------------------------------------------------------
# the image also carries a native linux gcc - never that one; CC= overrides
cc=${CC:-x86_64-w64-mingw32-gcc}
command -v "$cc" >/dev/null 2>&1 || { echo "win.sh: no $cc on PATH" >&2; exit 1; }
trip=$("$cc" -dumpmachine)
case "$trip" in
    *w64-mingw*) ;;
    *) echo "win.sh: $cc targets $trip, want *-w64-mingw32" >&2; exit 1 ;;
esac
command -v make >/dev/null 2>&1 || { echo "win.sh: no make" >&2; exit 1; }
strip_cmd="$trip-strip"
command -v "$strip_cmd" >/dev/null 2>&1 || strip_cmd=""

# ---- dependencies: pinned, cross-built, static -------------------------------
build_curl() {
    rm -rf /tmp/curlwin /tmp/curl.src /tmp/curl.tar.xz
    mkdir -p /tmp/curlwin /tmp/curl.src
    echo "win.sh: cross-building libcurl $CURL_VER (schannel, static)"
    fetch "https://curl.se/download/curl-$CURL_VER.tar.xz" /tmp/curl.tar.xz
    tar -xf /tmp/curl.tar.xz -C /tmp/curl.src --strip-components=1
    (cd /tmp/curl.src &&
     CC="$cc" AR="$trip-ar" RANLIB="$trip-ranlib" \
     ./configure --host="$trip" --prefix=/tmp/curlwin \
         --with-schannel --disable-shared --enable-static \
         --without-zlib --without-brotli --without-zstd \
         --without-libidn2 --without-libpsl --without-nghttp2 \
         --without-libssh2 --without-gssapi \
         --without-ca-bundle --without-ca-path \
         --disable-ldap --disable-ldaps --disable-manual >/dev/null &&
     make -s -j"$(nproc)" install >/dev/null)
    [ -f /tmp/curlwin/lib/libcurl.a ] \
        || { echo "win.sh: /tmp/curlwin/lib/libcurl.a missing after build" >&2; exit 1; }
}
build_curl
CC="$cc" AR="$trip-ar" sh tools/pkg/cjson.sh

# ---- build -------------------------------------------------------------------
rm -f llmkit llmkit.exe test/selfcheck test/selfcheck.exe  # no make clean: must not wipe dist/
# curl's static needs: ws2_32 (winsock), crypt32+secur32 (schannel/sspi),
# bcrypt, advapi32, iphlpapi (if_nametoindex)
make "VERSION=$VER" CC="$cc" "EXTRA_CFLAGS=-static -D__USE_MINGW_ANSI_STDIO=1 -DCURL_STATICLIB -I/tmp/curlwin/include -I/tmp/cj/include" "LDLIBS=/tmp/curlwin/lib/libcurl.a /tmp/cj/libcjson.a -lws2_32 -lcrypt32 -lsecur32 -lbcrypt -ladvapi32 -liphlpapi -lwinpthread"
exe=llmkit.exe
[ -f "$exe" ] || exe=llmkit
# a PE binary does not run on the linux host - the stamp check survives
# without wine, it just cannot verify
out=$(./"$exe" version 2>/dev/null || true)
if [ -n "$out" ]; then
    printf '%s' "$out" | grep -q "$VER" \
        || { echo "win.sh: version stamp not picked up" >&2; exit 1; }
else
    echo "win.sh: warning: cannot run $exe on this host - stamp not verified" >&2
fi
[ -z "$strip_cmd" ] || "$strip_cmd" "$exe"

# ---- pack --------------------------------------------------------------------
arch=$(printf '%s' "$trip" | cut -d- -f1)
stage="llmkit-$VER-windows-$arch"
rm -rf "dist/$stage"
mkdir -p "dist/$stage"
cp "$exe" LICENSE.md README.md "dist/$stage/"
if command -v zip >/dev/null 2>&1; then
    (cd dist && zip -q -r "$stage.zip" "$stage")
else
    (cd dist && python3 -c "import shutil; shutil.make_archive('$stage', 'zip', '.', '$stage')")
fi
rm -rf "dist/$stage"
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "win: dist/$stage.zip"
