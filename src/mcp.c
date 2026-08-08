#include "mcp.h"
#include "mcp_transport.h"
#include "jsonrpc.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

/*
 * Internal connection state.  Owned entirely by this module so callers never
 * need to include mcp_transport.h.  Safe because the program is single-
 * threaded and mcp.c is the sole lifecycle owner.
 */
static mcp_connection *s_connections = NULL;
static int s_conn_count = 0;

/* ------------------------------------------------------------------ */
/*  mcp_initialize  (static)                                          */
/* ------------------------------------------------------------------ */
/* Send an MCP initialize request and verify the protocol version in
 * the response.  Returns EXIT_SUCCESS or an exit code on failure. */

static int mcp_initialize(mcp_server_cfg *cfg, mcp_connection *conn) {
    char *req = NULL;
    char *resp = NULL;
    char *result = NULL;
    char *error = NULL;
    int rc;

    rc = jsonrpc_build_initialize(&req);
    if (rc != EXIT_SUCCESS || req == NULL) {
        log_activity("[error] Failed to build initialize request for '%s'", cfg->name);
        return EXIT_INTERNAL_ERR;
    }

    rc = transport_send(conn, req, cfg->init_timeout_ms, &resp);
    free(req);
    if (rc != EXIT_SUCCESS) {
        log_activity("[error] Initialize request failed for '%s' (timeout or transport error)",
                     cfg->name);
        return EXIT_MCP_INIT_ERR;
    }

    rc = jsonrpc_parse_response(resp, &result, &error);
    free(resp);
    if (rc != EXIT_SUCCESS) {
        if (error != NULL) {
            log_activity("[error] MCP '%s' initialize error: %s", cfg->name, error);
            free(error);
        }
        return EXIT_MCP_INIT_ERR;
    }

    /* Validate protocol version in result */
    cJSON *root = cJSON_Parse(result);
    if (root == NULL) {
        free(result);
        return EXIT_INTERNAL_ERR;
    }

    cJSON *proto = cJSON_GetObjectItem(root, "protocolVersion");
    if (proto == NULL || !cJSON_IsString(proto)) {
        log_activity("[error] MCP '%s' initialize response missing protocolVersion", cfg->name);
        cJSON_Delete(root);
        free(result);
        return EXIT_MCP_INIT_ERR;
    }

    log_activity("[init] MCP server '%s' initialized (protocol %s)", cfg->name, proto->valuestring);
    cJSON_Delete(root);
    free(result);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  mcp_connect_all                                                    */
/* ------------------------------------------------------------------ */

int mcp_connect_all(runtime_ctx *ctx) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;

    /* Free prior connections on re-entry (e.g. reconnect). */
    mcp_disconnect_all(ctx);

    s_conn_count = ctx->mcp_count;
    if (s_conn_count == 0) return EXIT_SUCCESS;

    s_connections = calloc((size_t)s_conn_count, sizeof(mcp_connection));
    if (s_connections == NULL) {
        log_activity("[error] OOM allocating MCP connections");
        return EXIT_INTERNAL_ERR;
    }

    for (int i = 0; i < s_conn_count; i++) {
        mcp_server_cfg *cfg = &ctx->mcps[i];
        mcp_connection *conn = &s_connections[i];

        log_activity("[init] Connecting to MCP server '%s'...", cfg->name);

        int rc = transport_open(cfg, conn);
        if (rc != EXIT_SUCCESS) {
            log_activity("[error] Failed to connect to MCP server '%s'", cfg->name);
            mcp_disconnect_all(ctx);
            return rc;
        }

        rc = mcp_initialize(cfg, conn);
        if (rc != EXIT_SUCCESS) {
            log_activity("[error] MCP server '%s' initialization failed", cfg->name);
            mcp_disconnect_all(ctx);
            return rc;
        }

        conn->initialized = true;
    }

    log_activity("[init] All MCP servers connected");
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  mcp_discover_tools                                                 */
/* ------------------------------------------------------------------ */
/* Free any previously discovered tools. */
static void free_tool_defs(tool_def *tools, int count) {
    if (tools == NULL) return;
    for (int i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].original);
        free(tools[i].description);
        free(tools[i].input_schema);
        free(tools[i].mcp_server);
    }
    free(tools);
}

/* Deterministic ordering for the tool block: sorted by namespaced name (then
 * server, then original name) so the serialized request stays byte-stable
 * across runs and turns (provider prefix caches key on those bytes). Empty
 * slots (calloc'd leftovers) sort last so they never precede real tools. */
static int tool_def_cmp(const void *a, const void *b) {
    const tool_def *ta = (const tool_def *)a;
    const tool_def *tb = (const tool_def *)b;
    if (ta->name == NULL && tb->name == NULL) return 0;
    if (ta->name == NULL) return 1;
    if (tb->name == NULL) return -1;
    int c = strcmp(ta->name, tb->name);
    if (c != 0) return c;
    c = strcmp(ta->mcp_server ? ta->mcp_server : "", tb->mcp_server ? tb->mcp_server : "");
    if (c != 0) return c;
    return strcmp(ta->original ? ta->original : "", tb->original ? tb->original : "");
}

