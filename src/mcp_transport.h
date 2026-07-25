#ifndef MCP_TRANSPORT_H
#define MCP_TRANSPORT_H

#include "llmkit.h"

typedef struct {
    mcp_server_cfg *cfg;
    mcp_transport_type transport;
    void *proc;
    void *pipe_in;
    void *pipe_out;
    void *curl;
    char *base_url;
    char *session_id;
    bool initialized;
} mcp_connection;

int transport_open(mcp_server_cfg *cfg, mcp_connection *conn);
int transport_send(mcp_connection *conn, const char *req_json,
                   int64_t timeout_ms, char **out_resp);
void transport_close(mcp_connection *conn);

#endif
