# examples

Runnable examples for every llmkit command and every jsonl record type.

```
examples/
├── records.md                 every record type: a minimal and a complete
│                              example, one record per line (valid jsonl)
├── runner/
│   ├── minimal.jsonl          the smallest conversation that runs
│   ├── complete.jsonl         every input record type in one file
│   └── continuation.jsonl     a replayable transcript plus a new user turn
├── agent-as-tool/
│   ├── minimal.jsonl          smallest valid agent seed
│   └── complete.jsonl         agent with tools, options, system, history
├── mcp-proxy/
│   ├── minimal.jsonl          proxy everything from one server
│   ├── expose.jsonl           whitelist mode with renames
│   └── hide.jsonl             blacklist mode
└── scripts/                   bash invocations of the commands
    ├── help-version.sh        llmkit help / llmkit version
    ├── runner-minimal.sh      llmkit runner < minimal.jsonl
    ├── runner-complete.sh     llmkit runner < complete.jsonl
    ├── runner-continue.sh     input + previous output + new user record
    ├── runner-steer.sh        steering: flush while a turn is in flight
    ├── agent-as-tool.sh       raw json-rpc over the stdio mcp interface
    └── mcp-proxy.sh           raw json-rpc over the stdio mcp interface
```

## prerequisites

```sh
make    # builds ./llmkit (needs libcjson and libcurl)
```

The runner examples talk to an openai-compatible or anthropic-compatible
endpoint. The shipped files assume a local ollama (`ollama serve`, then
`ollama pull llama3.1`); edit the `llm` record of any example to point it
elsewhere — see [records.md](records.md) for the field-by-field catalogue.
`examples/runner/complete.jsonl` targets the real anthropic api and contains
a placeholder `x-api-key` you must replace.

`mcp-proxy.sh` spawns `npx -y @modelcontextprotocol/server-filesystem /tmp`
as its upstream server, so it needs node/npx (first run downloads the
package). It never talks to an llm endpoint.

## quick start

```sh
./examples/scripts/help-version.sh
./examples/scripts/runner-minimal.sh
./examples/scripts/mcp-proxy.sh
```

Or by hand:

```sh
llmkit runner < examples/runner/minimal.jsonl
```

Every `.jsonl` file here is one record per line: jsonl is oneline by
definition, and each line is valid, formatted json you can paste into a
`jq .` or a json pretty printer to read it expanded:

```sh
jq . examples/runner/complete.jsonl
```
