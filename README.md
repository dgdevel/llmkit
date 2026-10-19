# llmkit

llmkit is one executable for llm and mcp interaction from the shell and from other programs.
One binary, eleven commands:

| command | what it does |
|---|---|
| `llmkit repl <flags>` | the interactive chat: type the turns, watch thinking and tool calls render live |
| `llmkit agent <flags>` | the repl with the built-in mcp tools, `AGENTS.md` instructions and an optional `--conversation-store` to persist and resume the conversation |
| `llmkit call <flags>` | one prompt in, one answer out: plain text on stdout, the shell one-liner front-end |
| `llmkit runner` | runs an llm conversation: jsonl records on stdin, jsonl records on stdout |
| `llmkit agent-as-tool <seed.jsonl>` | exposes one agent conversation as a single `invoke` tool on a stdio mcp interface |
| `llmkit mcp-proxy <config.jsonl>` | exposes a curated view (rename, redescribe, hide) of upstream mcp servers on stdio |
| `llmkit builtin-mcp` | serves the built-in generic-use mcp tools (web search/fetch, file list/search/read/create/edit/analyze, process exec/status, skills search/read) on stdio |
| `llmkit mcp-repl <flags>` | the interactive tool console for one mcp server: tab-completed `name(args)` calls, one timing line each |
| `llmkit prettyprint <conversation.jsonl>` | renders a recorded conversation jsonl with the repl's typography (stdin without a file) |
| `llmkit proxy <flags>` | a plain-http llm endpoint that forwards to one upstream and prints the passing conversation prettyprint-style |
| `llmkit models <flags>` | lists the models of one endpoint: aligned columns for reading, `--json` for scripts |

llmkit is written in C11. It talks to any openai-compatible endpoint (chat completions and responses apis) and any anthropic-compatible endpoint. Tools are mcp (Model Context Protocol) servers. The transports are `stdio`, `http` (streamable http), and `sse` (legacy). All current protocol revisions up to `2026-07-28` are supported.

The `repl` command is the easiest to use. The `agent` command gives the builtin tools to the `repl`. The `call` command serves scripting. The `runner` command is for external applications integration. The other commands serve interaction with mcps and debugging.

## Examples

For talking to a model, use `llmkit repl`. You type the turns. Every line is one turn of one growing conversation.

Ctrl-C stops the running turn without leaving. The session continues where the transcript left off. At a filled input line, Ctrl-C clears the line. A second Ctrl-C at a clear prompt exits with code 8. Ctrl-D exits cleanly.

If you pipe stdin, the session becomes scripted. The rendering stays the same. One line is one turn.

```sh
$ llmkit repl --openai http://localhost:9931/v1
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

Every separator line is stamped with the wall clock. Each turn that finishes its answer prints its timing. The timing shows the time to first token (prompt processing), thinking generation, and response generation. It also shows the token totals of the turn (input, output, and their sum) when the endpoint reports usage.

Two session controls ride along. `/help` lists the commands. `/models` fetches the model list from the session's endpoint and shows it as a table. You pick one and the session switches to it. `/cd <dir>` changes the working directory of the session: the tool children respawn in it, and before the next turn the model receives a system message naming the new directory (relative to the previous one when they share a path, like `../prj2`, absolute otherwise). `/new` drops the conversation and starts a fresh one: the model, the tools and the working directory stay, the system prompt (and the agent's headstart listing and `AGENTS.md` blocks) rebuild as at boot. A `--conversation-store` keeps appending: the new conversation lands in the same file.

In the input line, a word that starts with `@` names a file or directory path. Tab completes the path from the filesystem.

For work that needs tools, use `llmkit agent`. It is the same chat with the [built-in mcp tools](docs/builtin-mcp.md) attached. These tools are web search and fetch, file tools, process tools, helper subagent and skills. The `AGENTS.md` file of the working directory is injected after the system prompt.

```sh
$ llmkit agent --openai http://localhost:9931/v1 --model llama3.1 \
      --conversation-store ~/chats/project.jsonl
[10:31:04] =====================================================================
list the failing tests and fix them
[10:31:05] ---------------------------------------------------------------------
files_search {"path":".","regex":"FAIL"}
[10:31:06] ---------------------------------------------------------------------
...
```

`--conversation-store <file>` persists the conversation as jsonl (one json object per line) and resumes it. An existing store replays in full before the first prompt. Then the session continues where it left off.

The `agent` command aims at minimality. It has no configuration file and no conversation memory. It targets local llm users with very low token usage and can be easily integrated in scripted harnesses.

On the tool side, there is `llmkit mcp-repl`. It is a repl dedicated to one mcp server, the tool-debugging front-end. The command line names one server (`--stdio`, `--http`, `--sse`). The `tools/list` reply of the connection becomes the vocabulary. Every line is one direct call:

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

Arguments are json literals.

Tab completes tool names from the listing. `tools` re-lists. `help` explains. `quit` or Ctrl-D ends the session with exit code 0.

For one-shot use from scripts, there is `llmkit call`. Flags go in. The answer comes out as plain text on stdout. Thinking is omitted. There are no records to write.

```sh
$ llmkit call --openai http://localhost:9931/v1 --model llama3.1 \
      --system-prompt "You are a helpful assistant" \
      --prompt "hello, how are you?"
