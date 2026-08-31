#include "gateway.h"
#include "jsonrpc.h"
#include "llm.h"
#include "mcp.h"
#include "srv.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/*  Exposed tool descriptions                                          */
/* ------------------------------------------------------------------ */

#define GATEWAY_SERVER_NAME "llmkit-gateway"

#define DISCOVER_DESC                                                         \
    "Find backend tools matching a natural-language description of what you " \
    "want to do. Returns the full specs (name, description, inputSchema, "    \
    "server) of every matching tool, plus how the matches were selected "     \
    "(\"llm\" or \"keyword\"). Pass a returned name to `invoke` to call it."

#define INVOKE_DESC                                                     \
    "Invoke a backend MCP tool by its namespaced name (as returned by " \
    "`discover`) and pass the arguments through to it unchanged."

/* ------------------------------------------------------------------ */
/*  Schema builders                                                    */
/* ------------------------------------------------------------------ */

/* Build {"type":"object","properties":{...},"required":[...]} from a
 * list of (name, type, description) triples and the required property
 * names. Returns a malloc'd compact JSON string. */
static char *build_tool_schema(const char *const *props, int prop_count,
                               const char *const *required, int required_count) {
    cJSON *schema = cJSON_CreateObject();
    if (schema == NULL) return NULL;
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *properties = cJSON_AddObjectToObject(schema, "properties");
    if (properties == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }
    for (int i = 0; i < prop_count; i += 3) {
        cJSON *p = cJSON_AddObjectToObject(properties, props[i]);
        if (p == NULL) continue;
        cJSON_AddStringToObject(p, "type", props[i + 1]);
        cJSON_AddStringToObject(p, "description", props[i + 2]);
    }

    if (required_count > 0) {
        cJSON *req = cJSON_AddArrayToObject(schema, "required");
        if (req != NULL) {
            for (int i = 0; i < required_count; i++) {
                cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
            }
        }
    }

    char *out = cJSON_PrintUnformatted(schema);
    cJSON_Delete(schema);
    return out;
}

static char *discover_schema(void) {
    const char *props[] = {
        "query",
        "string",
        "Natural-language description of what you are looking for, e.g. \"write a file\".",
    };
    const char *required[] = {"query"};
    return build_tool_schema(props, 3, required, 1);
}

static char *invoke_schema(void) {
    const char *props[] = {
        "name",      "string", "Namespaced name of the tool to call, as returned by `discover`.",
        "arguments", "object", "Arguments object passed to the tool as-is. Optional.",
    };
    const char *required[] = {"name"};
    return build_tool_schema(props, 6, required, 1);
}

/* ------------------------------------------------------------------ */
/*  tools/list                                                         */
/* ------------------------------------------------------------------ */

/* Append one exposed tool definition to the array. Takes ownership of
 * schema_str on success. */
static int add_tool_def(cJSON *tools, const char *name, const char *desc, char *schema_str) {
    cJSON *schema = cJSON_Parse(schema_str ? schema_str : "{}");
    if (schema == NULL) schema = cJSON_CreateObject();
    cJSON *t = cJSON_CreateObject();
    if (t == NULL || schema == NULL) {
        cJSON_Delete(t);
        cJSON_Delete(schema);
        return EXIT_INTERNAL_ERR;
    }
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", schema);
    cJSON_AddItemToArray(tools, t);
    return EXIT_SUCCESS;
}

static int gateway_tools_list(const cJSON *id_node, char **out_resp) {
    *out_resp = NULL;

    char *disc_schema = discover_schema();
    char *inv_schema = invoke_schema();
    if (disc_schema == NULL || inv_schema == NULL) {
        free(disc_schema);
        free(inv_schema);
        return EXIT_INTERNAL_ERR;
    }

    cJSON *tools = cJSON_CreateArray();
    if (tools == NULL) {
        free(disc_schema);
        free(inv_schema);
        return EXIT_INTERNAL_ERR;
    }

    int rc = add_tool_def(tools, "discover", DISCOVER_DESC, disc_schema);
    if (rc == EXIT_SUCCESS) rc = add_tool_def(tools, "invoke", INVOKE_DESC, inv_schema);
    free(disc_schema);
    free(inv_schema);
    if (rc != EXIT_SUCCESS) {
        cJSON_Delete(tools);
        return rc;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(tools);
        return EXIT_INTERNAL_ERR;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Backend tool catalog (for discover)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;         /* exposed namespaced name (after rename) */
    char *description;  /* after redefine */
    char *input_schema; /* raw JSON string */
    char *server;       /* backend server name */
} catalog_entry;

static void catalog_free(catalog_entry *entries, int count) {
    if (entries == NULL) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].description);
        free(entries[i].input_schema);
        free(entries[i].server);
    }
    free(entries);
}

