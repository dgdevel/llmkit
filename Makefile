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
       src/builtin.c src/call.c src/editor.c src/repl.c src/mcprepl.c \
       src/pretty.c src/llmproxy.c src/prompts.gen.c \
       src/vendor/linenoise/linenoise.c
MAIN = src/main.c
TEST = test/selfcheck.c

# prompt texts bundled into the binary (the rule lives below `all`)
PROMPTS_TXT = $(shell find src/prompts -type f -name '*.txt' 2>/dev/null)

# the vendored openbsd regex (src/vendor/regex): windows only - mingw-w64
# ships no <regex.h>, posix links the libc one. detected from the compiler
# target, so the cross build (tools/pkg/win.sh) and a native msys2 make
# get it alike. the sources are separate translation units (regex2.h has
# no include guard - one amalgamated unit cannot compile) and cannot be
# modified, so their libc shims are force-included per unit from
# src/win_regex_glue.h.
host := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq (,$(findstring mingw,$(host)))
SRC += src/vendor/regex/regcomp.c src/vendor/regex/regexec.c \
       src/vendor/regex/regerror.c src/vendor/regex/regfree.c
CFLAGS += -Isrc/vendor/regex -include src/win_regex_glue.h
endif

all: llmkit

# prompt texts bundled into the binary: tools/gen-prompts.sh preprocesses
# src/prompts/**/*.txt into src/prompts.gen.{c,h} (a changed txt or script
# regenerates them before the next compilation). both targets share the
# one recipe run: gen-prompts.sh writes the pair together.
src/prompts.gen.c src/prompts.gen.h: $(PROMPTS_TXT) tools/gen-prompts.sh
	tools/gen-prompts.sh

# runs before every compilation (order-only: gates the build without
# forcing a relink when nothing changed)
check-ascii:
	@tools/check-ascii.sh

llmkit: $(MAIN) $(SRC) src/llmkit.h src/prompts.gen.h | check-ascii
	$(CC) $(CFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

test/selfcheck: $(TEST) $(SRC) src/llmkit.h src/prompts.gen.h | check-ascii
	@mkdir -p test
	$(CC) $(CFLAGS) -o $@ $(TEST) $(SRC) $(LDLIBS)

check: test/selfcheck
	./test/selfcheck

clean:
	rm -rf dist
	rm -f llmkit test/selfcheck llmkit.exe src/prompts.gen.c src/prompts.gen.h

# tag, build and publish binaries to GitHub Releases (needs gh)
# usage: make release TAG=v1.2.3
release:
	@tools/release.sh "$(TAG)"

# build distro packages (.deb/.rpm/.pkg.tar.zst/.zip) in docker; needs
# docker, artifacts land in dist/. usage: make packages TAG=v1.2.3 [TGT="deb arch"]
packages:
	@tools/package.sh "$(TAG)" $(TGT)

.PHONY: all check check-ascii clean release packages