I'm fine, thank you!
```

`--prompt -` reads the prompt from stdin. This makes pipes and captures possible:
`ANSWER=$(llmkit call --openai "$API_BASE" --prompt - < input.txt)`.

Anything past one prompt needs `runner`.

```sh
{ echo '{"type":"llm","endpoint_protocol":"openai","api_base":"http://localhost:9931/v1","model":"llama3.1","inference_options":{"max_tokens":1024}}'
  echo '{"type":"user","content":[{"type":"text","text":"hello"}]}'
} | llmkit runner
```

stdout (one json object per line):

```
{"type":"header","version":1}
{"type":"response","text":"Hello! How can I help you today?","partial":false,"finish_reason":"stop","usage":{...}}
```

Tools: add a `tools` record before the `user` record.

```json
{"type":"tools","tools":[{"type":"stdio","name":"fs","command_line":"npx -y @modelcontextprotocol/server-filesystem /tmp"}]}
```

`llmkit call --mcp-proxy cfg.jsonl --terminal-tool fs.confirm` marks tools the same way from the command line. The answer is then the text of the tool. `llmkit agent-as-tool` seeds honor the attribute too. The `invoke` reply is the answer of the terminal tool.

No external tool server at hand? `llmkit builtin-mcp` is one. It serves sixteen generic-use tools from the same binary: web search and fetch, download, file list/search/read/create/edit/delete/analyze, process exec/status/wait/send-signal, skills search and read.
```sh
llmkit mcp-repl --stdio 'llmkit builtin-mcp'
```

See [docs/builtin-mcp.md](docs/builtin-mcp.md) for the tool contracts, the response formats, and the description editing spot.

A saved transcript reads back human again with `llmkit prettyprint`. The runner's jsonl goes in, from a file argument or from stdin. The repl's rendering comes out. Every user block sits under its heavy rule. Thinking is italic. Tool traffic is bold. One usage line prints per turn that reports one. Nothing is sent anywhere. The file is only read and drawn.

To watch a live conversation instead of a recorded one, use `llmkit proxy`. It listens on plain http. It forwards every POST to one fixed llm endpoint. It renders whatever passes with the same typography on stdout. The traffic includes requests and responses, streaming included. Point the base url of any client at it:

```sh
$ llmkit proxy --openai http://localhost:9931/v1
llmkit proxy: listening on http://127.0.0.1:8080 (plain tcp, no tls)
llmkit proxy: forwarding to openai http://localhost:9931/v1/chat/completions
```

The protocol flag names the language the upstream speaks (`--anthropic`, `--openai`, `--openai-responses`). Both directions parse. A request renders as the conversation it carries. A response renders live as it streams: partials, tool calls, thinking, and the closing usage line. This is exactly the prettyprint view. The listening side is plain tcp only, without tls. The upstream can be https. curl carries that half. `--listen [host:]port` moves the endpoint. `--key` supplies auth that the client did not send.

Runnable examples for every command live in [examples/](examples/). See [docs/records.md](docs/records.md) for the minimal and complete form of each record.

## Command Line Reference

`usage: llmkit <command> [args]` (run `llmkit help` for the full text). The commands:

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
| `llmkit models <flags>` | flags below, one protocol flag required |
| `llmkit help` | none: the usage text on stdout |
| `llmkit version` | none: the version on stdout |
| `llmkit update` | `--yes` optional, check for updates, try to update the binary or the package that contains it |

The `call` and `repl` commands share one hand-rolled flag parser, without getopt. Flags go in any order. The value is the next argv token. There is no `--flag=value` form, no short form, no abbreviation. Exactly one protocol flag and the `api_base` positional are required. Everything else is optional.

- `--anthropic` / `--openai` / `--openai-responses` - the endpoint protocol,
  exactly one of the three
- `<api_base>` - positional, the endpoint base url
  (e.g. `http://localhost:9931/v1`)
- `--key <token>` - the api key, once
- `--model <name>` - the model name, once
- `--max-tokens <n>` - positive integer, an inference knob, once
- `--reasoning-effort <value>` - the reasoning effort, once. The value is
  passed through unconstrained. Providers differ: `high`, `medium`, `low`,
  numbers, and more. An unsupported value surfaces as the endpoint's own
  `api_error`. Under `--anthropic`, the option is silently not sent.
- `--system-prompt <text>` - the system prompt, once. `repl` starts from
  a bundled default prompt when the flag is absent. The flag's text
  replaces it.
