#ifndef JSONRPC_H
#define JSONRPC_H

#include "llmkit.h"

char *jsonrpc_build_request(const char *method, const char *params_json, const char *id);
int jsonrpc_parse_response(const char *resp_json, char **out_result, char **out_error);
int jsonrpc_build_initialize(char **out_json);
int jsonrpc_build_list_tools(char **out_json);
int jsonrpc_build_call_tool(const char *name, const char *args_json, char **out_json);
int jsonrpc_build_list_resources(char **out_json);
int jsonrpc_build_list_prompts(char **out_json);

#endif
