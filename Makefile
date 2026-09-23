CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -pthread -Isrc
LDLIBS   = -lcjson -lcurl -pthread

SRC  = src/buf.c src/sse.c src/platform.c src/jsonl.c src/wire_openai.c \
       src/wire_anthropic.c src/engine.c src/mcp.c src/agent.c src/proxy.c \
       src/call.c
MAIN = src/main.c
TEST = test/selfcheck.c

all: llmkit

llmkit: $(MAIN) $(SRC) src/llmkit.h
	$(CC) $(CFLAGS) -o $@ $(MAIN) $(SRC) $(LDLIBS)

test/selfcheck: $(TEST) $(SRC) src/llmkit.h
	@mkdir -p test
	$(CC) $(CFLAGS) -o $@ $(TEST) $(SRC) $(LDLIBS)

check: test/selfcheck
	./test/selfcheck

clean:
	rm -f llmkit test/selfcheck

.PHONY: all check clean
