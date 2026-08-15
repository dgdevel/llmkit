#ifndef LLMKIT_H
#define LLMKIT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LLMKIT_VERSION "0.1.0"

/* Exit codes */
#define EXIT_SUCCESS      0
#define EXIT_CONFIG_ERR   1
#define EXIT_ARGS_ERR     2
#define EXIT_FILE_ERR     3
#define EXIT_LLM_ERR      4
#define EXIT_MCP_ERR      5
#define EXIT_MCP_INIT_ERR 6
#define EXIT_INTERNAL_ERR 7

/* Transport type */
typedef enum {
    MCP_STDIO,
    MCP_HTTP,
    MCP_SSE
} mcp_transport_type;

/* Call timeout behavior */
typedef enum {
    TIMEOUT_FAIL,
    TIMEOUT_CONTINUE
} timeout_behavior;

/* Conversation entry types */
typedef enum {
    ENTRY_META,
    ENTRY_USER,
    ENTRY_ASSISTANT,
    ENTRY_TOOL_CALL,
    ENTRY_TOOL_RESULT,
    ENTRY_ERROR,
    /* Subagent trace brackets: written around the scoped entries of one
     * agent-as-tool run, skipped during reconstruction like meta/error. */
    ENTRY_SUBAGENT_START,
    ENTRY_SUBAGENT_END
} entry_type;

/* Scope of a subagent (nested) conversation trace. Entries written with a
 * non-NULL scope carry "depth" (>= 1), "subagent" (tool name) and "run_id"
 * fields; entries written without one are top-level. Readers filter by
 * run_id: NULL run_id = top-level entries only. */
typedef struct {
    int depth;            /* 1 for root subagents, deeper for nested ones */
    const char *subagent; /* subagent tool name */
    const char *run_id;   /* UUID of this subagent run */
} conv_scope;

/* MCP server configuration (parsed from YAML) */
typedef struct {
    char *name;
    mcp_transport_type transport;
    char *cmdline;
    char *url;
    char **headers;
    int64_t init_timeout_ms;
    int64_t call_timeout_ms;
    timeout_behavior call_timeout_beh;
    bool hide;
    char *namespace;
    char **rename_keys;
    char **redefine_keys;
    char **whitelist;
    char **blacklist;
    int max_reconnect;
    int64_t reconnect_delay_ms;
    /* Subagent-only: entry has just a 'name' and refers to a server defined
     * in the top-level 'mcps' list. Never connected/disconnected on its own. */
    bool reference;
} mcp_server_cfg;

/* LLM configuration */
typedef struct {
    char *api_base;
    char *api_key;
    char *model;
    char **headers;
    int retain_reasoning; /* if true, send reasoning_content back to the model on later requests */
} llm_cfg;

/* Agent configuration */
typedef struct {
    char *system_prompt;
    /* Prefix-cache-aware compaction (agent.compact.*). */
    int compact_enabled;        /* default 0 (off) */
    int64_t compact_max_tokens; /* default 16384 */
    double compact_threshold;   /* default 0.8 (fraction of max_tokens) */
    int compact_summarize;      /* default 0: static placeholder summary */
} agent_cfg;

/* Subagent tool attribute (subagents[].tool_definition.attributes.<name>).
 * Insertion order is preserved so the generated JSON schema is deterministic
 * (byte-stable tool blocks keep provider prefix caches warm). */
typedef struct {
    char *name;
    char *type;        /* string|integer|number|boolean|array|object */
    char *description; /* may be NULL */
    int required;      /* default 1 */
} subagent_attr;

/* Tool definition presented to the parent agent for a subagent. */
typedef struct {
    char *name;
    char *description;
    subagent_attr *attributes; /* insertion order, may be NULL */
    int attribute_count;
} subagent_tool_def;

/* Subagent specification. Recursive: a subagent may expose its own
 * subagents as tools. The LLM config is inherited from the parent. */
typedef struct subagent_spec {
    subagent_tool_def tool;
    char *system_prompt;  /* may be NULL */
    char *user_prompt;    /* may be NULL */
    mcp_server_cfg *mcps; /* private servers + references, may be NULL */
    int mcp_count;
    struct subagent_spec *subagents; /* may be NULL */
    int subagent_count;
} subagent_spec;

/* Maximum subagent nesting depth (root subagents are depth 1). */
#define SUBAGENT_MAX_DEPTH 8

/* Tool definition (cached from tools/list) */
typedef struct {
    char *name;
    char *original;
    char *description;
    char *input_schema;
    char *mcp_server;
} tool_def;

/* Tool call within an assistant message */
typedef struct {
    char *id;
    char *name;      /* function name */
    char *arguments; /* JSON arguments string */
} tool_call;

/* Token usage from LLM API response */
typedef struct {
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int prompt_cache_hit_tokens;  /* DeepSeek: usage.prompt_cache_hit_tokens */
    int prompt_cache_miss_tokens; /* DeepSeek: usage.prompt_cache_miss_tokens */
    int cached_tokens;            /* OpenAI: usage.prompt_tokens_details.cached_tokens */
} usage_info;

/* A single message in the LLM chat request */
typedef struct {
    char *role;            /* "system", "user", "assistant", "tool" */
    char *content;         /* text content (may be NULL/empty) */
    char *reasoning;       /* reasoning/thinking content (assistant only, may be NULL/empty) */
    tool_call *tool_calls; /* for assistant messages with tool_calls */
    int tool_call_count;
    char *tool_call_id; /* for tool result messages */
} json_message;

/* Runtime context */
typedef struct {
    llm_cfg llm;
    mcp_server_cfg *mcps;
    int mcp_count;
    agent_cfg agent;
    subagent_spec *subagents; /* root-level subagents, may be NULL */
    int subagent_count;
    tool_def *tools;
    int tool_count;
    char *convo_path;
    char *prompt;
    char *prompt_source;
    char config_hash[65];
    char run_id[37];
} runtime_ctx;

#endif /* LLMKIT_H */
