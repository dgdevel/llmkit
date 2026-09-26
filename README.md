# llmkit

A single static-purpose executable for llm interaction from the shell and
from other programs. One binary, five commands:

| command | what it does |
|---|---|
| `llmkit repl <flags>` | the interactive chat: type the turns, watch thinking and tool calls render live |
| `llmkit call <flags>` | one prompt in, one answer out: plain text on stdout, the shell one-liner front-end |
| `llmkit runner` | runs an llm conversation: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | exposes one agent conversation as a single `invoke` tool on a stdio mcp interface |
| `llmkit mcp-proxy <config.jsonl>` | exposes a curated view (rename, redescribe, hide) of upstream mcp servers on stdio |

Written in C11. Talks to any openai-compatible endpoint (chat completions
and responses apis) and any anthropic-compatible endpoint. Tools are mcp
servers: `stdio`, `http` (streamable http) and `sse` (legacy) transports,
protocol revisions v1 (`2024-11-05` ... `2025-11-25`) and v2 (`2026-07-28`).

The runner drives the whole loop - model turns, tool calls, steering -
with a record model designed for replay: the transcript a run produced is
a valid input for the next run, and the serialized request prefix is byte
stable across turns so endpoint prefix caches actually hit.

## example

For talking to a model there is `llmkit repl`: same flags as `call`,
minus `--prompt` - the turns are typed, every line is one turn of one
growing conversation. The whole exchange renders as it happens - thinking
italic, tool calls and their results bold, ascii rules framing each block -
on terminals that support the typography, plain everywhere else. Ctrl-C
stops the running turn without leaving (the session continues where the
transcript left off) or clears the input line; a second Ctrl-C at a clear
prompt exits 8, Ctrl-D exits cleanly. Piping stdin turns it into a scripted
session with the same rendering, one turn per line.

```sh
$ llmkit repl --openai http://localhost:11434/v1 --model llama3.1
================================================================================
> hello, what is 2+2?
--------------------------------------------------------------------------------
The user asks simple arithmetic.
--------------------------------------------------------------------------------
2 + 2 equals 4.
================================================================================
>
```

For one-shot use from the shell there is `llmkit call`: flags in, the
answer as plain text on stdout, thinking omitted, no records to write -

```sh
$ llmkit call --openai http://localhost:11434/v1 --model llama3.1 \
      --system-prompt "You are a helpful assistant" \
      --prompt "hello, how are you?"
I'm fine, thank you!
```

`--prompt -` reads the prompt from stdin, so it pipes and captures:
`ANSWER=$(llmkit call --openai "$API_BASE" --prompt - < input.txt)`.
Anything past one prompt is `runner` territory.

```sh
{ echo '{"type":"llm","endpoint_protocol":"openai","api_base":"http://localhost:11434/v1","model":"llama3.1","inference_options":{"max_tokens":1024}}'
  echo '{"type":"user","content":[{"type":"text","text":"hello"}]}'
} | llmkit runner
```

stdout (one json object per line):

```
{"type":"header","version":1}
{"type":"response","text":"Hello! How can I help you today?","partial":false,"finish_reason":"stop","usage":{...}}
```

Tools: add a `tools` record before the `user` record -

```json
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp"}]}
```

`llmkit call --mcp-proxy cfg.jsonl --terminal-tool fs.confirm` marks
tools the same way from the command line; the answer is then the tool's
text. `llmkit agent-as-tool` seeds honor the attribute too: the `invoke`
reply is the terminal tool's answer.

Integrations that need steering keep stdin open, feed records and a
`{"type":"flush"}` while a turn is in flight; the records apply at the
next turn boundary. The full record catalogue, the conversation rules and
the per-protocol wire mapping are specified in
[docs/requirements.md](docs/requirements.md), the implementation decisions
(libraries, exit codes, `cache_control` placement, threading model) in
[docs/design.md](docs/design.md). Runnable examples for every command live in
[examples/](examples/) - see [docs/records.md](docs/records.md) for the
minimal and complete form of each record.

## building

Linux, a C11 compiler, plus:

- **cJSON** (`libcjson-dev`, any 1.x)
- **libcurl** (easy api, >= 7.80)
- pthreads

```sh
make          # -> ./llmkit
make check    # builds and runs the selfcheck (no network needed)
```

No build system beyond the plain Makefile.

Windows builds need no local mingw: the `win` target of
[tools/package.sh](tools/package.sh) builds in the
`africanfuture/msys2-base:linux-latest` docker image (arch linux with the
mingw-w64 cross toolchain). the image carries no libcurl or libcjson for
the target, so both are pinned and cross-built from source there - libcurl
static against schannel - leaving one fully static `llmkit.exe` in
`dist/llmkit-<ver>-windows-x86_64.zip`, no dlls next to it:

```sh
tools/package.sh v1.2.3 win
```

The cross-build needs network access in the container (source tarballs).
`make check` stays a linux affair for now (the selfcheck harness forks
and runs python helpers); everything it covers except the harness itself
is platform-shared code.

## behavior notes

- Exit code 0 only when the conversation ended with a successful final
  `response`; every fatal condition has its own exit code (see design
  sec.13). Exit 9 is the terminal-tool ending: requested, not an error.
- SIGINT is an orderly stop: buffered text is flushed as a trailing
  partial record, running tools complete, a second SIGINT kills the
  process immediately.
- Mid-conversation `llm` / `tools` / `options` / `system` changes are
  supported and apply at the next turn boundary.
- Text only in this version; UTF-8 enforced everywhere; `\n` line endings.

## license

Copyright (c) 2026 Daniele Guttuso

Distributed under the **European Union Public Licence v1.2 (EUPL-1.2)** -
`SPDX-License-Identifier: EUPL-1.2`. The full license text is available at
<https://joinup.ec.europa.eu/collection/eupl/eupl-text-eupl-12> and in the
official translations published there. By EUPL sec.13 the work is also
additionally distributable under later EUPL versions, and - where the
license so provides - under the GPL-2.0 / GPL-3.0 / LGPL / AGPL / MPL /
CeCILL compatible-list licenses.
