#include "proxy.h"
#include "mcp.h"
#include "jsonrpc.h"
#include "platform.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *get_ns(mcp_server_cfg *cfg) {
    return (cfg->namespace != NULL && cfg->namespace[0]) ? cfg->namespace : cfg->name;
}

/* Check if a namespaced name starts with a given namespace prefix + dot. */
static int has_namespace(const char *name, const char *ns) {
    size_t nlen = strlen(ns);
    return (strncmp(name, ns, nlen) == 0 && name[nlen] == '.');
}

/* Advance past "{ns}." to get the local name. Returns NULL if prefix doesn't match. */
static const char *local_name(const char *name, const char *ns) {
    if (!has_namespace(name, ns)) return NULL;
    return name + strlen(ns) + 1;
}

/* Check whitelist/blacklist. Returns true if the item should be INCLUDED. */
static bool check_filters(mcp_server_cfg *cfg, const char *namespaced_name) {
    /* Whitelist: if non-empty, only items in the list pass. */
    if (cfg->whitelist != NULL && cfg->whitelist[0] != NULL) {
        bool found = false;
        for (int i = 0; cfg->whitelist[i] != NULL; i++) {
            if (strcmp(cfg->whitelist[i], namespaced_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    /* Blacklist: if non-empty, items in the list are excluded. */
    if (cfg->blacklist != NULL && cfg->blacklist[0] != NULL) {
        for (int i = 0; cfg->blacklist[i] != NULL; i++) {
            if (strcmp(cfg->blacklist[i], namespaced_name) == 0) return false;
        }
    }
    return true;
}

/* Apply rename map: if namespaced_name matches a key, replace *name with the value. */
static void apply_rename(mcp_server_cfg *cfg, const char *namespaced_name, const char **name) {
    if (cfg->rename_keys == NULL) return;
    for (int i = 0; cfg->rename_keys[i] != NULL; i++) {
        const char *eq = strchr(cfg->rename_keys[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg->rename_keys[i]);
        if (strlen(namespaced_name) == klen &&
            strncmp(cfg->rename_keys[i], namespaced_name, klen) == 0) {
            *name = eq + 1;
            return;
        }
    }
}

/* Apply redefine map: if namespaced_name matches a key, replace *desc. */
static void apply_redefine(mcp_server_cfg *cfg, const char *namespaced_name, const char **desc) {
    if (cfg->redefine_keys == NULL) return;
    for (int i = 0; cfg->redefine_keys[i] != NULL; i++) {
        const char *eq = strchr(cfg->redefine_keys[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg->redefine_keys[i]);
        if (strlen(namespaced_name) == klen &&
            strncmp(cfg->redefine_keys[i], namespaced_name, klen) == 0) {
            *desc = eq + 1;
            return;
        }
    }
}

/* Build the namespaced name: "{ns}.{original}" (malloc'd, caller frees). */
static char *make_namespaced(const char *ns, const char *original) {
    size_t ns_len = strlen(ns);
    size_t on_len = strlen(original);
    char *out = malloc(ns_len + 1 + on_len + 1);
    if (out == NULL) {
        log_activity("[error] OOM");
        exit(EXIT_INTERNAL_ERR);
    }
    memcpy(out, ns, ns_len);
    out[ns_len] = '.';
    memcpy(out + ns_len + 1, original, on_len);
    out[ns_len + 1 + on_len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/*  Reverse translation helpers for list responses                     */
/* ------------------------------------------------------------------ */

/* For each item in a JSON array (e.g., tools or resources), prepend the
 * namespace to identifying fields (name, uri) and apply filters/rename/redefine. */
static void translate_list_reverse(mcp_server_cfg *cfg, cJSON *items, const char *name_field,
                                   const char *uri_field) {
    if (items == NULL || !cJSON_IsArray(items)) return;
    const char *ns = get_ns(cfg);
    int count = cJSON_GetArraySize(items);

    /* Iterate backwards so removal is safe. */
    for (int i = count - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (item == NULL) continue;

        /* Determine the namespaced name for filtering. */
        cJSON *name_j = cJSON_GetObjectItem(item, name_field);
        const char *orig_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
        char *ns_name = make_namespaced(ns, orig_name);

        /* Apply filters. */
        if (!check_filters(cfg, ns_name)) {
            free(ns_name);
            cJSON_DeleteItemFromArray(items, i);
            continue;
        }

        /* Apply rename to the name field. */
        const char *exposed_name = ns_name;
        apply_rename(cfg, ns_name, &exposed_name);
        cJSON_DeleteItemFromObject(item, name_field);
        cJSON_AddStringToObject(item, "name", exposed_name);

        /* Apply redefine to description, if it exists. */
        if (cfg->redefine_keys != NULL) {
            cJSON *desc_j = cJSON_GetObjectItem(item, "description");
            if (desc_j && cJSON_IsString(desc_j)) {
                const char *new_desc = desc_j->valuestring;
                apply_redefine(cfg, ns_name, &new_desc);
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
                char *ns_uri = make_namespaced(ns, orig_uri);
                cJSON_DeleteItemFromObject(item, uri_field);
                cJSON_AddStringToObject(item, uri_field, ns_uri);
                free(ns_uri);
            }
        }

        free(ns_name);
    }
}

/* ------------------------------------------------------------------ */
/*  Forward translation: strip namespace from a namespaced name        */
/*  and return the backend-local name.  The caller must NOT free the   */
/*  returned pointer (it points into the input).                       */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/*  Build a JSON-RPC response from a result or error.                  */
/* ------------------------------------------------------------------ */
static char *build_response(const char *id, cJSON *result, const char *error_msg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id != NULL) {
        cJSON_AddStringToObject(root, "id", id);
    }
    if (error_msg != NULL) {
        cJSON *err = cJSON_CreateObject();
        if (err != NULL) {
            cJSON_AddNumberToObject(err, "code", -32000);
            cJSON_AddStringToObject(err, "message", error_msg);
            cJSON_AddItemToObject(root, "error", err);
        }
    } else if (result != NULL) {
        cJSON_AddItemToObject(root, "result", result);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Build a successful JSON-RPC response wrapping a result JSON string. */
static char *build_success(const char *id, const char *result_json) {
    cJSON *result = cJSON_Parse(result_json ? result_json : "{}");
    if (result == NULL) result = cJSON_CreateObject();
    return build_response(id, result, NULL);
}

/* Build an error JSON-RPC response. */
static char *build_error(const char *id, const char *msg) {
    return build_response(id, NULL, msg ? msg : "Internal error");
}

/* ------------------------------------------------------------------ */
/*  handle_mcp_request                                                 */
/* ------------------------------------------------------------------ */

static int handle_mcp_request(runtime_ctx *ctx, const char *req_json, char **out_resp) {
    *out_resp = NULL;

    cJSON *req = cJSON_Parse(req_json);
    if (req == NULL) {
        *out_resp = build_error(NULL, "Parse error");
        return EXIT_SUCCESS;
    }

    cJSON *id_j = cJSON_GetObjectItem(req, "id");
    const char *id_str = (id_j && cJSON_IsString(id_j)) ? id_j->valuestring : NULL;

    cJSON *method_j = cJSON_GetObjectItem(req, "method");
    if (method_j == NULL || !cJSON_IsString(method_j)) {
        *out_resp = build_error(id_str, "Method not specified");
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
        *out_resp = build_response(id_str, result, NULL);
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
        *out_resp = build_success(id_str, "{}");
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
        *out_resp = build_response(id_str, result, NULL);
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
            const char *ns = get_ns(&ctx->mcps[i]);
            const char *loc = local_name(tc_name, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_tool = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = build_error(id_str, "Tool not found on any backend");
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
            *out_resp = build_error(id_str, "Failed to build tool call");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }
        free(args_str);

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, call_req, &backend_resp);
        free(call_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = build_error(id_str, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        /* Forward the raw backend response as-is (it's already JSON-RPC). */
        *out_resp = backend_resp;
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
        *out_resp = build_response(id_str, result, NULL);
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
            const char *ns = get_ns(&ctx->mcps[i]);
            const char *loc = local_name(uri, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_uri = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = build_error(id_str, "Resource not found on any backend");
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
        char *read_req = jsonrpc_build_request("resources/read", rp_str, id_str);
        free(rp_str);

        if (read_req == NULL) {
            *out_resp = build_error(id_str, "Failed to build request");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, read_req, &backend_resp);
        free(read_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = build_error(id_str, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        *out_resp = backend_resp;
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
        *out_resp = build_response(id_str, result, NULL);
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
            const char *ns = get_ns(&ctx->mcps[i]);
            const char *loc = local_name(pname, ns);
            if (loc != NULL) {
                backend = &ctx->mcps[i];
                local_pname = loc;
                break;
            }
        }

        if (backend == NULL) {
            *out_resp = build_error(id_str, "Prompt not found on any backend");
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
        char *get_req = jsonrpc_build_request("prompts/get", pp_str, id_str);
        free(pp_str);

        if (get_req == NULL) {
            *out_resp = build_error(id_str, "Failed to build request");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        char *backend_resp = NULL;
        int rc = mcp_send_request(ctx, backend->name, get_req, &backend_resp);
        free(get_req);

        if (rc != EXIT_SUCCESS || backend_resp == NULL) {
            free(backend_resp);
            *out_resp = build_error(id_str, "Backend request failed");
            cJSON_Delete(req);
            return EXIT_SUCCESS;
        }

        *out_resp = backend_resp;
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- Unknown method ---- */
    *out_resp = build_error(id_str, "Method not supported");
    cJSON_Delete(req);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  stdio proxy loop                                                   */
/* ------------------------------------------------------------------ */

static int proxy_loop_stdio(runtime_ctx *ctx) {
    char line[65536];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        /* Trim trailing newline/whitespace. */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len == 0) continue;

        char *resp = NULL;
        int rc = handle_mcp_request(ctx, line, &resp);
        if (rc != EXIT_SUCCESS) return rc;

        if (resp != NULL && resp[0] != '\0') {
            fprintf(stdout, "%s\n", resp);
            fflush(stdout);
        }
        free(resp);
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  HTTP proxy helpers                                                 */
/* ------------------------------------------------------------------ */

/* Parse "host:port" into host string and port. Returns 0 on success. */
static int parse_listen_addr(const char *addr, char *host_out, int host_size, int *port_out) {
    if (addr == NULL) return -1;
    const char *colon = strrchr(addr, ':');
    if (colon == NULL) return -1;

    size_t host_len = (size_t)(colon - addr);
    if (host_len >= (size_t)host_size) host_len = (size_t)(host_size - 1);
    memcpy(host_out, addr, host_len);
    host_out[host_len] = '\0';

    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || p <= 0 || p > 65535) return -1;
    *port_out = (int)p;
    return 0;
}

/* Read an HTTP request body from a socket FILE*. Returns EXIT_SUCCESS
 * or an error code.  This is a minimal HTTP parser. */
static int read_http_request(FILE *client, char **out_body) {
    *out_body = NULL;

    /* Read headers until \r\n\r\n or \n\n. */
    char header_buf[8192];
    size_t hdr_len = 0;
    int blank_line = 0;

    while (fgets(header_buf + hdr_len, (int)(sizeof(header_buf) - hdr_len), client) != NULL) {
        size_t chunk = strlen(header_buf + hdr_len);
        hdr_len += chunk;

        if (hdr_len >= 2 && memcmp(header_buf + hdr_len - 2, "\n\n", 2) == 0) {
            blank_line = 1;
            break;
        }
        if (hdr_len >= 4 && memcmp(header_buf + hdr_len - 4, "\r\n\r\n", 4) == 0) {
            blank_line = 1;
            break;
        }
        if (hdr_len >= sizeof(header_buf) - 1) break;
    }

    if (!blank_line) return EXIT_MCP_ERR;

    /* Parse Content-Length from headers. */
    long content_length = 0;
    {
        /* Simple search for Content-Length: */
        const char *cl = strstr(header_buf, "Content-Length:");
        if (cl == NULL) cl = strstr(header_buf, "content-length:");
        if (cl != NULL) {
            cl += 15; /* skip past "Content-Length:" */
            while (*cl == ' ' || *cl == '\t') cl++;
            char *end = NULL;
            long parsed = strtol(cl, &end, 10);
            if (end == cl || parsed < 0) {
                content_length = 0;
            } else {
                content_length = parsed;
            }
        }
    }

    if (content_length <= 0) return EXIT_SUCCESS;

    /* Read body. */
    *out_body = malloc((size_t)content_length + 1);
    if (*out_body == NULL) return EXIT_INTERNAL_ERR;

    size_t total = 0;
    while (total < (size_t)content_length) {
        size_t n = fread(*out_body + total, 1, (size_t)(content_length - total), client);
        if (n == 0) break;
        total += n;
    }
    (*out_body)[total] = '\0';
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  HTTP proxy loop                                                    */
/* ------------------------------------------------------------------ */

static int proxy_loop_http(runtime_ctx *ctx, const char *addr) {
    char host[256];
    int port = 0;

    if (parse_listen_addr(addr, host, sizeof(host), &port) != 0) {
        log_activity("[error] Invalid listen address: %s", addr);
        return EXIT_ARGS_ERR;
    }

    int listen_fd = platform_tcp_listen(host, port);
    if (listen_fd < 0) {
        log_activity("[error] Failed to bind to %s:%d", host, port);
        return EXIT_MCP_ERR;
    }

    log_activity("[init] Proxy listening on %s:%d", host, port);

    while (1) {
        int client_fd = platform_tcp_accept(listen_fd, -1);
        if (client_fd < 0) {
            log_activity("[error] Accept failed");
            continue;
        }

        FILE *client = fdopen(client_fd, "r+");
        if (client == NULL) {
            close(client_fd);
            continue;
        }

        char *body = NULL;
        int rc = read_http_request(client, &body);
        if (rc != EXIT_SUCCESS || body == NULL) {
            free(body);
            fclose(client);
            continue;
        }

        char *resp = NULL;
        handle_mcp_request(ctx, body, &resp);
        free(body);

        /* Write HTTP response. */
        if (resp != NULL && resp[0] != '\0') {
            fprintf(client,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    strlen(resp), resp);
        } else {
            fprintf(client, "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: 2\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{}");
        }
        fflush(client);
        free(resp);
        fclose(client);
    }

    /* unreachable */
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

    if (listen_addr != NULL && listen_addr[0] != '\0') {
        rc = proxy_loop_http(ctx, listen_addr);
    } else {
        rc = proxy_loop_stdio(ctx);
    }

    mcp_disconnect_all(ctx);
    return rc;
}
