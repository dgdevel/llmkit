# llmkit

A single static-purpose executable for llm interaction from the shell and from other programs. One binary, nine commands:

| command | what it does |
|---|---|
| `llmkit repl <flags>` | the interactive chat: type the turns, watch thinking and tool calls render live |
| `llmkit agent <flags>` | the repl with the built-in mcp tools, `AGENTS.md` instructions and an optional `--conversation-store` to persist and resume the conversation |
| `llmkit call <flags>` | one prompt in, one answer out: plain text on stdout, the shell one-liner front-end |
| `llmkit runner` | runs an llm conversation: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | exposes one agent conversation as a single `invoke` tool on a stdio mcp interface |
| `llmkit mcp-proxy <config.jsonl>` | exposes a curated view (rename, redescribe, hide) of upstream mcp servers on stdio |
| `llmkit builtin-mcp` | serves the built-in generic-use mcp tools (web search/fetch, file list/search/read/create/edit, process exec/status) on stdio |
| `llmkit mcp-repl <flags>` | the interactive tool console for one mcp server: tab-completed `name(args)` calls, one timing line each |
| `llmkit prettyprint <conversation.jsonl>` | renders a recorded conversation jsonl with the repl's typography (stdin without a file) |
| `llmkit proxy <flags>` | a plain-http llm endpoint that forwards to one upstream and prints the passing conversation prettyprint-style |

Written in C11. Talks to any openai-compatible endpoint (chat completions and responses apis) and any anthropic-compatible endpoint. Tools are mcp servers: `stdio`, `http` (streamable http) and `sse` (legacy) transports, in all current protocol revisions up to `2026-07-28`.

`repl` is the easies to use, `call` is aimed at use in scripting, `runner` is the full package for applications interaction. Other utilities are provided for interaction with mcps and debugging.

## Examples

For talking to a model there is `llmkit repl`: the turns are typed, every line is one turn of one growing conversation.  
The whole exchange renders as it happens - thinking italic, tool calls and their results bold, ascii rules framing each block - on terminals that support the typography, plain everywhere else. Ctrl-C stops the running turn without leaving (the session continues where the transcript left off) or clears the input line; a second Ctrl-C at a clear prompt exits 8, Ctrl-D exits cleanly. Piping stdin turns it into a scripted session with the same rendering, one turn per line.

```sh
$ llmkit repl --openai http://localhost:11434/v1 --model llama3.1
[10:31:04] =====================================================================
> hello, what is 2+2?
[10:31:04] ---------------------------------------------------------------------
The user asks simple arithmetic.
[10:31:05] ---------------------------------------------------------------------
2 + 2 equals 4.
[10:31:05] first token 0.31s | thinking 0.87s | response 0.22s | input 61 tok | output 12 tok | total 73 tok
[10:31:09] =====================================================================
>
```

Every separator line is stamped with the wall clock, and each turn that finishes its answer prints its timing: time to first token (prompt processing), thinking generation, response generation - plus the turn's token totals (input, output and their sum) when the endpoint reports usage.

For work that needs hands there is `llmkit agent`: the same chat with the [built-in mcp tools](docs/builtin-mcp.md) attached - web search and fetch, file tools, process tools, skills - and the working directory's `AGENTS.md` injected after the system prompt. `--conversation-store <file>` persists the conversation as jsonl and resumes it: an existing store replays in full before the first prompt, then the session continues where it left off.

```sh
$ llmkit agent --openai http://localhost:11434/v1 --model llama3.1 \
      --conversation-store ~/chats/project.jsonl
[10:31:04] =====================================================================
list the failing tests and fix them
[10:31:05] ---------------------------------------------------------------------
builtin.files_search {"path":".","regex":"FAIL"}
[10:31:06] ---------------------------------------------------------------------
...
```

On the tool side of the house there is `llmkit mcp-repl`: a repl dedicated to one mcp server, the tool-debugging front-end. No model in the loop - the command line names one server (`--stdio`, `--http`, `--sse`), the connection's `tools/list` becomes the vocabulary, and every line is one direct call:

```sh
$ llmkit mcp-repl --stdio './calculator-mcp'
Tools available:
add(float a, float b): add two numbers
subtract(float a, float b): subtract b from a
multiply(float a, float b): multiply two numbers
divide(float a, float b): divide a by b
- float b: must not be zero
> add(1, 1)
2
[10:31:04] 0.004s
> divide(1, 0)
! tool error: division by zero
[10:31:09] 0.003s
```

Arguments are json literals, bound positionally onto the tool's schema (`add(1, 1)` sends `{"a":1,"b":2}`; an integer literal is a json number, valid for a float parameter). Tab completes tool names from the listing, `tools` re-lists, `help` explains, `quit` or Ctrl-D ends (exit 0, Ctrl-C at a clear prompt exit 8). Tool errors, unknown tools and syntax mistakes render as `!`-lines and the session lives on.

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