int mcp_discover_tools(runtime_ctx *ctx) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;

    /* Free previous tools if this is a rediscovery (multi-turn agent). */
    free_tool_defs(ctx->tools, ctx->tool_count);
    ctx->tools = NULL;
    ctx->tool_count = 0;

    if (s_connections == NULL || s_conn_count == 0) return EXIT_SUCCESS;

    /* First pass: count tools so we can allocate exactly once. */
    int total = 0;
    for (int i = 0; i < s_conn_count; i++) {
        if (!s_connections[i].initialized) continue;
        mcp_server_cfg *cfg = s_connections[i].cfg;

        char *req = NULL;
        char *resp = NULL;
        char *result = NULL;
        char *error = NULL;

        if (jsonrpc_build_list_tools(&req) != EXIT_SUCCESS) continue;

        /* Use init_timeout (minimum 30 s) for the list call. */
        int64_t timeout = cfg->init_timeout_ms;
        if (timeout < 30000) timeout = 30000;

        int rc = transport_send(&s_connections[i], req, timeout, &resp);
        free(req);
        if (rc != EXIT_SUCCESS) continue;

        rc = jsonrpc_parse_response(resp, &result, &error);
        free(resp);
        if (rc != EXIT_SUCCESS || result == NULL) {
            free(error);
            continue;
        }

        cJSON *root = cJSON_Parse(result);
        free(result);
        if (root == NULL) continue;

        cJSON *tools_arr = cJSON_GetObjectItem(root, "tools");
        if (tools_arr != NULL && cJSON_IsArray(tools_arr)) {
            total += cJSON_GetArraySize(tools_arr);
        }
        cJSON_Delete(root);
    }

    if (total == 0) return EXIT_SUCCESS;

    ctx->tools = calloc((size_t)total, sizeof(tool_def));
    if (ctx->tools == NULL) {
        log_activity("[error] OOM allocating tool definitions");
        return EXIT_INTERNAL_ERR;
    }

    /* Second pass: populate the array. */
    int idx = 0;
    for (int i = 0; i < s_conn_count && idx < total; i++) {
        if (!s_connections[i].initialized) continue;
        mcp_server_cfg *cfg = s_connections[i].cfg;

        char *req = NULL;
        char *resp = NULL;
        char *result = NULL;
        char *error = NULL;

        if (jsonrpc_build_list_tools(&req) != EXIT_SUCCESS) continue;

        int64_t timeout = cfg->init_timeout_ms;
        if (timeout < 30000) timeout = 30000;

        int rc = transport_send(&s_connections[i], req, timeout, &resp);
        free(req);
        if (rc != EXIT_SUCCESS) continue;

        rc = jsonrpc_parse_response(resp, &result, &error);
        free(resp);
        if (rc != EXIT_SUCCESS || result == NULL) {
            free(error);
            continue;
        }

        cJSON *root = cJSON_Parse(result);
        free(result);
        if (root == NULL) continue;

        cJSON *tools_arr = cJSON_GetObjectItem(root, "tools");
        if (tools_arr == NULL || !cJSON_IsArray(tools_arr)) {
            cJSON_Delete(root);
            continue;
        }

        const char *ns =
            (cfg->namespace != NULL && cfg->namespace[0] != '\0') ? cfg->namespace : cfg->name;

        int arr_size = cJSON_GetArraySize(tools_arr);
        for (int j = 0; j < arr_size && idx < total; j++, idx++) {
            cJSON *tool = cJSON_GetArrayItem(tools_arr, j);
            if (tool == NULL) {
                continue;
            }

            cJSON *name_j = cJSON_GetObjectItem(tool, "name");
            cJSON *desc_j = cJSON_GetObjectItem(tool, "description");
            cJSON *schema_j = cJSON_GetObjectItem(tool, "inputSchema");

            const char *orig_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
            const char *desc = (desc_j && cJSON_IsString(desc_j)) ? desc_j->valuestring : "";

            /* Namespaced name: {ns}.{original} */
            size_t ns_len = strlen(ns);
            size_t on_len = strlen(orig_name);
            char *full_name = malloc(ns_len + 1 + on_len + 1);
            if (full_name == NULL) {
                log_activity("[error] OOM");
                exit(EXIT_INTERNAL_ERR);
            }
            memcpy(full_name, ns, ns_len);
            full_name[ns_len] = '.';
            memcpy(full_name + ns_len + 1, orig_name, on_len);
            full_name[ns_len + 1 + on_len] = '\0';

            ctx->tools[idx].name = full_name;
            ctx->tools[idx].original = util_strdup(orig_name);
            ctx->tools[idx].description = util_strdup(desc);
            ctx->tools[idx].mcp_server = util_strdup(cfg->name);

            if (schema_j != NULL) {
                char *s = cJSON_PrintUnformatted(schema_j);
                ctx->tools[idx].input_schema = (s != NULL) ? s : util_strdup("{}");
            } else {
                ctx->tools[idx].input_schema = util_strdup("{}");
            }
        }

        cJSON_Delete(root);
    }

    ctx->tool_count = idx;
    if (idx > 1) qsort(ctx->tools, (size_t)idx, sizeof(tool_def), tool_def_cmp);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  mcp_call_tool                                                      */
