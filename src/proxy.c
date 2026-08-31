#include "proxy.h"
#include "mcp.h"
#include "jsonrpc.h"
#include "srv.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/*  Reverse translation helpers for list responses                     */
/* ------------------------------------------------------------------ */

/* For each item in a JSON array (e.g., tools or resources), prepend the
 * namespace to identifying fields (name, uri) and apply filters/rename/redefine. */
static void translate_list_reverse(mcp_server_cfg *cfg, cJSON *items, const char *name_field,
                                   const char *uri_field) {
    if (items == NULL || !cJSON_IsArray(items)) return;
    const char *ns = srv_get_ns(cfg);
    int count = cJSON_GetArraySize(items);

    /* Iterate backwards so removal is safe. */
    for (int i = count - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (item == NULL) continue;

        /* Determine the namespaced name for filtering. */
        cJSON *name_j = cJSON_GetObjectItem(item, name_field);
        const char *orig_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
        char *ns_name = srv_make_namespaced(ns, orig_name);

        /* Apply filters. */
        if (!srv_check_filters(cfg, ns_name)) {
            free(ns_name);
            cJSON_DeleteItemFromArray(items, i);
            continue;
        }

        /* Apply rename to the name field. */
        const char *exposed_name = ns_name;
        srv_apply_rename(cfg, ns_name, &exposed_name);
        cJSON_DeleteItemFromObject(item, name_field);
        cJSON_AddStringToObject(item, "name", exposed_name);

        /* Apply redefine to description, if it exists. */
        if (cfg->redefine_keys != NULL) {
            cJSON *desc_j = cJSON_GetObjectItem(item, "description");
            if (desc_j && cJSON_IsString(desc_j)) {
                const char *new_desc = desc_j->valuestring;
                srv_apply_redefine(cfg, ns_name, &new_desc);
                if (new_desc != desc_j->valuestring) {
                    cJSON_DeleteItemFromObject(item, "description");
                    cJSON_AddStringToObject(item, "description", new_desc);
                }
            }
        }

        /* For resources, also translate the uri field. */
        if (uri_field != NULL) {
            cJSON *uri_j = cJSON_GetObjectItem(item, uri_field);
            if (uri_j && cJSON_IsString(uri_j)) {
                const char *orig_uri = uri_j->valuestring;
                char *ns_uri = srv_make_namespaced(ns, orig_uri);
                cJSON_DeleteItemFromObject(item, uri_field);
                cJSON_AddStringToObject(item, uri_field, ns_uri);
                free(ns_uri);
            }
        }

        free(ns_name);
    }
}

/* ------------------------------------------------------------------ */
/*  handle_mcp_request                                                 */
/* ------------------------------------------------------------------ */

