CC       ?= gcc
CFLAGS   ?= -Os -g0 -Wall -Wextra -Wpedantic -std=c17 -D_DEFAULT_SOURCE
LDFLAGS  ?=
STATIC   ?= 0

# Lint / format tools (overridable for CI or alternative toolchains)
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy

SRCDIR   := src
OBJDIR   := build
TARGET   := llmkit

SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

SCRIPTS  := scripts
SOURCES  := $(SRCS) $(wildcard $(SRCDIR)/*.h)

# Files checked by clang-format / clang-tidy.
# NOTE: $(SRCDIR)/*.c is a non-recursive glob, so src/vendor/* is excluded
# automatically. Vendored third-party code is never reformatted or linted.
FORMAT_SOURCES := $(SRCDIR)/*.c $(SRCDIR)/*.h tests/*.c
LINT_SOURCES   := $(SRCS) $(wildcard tests/*.c)

ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

# Auto-detect library paths via pkg-config
YAML_CFLAGS  := $(shell pkg-config --cflags yaml-0.1 2>/dev/null)
YAML_LIBS    := $(shell pkg-config --libs --static yaml-0.1 2>/dev/null || echo "-lyaml")
CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS    := $(shell pkg-config --libs --static libcurl 2>/dev/null || echo "-lcurl")
CRYPTO_CFLAGS:= $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS  := $(shell pkg-config --libs --static libcrypto 2>/dev/null || echo "-lcrypto")
SSL_LIBS     := $(shell pkg-config --libs --static libssl 2>/dev/null || echo "-lssl")

# cJSON: prefer system, fall back to vendored
CJSON_CFLAGS :=
CJSON_LIBS   :=
ifneq ($(shell pkg-config --exists libcjson 2>/dev/null && echo yes),yes)
  ifneq ($(wildcard $(SRCDIR)/vendor/cJSON.c),)
    CJSON_CFLAGS := -I$(SRCDIR)/vendor
    CJSON_SRC    := $(SRCDIR)/vendor/cJSON.c
  else
    $(warning cJSON not found via pkg-config and no vendored copy in $(SRCDIR)/vendor/)
    $(warning Run 'make vendors' to fetch cJSON or install libcjson via package manager)
  endif
else
  CJSON_CFLAGS := $(shell pkg-config --cflags libcjson)
  CJSON_LIBS   := $(shell pkg-config --libs --static libcjson 2>/dev/null || echo "-lcjson")
endif

override CFLAGS += $(YAML_CFLAGS) $(CURL_CFLAGS) $(CRYPTO_CFLAGS) $(CJSON_CFLAGS)
LIBS := $(YAML_LIBS) $(CURL_LIBS) $(CRYPTO_LIBS) $(SSL_LIBS) $(CJSON_LIBS)

# Compile flags passed to clang-tidy. Mirrors CFLAGS minus the warning flags
# (clang-tidy has its own diagnostics) plus the include paths gathered above.
TIDY_FLAGS := -std=c17 -D_DEFAULT_SOURCE -I $(SRCDIR) \
              $(YAML_CFLAGS) $(CURL_CFLAGS) $(CRYPTO_CFLAGS) $(CJSON_CFLAGS)

.PHONY: all debug profile test clean install uninstall dist check-ascii \
        vendors check-deps test_utf8 test_util test_config test_jsonrpc test_transport \
        test_mcp test_conversation test_llm test_cli test_agent test_proxy \
        format format-check lint lint-analyzer windows windows32 clean-win \
        macos dist-linux dist-windows dist-macos

# The build phase runs the ASCII check, the format check, and the linter
# before compiling. Skip with `make $(TARGET)` if you only need the binary.
all: check-ascii format-check lint $(TARGET)

debug: CFLAGS = -Og -g3 -Wall -Wextra -Wpedantic -std=c17 -D_DEFAULT_SOURCE -DLLMKIT_DEBUG
debug: LDFLAGS += -fsanitize=address
debug: all

profile: CFLAGS = -O2 -g -pg -Wall -Wextra -Wpedantic -std=c17 -D_DEFAULT_SOURCE
profile: LDFLAGS += -pg
profile: all

check-ascii:
	@$(SCRIPTS)/check-ascii.py $(SOURCES) $(SCRIPTS)/check-ascii.py Makefile Makefile.cross

# --- Code formatting (clang-format) -------------------------------------
# `format`        rewrites files in place; run it before committing.
# `format-check`  is a read-only check used by the build / CI; it fails the
#                 build if any file is not formatted. Vendored files under
#                 src/vendor/ are excluded by the non-recursive glob above.
format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

format-check:
	@$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

# --- Static analysis (clang-tidy) ---------------------------------------
# Runs clang-tidy in parallel over every translation unit in src/ and tests/.
# Warnings are promoted to errors via --warnings-as-errors so the build fails
# on any lint violation. The checks and naming rules live in .clang-tidy at the
# project root.
#
# `clang-analyzer-*` checks are *expensive* (path-sensitive symbolic execution)
# and are excluded from the default lint target. Run `make lint-analyzer` to
# include them (e.g. pre-commit or CI on-demand).
#
#   make lint          -- fast: all checks except clang-analyzer
#   make lint-analyzer -- deep: full checks including clang-analyzer
#
lint:
	@printf "%s\n" $(LINT_SOURCES) | \
	  xargs -P $$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2) -r -I{} \
	    $(CLANG_TIDY) --quiet --warnings-as-errors='*' \
	      --config-file=.clang-tidy --checks='-clang-analyzer-*' \
	      "{}" -- $(TIDY_FLAGS)

lint-analyzer:
	@printf "%s\n" $(LINT_SOURCES) | \
	  xargs -P $$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2) -r -I{} \
	    $(CLANG_TIDY) --quiet --warnings-as-errors='*' \
	      --config-file=.clang-tidy \
	      "{}" -- $(TIDY_FLAGS)

$(TARGET): $(OBJS) $(if $(CJSON_SRC),$(OBJDIR)/cJSON.o)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

ifdef CJSON_SRC
$(OBJDIR)/cJSON.o: $(CJSON_SRC) | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<
endif

$(OBJDIR):
	mkdir -p $(OBJDIR)

TEST_BINS := tests/test_utf8 tests/test_util tests/test_config tests/test_jsonrpc \
             tests/test_transport tests/test_mcp tests/test_conversation tests/test_llm \
             tests/test_steering

clean:
	rm -rf $(OBJDIR) $(TARGET) build-win llmkit.exe $(TEST_BINS) dist

install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

# --- Cross-compilation (Linux -> Windows via MinGW-w64) --------------------
# The cross-build logic lives in Makefile.cross. These targets delegate to it
# so `make windows` / `make windows32` work from the project root.
windows:
	$(MAKE) -f Makefile.cross windows

windows32:
	$(MAKE) -f Makefile.cross windows32

clean-win:
	$(MAKE) -f Makefile.cross clean-win

# --- Native macOS build ------------------------------------------------------
# macOS ships LibreSSL (no <openssl/sha.h>) and a keg-only Homebrew OpenSSL,
# so the Homebrew prefix must be on PKG_CONFIG_PATH for the libcrypto probe to
# succeed. This target finds Homebrew openssl@3 (falling back to openssl) and
# exports its pkgconfig dir before re-invoking the native build.
#
# macOS has NO cross-compile path from Linux (no standard toolchain like
# MinGW), so this target refuses to run off macOS. Verify macOS builds via a
# macOS CI runner or by running `make macos` on a real Mac.
macos:
	@if [ "$$(uname -s)" != "Darwin" ]; then \
		echo "Error: 'make macos' builds natively on macOS and cannot be cross-compiled." >&2; \
		echo "       Run it on a Mac, or verify via a macOS CI runner." >&2; \
		exit 1; \
	fi
	@brew_prefix=$$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null); \
	if [ -n "$$brew_prefix" ] && [ -d "$$brew_prefix/lib/pkgconfig" ]; then \
		echo "[macos] using Homebrew OpenSSL at $$brew_prefix"; \
		export PKG_CONFIG_PATH="$$brew_prefix/lib/pkgconfig:$$PKG_CONFIG_PATH"; \
	else \
		echo "[macos] no Homebrew OpenSSL found; assuming libcrypto is already on the pkg-config path"; \
	fi; \
	$(MAKE) $(TARGET)

# --- Release archives for all supported platforms ----------------------------
# `dist` dispatches by host OS:
#   Linux  -> per-platform archives for Linux (native) + Windows (cross-compiled)
#   macOS  -> per-platform archive for macOS (native; Linux/Windows not built here)
# Each archive contains the stripped binary + README + LICENSE + docs/.
HOST_OS   := $(shell uname -s)
WIN_TARGET := llmkit.exe
VERSION    := $(shell git describe --tags 2>/dev/null || echo "dev")
DIST_DOCS  := README.md LICENSE.md docs/

ifeq ($(HOST_OS),Linux)
dist: dist-linux dist-windows
else ifeq ($(HOST_OS),Darwin)
dist: dist-macos
else
dist:
	@echo "Error: unsupported host OS for 'make dist': $(HOST_OS)" >&2; exit 1
endif

dist-linux: all
	@if [ "$(HOST_OS)" != "Linux" ]; then \
		echo "Error: 'make dist-linux' builds the Linux native binary and must run on Linux." >&2; exit 1; \
	fi
	@mkdir -p dist
	strip $(TARGET)
	tar czf dist/llmkit-linux-x86_64-$(VERSION).tar.gz $(TARGET) $(DIST_DOCS)
	@echo "[dist] -> dist/llmkit-linux-x86_64-$(VERSION).tar.gz"

dist-windows: windows
	@if [ "$(HOST_OS)" != "Linux" ]; then \
		echo "Error: 'make dist-windows' cross-compiles from a Linux host." >&2; exit 1; \
	fi
	@mkdir -p dist
	x86_64-w64-mingw32-strip $(WIN_TARGET)
	zip -qr dist/llmkit-windows-x86_64-$(VERSION).zip $(WIN_TARGET) $(DIST_DOCS)
	@echo "[dist] -> dist/llmkit-windows-x86_64-$(VERSION).zip"

dist-macos: macos
	@mkdir -p dist
	strip $(TARGET)
	tar czf dist/llmkit-macos-x86_64-$(VERSION).tar.gz $(TARGET) $(DIST_DOCS)
	@echo "[dist] -> dist/llmkit-macos-x86_64-$(VERSION).tar.gz"

vendors:
	@echo "Fetching vendored dependencies..."
	@mkdir -p $(SRCDIR)/vendor
	@if [ ! -f $(SRCDIR)/vendor/cJSON.c ]; then \
		echo "Downloading cJSON..."; \
		curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o $(SRCDIR)/vendor/cJSON.c; \
		curl -sL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o $(SRCDIR)/vendor/cJSON.h; \
		echo "cJSON vendored to $(SRCDIR)/vendor/"; \
	else \
		echo "cJSON already vendored."; \
	fi

check-deps:
	@echo "Checking dependencies..."
	@for lib in yaml-0.1 libcurl libcrypto libssl; do \
		if pkg-config --exists $$lib 2>/dev/null; then \
			echo "  [ok] $$lib"; \
		else \
			echo "  [MISSING] $$lib"; \
		fi; \
	done
	@if pkg-config --exists libcjson 2>/dev/null; then \
		echo "  [ok] libcjson (system)"; \
	elif [ -f $(SRCDIR)/vendor/cJSON.c ]; then \
		echo "  [ok] cJSON (vendored)"; \
	else \
		echo "  [MISSING] cJSON -- install libcjson or run 'make vendors'"; \
	fi
	@echo "Done."

test: test_utf8 test_util test_config test_jsonrpc test_transport test_mcp test_conversation test_llm \
       test_steering test_cli test_agent test_proxy
	@echo "All tests passed."

test_utf8: tests/test_utf8.c src/utf8.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_utf8 tests/test_utf8.c src/utf8.c
	./tests/test_utf8

test_config: tests/test_config.c src/config.c src/util.c src/utf8.c src/platform.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_config tests/test_config.c src/config.c src/util.c src/utf8.c src/platform.c $(LIBS)
	./tests/test_config

test_util: tests/test_util.c src/util.c src/utf8.c src/platform.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_util tests/test_util.c src/util.c src/utf8.c src/platform.c $(LIBS)
	./tests/test_util

test_jsonrpc: tests/test_jsonrpc.c src/jsonrpc.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC)
	$(CC) $(CFLAGS) -Isrc -o tests/test_jsonrpc tests/test_jsonrpc.c src/jsonrpc.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC) $(LIBS)
	./tests/test_jsonrpc

test_transport: tests/test_transport.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_transport tests/test_transport.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c $(LIBS)
	./tests/test_transport

test_mcp: tests/test_mcp.c src/mcp.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c src/jsonrpc.c $(CJSON_SRC)
	$(CC) $(CFLAGS) -Isrc -o tests/test_mcp tests/test_mcp.c src/mcp.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c src/jsonrpc.c $(CJSON_SRC) $(LIBS)
	./tests/test_mcp

test_conversation: tests/test_conversation.c src/conversation.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC)
	$(CC) $(CFLAGS) -Isrc -o tests/test_conversation tests/test_conversation.c src/conversation.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC) $(LIBS)
	./tests/test_conversation

test_llm: tests/test_llm.c src/llm.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC)
	$(CC) $(CFLAGS) -Isrc -o tests/test_llm tests/test_llm.c src/llm.c src/util.c src/utf8.c src/platform.c $(CJSON_SRC) $(LIBS)
	./tests/test_llm

test_steering: tests/test_steering.c src/steering.c src/platform.c src/util.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_steering tests/test_steering.c src/steering.c src/platform.c src/util.c $(LIBS)
	./tests/test_steering

# --- Integration tests (Phase 12) --------------------------------------
# These exercise the compiled binary end-to-end. They require python3
# (for the mock LLM/MCP helpers) and a freshly built `llmkit` binary.
test_cli: $(TARGET)
	sh tests/test_cli.sh

test_agent: $(TARGET) tests/fixtures/fake_mcp.py tests/test_agent_integration.py
	python3 tests/test_agent_integration.py

test_proxy: $(TARGET) tests/fixtures/fake_mcp.py tests/test_proxy_integration.py
	python3 tests/test_proxy_integration.py

