CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -pthread -Isrc
LDLIBS   = -lcjson -lcurl -pthread

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
	rm -f llmkit test/selfcheck

.PHONY: all check check-ascii clean
