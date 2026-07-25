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
    ENTRY_ERROR
} entry_type;

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
} mcp_server_cfg;

/* LLM configuration */
typedef struct {
    char *api_base;
    char *api_key;
    char *model;
    char **headers;
} llm_cfg;

/* Agent configuration */
typedef struct {
    char *system_prompt;
} agent_cfg;

/* Tool definition (cached from tools/list) */
typedef struct {
    char *name;
    char *original;
    char *description;
    char *input_schema;
    char *mcp_server;
} tool_def;

/* Runtime context */
typedef struct {
    llm_cfg llm;
    mcp_server_cfg *mcps;
    int mcp_count;
    agent_cfg agent;
    tool_def *tools;
    int tool_count;
    char *convo_path;
    char *prompt;
    char *prompt_source;
    char config_hash[65];
    char run_id[37];
} runtime_ctx;

#endif /* LLMKIT_H */
