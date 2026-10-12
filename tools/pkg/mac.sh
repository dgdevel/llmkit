#!/bin/sh
# mac.sh - build llmkit for macOS x86_64; runs inside the osxcross
# container (image built by tools/pkg/mac.Dockerfile, tools/package.sh
# invokes it).
#
# the container is ubuntu + the osxcross toolchain from crazymax/osxcross
# (clang wrappers o64-clang, cctools ld64/ar/ranlib/strip/lipo, the
# packaged MacOSX 11.3 SDK; osxcross built with OSX_VERSION_MIN=10.13).
# the SDK ships no libcurl, no libcjson and no TLS stack, so all three
# are pinned and cross-built from source, like the windows build
# (tools/pkg/win.sh): openssl (TLS: curl removed its secure-transport
# backend in 8.22.0, so apple-system TLS is not an option - openssl it
# is, with the CA verification pointing at the cert store macOS itself
# ships, /etc/ssl/cert.pem), libcurl static against that openssl with
# every optional third-party dep off, and cjson via tools/pkg/cjson.sh
# with CC/AR pointed at the cross tools. unlike mingw, darwin has
# <regex.h>, so the vendored openbsd regex is not pulled in (the
# Makefile decides from -dumpmachine).
#
# the result is not fully static - no darwin binary is - it links the
# system libSystem only, so the zip holds a portable binary: macOS
# 10.13+ x86_64, apple silicon via rosetta 2.
set -eu
VER=${VER:?}
CURL_VER=${CURL_VER:-8.22.0}
OPENSSL_VER=${OPENSSL_VER:-3.5.9}
cd "$(dirname "$0")/../.."

fetch() { # url out: curl when installed, python3 otherwise
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    else
        python3 -c "import sys, urllib.request; \
                    urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$1" "$2"
    fi
}

# ---- toolchain ---------------------------------------------------------------
# never the native linux clang; CC= overrides
cc=${CC:-o64-clang}
command -v "$cc" >/dev/null 2>&1 || { echo "mac.sh: no $cc on PATH" >&2; exit 1; }
trip=$("$cc" -dumpmachine)
case "$trip" in
    *-apple-darwin*) ;;
    *) echo "mac.sh: $cc targets $trip, want *-apple-darwin" >&2; exit 1 ;;
esac
command -v make >/dev/null 2>&1 || { echo "mac.sh: no make" >&2; exit 1; }
strip_cmd="$trip-strip"
command -v "$strip_cmd" >/dev/null 2>&1 || strip_cmd=""

# ---- dependencies: pinned, cross-built, static -------------------------------
# openssl and curl together take a while to cross-build, so the installed
# prefixes are cached between runs (same scheme as win): package.sh
# bind-mounts ~/.cache/llmkit-pkg/mac at /pkgcache, a stamp (versions +
# triplet + configure flags) says whether the cache is still current. no
# /pkgcache (standalone run) just builds like before.
cache=${CURL_CACHE:-/pkgcache}
minver=10.13
sslflags="--prefix=/tmp/sslmac no-shared no-tests no-docs no-async"
conf="--with-openssl=/tmp/sslmac --with-ca-bundle=/etc/ssl/cert.pem --disable-shared --enable-static --without-zlib --without-brotli --without-zstd --without-libidn2 --without-libpsl --without-nghttp2 --without-libssh2 --without-gssapi --disable-ldap --disable-ldaps --disable-manual"
stamp="$CURL_VER $OPENSSL_VER $trip $conf"

build_ssl() {
    rm -rf /tmp/sslmac /tmp/ssl.src /tmp/ssl.tar.gz
    mkdir -p /tmp/sslmac /tmp/ssl.src
    echo "mac.sh: cross-building openssl $OPENSSL_VER (static)"
    # openssl-library.org soft-404s (http 200 + html error page) when it
    # lacks a file - the github release asset is the reliable mirror
    fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz" /tmp/ssl.tar.gz
    tar -xzf /tmp/ssl.tar.gz -C /tmp/ssl.src --strip-components=1
    # clang wrappers: darwin64-x86_64-cc selects the darwin target, CC=/
    # AR=/RANLIB= point the build at the osxcross tools, and the
    # macosx-version-min stamp goes through CFLAGS so everything sees it
    (cd /tmp/ssl.src &&
     CC="$cc" CFLAGS="-mmacosx-version-min=$minver" \
     AR="$trip-ar" RANLIB="$trip-ranlib" \
     ./Configure darwin64-x86_64-cc $sslflags >/dev/null &&
     make -s -j"$(nproc)" build_sw >/dev/null &&
     make -s install_sw >/dev/null)
    [ -f /tmp/sslmac/lib/libssl.a ] \
        || { echo "mac.sh: /tmp/sslmac/lib/libssl.a missing after build" >&2; exit 1; }
}

