#ifndef MCP_H
#define MCP_H

#include "llmkit.h"

int mcp_connect_all(runtime_ctx *ctx);
int mcp_discover_tools(runtime_ctx *ctx);

/* Discover tools only from the named servers (a subagent sees only its own
 * MCP list). allowed == NULL means all connected servers. */
int mcp_discover_tools_for(runtime_ctx *ctx, const char *const *allowed, int allowed_count);

/* Is a server with this name connected and initialized? */
bool mcp_server_connected(const char *name);

/* Lazily connect one MCP server (subagent private servers on first use).
 * No-op for reference entries and already-connected names. */
int mcp_connect_one(mcp_server_cfg *cfg);

int mcp_call_tool(runtime_ctx *ctx, const char *server_name, const char *tool_name,
                  const char *args_json, char **out_result, bool *out_is_error);

/* Send an arbitrary JSON-RPC request to a specific backend server and
 * return the raw response string.  The caller owns *out_response.
 * Returns EXIT_SUCCESS or an exit code on failure. */
int mcp_send_request(runtime_ctx *ctx, const char *server_name, const char *request_json,
                     char **out_response);

void mcp_disconnect_all(runtime_ctx *ctx);

/* Free an array of discovered (or merged) tool definitions. */
void mcp_free_tool_defs(tool_def *tools, int count);

/* Deterministic ordering for the tools block: sorted by namespaced name,
 * then server, then original name. Shared so callers that merge synthetic
 * tool definitions (subagents) into ctx->tools keep the identical order. */
int mcp_tool_def_cmp(const void *a, const void *b);

#endif
