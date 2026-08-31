#ifndef SRV_H
#define SRV_H

#include "llmkit.h"
#include <cJSON.h>

/*
 * Shared MCP server-side plumbing used by both `proxy` and `gateway`.
 *
 * srv_serve reads line-delimited JSON-RPC from stdin (stdio mode) or HTTP
 * POST bodies (`-l host:port` mode) and dispatches each request to the
 * caller-supplied handler, which fills *out_resp with a malloc'd JSON-RPC
 * response ("" for notifications, which get no reply).
 */

/* ------------------------------------------------------------------ */
/*  JSON-RPC response builders                                         */
/* ------------------------------------------------------------------ */

/* Build a JSON-RPC response echoing the request id verbatim, carrying
 * either a result object or an error message. Consumes `result`. */
char *srv_build_response(const cJSON *id_node, cJSON *result, const char *error_msg);

/* Build a successful JSON-RPC response wrapping a result JSON string. */
char *srv_build_success(const cJSON *id_node, const char *result_json);

/* Build an error JSON-RPC response (code -32000). */
char *srv_build_error(const cJSON *id_node, const char *msg);

/* Rewrite the id of a forwarded backend response so it matches the
 * client's original request id. backend_resp is a malloc'd string that
 * is freed here; a new malloc'd string is returned (or the original on
 * parse failure). */
char *srv_rewrite_response_id(const cJSON *id_node, char *backend_resp);

/* Convert a cJSON id node (number/string) to a malloc'd string for use
 * with jsonrpc_build_request, which takes a const char* id. Returns NULL
 * if id_node is NULL or not a number/string. */
char *srv_id_to_str(const cJSON *id_node);

/* ------------------------------------------------------------------ */
/*  Namespace / filter helpers shared by proxy and gateway             */
/* ------------------------------------------------------------------ */

/* Namespace prefix of a server: `namespace` when set, else `name`. */
const char *srv_get_ns(const mcp_server_cfg *cfg);

/* Advance past "{ns}." to get the local name. Returns NULL if the
 * prefix doesn't match. The result points into the input string. */
const char *srv_local_name(const char *name, const char *ns);

/* Check whitelist/blacklist. Returns true if the item should be INCLUDED. */
int srv_check_filters(const mcp_server_cfg *cfg, const char *namespaced_name);

/* Apply rename map: if namespaced_name matches a key, replace *name with
 * the mapped value. */
void srv_apply_rename(const mcp_server_cfg *cfg, const char *namespaced_name, const char **name);

/* Apply redefine map: if namespaced_name matches a key, replace *desc
 * with the mapped value. */
void srv_apply_redefine(const mcp_server_cfg *cfg, const char *namespaced_name, const char **desc);

/* Build the namespaced name "{ns}.{original}" (malloc'd, caller frees).
 * Exits with EXIT_INTERNAL_ERR on OOM, like the rest of the codebase. */
char *srv_make_namespaced(const char *ns, const char *original);

/* ------------------------------------------------------------------ */
/*  Serve loop                                                         */
/* ------------------------------------------------------------------ */

/* Request handler: fills *out_resp with a malloc'd JSON-RPC response
 * ("" for notifications). Returns EXIT_SUCCESS to keep serving, or any
 * other exit code to stop the loop. */
typedef int (*srv_handler_fn)(runtime_ctx *ctx, const char *req_json, char **out_resp);

/* Serve JSON-RPC requests: stdio (stdin/stdout) when listen_addr is NULL
 * or empty, otherwise HTTP on "host:port". server_label names the server
 * in log lines (e.g. "Proxy", "Gateway"). Returns the handler's terminal
 * exit code or an error code. */
int srv_serve(runtime_ctx *ctx, const char *listen_addr, srv_handler_fn handler,
              const char *server_label);

#endif /* SRV_H */
