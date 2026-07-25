#include "jsonrpc.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <cJSON.h>

static const char JSONRPC_VERSION[] = "2.0";

static void jsonrpc_next_id(char *buf, size_t len) {
    static unsigned long counter = 0;
    counter++;
    snprintf(buf, len, "%lu", counter);
}

char *jsonrpc_build_request(const char *method, const char *params_json, const char *id) {
    if (method == NULL) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    cJSON_AddStringToObject(root, "jsonrpc", JSONRPC_VERSION);
    cJSON_AddStringToObject(root, "method", method);
    if (id != NULL) {
        cJSON_AddStringToObject(root, "id", id);
    }

    if (params_json != NULL) {
        cJSON *params = cJSON_Parse(params_json);
        if (params == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(root, "params", params);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

int jsonrpc_parse_response(const char *resp_json, char **out_result, char **out_error) {
    if (resp_json == NULL || out_result == NULL || out_error == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    *out_result = NULL;
    *out_error = NULL;

    cJSON *root = cJSON_Parse(resp_json);
    if (root == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result != NULL) {
        char *printed = cJSON_PrintUnformatted(result);
        cJSON_Delete(root);
        if (printed == NULL) {
            return EXIT_INTERNAL_ERR;
        }
        *out_result = printed;
        return EXIT_SUCCESS;
    }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error != NULL) {
        const char *msg = NULL;
        cJSON *message = cJSON_GetObjectItem(error, "message");
        if (message != NULL && cJSON_IsString(message)) {
            msg = message->valuestring;
        }
        *out_error = util_strdup(msg != NULL ? msg : "Unknown MCP error");
        cJSON_Delete(root);
        return EXIT_MCP_ERR;
    }

    cJSON_Delete(root);
    return EXIT_INTERNAL_ERR;
}

int jsonrpc_build_initialize(char **out_json) {
    if (out_json == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_json = NULL;

    cJSON *params = cJSON_CreateObject();
    if (params == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON *client_info = cJSON_AddObjectToObject(params, "clientInfo");
    if (client_info != NULL) {
        cJSON_AddStringToObject(client_info, "name", "llmkit");
        cJSON_AddStringToObject(client_info, "version", LLMKIT_VERSION);
    }

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_str == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    char id[32];
    jsonrpc_next_id(id, sizeof(id));
    *out_json = jsonrpc_build_request("initialize", params_str, id);
    free(params_str);
    return (*out_json != NULL) ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
}

int jsonrpc_build_list_tools(char **out_json) {
    if (out_json == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_json = NULL;
    char id[32];
    jsonrpc_next_id(id, sizeof(id));
    *out_json = jsonrpc_build_request("tools/list", NULL, id);
    return (*out_json != NULL) ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
}

int jsonrpc_build_list_resources(char **out_json) {
    if (out_json == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_json = NULL;
    char id[32];
    jsonrpc_next_id(id, sizeof(id));
    *out_json = jsonrpc_build_request("resources/list", NULL, id);
    return (*out_json != NULL) ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
}

int jsonrpc_build_list_prompts(char **out_json) {
    if (out_json == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_json = NULL;
    char id[32];
    jsonrpc_next_id(id, sizeof(id));
    *out_json = jsonrpc_build_request("prompts/list", NULL, id);
    return (*out_json != NULL) ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
}

int jsonrpc_build_call_tool(const char *name, const char *args_json, char **out_json) {
    if (out_json == NULL || name == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_json = NULL;

    cJSON *params = cJSON_CreateObject();
    if (params == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    cJSON_AddStringToObject(params, "name", name);

    cJSON *args = NULL;
    if (args_json != NULL) {
        args = cJSON_Parse(args_json);
    }
    if (args == NULL) {
        if (args_json != NULL) {
            cJSON_Delete(params);
            return EXIT_INTERNAL_ERR;
        }
        args = cJSON_CreateObject();
        if (args == NULL) {
            cJSON_Delete(params);
            return EXIT_INTERNAL_ERR;
        }
    }
    cJSON_AddItemToObject(params, "arguments", args);

    char *params_str = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_str == NULL) {
        return EXIT_INTERNAL_ERR;
    }

    char id[32];
    jsonrpc_next_id(id, sizeof(id));
    *out_json = jsonrpc_build_request("tools/call", params_str, id);
    free(params_str);
    return (*out_json != NULL) ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
}