/* ------------------------------------------------------------------ */
/*  Keyword fallback matcher                                           */
/* ------------------------------------------------------------------ */

/* Filler words ignored by the fallback matcher. */
static const char *const STOPWORDS[] = {
    "a",   "an",   "the",   "my",    "me",     "i",      "you",  "your", "to",  "for",
    "of",  "in",   "on",    "at",    "with",   "and",    "or",   "is",   "are", "be",
    "do",  "can",  "could", "would", "should", "please", "want", "need", "use", "using",
    "how", "what", "when",  "where", "which",  "that",   "this", "it",   "its", NULL,
};

static int is_stopword(const char *tok) {
    for (int i = 0; STOPWORDS[i] != NULL; i++) {
        if (strcmp(STOPWORDS[i], tok) == 0) return 1;
    }
    return 0;
}

/* A tool matches when ANY query token (case-insensitive, stopwords
 * ignored) appears in its name or description. OR-matching keeps the
 * fallback usable on natural-language queries with filler words. */
static int catalog_matches_keywords(const catalog_entry *e, const char *const *tokens,
                                    int token_count) {
    if (token_count == 0) return 0;

    size_t hay_len = strlen(e->name) + 1 + strlen(e->description) + 1;
    char *hay = malloc(hay_len);
    if (hay == NULL) return 0;
    snprintf(hay, hay_len, "%s %s", e->name, e->description);
    for (size_t i = 0; i < hay_len - 1; i++) {
        hay[i] = (char)tolower((unsigned char)hay[i]);
    }

    int matched = 0;
    for (int i = 0; i < token_count; i++) {
        if (is_stopword(tokens[i])) continue;
        if (strstr(hay, tokens[i]) != NULL) {
            matched = 1;
            break;
        }
    }
    free(hay);
    return matched;
}

/* Split the query into lowercase alphanumeric tokens (>= 2 chars). */
static char **keyword_tokens(const char *query, int *out_count) {
    *out_count = 0;
    if (query == NULL) return NULL;

    size_t n = strlen(query);
    char **tokens = NULL;
    int count = 0;

    char *buf = malloc(n + 1);
    if (buf == NULL) return NULL;

    size_t t = 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)query[i];
        if (isalnum(c)) {
            buf[t++] = (char)tolower(c);
        } else if (t > 0) {
            buf[t] = '\0';
            if (t >= 2) {
                char **tmp = realloc(tokens, (size_t)(count + 1) * sizeof(char *));
                if (tmp == NULL) {
                    free(buf);
                    for (int k = 0; k < count; k++) free(tokens[k]);
                    free(tokens);
                    return NULL;
                }
                tokens = tmp;
                tokens[count] = util_strdup(buf);
                if (tokens[count] == NULL) {
                    free(buf);
                    for (int k = 0; k < count; k++) free(tokens[k]);
                    free(tokens);
                    return NULL;
                }
                count++;
            }
            t = 0;
        }
    }
    free(buf);
    *out_count = count;
    return tokens;
}

static void keyword_tokens_free(char **tokens, int count) {
    if (tokens == NULL) return;
    for (int i = 0; i < count; i++) free(tokens[i]);
    free(tokens);
}

/* ------------------------------------------------------------------ */
/*  LLM-driven selection                                               */
/* ------------------------------------------------------------------ */

#define DISCOVER_SYSTEM_PROMPT                                                 \
    "You are the tool-discovery assistant of an MCP gateway. You receive a "   \
    "user query and a numbered catalog of the available tools. Select every "  \
    "tool that is relevant to fulfilling the query. Reply with ONLY a JSON "   \
    "array containing the names of the selected tools, exactly as written in " \
    "the catalog. Reply with [] if no tool matches. No prose, no markdown."

/* ------------------------------------------------------------------ */
/*  tools/call: discover                                               */
/* ------------------------------------------------------------------ */

/* Wrap a payload JSON string as a successful tools/call result:
 * {"content":[{"type":"text","text":payload}],"isError":false}. */
static cJSON *build_tool_text_result(const char *payload_json) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    if (content == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", payload_json ? payload_json : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", 0);
    return result; /* ownership transferred to srv_build_response */
}

/* Extract a JSON array of tool names from an LLM reply. Returns a
 * cJSON array (caller frees) or NULL if the reply is not parseable. */
