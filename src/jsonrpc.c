#include "jsonrpc.h"

char *jsonrpc_build_request(const char *method, const char *params_json, const char *id) {
    (void)method;
    (void)params_json;
    (void)id;
    return NULL;
}

int jsonrpc_parse_response(const char *resp_json, char **out_result, char **out_error) {
    (void)resp_json;
    (void)out_result;
    (void)out_error;
    return EXIT_SUCCESS;
}

int jsonrpc_build_initialize(char **out_json) {
    (void)out_json;
    return EXIT_SUCCESS;
}

int jsonrpc_build_list_tools(char **out_json) {
    (void)out_json;
    return EXIT_SUCCESS;
}

int jsonrpc_build_call_tool(const char *name, const char *args_json, char **out_json) {
    (void)name;
    (void)args_json;
    (void)out_json;
    return EXIT_SUCCESS;
}

int jsonrpc_build_list_resources(char **out_json) {
    (void)out_json;
    return EXIT_SUCCESS;
}

int jsonrpc_build_list_prompts(char **out_json) {
    (void)out_json;
    return EXIT_SUCCESS;
}
