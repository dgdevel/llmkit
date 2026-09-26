# llmkit

A single static-purpose executable for llm interaction from the shell and from other programs. One binary, five commands:

| command | what it does |
|---|---|
| `llmkit repl <flags>` | the interactive chat: type the turns, watch thinking and tool calls render live |
| `llmkit call <flags>` | one prompt in, one answer out: plain text on stdout, the shell one-liner front-end |
| `llmkit runner` | runs an llm conversation: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | exposes one agent conversation as a single `invoke` tool on a stdio mcp interface |
| `llmkit mcp-proxy <config.jsonl>` | exposes a curated view (rename, redescribe, hide) of upstream mcp servers on stdio |

Written in C11. Talks to any openai-compatible endpoint (chat completions and responses apis) and any anthropic-compatible endpoint. Tools are mcp servers: `stdio`, `http` (streamable http) and `sse` (legacy) transports, in all current protocol revisions up to `2026-07-28`.

`repl` is the easies to use, `call` is aimed at use in scripting, `runner` is the full package for applications interaction.

## Examples

For talking to a model there is `llmkit repl`: the turns are typed, every line is one turn of one growing conversation.  
The whole exchange renders as it happens - thinking italic, tool calls and their results bold, ascii rules framing each block - on terminals that support the typography, plain everywhere else. Ctrl-C stops the running turn without leaving (the session continues where the transcript left off) or clears the input line; a second Ctrl-C at a clear prompt exits 8, Ctrl-D exits cleanly. Piping stdin turns it into a scripted session with the same rendering, one turn per line.

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

For one-shot use from the shell there is `llmkit call`: flags in, the answer as plain text on stdout, thinking omitted, no records to write -

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

`llmkit call --mcp-proxy cfg.jsonl --terminal-tool fs.confirm` marks tools the same way from the command line; the answer is then the tool's text. `llmkit agent-as-tool` seeds honor the attribute too: the `invoke` reply is the terminal tool's answer.

Runnable examples for every command live in [examples/](examples/) - see [docs/records.md](docs/records.md) for the minimal and complete form of each record.

## Command Line Reference

`usage: llmkit <command> [args]` - the commands:

| command | arguments |
|---|---|
| `llmkit runner` | none: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | the seed file |
| `llmkit mcp-proxy <config.jsonl>` | the config file |
| `llmkit call <flags>` | flags below, `--prompt` required |
| `llmkit repl <flags>` | flags below, no `--prompt` (the turns are typed) |
| `llmkit help` | none: the usage text on stdout |
| `llmkit version` | none: the version on stdout |

`call` and `repl` share one hand-rolled flag parser (no getopt): flags in any order, the value is the next argv token, no `--flag=value` form, no short forms, no abbreviation. Exactly one protocol flag and the `api_base` positional are required; everything else is optional.

- `--anthropic` / `--openai` / `--openai-responses` - the endpoint protocol,
  exactly one of the three
- `<api_base>` - positional, the endpoint base url
  (e.g. `http://localhost:11434/v1`)
- `--key <token>` - the api key, once
- `--model <name>` - the model name, once
- `--max-tokens <n>` - positive integer, the one inference knob, once
- `--system-prompt <text>` - the system prompt, once
- `--prompt <text|->` - `call` only: the one prompt; `-` reads it from
  stdin
- `--header <name>=<value>` - extra http header; repeatable, a later
  same-name flag replaces the earlier value
- `--mcp-proxy <config.jsonl>` - repeatable; each config runs one
  `llmkit mcp-proxy <config>` child server, named after the file: basename
  minus last extension, any extension
- `--terminal-tool <server.tool>` - repeatable; marks tools terminal (the
  answer is the terminal tool's text, exit 9) - the server prefix must
  name a `--mcp-proxy` server of the same command line

## Building

Linux, a C11 compiler, plus:

- **cJSON** (`libcjson-dev`, any 1.x)
- **libcurl** (easy api, >= 7.80)
- pthreads

```sh
make          # -> ./llmkit
make check    # builds and runs the selfcheck (no network needed)
```

Windows build are done from Linux using msys2. See `tools/package.sh` for details.

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