static cJSON *parse_llm_selection(const char *content) {
    if (content == NULL) return NULL;
    const char *start = strchr(content, '[');
    if (start == NULL) return NULL;
    const char *end = strrchr(content, ']');
    if (end == NULL || end < start) return NULL;

    size_t len = (size_t)(end - start + 1);
    char *slice = malloc(len + 1);
    if (slice == NULL) return NULL;
    memcpy(slice, start, len);
    slice[len] = '\0';

    cJSON *arr = cJSON_Parse(slice);
    free(slice);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

static int gateway_tool_discover(runtime_ctx *ctx, const cJSON *id_node, cJSON *args,
                                 char **out_resp) {
    *out_resp = NULL;

    cJSON *query_j = args ? cJSON_GetObjectItem(args, "query") : NULL;
    const char *query = (query_j && cJSON_IsString(query_j)) ? query_j->valuestring : "";
    if (query[0] == '\0') {
        *out_resp = srv_build_error(id_node, "discover requires a string 'query' argument");
        return EXIT_SUCCESS;
    }

    /* Build the catalog of discoverable tools. */
    catalog_entry *entries = NULL;
    int entry_count = 0;
    int oom = 0;

    for (int i = 0; i < ctx->mcp_count && !oom; i++) {
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

        cJSON *result = cJSON_GetObjectItem(resp_obj, "result");
        cJSON *tools_arr = result ? cJSON_GetObjectItem(result, "tools") : NULL;
        if (tools_arr != NULL && cJSON_IsArray(tools_arr)) {
            const char *ns = srv_get_ns(cfg);
            int tcount = cJSON_GetArraySize(tools_arr);
            for (int j = 0; j < tcount; j++) {
                cJSON *tool = cJSON_GetArrayItem(tools_arr, j);
                if (tool == NULL) continue;

                cJSON *name_j = cJSON_GetObjectItem(tool, "name");
                cJSON *desc_j = cJSON_GetObjectItem(tool, "description");
                cJSON *schema_j = cJSON_GetObjectItem(tool, "inputSchema");
                const char *orig_name =
                    (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
                const char *desc = (desc_j && cJSON_IsString(desc_j)) ? desc_j->valuestring : "";

                char *ns_name = srv_make_namespaced(ns, orig_name);
                if (!srv_check_filters(cfg, ns_name)) {
                    free(ns_name);
                    continue;
                }

                catalog_entry *tmp =
                    realloc(entries, (size_t)(entry_count + 1) * sizeof(catalog_entry));
                if (tmp == NULL) {
                    free(ns_name);
                    oom = 1;
                    break;
                }
                entries = tmp;

                const char *exposed = ns_name;
                srv_apply_rename(cfg, ns_name, &exposed);
                const char *final_desc = desc;
                srv_apply_redefine(cfg, ns_name, &final_desc);

                entries[entry_count].name = util_strdup(exposed);
                entries[entry_count].description = util_strdup(final_desc);
                entries[entry_count].server = util_strdup(cfg->name);
                if (schema_j != NULL) {
                    char *s = cJSON_PrintUnformatted(schema_j);
                    entries[entry_count].input_schema = (s != NULL) ? s : util_strdup("{}");
                } else {
                    entries[entry_count].input_schema = util_strdup("{}");
                }
                if (entries[entry_count].name == NULL || entries[entry_count].description == NULL ||
                    entries[entry_count].server == NULL ||
                    entries[entry_count].input_schema == NULL) {
                    oom = 1;
                }
                entry_count++;
                free(ns_name);
                if (oom) break;
            }
        }
        cJSON_Delete(resp_obj);
    }

    if (oom) {
        catalog_free(entries, entry_count);
        return EXIT_INTERNAL_ERR;
    }

    /* Decide matches: LLM first, keyword fallback. */
    const char *matched_by = "llm";
    int *matched = calloc((size_t)(entry_count > 0 ? entry_count : 1), sizeof(int));
    if (matched == NULL) {
        catalog_free(entries, entry_count);
        return EXIT_INTERNAL_ERR;
    }

    int used_llm = 0;
    if (entry_count > 0) {
        /* Render the catalog as numbered "name: description" lines. */
        size_t cap = strlen(query) + 256;
        for (int i = 0; i < entry_count; i++) {
            cap += strlen(entries[i].name) + strlen(entries[i].description) + 32;
        }
        char *cat_text = malloc(cap);
        char *user_text = NULL;
        if (cat_text != NULL) {
            size_t off = 0;
            for (int i = 0; i < entry_count; i++) {
                off += (size_t)snprintf(cat_text + off, cap - off, "%d. %s: %s\n", i + 1,
                                        entries[i].name, entries[i].description);
            }
            user_text = malloc(strlen(query) + strlen(cat_text) + 64);
            if (user_text != NULL) {
                snprintf(user_text, strlen(query) + strlen(cat_text) + 64,
                         "Query: %s\n\nAvailable tools:\n%s", query, cat_text);
            }
            free(cat_text);
        }

        if (user_text != NULL) {
            json_message msgs[2];
            memset(msgs, 0, sizeof(msgs));
            msgs[0].role = "system";
            msgs[0].content = DISCOVER_SYSTEM_PROMPT;
            msgs[1].role = "user";
            msgs[1].content = user_text;

            char *content = NULL;
            char *reasoning = NULL;
            char *model = NULL;
            tool_call *calls = NULL;
            int call_count = 0;
            usage_info usage;
            memset(&usage, 0, sizeof(usage));

            int rc = llm_chat_complete(ctx, msgs, 2, NULL, 0, &content, &reasoning, &model, &calls,
                                       &call_count, &usage);

            free(user_text);
            free(reasoning);
            free(model);
            for (int i = 0; i < call_count; i++) {
                free(calls[i].id);
                free(calls[i].name);
                free(calls[i].arguments);
            }
            free(calls);

            if (rc == EXIT_SUCCESS) {
                cJSON *sel = parse_llm_selection(content);
                if (sel != NULL) {
                    used_llm = 1;
                    int sel_count = cJSON_GetArraySize(sel);
                    for (int i = 0; i < sel_count; i++) {
                        cJSON *item = cJSON_GetArrayItem(sel, i);
                        if (!cJSON_IsString(item)) continue;
                        for (int k = 0; k < entry_count; k++) {
                            if (strcmp(entries[k].name, item->valuestring) == 0) {
                                matched[k] = 1;
                                break;
                            }
                        }
                    }
                    cJSON_Delete(sel);
                }
                /* LLM answered but not in the expected format -> fall back. */
            } else {
                log_activity("[gateway] discover: LLM call failed, using keyword fallback");
            }
            free(content);
        }
    }

    if (!used_llm) {
        matched_by = "keyword";
        int token_count = 0;
        char **tokens = keyword_tokens(query, &token_count);
        if (tokens == NULL && token_count == 0) {
            /* Allocation failed or no usable tokens: no matches. */
            token_count = 0;
        }
        for (int k = 0; k < entry_count; k++) {
            matched[k] =
                catalog_matches_keywords(&entries[k], (const char *const *)tokens, token_count);
        }
        keyword_tokens_free(tokens, token_count);
    }

    /* Build the JSON payload: query, matched_by, tools[...]. */
    cJSON *payload = cJSON_CreateObject();
    cJSON *tools_arr = cJSON_CreateArray();
    if (payload == NULL || tools_arr == NULL) {
        free(matched);
        catalog_free(entries, entry_count);
        cJSON_Delete(payload);
        return EXIT_INTERNAL_ERR;
    }
    cJSON_AddStringToObject(payload, "query", query);
    cJSON_AddStringToObject(payload, "matched_by", matched_by);
    cJSON_AddItemToObject(payload, "tools", tools_arr);

    for (int k = 0; k < entry_count; k++) {
        if (!matched[k]) continue;
        cJSON *t = cJSON_CreateObject();
        if (t == NULL) continue;
        cJSON_AddStringToObject(t, "name", entries[k].name);
        cJSON_AddStringToObject(t, "description", entries[k].description);
        cJSON *schema = cJSON_Parse(entries[k].input_schema ? entries[k].input_schema : "{}");
        if (schema == NULL) schema = cJSON_CreateObject();
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddStringToObject(t, "server", entries[k].server);
        cJSON_AddItemToArray(tools_arr, t);
    }

    char *payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    free(matched);
    catalog_free(entries, entry_count);

    if (payload_str == NULL) return EXIT_INTERNAL_ERR;

    cJSON *result = build_tool_text_result(payload_str);
    free(payload_str);
    if (result == NULL) return EXIT_INTERNAL_ERR;

    *out_resp = srv_build_response(id_node, result, NULL);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  tools/call: invoke                                                 */
/* ------------------------------------------------------------------ */

static int gateway_tool_invoke(runtime_ctx *ctx, const cJSON *id_node, cJSON *args,
                               char **out_resp) {
    *out_resp = NULL;

    cJSON *name_j = args ? cJSON_GetObjectItem(args, "name") : NULL;
    const char *tool_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
    if (tool_name[0] == '\0') {
        *out_resp = srv_build_error(id_node, "invoke requires a string 'name' argument");
        return EXIT_SUCCESS;
    }

    /* Route by namespace prefix, exactly like the proxy's tools/call. */
    mcp_server_cfg *backend = NULL;
    const char *local_tool = tool_name;
    for (int i = 0; i < ctx->mcp_count; i++) {
        const char *ns = srv_get_ns(&ctx->mcps[i]);
        const char *loc = srv_local_name(tool_name, ns);
        if (loc != NULL) {
            backend = &ctx->mcps[i];
            local_tool = loc;
            break;
        }
    }
    if (backend == NULL) {
        *out_resp = srv_build_error(id_node, "Tool not found on any backend");
        return EXIT_SUCCESS;
    }

    cJSON *args_j = args ? cJSON_GetObjectItem(args, "arguments") : NULL;
    char *args_str = NULL;
    if (args_j != NULL) {
        if (!cJSON_IsObject(args_j)) {
            *out_resp = srv_build_error(id_node, "invoke 'arguments' must be an object");
            return EXIT_SUCCESS;
        }
        args_str = cJSON_PrintUnformatted(args_j);
    }

    char *call_req = NULL;
    int rc = jsonrpc_build_call_tool(local_tool, args_str, &call_req);
    free(args_str);
    if (rc != EXIT_SUCCESS || call_req == NULL) {
        *out_resp = srv_build_error(id_node, "Failed to build tool call");
        return EXIT_SUCCESS;
    }

    char *backend_resp = NULL;
    rc = mcp_send_request(ctx, backend->name, call_req, &backend_resp);
    free(call_req);
    if (rc != EXIT_SUCCESS || backend_resp == NULL) {
        free(backend_resp);
        *out_resp = srv_build_error(id_node, "Backend request failed");
        return EXIT_SUCCESS;
    }

    /* Rewrite the backend's id to echo the client's request id. */
    *out_resp = srv_rewrite_response_id(id_node, backend_resp);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  handle_mcp_request                                                 */
/* ------------------------------------------------------------------ */

static int gateway_handle_request(runtime_ctx *ctx, const char *req_json, char **out_resp) {
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
        }
        cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
        if (info != NULL) {
            cJSON_AddStringToObject(info, "name", GATEWAY_SERVER_NAME);
            cJSON_AddStringToObject(info, "version", LLMKIT_VERSION);
        }
        *out_resp = srv_build_response(id_j, result, NULL);
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- notifications: ack silently ---- */
    if (strncmp(method, "notifications/", 14) == 0) {
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

    /* ---- tools/list: exactly the two gateway tools ---- */
    if (strcmp(method, "tools/list") == 0) {
        int rc = gateway_tools_list(id_j, out_resp);
        cJSON_Delete(req);
        return rc;
    }

    /* ---- tools/call: discover or invoke ---- */
    if (strcmp(method, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *name_j = params ? cJSON_GetObjectItem(params, "name") : NULL;
        cJSON *args = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        const char *tc_name = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";

        int rc;
        if (strcmp(tc_name, "discover") == 0) {
            rc = gateway_tool_discover(ctx, id_j, args, out_resp);
        } else if (strcmp(tc_name, "invoke") == 0) {
            rc = gateway_tool_invoke(ctx, id_j, args, out_resp);
        } else {
            *out_resp = srv_build_error(id_j, "Unknown tool");
            rc = EXIT_SUCCESS;
        }
        cJSON_Delete(req);
        return rc;
    }

    /* ---- resources/prompts: empty listings, unsupported reads ---- */
    if (strcmp(method, "resources/list") == 0) {
        *out_resp = srv_build_success(id_j, "{\"resources\":[]}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }
    if (strcmp(method, "prompts/list") == 0) {
        *out_resp = srv_build_success(id_j, "{\"prompts\":[]}");
        cJSON_Delete(req);
        return EXIT_SUCCESS;
    }

    /* ---- Unknown method ---- */
    *out_resp = srv_build_error(id_j, "Method not supported");
    cJSON_Delete(req);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  gateway_run                                                        */
/* ------------------------------------------------------------------ */

int gateway_run(runtime_ctx *ctx, const char *listen_addr) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;

    if (ctx->llm.api_base == NULL || ctx->llm.api_base[0] == '\0') {
        log_activity("[error] gateway requires the llm.api_base config key");
        return EXIT_CONFIG_ERR;
    }

    log_activity("[init] Connecting to MCP servers...");
    int rc = mcp_connect_all(ctx);
    if (rc != EXIT_SUCCESS) {
        log_activity("[error] Failed to connect to MCP servers");
        return rc;
    }

    rc = srv_serve(ctx, listen_addr, gateway_handle_request, "Gateway");

    mcp_disconnect_all(ctx);
    return rc;
}
