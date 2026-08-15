# Configuration

llmkit is configured through a single YAML file passed via `-c <config.yml>`.
Both modes (`agent` and `proxy`) share the same schema; the difference is which
root keys are accepted.

- **Agent config:** `llm`, `mcps`, `agent` (all optional except `llm.api_base`)
- **Proxy config:** only `mcps` (required)

## LLM (`llm`)

| Field      | Required | Default        | Description                          |
|------------|----------|----------------|--------------------------------------|
| `api_base` | Yes      | -              | OpenAI-compatible base URL           |
| `api_key`  | No       | `""`           | API key for Authorization header     |
| `model`    | No       | `gpt-4o-mini`  | Model identifier                     |
| `headers`  | No       | `{}`           | Additional HTTP headers              |
| `retain_reasoning` | No | `false` | Re-send reasoning on later turns (see below) |

### `retain_reasoning`

Some models (e.g. DeepSeek-R1, Qwen3-thinking, OpenAI o-series) return a
`reasoning_content` field alongside `content` — the model's chain-of-thought.
llmkit always captures this reasoning and stores it in the conversation file
(as a `reasoning` field on assistant entries), and surfaces it in `--stream`
events and `--debug` output.

By default, reasoning is **discarded** when building the next request: the
assistant message is sent back with only its `content`, not its reasoning.
Set `retain_reasoning: true` to re-send the `reasoning_content` field to the
model on subsequent turns (useful for models that benefit from reasoning
context across turns, at the cost of extra tokens).

```yaml
llm:
  api_base: "https://api.deepseek.com/v1"
  model: "deepseek-reasoner"
  retain_reasoning: true
```

## MCP servers (`mcps`)

Array of server entries:

| Field                  | Default   | Description                                            |
|------------------------|-----------|--------------------------------------------------------|
| `name`                 | -         | Unique identifier                                     |
| `type`                 | `stdio`   | `stdio`, `http`, or `sse`                             |
| `cmdline`              | -         | Command line (stdio)                                  |
| `url`                  | -         | Endpoint URL (http/sse)                               |
| `init_timeout`         | `30s`     | Max time for full MCP initialization                  |
| `call_timeout`         | `10m`     | Max time for a single tools/call                      |
| `call_timeout_behavior`| `fail`    | `fail` (exit 5) or `continue` (return error to LLM)   |
| `namespace`            | `name`    | Proxy: prefix for tools/resources/prompts             |
| `hide`                 | `false`   | Proxy: hide all items from this server                |
| `rename`               | `{}`      | Proxy: map namespaced name -> new exposed name        |
| `redefine`             | `{}`      | Proxy: override descriptions                          |
| `whitelist`            | `[]`      | Proxy: only expose these namespaced names             |
| `blacklist`            | `[]`      | Proxy: exclude these namespaced names                 |

## Agent (`agent`)

| Key                  | Default   | Description                                                    |
|----------------------|-----------|----------------------------------------------------------------|
| `system_prompt`      | (none)    | System prompt prepended to every request (position 0)          |
| `compact.enabled`    | `false`   | Enable prefix-cache-aware compaction of long conversations     |
| `compact.max_tokens` | `16384`   | Token budget; compaction triggers when the estimated prompt    |
|                      |           | exceeds `max_tokens * threshold`                               |
| `compact.threshold`  | `0.8`     | Fraction of `max_tokens` that triggers compaction (0 < t <= 1) |
| `compact.summarize`  | `false`   | `true`: summarize the middle of the conversation via the LLM;  |
|                      |           | `false`: insert a static placeholder instead                   |

Compaction keeps the canonical conversation JSONL untouched (append-only) and
writes a projection sidecar (`<conversation>.context.json`) so the
provider-visible prefix (system prompt + pinned early turns + one summary +
recent tail) stays byte-stable across turns, keeping provider prefix caches
(e.g. DeepSeek's automatic prefix cache) warm. Activating compaction is a
deliberate cache-reset point: the first request after it pays one cache miss,
after which the prefix grows append-only again.

## Subagents (`subagents`) — agent-as-tool

A subagent is a nested agent exposed to its parent as a single tool. The
parent LLM calls the tool like any other; llmkit runs a fresh conversation
with the subagent's own prompts and MCP servers and returns the subagent's
final answer as the tool result.

`subagents` is a list of subagent specifications:

| Key               | Required | Description                                              |
|-------------------|----------|----------------------------------------------------------|
| `tool_definition` | Yes      | How the subagent is presented to the parent agent        |
| `system_prompt`   | No       | System prompt of the subagent conversation               |
| `user_prompt`     | No       | User message that starts the subagent conversation       |
| `mcps`            | No       | MCP servers available to the subagent (see below)        |
| `subagents`       | No       | The subagent's own subagents (recursive, max depth 8)    |

### `tool_definition`

| Key          | Required | Description                                        |
|--------------|----------|----------------------------------------------------|
| `name`       | Yes      | Tool name, must be unique among sibling subagents  |
| `description`| No       | Tool description shown to the parent LLM           |
| `attributes` | No       | Mapping of attribute name -> spec                  |

Each attribute supports:

| Key          | Default    | Description                                        |
|--------------|------------|----------------------------------------------------|
| `type`       | `string`   | `string`, `integer`, `number`, `boolean`, `array` or `object` |
| `description`| (none)     | Attribute description                              |
| `required`   | `true`     | Whether the parent LLM must provide the attribute  |

The attributes become the tool's JSON schema, and their values are
interpolated into `system_prompt` and `user_prompt` via `{attribute}`
placeholders:

- `string` attributes interpolate their raw text;
- other types interpolate their JSON literal (`16`, `true`, `[1,2]`, ...);
- a missing (optional) attribute interpolates the empty string;
- unknown placeholders are left untouched.

The subagent always uses the same LLM configuration (`llm`) as the main
agent; there is no per-subagent LLM setting.

### Subagent MCP servers (`subagents[].mcps`)

Entries use the same format as the top-level `mcps` list, plus one special
form: an entry with **only a `name`** is a *reference* to a server defined
in the top-level `mcps` list — the subagent reuses that already-running
connection instead of starting its own.

Fully-specified entries are *private* to the subagent: the server process is
started lazily on the subagent's first tool use and kept alive for the rest
of the run. The parent (and sibling subagents) do not see its tools.

MCP server names are the routing key at runtime, so a name may be defined
only once in the whole config: a private entry must not reuse a name defined
in the top-level `mcps` list or anywhere else, and references must resolve
to a top-level entry.

## Example agent config

```yaml
llm:
  api_base: "http://localhost:11434/v1"
  api_key: "sk-..."
  model: "llama3"
agent:
  system_prompt: "You are a helpful assistant."
  compact:
    enabled: true
    max_tokens: 16384
    threshold: 0.8
    summarize: false
mcps:
  - name: fs
    cmdline: "npx -y @modelcontextprotocol/server-filesystem /tmp"
    init_timeout: "30s"
    call_timeout: "10m"
```

## Example proxy config

```yaml
mcps:
  - name: fs
    cmdline: "npx -y @modelcontextprotocol/server-filesystem /tmp"
    namespace: fs
    whitelist:
      - "fs.read_file"
  - name: internal
    type: http
    url: "http://localhost:9000/mcp"
    hide: true
```