/* ------------------------------------------------------------------ */

int mcp_call_tool(runtime_ctx *ctx, const char *server_name, const char *tool_name,
                  const char *args_json, char **out_result, bool *out_is_error) {
    (void)ctx;
    if (out_result == NULL || out_is_error == NULL) return EXIT_INTERNAL_ERR;
    *out_result = NULL;
    *out_is_error = false;

    /* Locate the connection by server name. */
    mcp_connection *conn = NULL;
    mcp_server_cfg *cfg = NULL;
    for (int i = 0; i < s_conn_count; i++) {
        if (s_connections[i].cfg != NULL && strcmp(s_connections[i].cfg->name, server_name) == 0) {
            conn = &s_connections[i];
            cfg = s_connections[i].cfg;
            break;
        }
    }

    if (conn == NULL || !conn->initialized) {
        log_activity("[error] MCP server '%s' not found or not initialized", server_name);
        *out_is_error = true;
        *out_result = util_strdup("MCP server not found or not initialized");
        return EXIT_MCP_ERR;
    }

    char *req = NULL;
    if (jsonrpc_build_call_tool(tool_name, args_json, &req) != EXIT_SUCCESS) {
        *out_is_error = true;
        *out_result = util_strdup("Failed to build tool call request");
        return EXIT_INTERNAL_ERR;
    }

    int64_t start_ms = platform_now_ms();
    char *resp = NULL;
    int rc = transport_send(conn, req, cfg->call_timeout_ms, &resp);
    free(req);

    if (rc != EXIT_SUCCESS) {
        int64_t elapsed = platform_now_ms() - start_ms;
        int is_timeout = (cfg->call_timeout_ms > 0 && elapsed >= cfg->call_timeout_ms) ? 1 : 0;

        if (is_timeout && cfg->call_timeout_beh == TIMEOUT_CONTINUE) {
            /* "continue" behaviour: return timeout error as tool result. */
            *out_is_error = true;
            *out_result = util_strdup("Tool call timed out");
            return EXIT_SUCCESS;
        }

        /* "fail" behaviour (or non-timeout error): conversation stops. */
        *out_is_error = true;
        *out_result = util_strdup(is_timeout ? "Tool call timed out" : "Tool call failed");
        return EXIT_MCP_ERR;
    }

    /* Parse the JSON-RPC response. */
    char *result_json = NULL;
    char *error_msg = NULL;
    rc = jsonrpc_parse_response(resp, &result_json, &error_msg);
    free(resp);

    if (rc == EXIT_MCP_ERR && error_msg != NULL) {
        /* MCP server returned a structured error - deliver to LLM. */
        *out_is_error = true;
        *out_result = error_msg; /* ownership transferred */
        return EXIT_SUCCESS;
    }

    if (rc != EXIT_SUCCESS || result_json == NULL) {
        free(error_msg);
        *out_is_error = true;
        *out_result = util_strdup("Failed to parse tool call response");
        return EXIT_MCP_ERR;
    }

    *out_result = result_json; /* ownership transferred to caller */
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  mcp_send_request                                                   */
/* ------------------------------------------------------------------ */

int mcp_send_request(runtime_ctx *ctx, const char *server_name, const char *request_json,
                     char **out_response) {
    (void)ctx;
    if (server_name == NULL || request_json == NULL || out_response == NULL)
        return EXIT_INTERNAL_ERR;
    *out_response = NULL;

    mcp_connection *conn = NULL;
    mcp_server_cfg *cfg = NULL;
    for (int i = 0; i < s_conn_count; i++) {
        if (s_connections[i].cfg != NULL && strcmp(s_connections[i].cfg->name, server_name) == 0) {
            conn = &s_connections[i];
            cfg = s_connections[i].cfg;
            break;
        }
    }

    if (conn == NULL || !conn->initialized) {
        log_activity("[error] MCP server '%s' not found or not initialized", server_name);
        return EXIT_MCP_ERR;
    }

    return transport_send(conn, request_json, cfg->call_timeout_ms, out_response);
}

/* ------------------------------------------------------------------ */
/*  mcp_disconnect_all                                                 */
/* ------------------------------------------------------------------ */

void mcp_disconnect_all(runtime_ctx *ctx) {
    (void)ctx;

    for (int i = 0; i < s_conn_count; i++) {
        transport_close(&s_connections[i]);
    }

    free(s_connections);
    s_connections = NULL;
    s_conn_count = 0;
}
