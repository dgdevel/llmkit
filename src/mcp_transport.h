#ifndef MCP_TRANSPORT_H
#define MCP_TRANSPORT_H

#include "llmkit.h"
#include "platform.h"
#include <curl/curl.h>

/* One connection to an MCP server. Fields are union-like: only those matching
 * `transport` are meaningful. Owned by the caller (mcp.c); zeroed before use. */
typedef struct {
    mcp_server_cfg *cfg;
    mcp_transport_type transport;
    bool initialized;

    /* stdio */
    platform_process proc;
    platform_pipe pipe_in;  /* write to child stdin */
    platform_pipe pipe_out; /* read from child stdout */
    bool proc_spawned;

    /* http + sse (POST requests) */
    CURL *curl;       /* easy handle reused across sends (http) */
    char *base_url;   /* configured url */
    char *session_id; /* Mcp-Session-Id header value, if server returns one */

    /* sse (deprecated HTTP+SSE) */
    CURLM *multi;   /* multi handle driving the persistent GET stream */
    CURL *sse_easy; /* easy handle for the GET stream */
    struct curl_slist *sse_hdrs;
    char *sse_endpoint; /* POST endpoint URL received via the endpoint event */
    char *sse_buf;      /* raw SSE accumulator */
    size_t sse_buf_len;
    size_t sse_buf_cap;
} mcp_connection;

/* Open the transport described by `cfg`. On success conn is ready for sends.
 * Returns EXIT_SUCCESS or an exit code (EXIT_MCP_ERR / EXIT_INTERNAL_ERR). */
int transport_open(mcp_server_cfg *cfg, mcp_connection *conn);

/* Send a JSON-RPC request and read its response. *out_resp is a malloc'd
 * string the caller must free. timeout_ms bounds the whole exchange.
 * Returns EXIT_SUCCESS or an exit code. */
int transport_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms, char **out_resp);

/* Close the transport and release all resources. Safe to call on a partially
 * initialized connection. */
void transport_close(mcp_connection *conn);

#endif
