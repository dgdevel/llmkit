#include "mcp.h"

int mcp_connect_all(runtime_ctx *ctx) {
    (void)ctx;
    return EXIT_SUCCESS;
}

int mcp_discover_tools(runtime_ctx *ctx) {
    (void)ctx;
    return EXIT_SUCCESS;
}

int mcp_call_tool(runtime_ctx *ctx, const char *server_name, const char *tool_name,
                  const char *args_json, char **out_result, bool *out_is_error) {
    (void)ctx;
    (void)server_name;
    (void)tool_name;
    (void)args_json;
    (void)out_result;
    (void)out_is_error;
    return EXIT_SUCCESS;
}

void mcp_disconnect_all(runtime_ctx *ctx) {
    (void)ctx;
}