- `--prompt <text|->` - `call` only: the one prompt. `-` reads it from
  stdin.
- `--header <name>=<value>` - extra http header, repeatable. A later
  same-name flag replaces the earlier value.
- `--mcp-proxy <config.jsonl>` - repeatable. Each config file runs one
  `llmkit mcp-proxy <config>` child server, named after the file: basename
  minus last extension, any extension.
- `--terminal-tool <server.tool>` - repeatable. Marks tools terminal. The
  answer is the terminal tool's text and the exit code is 9. The server
  prefix must name a `--mcp-proxy` server of the same command line.
- `--verbose`, `--conversation-store <file>`, `--no-helper` - `agent`
  only: full tool traffic (in the session and its helper subagent's
  mirror), a persistent jsonl conversation, and leaving the `helper`
  subagent tool out.

`mcp-repl` has its own flags. Exactly one transport flag is required:

- `--stdio <command>` - spawn the shell command line. json-rpc runs over
  the stdio of the child.
- `--http <url>` - streamable http transport
- `--sse <url>` - the legacy http+sse transport
- `--protocol <revision>` - the mcp revision to speak, default
  `2025-11-25` (v1) or `2026-07-28` (v2)
- `--header <name>=<value>` - extra http header on every request to this
  server, repeatable

`proxy` takes the endpoint on its protocol flag and serves plain http:

- `--anthropic` / `--openai` / `--openai-responses` `<api_base>` - the
  protocol the upstream speaks, exactly one. The value is the upstream
  base url. Requests route to its `<api_base>/messages`,
  `<api_base>/chat/completions`, or `<api_base>/responses`.
- `--listen <host:port>` - the local endpoint, ipv4 names or numbers.
  The host can be empty (all interfaces). The default is
  `127.0.0.1:8080`, no tls.
- `--key <token>` - auth for the upstream when the client sent none.
  `x-api-key` under `--anthropic`, `Authorization: Bearer` otherwise.

`models` shares the protocol flags of `call` and talks to no conversation:

```
llmkit models (--anthropic | --openai | --openai-responses) <api_base>
              [--key <token>] [--header <name=value>]... [--json]
```

- `--anthropic` / `--openai` / `--openai-responses` - the protocol flag,
  exactly one. It picks the auth headers, nothing else.
- `<api_base>` - the same base url the other commands take, version
  segment included (e.g. `http://localhost:9931/v1`).
- `--key <token>` - `Authorization: Bearer` under the openai protocols,
  `x-api-key` plus `anthropic-version` under `--anthropic`.
- `--header <name=value>` - sent verbatim, repeatable. A later same-name
  header replaces the earlier one. A custom `Authorization` or
  `x-api-key` suppresses the built-in auth.
- `--json` - switches the output from aligned columns to one normalized
  json object, `{"object":"list","data":[...],"count":<n>}`. Each entry
  keeps the provider's model object under `raw`.

The command paginates on its own (`has_more` + `last_id`) and folds all
pages into one output. Errors follow the `call` exit codes: connect
failure 3, http error 4, unusable response body 5, usage error 1.

## Building

Linux, a C11 compiler, plus:

- **cJSON** (`libcjson-dev`, any 1.x)
- **libcurl** (easy api, >= 7.80)
- pthreads

The line editor is [linenoise](https://github.com/antirez/linenoise). It is vendored in `src/vendor/linenoise` and patched onto the tty layer of llmkit. The same editor runs in the windows console too. No package is needed.

```sh
make          # -> ./llmkit
make check    # builds and runs the selfcheck (no network needed)
```

Windows builds are done from Linux using msys2. See `tools/package.sh` for details.

## behavior notes

- The exit code is 0 only when the conversation ended with a successful
  final `response`. Every fatal condition has its own exit code (see
  design sec.13). Exit 9 is the terminal-tool ending. It is requested,
  not an error.
- SIGINT is an orderly stop. Buffered text is flushed as a trailing
  partial record. Running tools complete. A second SIGINT kills the
  process immediately.
- Mid-conversation `llm` / `tools` / `options` / `system` changes are
  supported and apply at the next turn boundary.
- Text only in this version. UTF-8 is enforced everywhere. Line endings
  are `\n`.

## license

Copyright (c) 2026 Daniele Guttuso

Distributed under the European Union Public Licence v1.2 (EUPL-1.2),
`SPDX-License-Identifier: EUPL-1.2`. The full license text is available at
<https://joinup.ec.europa.eu/collection/eupl/eupl-text-eupl-12> and in the
official translations published there. By EUPL sec.13, the work is also
additionally distributable under later EUPL versions. Where the license
so provides, it is also distributable under the GPL-2.0 / GPL-3.0 / LGPL /
AGPL / MPL / CeCILL compatible-list licenses.
