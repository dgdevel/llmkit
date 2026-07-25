#ifndef MCP_H
#define MCP_H

#include "llmkit.h"

int mcp_connect_all(runtime_ctx *ctx);
int mcp_discover_tools(runtime_ctx *ctx);
int mcp_call_tool(runtime_ctx *ctx, const char *server_name, const char *tool_name,
                  const char *args_json, char **out_result, bool *out_is_error);

/* Send an arbitrary JSON-RPC request to a specific backend server and
 * return the raw response string.  The caller owns *out_response.
 * Returns EXIT_SUCCESS or an exit code on failure. */
int mcp_send_request(runtime_ctx *ctx, const char *server_name, const char *request_json,
                     char **out_response);

void mcp_disconnect_all(runtime_ctx *ctx);

#endif
