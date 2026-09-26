#!/bin/sh
# cjson.sh - build a pinned static libcjson (v1.7.18) into /tmp/cj.
#
# distro packages statically embed the json parser: libcjson is not packaged
# everywhere (el9, older fedora) while libcurl.so.4 is universal, so packages
# end up depending on libcurl only. the normal non-docker build keeps linking
# the system libcjson. CC=/AR= point it at cross tools (tools/pkg/win.sh).
set -eu
rm -rf /tmp/cj /tmp/cjson.src /tmp/cjson.tar.gz
mkdir -p /tmp/cj/include/cjson /tmp/cjson.src

fetch() { # url out: curl when installed, python3 otherwise (some containers ship neither)
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    else
        python3 -c "import sys, urllib.request; \
                    urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$1" "$2"
    fi
}

# fetch to a file first so a download failure is a clean error, not
# "tar: unexpected end of file"
fetch https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz /tmp/cjson.tar.gz
tar -xzf /tmp/cjson.tar.gz -C /tmp/cjson.src --strip-components=1
"${CC:-cc}" -O2 -fPIC -I/tmp/cjson.src -c /tmp/cjson.src/cJSON.c -o /tmp/cj/cJSON.o
cp /tmp/cjson.src/cJSON.h /tmp/cj/include/cjson/
"${AR:-ar}" rcs /tmp/cj/libcjson.a /tmp/cj/cJSON.o
