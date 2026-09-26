CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -pthread -Isrc
CFLAGS  += $(EXTRA_CFLAGS)
# json parser: system libcjson by default; distro packaging links a pinned
# static build instead (make CJSON=/tmp/cj/libcjson.a EXTRA_CFLAGS=-I/tmp/cj/include)
CJSON    = -lcjson
LDLIBS   = $(CJSON) -lcurl -pthread

# release builds stamp the version into the binary (tools/release.sh)
ifneq ($(VERSION),)
CFLAGS  += -DLLMKIT_VERSION=\"$(VERSION)\"
endif

SRC  = src/buf.c src/sse.c src/platform.c src/jsonl.c src/wire_openai.c \
       src/wire_anthropic.c src/engine.c src/mcp.c src/agent.c src/proxy.c \
       src/call.c src/repl.c
MAIN = src/main.c
TEST = test/selfcheck.c

all: llmkit

# runs before every compilation (order-only: gates the build without
# forcing a relink when nothing changed)
check-ascii:
	@tools/check-ascii.sh

llmkit: $(MAIN) $(SRC) src/llmkit.h | check-ascii
	$(CC) $(CFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

test/selfcheck: $(TEST) $(SRC) src/llmkit.h | check-ascii
	@mkdir -p test
	$(CC) $(CFLAGS) -o $@ $(TEST) $(SRC) $(LDLIBS)

check: test/selfcheck
	./test/selfcheck

clean:
	rm -rf dist
	rm -f llmkit test/selfcheck

# tag, build and publish binaries to GitHub Releases (needs gh)
# usage: make release TAG=v1.2.3
release:
	@tools/release.sh "$(TAG)"

# build distro packages (.deb/.rpm/.pkg.tar.zst/.zip) in docker; needs
# docker, artifacts land in dist/. usage: make packages TAG=v1.2.3 [TGT="deb arch"]
packages:
	@tools/package.sh "$(TAG)" $(TGT)

.PHONY: all check check-ascii clean release packages
