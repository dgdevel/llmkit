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
       src/builtin.c src/builtin/util.c src/builtin/files.c \
       src/builtin/web.c src/builtin/analyze.c src/builtin/process.c \
       src/builtin/skills.c src/builtin/now.c \
       src/crash_report.c \
       src/call.c src/models.c src/editor.c src/repl.c src/mcprepl.c \
       src/update.c \
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
# FindMimeFromData (builtin/analyze.c content-type sniff)
LDLIBS += -lurlmon -lole32
endif

all: llmkit

# fuzzing: libFuzzer harnesses over the project sources (fuzz/README.md).
# clang only, built on explicit request. FUZZFLAGS defaults to asan;
FUZZFLAGS ?= -fsanitize=address
#   make fuzz FUZZFLAGS=-fsanitize=address,undefined
# run one: ./fuzz/fuzz_sse fuzz/seeds/sse
# fuzz_mcp_tools (real tools/call) is docker-only: fuzz/docker-fuzz.sh
FUZZ_HARNESS = sse jsonl html regex proxy mcp mcp_tools

fuzz: $(foreach h,$(FUZZ_HARNESS),fuzz/fuzz_$(h))

fuzz/fuzz_%: fuzz/fuzz_%.c $(SRC) src/llmkit.h src/prompts.gen.h | check-ascii
	@mkdir -p $(@D)
	clang $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link \
	      $(FUZZFLAGS) -o $@ $< $(SRC) $(LDLIBS) -fsanitize=fuzzer $(FUZZFLAGS)

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

# valgrind pass over the self-check suite (needs valgrind installed)
# usage: make check-vg   [VG=...] swaps the tool, [VGFLAGS=...] extra flags
VG      ?= valgrind
VGFLAGS ?= --leak-check=full --show-leak-kinds=definite \
           --errors-for-leak-kinds=definite --track-origins=yes \
           --child-silent-after-fork=yes --fair-sched=yes \
           --error-exitcode=99 --log-file=test/valgrind.log

# -O1 instead of -O2: faster under valgrind, still debuggable
test/selfcheck-vg: $(TEST) $(SRC) src/llmkit.h src/prompts.gen.h | check-ascii
	@mkdir -p test
	$(CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -o $@ $(TEST) $(SRC) $(LDLIBS)

check-vg: test/selfcheck-vg
	$(VG) $(VGFLAGS) ./test/selfcheck-vg
	@echo "valgrind clean (log: test/valgrind.log)"

# sanitizer build of the same binary (plan.md task 2): the default
# `make` target stays untouched; the sanitized objects are compiled
# fresh into build/asan so they never collide with the plain ones.
# usage: make asan && LLMKIT_CRASH_BACKTRACE=1 ./build/asan/llmkit-asan builtin-mcp
ASANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g

build/asan/llmkit-asan: $(MAIN) $(SRC) src/llmkit.h src/prompts.gen.h | check-ascii
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(ASANFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

asan: build/asan/llmkit-asan

clean:
	rm -rf dist
	rm -rf fuzz/out
	rm -rf build
	rm -f llmkit test/selfcheck test/selfcheck-vg test/valgrind.log \
	      llmkit.exe src/prompts.gen.c src/prompts.gen.h \
	      fuzz/fuzz_sse fuzz/fuzz_jsonl fuzz/fuzz_html \
	      fuzz/fuzz_regex fuzz/fuzz_proxy fuzz/fuzz_mcp \
	      fuzz/fuzz_mcp_tools

# tag, build and publish binaries to GitHub Releases (needs gh)
# usage: make release TAG=v1.2.3
release:
	@tools/release.sh "$(TAG)"

# build distro packages (.deb/.rpm/.pkg.tar.zst/.zip) in docker; needs
# docker, artifacts land in dist/. usage: make packages TAG=v1.2.3 [TGT="deb arch"]
packages:
	@tools/package.sh "$(TAG)" $(TGT)

.PHONY: all check check-ascii check-vg clean release packages fuzz asan
