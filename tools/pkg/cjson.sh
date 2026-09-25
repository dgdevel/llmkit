#!/bin/sh
# cjson.sh - build a pinned static libcjson (v1.7.18) into /tmp/cj.
#
# distro packages statically embed the json parser: libcjson is not packaged
# everywhere (el9, older fedora) while libcurl.so.4 is universal, so packages
# end up depending on libcurl only. the normal non-docker build keeps linking
# the system libcjson.
set -eu
rm -rf /tmp/cj /tmp/cjson.src /tmp/cjson.tar.gz
mkdir -p /tmp/cj/include/cjson /tmp/cjson.src
# fetch to a file first so a download failure is a clean curl error, not
# "tar: unexpected end of file"; needs ca-certificates, which the pkg
# scripts install before calling this (base containers ship without it)
curl -fsSL -o /tmp/cjson.tar.gz https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
tar -xzf /tmp/cjson.tar.gz -C /tmp/cjson.src --strip-components=1
cc -O2 -fPIC -I/tmp/cjson.src -c /tmp/cjson.src/cJSON.c -o /tmp/cj/cJSON.o
cp /tmp/cjson.src/cJSON.h /tmp/cj/include/cjson/
ar rcs /tmp/cj/libcjson.a /tmp/cj/cJSON.o