build_curl() {
    rm -rf /tmp/curlmac /tmp/curl.src /tmp/curl.tar.xz
    mkdir -p /tmp/curlmac /tmp/curl.src
    echo "mac.sh: cross-building libcurl $CURL_VER (openssl, static)"
    fetch "https://curl.se/download/curl-$CURL_VER.tar.xz" /tmp/curl.tar.xz
    tar -xf /tmp/curl.tar.xz -C /tmp/curl.src --strip-components=1
    (cd /tmp/curl.src &&
     CC="$cc" AR="$trip-ar" RANLIB="$trip-ranlib" \
     CFLAGS="-mmacosx-version-min=$minver" \
     PKG_CONFIG_PATH=/tmp/sslmac/lib/pkgconfig \
     ./configure --host="$trip" --prefix=/tmp/curlmac $conf >/dev/null &&
     make -s -j"$(nproc)" install >/dev/null)
    [ -f /tmp/curlmac/lib/libcurl.a ] \
        || { echo "mac.sh: /tmp/curlmac/lib/libcurl.a missing after build" >&2; exit 1; }
}

cache_save() {
    [ -d "$cache" ] || return 0
    if [ ! -w "$cache" ]; then
        echo "mac.sh: warning: $cache not writable - nothing cached" >&2
        return 0
    fi
    rm -rf "$cache/ssl.new" "$cache/curl.new"
    cp -a /tmp/sslmac "$cache/ssl.new"
    cp -a /tmp/curlmac "$cache/curl.new"
    printf '%s\n' "$stamp" >"$cache/ssl.new/.stamp"
    rm -rf "$cache/ssl" "$cache/curl"
    mv "$cache/ssl.new" "$cache/ssl"
    mv "$cache/curl.new" "$cache/curl"
    # the container runs as root; keep the host user able to rm the cache
    [ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" "$cache/ssl" "$cache/curl" || true
    echo "mac.sh: cached openssl $OPENSSL_VER + libcurl $CURL_VER in $cache for next runs"
}

if [ -f "$cache/ssl/lib/libssl.a" ] && [ -f "$cache/curl/lib/libcurl.a" ] \
   && [ "$(cat "$cache/ssl/.stamp" 2>/dev/null || true)" = "$stamp" ]; then
    echo "mac.sh: cached openssl $OPENSSL_VER + libcurl $CURL_VER current - reusing"
    rm -rf /tmp/sslmac /tmp/curlmac
    cp -a "$cache/ssl" /tmp/sslmac
    cp -a "$cache/curl" /tmp/curlmac
else
    build_ssl
    build_curl
    cache_save
fi
CC="$cc" AR="$trip-ar" sh tools/pkg/cjson.sh

# ---- build -------------------------------------------------------------------
rm -f llmkit llmkit.exe test/selfcheck test/selfcheck.exe  # no make clean: must not wipe dist/
# static curl against static openssl: the .a files carry no darwin
# dylib deps, libSystem is implicit. the two frameworks are curl's
# macos.o system-proxy support (CoreFoundation + SystemConfiguration) -
# linked regardless of the TLS backend.
make "VERSION=$VER" CC="$cc" "EXTRA_CFLAGS=-mmacosx-version-min=$minver -DCURL_STATICLIB -I/tmp/curlmac/include -I/tmp/sslmac/include -I/tmp/cj/include" "LDLIBS=/tmp/curlmac/lib/libcurl.a /tmp/sslmac/lib/libssl.a /tmp/sslmac/lib/libcrypto.a /tmp/cj/libcjson.a -framework CoreFoundation -framework SystemConfiguration"
bin=llmkit
[ -f "$bin" ] || { echo "mac.sh: $bin missing after build" >&2; exit 1; }
# a mach-o binary does not run on the linux host - like win, the stamp
# check survives without a mac, it just cannot verify
out=$(./"$bin" version 2>/dev/null || true)
if [ -n "$out" ]; then
    printf '%s' "$out" | grep -q "$VER" \
        || { echo "mac.sh: version stamp not picked up" >&2; exit 1; }
else
    echo "mac.sh: warning: cannot run $bin on this host - stamp not verified" >&2
fi
[ -z "$strip_cmd" ] || "$strip_cmd" "$bin"
file_out=$(file "$bin" 2>/dev/null || true)
printf '%s\n' "$file_out" | grep -q "Mach-O" \
    || { echo "mac.sh: $bin is not a Mach-O binary: $file_out" >&2; exit 1; }

# ---- pack --------------------------------------------------------------------
arch=$(printf '%s' "$trip" | cut -d- -f1)
stage="llmkit-$VER-macos-$arch"
rm -rf "dist/$stage"
mkdir -p "dist/$stage"
cp "$bin" LICENSE.md README.md "dist/$stage/"
if command -v zip >/dev/null 2>&1; then
    (cd dist && zip -q -r "$stage.zip" "$stage")
else
    (cd dist && python3 -c "import shutil; shutil.make_archive('$stage', 'zip', '.', '$stage')")
fi
rm -rf "dist/$stage"
[ "$(id -u)" = 0 ] && chown -R "$(stat -c '%u:%g' Makefile)" dist || true
echo "mac: dist/$stage.zip"