static int handle_mcp_request(runtime_ctx *ctx, const char *req_json, char **out_resp) {
    *out_resp = NULL;

    cJSON *req = cJSON_Parse(req_json);
    if (req == NULL) {
        *out_resp = srv_build_error(NULL, "Parse error");
        return EXIT_SUCCESS;
    }

    cJSON *id_j = cJSON_GetObjectItem(req, "id");
    /* id_j may be NULL (notification), a number, a string, or null.
     * Preserve it verbatim for the response. */

    cJSON *method_j = cJSON_GetObjectItem(req, "method");
    if (method_j == NULL || !cJSON_IsString(method_j)) {
        *out_resp = srv_build_error(id_j, "Method not specified");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }
    const char *method = method_j->valuestring;

    /* ---- initialize: respond directly ---- */
    if (strcmp(method, "initialize") == 0) {
        cJSON *result = cJSON_CreateObject();
        if (result == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
        cJSON *cap = cJSON_AddObjectToObject(result, "capabilities");
        if (cap != NULL) {
            cJSON_AddObjectToObject(cap, "tools");
            cJSON_AddObjectToObject(cap, "resources");
            cJSON_AddObjectToObject(cap, "prompts");
        }
        cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
        if (info != NULL) {
            cJSON_AddStringToObject(info, "name", "llmkit-proxy");
            cJSON_AddStringToObject(info, "version", LLMKIT_VERSION);
        }
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- notifications: ack silently ---- */
    if (strcmp(method, "notifications/initialized") == 0 ||
        strcmp(method, "notifications/ canceled") == 0 ||
        strncmp(method, "notifications/", 14) == 0) {
        *out_resp = util_strdup("");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- ping ---- */
    if (strcmp(method, "ping") == 0) {
        *out_resp = srv_build_success(id_j, "{}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- tools/list: aggregate from all backends ---- */
    if (strcmp(method, "tools/list") == 0) {
        cJSON *all_tools = cJSON_CreateArray();
        if (all_tools == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }

        for (int i = 0; i < ctx->mcp_count; i++) {
            mcp_server_cfg *cfg = &ctx->mcps[i];
            if (cfg->hide) continue;

            char *list_req = NULL;
            if (jsonrpc_build_list_tools(&list_req) != EXIT_SUCCESS) continue;

            char *list_resp = NULL;
            int rc = mcp_send_request(ctx, cfg->name, list_req, &list_resp);
            free(list_req);

            if (rc != EXIT_SUCCESS || list_resp == NULL) {
                free(list_resp);
                continue;
            }

            cJSON *resp_obj = cJSON_Parse(list_resp);
            free(list_resp);
            if (resp_obj == NULL) continue;

            /* Extract result.tools */
            cJSON *res = cJSON_GetObjectItem(resp_obj, "result");
            if (res != NULL) {
                cJSON *tools_arr = cJSON_GetObjectItem(res, "tools");
                if (tools_arr != NULL && cJSON_IsArray(tools_arr)) {
                    /* Translate in place, then append to all_tools */
                    translate_list_reverse(cfg, tools_arr, "name", NULL);
                    int tcount = cJSON_GetArraySize(tools_arr);
                    for (int j = 0; j < tcount; j++) {
                        cJSON *tool = cJSON_GetArrayItem(tools_arr, j);
                        if (tool) {
                            cJSON *copy = cJSON_Duplicate(tool, 1);
                            if (copy) cJSON_AddItemToArray(all_tools, copy);
                        }
                    }
                }
            }
            cJSON_Delete(resp_obj);
        }

        cJSON *result = cJSON_CreateObject();
        if (result == NULL) {
            cJSON_Delete(all_tools);
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddItemToObject(result, "tools", all_tools);
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- tools/call: route to specific backend ---- */
    if (strcmp(method, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *name_j = params ? cJSON_GetObjectItem(params, "name") : NULL;
        const char *tc_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";

        /* Find the backend that owns this namespaced tool. */
        mcp_server_cfg *backend = NULL;
        const char *local_tool = tc_name;
        for (int i = 0; i < ctx->mcp_count; i++) {
            const char *ns = srv_get_ns(&ctx->mcps[i]);
            const char *loc = srv_local_name(tc_name, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_tool = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = srv_build_error(id_j, "Tool not found on any backend");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        /* Build the call request with the local (non-namespaced) name. */
        char *call_req = NULL;
        cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        char *args_str = NULL;
        if (args != NULL) {
            args_str = cJSON_PrintUnformatted(args);
        }
        if (jsonrpc_build_call_tool(local_tool, args_str, &call_req) != EXIT_SUCCESS) {
            free(args_str);
            *out_resp = srv_build_error(id_j, "Failed to build tool call");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }
        free(args_str);

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, call_req, &backend_resp);
        free(call_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = srv_build_error(id_j, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        /* Rewrite the backend's id to echo the client's request id. */
        *out_resp = srv_rewrite_response_id(id_j, backend_resp);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- resources/list ---- */
    if (strcmp(method, "resources/list") == 0) {
        cJSON *all_res = cJSON_CreateArray();
        if (all_res == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }

        for (int i = 0; i < ctx->mcp_count; i++) {
            mcp_server_cfg *cfg = &ctx->mcps[i];
            if (cfg->hide) continue;

            char *lr = NULL;
            if (jsonrpc_build_list_resources(&lr) != EXIT_SUCCESS) continue;
            char *resp = NULL;
            int rc = mcp_send_request(ctx, cfg->name, lr, &resp);
            free(lr);
            if (rc != EXIT_SUCCESS || resp == NULL) {
                free(resp);
                continue;
            }

            cJSON *o = cJSON_Parse(resp);
            free(resp);
            if (o == NULL) continue;
            cJSON *res = cJSON_GetObjectItem(o, "result");
            if (res != NULL) {
                cJSON *arr = cJSON_GetObjectItem(res, "resources");
                translate_list_reverse(cfg, arr, "name", "uri");
                int cnt = arr ? cJSON_GetArraySize(arr) : 0;
                for (int j = 0; j < cnt; j++) {
                    cJSON *item = cJSON_GetArrayItem(arr, j);
                    if (item) {
                        cJSON *copy = cJSON_Duplicate(item, 1);
                        if (copy) cJSON_AddItemToArray(all_res, copy);
                    }
                }
            }
            cJSON_Delete(o);
        }

        cJSON *result = cJSON_CreateObject();
        if (result == NULL) {
            cJSON_Delete(all_res);
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddItemToObject(result, "resources", all_res);
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- resources/read ---- */
    if (strcmp(method, "resources/read") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *uri_j = params ? cJSON_GetObjectItem(params, "uri") : NULL;
        const char *uri = (uri_j && cJSON_IsString(uri_j)) ? uri_j->valuestring : "";

        mcp_server_cfg *backend = NULL;
        const char *local_uri = uri;
        for (int i = 0; i < ctx->mcp_count; i++) {
            const char *ns = srv_get_ns(&ctx->mcps[i]);
            const char *loc = srv_local_name(uri, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_uri = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = srv_build_error(id_j, "Resource not found on any backend");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        /* Build a resources/read request. */
        cJSON *rp = cJSON_CreateObject();
        if (rp == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddStringToObject(rp, "uri", local_uri);
        char *rp_str = cJSON_PrintUnformatted(rp);
        cJSON_Delete(rp);
        char *req_id = srv_id_to_str(id_j);
        char *read_req = jsonrpc_build_request("resources/read", rp_str, req_id);
        free(req_id);
        free(rp_str);

        if (read_req == NULL) {
            *out_resp = srv_build_error(id_j, "Failed to build request");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, read_req, &backend_resp);
        free(read_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = srv_build_error(id_j, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        *out_resp = srv_rewrite_response_id(id_j, backend_resp);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- prompts/list ---- */
    if (strcmp(method, "prompts/list") == 0) {
        cJSON *all_pr = cJSON_CreateArray();
        if (all_pr == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }

        for (int i = 0; i < ctx->mcp_count; i++) {
            mcp_server_cfg *cfg = &ctx->mcps[i];
            if (cfg->hide) continue;

            char *lp = NULL;
            if (jsonrpc_build_list_prompts(&lp) != EXIT_SUCCESS) continue;
            char *resp = NULL;
            int rc = mcp_send_request(ctx, cfg->name, lp, &resp);
            free(lp);
            if (rc != EXIT_SUCCESS || resp == NULL) {
                free(resp);
                continue;
            }

            cJSON *o = cJSON_Parse(resp);
            free(resp);
            if (o == NULL) continue;
            cJSON *res = cJSON_GetObjectItem(o, "result");
            if (res != NULL) {
                cJSON *arr = cJSON_GetObjectItem(res, "prompts");
                translate_list_reverse(cfg, arr, "name", NULL);
                int cnt = arr ? cJSON_GetArraySize(arr) : 0;
                for (int j = 0; j < cnt; j++) {
                    cJSON *item = cJSON_GetArrayItem(arr, j);
                    if (item) {
                        cJSON *copy = cJSON_Duplicate(item, 1);
                        if (copy) cJSON_AddItemToArray(all_pr, copy);
                    }
                }
            }
            cJSON_Delete(o);
        }

        cJSON *result = cJSON_CreateObject();
        if (result == NULL) {
            cJSON_Delete(all_pr);
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddItemToObject(result, "prompts", all_pr);
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- prompts/get ---- */
    if (strcmp(method, "prompts/get") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *name_j = params ? cJSON_GetObjectItem(params, "name") : NULL;
        const char *pname = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";

        mcp_server_cfg *backend = NULL;
        const char *local_pname = pname;
        for (int i = 0; i < ctx->mcp_count; i++) {
            const char *ns = srv_get_ns(&ctx->mcps[i]);
            const char *loc = srv_local_name(pname, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_pname = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = srv_build_error(id_j, "Prompt not found on any backend");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        /* Build prompts/get request. */
        cJSON *pp = cJSON_CreateObject();
        if (pp == NULL) {
            cJSON_Delete(req);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddStringToObject(pp, "name", local_pname);
        /* Copy arguments if present. */
        cJSON *orig_args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        if (orig_args != NULL) {
            cJSON *args_copy = cJSON_Duplicate(orig_args, 1);
            if (args_copy) cJSON_AddItemToObject(pp, "arguments", args_copy);
        }
        char *pp_str = cJSON_PrintUnformatted(pp);
        cJSON_Delete(pp);
        char *req_id = srv_id_to_str(id_j);
        char *get_req = jsonrpc_build_request("prompts/get", pp_str, req_id);
        free(req_id);
        free(pp_str);

        if (get_req == NULL) {
            *out_resp = srv_build_error(id_j, "Failed to build request");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, get_req, &backend_resp);
        free(get_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = srv_build_error(id_j, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        *out_resp = srv_rewrite_response_id(id_j, backend_resp);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- Unknown method ---- */
    *out_resp = srv_build_error(id_j, "Method not supported");
    cJSON_Delete(req);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  proxy_run                                                          */
/* ------------------------------------------------------------------ */

int proxy_run(runtime_ctx *ctx, const char *listen_addr) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;

    log_activity("[init] Connecting to MCP servers...");
    int rc = mcp_connect_all(ctx);
    if (rc != EXIT_SUCCESS) {
        log_activity("[error] Failed to connect to MCP servers");
        return rc;
    }

    rc = srv_serve(ctx, listen_addr, handle_mcp_request, "Proxy");

    mcp_disconnect_all(ctx);
    return rc;
}
