# llmkit

A single static-purpose executable for llm interaction from the shell and
from other programs. One binary, four commands:

| command | what it does |
|---|---|
| `llmkit runner` | runs an llm conversation: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | exposes one agent conversation as a single `invoke` tool on a stdio mcp interface |
| `llmkit mcp-proxy <config.jsonl>` | exposes a curated view (rename, redescribe, hide) of upstream mcp servers on stdio |
| `llmkit call <flags>` | one prompt in, one answer out: plain text on stdout, the shell one-liner front-end |

Written in C11. Talks to any openai-compatible endpoint (chat completions
and responses apis) and any anthropic-compatible endpoint. Tools are mcp
servers: `stdio`, `http` (streamable http) and `sse` (legacy) transports,
protocol revisions v1 (`2024-11-05` ... `2025-11-25`) and v2 (`2026-07-28`).

The runner drives the whole loop - model turns, tool calls, steering -
with a record model designed for replay: the transcript a run produced is
a valid input for the next run, and the serialized request prefix is byte
stable across turns so endpoint prefix caches actually hit.

## example

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

A tool can be marked **terminal** through its server's `terminal_tools`
list: once the model selects and uses it, the runner answers the call,
emits the `tool_response` and terminates - no further llm interaction,
exit code 9, no error record (a requested ending, not a failure). The
rest of the batch is answered `is_error: true` so the transcript stays
replayable -

```json
{"type":"tools","tools":[{"type":"stdio","name":"ui","command_line":"...","terminal_tools":["confirm","cancel"]}]}
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
[docs/design.md](docs/design.md). Runnable examples for every command and
every record type live in [examples/](examples/) - see
[examples/records.md](examples/records.md) for the minimal and complete
form of each record.

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

## behavior notes

- Exit code 0 only when the conversation ended with a successful final
  `response`; every fatal condition has its own exit code (see design
  sec.12). Exit 9 is the terminal-tool ending: requested, not an error.
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