No external tool server at hand? `llmkit builtin-mcp` is one: nine generic-use tools (web search and fetch, file list/search/read/create/edit, process exec and status) served from the same binary.
```sh
llmkit mcp-repl --stdio 'llmkit builtin-mcp'
```

See [docs/builtin-mcp.md](docs/builtin-mcp.md) for the tool contracts, response formats and the description editing spot.

A saved transcript reads back human again with `llmkit prettyprint`: the runner's jsonl in - a file argument or stdin - the repl's rendering out, every user block under its heavy rule, thinking italic, tool traffic bold, one usage line per turn that reports one. Nothing is sent anywhere; the file is only read and drawn.

To watch a live conversation instead of a recorded one there is `llmkit proxy`: it listens on plain http, forwards every POST to one fixed llm endpoint and renders whatever passes - requests and responses, streaming included - with the same typography on stdout. Point any client's base url at it:

```sh
$ llmkit proxy --openai http://localhost:11434/v1
llmkit proxy: listening on http://127.0.0.1:8080 (plain tcp, no tls)
llmkit proxy: forwarding to openai http://localhost:11434/v1/chat/completions
```

The protocol flag names the language the upstream speaks (`--anthropic`, `--openai`, `--openai-responses`), so both directions parse: a request renders as the conversation it carries, a response renders live as it streams - partials, tool calls, thinking and the closing usage line, exactly the prettyprint view. The listening side is plain tcp only (no tls); the upstream may be https, curl carries that half. `--listen [host:]port` moves the endpoint, `--key` supplies auth the client did not send.

Runnable examples for every command live in [examples/](examples/) - see [docs/records.md](docs/records.md) for the minimal and complete form of each record.

## Command Line Reference

`usage: llmkit <command> [args]` - the commands:

| command | arguments |
|---|---|
| `llmkit runner` | none: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | the seed file |
| `llmkit mcp-proxy <config.jsonl>` | the config file |
| `llmkit builtin-mcp` | none: the built-in mcp tools on stdio |
| `llmkit call <flags>` | flags below, `--prompt` required |
| `llmkit repl <flags>` | flags below, no `--prompt` (the turns are typed) |
| `llmkit mcp-repl <flags>` | one transport flag below |
| `llmkit prettyprint [conversation.jsonl]` | the conversation file, or none: stdin then |
| `llmkit proxy <flags>` | flags below, one protocol flag required |
| `llmkit help` | none: the usage text on stdout |
| `llmkit version` | none: the version on stdout |

`call` and `repl` share one hand-rolled flag parser (no getopt): flags in any order, the value is the next argv token, no `--flag=value` form, no short forms, no abbreviation. Exactly one protocol flag and the `api_base` positional are required; everything else is optional.

- `--anthropic` / `--openai` / `--openai-responses` - the endpoint protocol,
  exactly one of the three
- `<api_base>` - positional, the endpoint base url
  (e.g. `http://localhost:11434/v1`)
- `--key <token>` - the api key, once
- `--model <name>` - the model name, once
- `--max-tokens <n>` - positive integer, an inference knob, once
- `--reasoning-effort <value>` - the reasoning effort, once; the value is
  passed through unconstrained (providers differ: `high`, `medium`, `low`,
  numbers, ...) - an unsupported value surfaces as the endpoint's own
  `api_error`; under `--anthropic` the option is silently not sent
- `--system-prompt <text>` - the system prompt, once; `repl` starts from
  a bundled default prompt when the flag is absent, the flag's text
  replaces it
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

`mcp-repl` has its own three flags, exactly one transport required:

- `--stdio <command>` - spawn the shell command line, json-rpc over the
  child's stdio
- `--http <url>` - streamable http transport
- `--sse <url>` - the legacy http+sse transport
- `--protocol <revision>` - the mcp revision to speak, default
  `2025-11-25` (v1) or `2026-07-28` (v2)
- `--header <name>=<value>` - extra http header on every request to this
  server, repeatable

`proxy` takes the endpoint on its protocol flag and serves plain http:

- `--anthropic` / `--openai` / `--openai-responses` `<api_base>` - the
  protocol the upstream speaks, exactly one; the value is the upstream
  base url (requests route to its `<api_base>/messages`,
  `<api_base>/chat/completions` or `<api_base>/responses`)
- `--listen <host:port>` - the local endpoint, ipv4 names or numbers,
  host may be empty (all interfaces); default `127.0.0.1:8080`, no tls
- `--key <token>` - auth for the upstream when the client sent none
  (`x-api-key` under `--anthropic`, `Authorization: Bearer` otherwise)

## Building

Linux, a C11 compiler, plus:

- **cJSON** (`libcjson-dev`, any 1.x)
- **libcurl** (easy api, >= 7.80)
- pthreads

The line editor is [linenoise](https://github.com/antirez/linenoise),
vendored in `src/vendor/linenoise` (patched onto llmkit's own tty layer,
so the same editor runs in the windows console too - no package needed).

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
