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

## Example agent config

```yaml
llm:
  api_base: "http://localhost:11434/v1"
  api_key: "sk-..."
  model: "llama3"
agent:
  system_prompt: "You are a helpful assistant."
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
