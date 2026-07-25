#include "mcp_transport.h"

int transport_open(mcp_server_cfg *cfg, mcp_connection *conn) {
    (void)cfg; (void)conn;
    return EXIT_SUCCESS;
}

int transport_send(mcp_connection *conn, const char *req_json,
                   int64_t timeout_ms, char **out_resp) {
    (void)conn; (void)req_json; (void)timeout_ms; (void)out_resp;
    return EXIT_SUCCESS;
}

void transport_close(mcp_connection *conn) {
    (void)conn;
}
