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
        format format-check lint

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
# Runs clang-tidy over every translation unit in src/ and tests/. Warnings
# are promoted to errors via --warnings-as-errors so the build fails on any
# lint violation. The checks and naming rules live in .clang-tidy at the
# project root. Run `make lint` directly to see full diagnostics.
lint:
	@set -e; \
	for src in $(LINT_SOURCES); do \
		printf "[clang-tidy] %s\n" "$$src"; \
		$(CLANG_TIDY) --quiet --warnings-as-errors='*' \
		              --config-file=.clang-tidy \
		              "$$src" -- $(TIDY_FLAGS); \
	done

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

clean:
	rm -rf $(OBJDIR) $(TARGET) build-win llmkit.exe

install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

dist: all
	strip $(TARGET)
	tar czf llmkit-$(shell git describe --tags 2>/dev/null || echo "dev").tar.gz $(TARGET) docs/ Makefile Makefile.cross scripts/ src/ tests/

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

test: test_utf8 test_util test_config test_jsonrpc test_transport
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

test_jsonrpc: tests/test_jsonrpc.c src/jsonrpc.c src/util.c src/utf8.c src/platform.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_jsonrpc tests/test_jsonrpc.c src/jsonrpc.c src/util.c src/utf8.c src/platform.c $(LIBS)
	./tests/test_jsonrpc

test_transport: tests/test_transport.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c
	$(CC) $(CFLAGS) -Isrc -o tests/test_transport tests/test_transport.c src/mcp_transport.c src/util.c src/utf8.c src/platform.c $(LIBS)
	./tests/test_transport

