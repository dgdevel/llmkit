#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsonrpc.h"
#include "util.h"
#include <cJSON.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, msg); \
            tests_failed++;                                                   \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                       \
    do {                                                                                          \
        tests_run++;                                                                              \
        if ((a) != (b)) {                                                                         \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected %lld, got %lld\n", __FILE__, __LINE__, \
                    msg, (long long)(b), (long long)(a));                                         \
            tests_failed++;                                                                       \
        }                                                                                         \
    } while (0)

#define CHECK_STR_EQ(a, b, msg)                                                             \
    do {                                                                                    \
        tests_run++;                                                                        \
        if (!(a) || !(b) || strcmp((a), (b)) != 0) {                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected \"%s\", got \"%s\"\n", __FILE__, \
                    __LINE__, msg, (b) ? (b) : "NULL", (a) ? (a) : "NULL");                 \
            tests_failed++;                                                                 \
        }                                                                                   \
    } while (0)

#define CHECK_STR_NULL(a, msg)                                                            \
    do {                                                                                  \
        tests_run++;                                                                      \
        if ((a) != NULL) {                                                                \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected NULL, got \"%s\"\n", __FILE__, \
                    __LINE__, msg, (a));                                                  \
            tests_failed++;                                                               \
        }                                                                                 \
    } while (0)

/* helper: extract a string field from a JSON request string into a freshly
 * allocated copy the test can own. Returns NULL if absent or non-string. */
static char *dup_string_field(const char *json, const char *key) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return NULL;
    cJSON *item = cJSON_GetObjectItem(root, key);
    char *copy = NULL;
    if (item != NULL && cJSON_IsString(item)) {
        copy = util_strdup(item->valuestring);
    }
    cJSON_Delete(root);
    return copy;
}

static int has_field(const char *json, const char *key) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return 0;
    cJSON *item = cJSON_GetObjectItem(root, key);
    int present = (item != NULL);
    cJSON_Delete(root);
    return present;
}

static void test_build_request_basic(void) {
    char *req = jsonrpc_build_request("tools/list", "{}", "1");
    CHECK(req != NULL, "build_request should return non-NULL");

    cJSON *root = cJSON_Parse(req);
    CHECK(root != NULL, "built request should be valid JSON");

    cJSON *jr = cJSON_GetObjectItem(root, "jsonrpc");
    CHECK(jr != NULL && cJSON_IsString(jr) && strcmp(jr->valuestring, "2.0") == 0,
          "jsonrpc version");

    cJSON *method = cJSON_GetObjectItem(root, "method");
    CHECK(method != NULL && cJSON_IsString(method) &&
              strcmp(method->valuestring, "tools/list") == 0,
          "method field");

    cJSON *id = cJSON_GetObjectItem(root, "id");
    CHECK(id != NULL && cJSON_IsString(id) && strcmp(id->valuestring, "1") == 0, "id field");

    cJSON *params = cJSON_GetObjectItem(root, "params");
    CHECK(params != NULL && cJSON_IsObject(params), "params object present");

    cJSON_Delete(root);
    free(req);

    fprintf(stderr, "  [ok] test_build_request_basic\n");
}

static void test_build_request_no_params(void) {
    char *req = jsonrpc_build_request("tools/list", NULL, "7");
    CHECK(req != NULL, "build_request with NULL params");

    CHECK(has_field(req, "jsonrpc"), "jsonrpc field present");
    CHECK(has_field(req, "method"), "method field present");
    CHECK(has_field(req, "id"), "id field present");
    CHECK(!has_field(req, "params"), "params field should be absent");

    free(req);
    fprintf(stderr, "  [ok] test_build_request_no_params\n");
}

static void test_build_request_no_id(void) {
    char *req = jsonrpc_build_request("notifications/progress", "{\"p\":50}", NULL);
    CHECK(req != NULL, "build_request notification");

    CHECK(!has_field(req, "id"), "id field should be absent for notification");
    CHECK(has_field(req, "params"), "params present for notification");

    free(req);
    fprintf(stderr, "  [ok] test_build_request_no_id\n");
}

static void test_build_request_invalid_params(void) {
    char *req = jsonrpc_build_request("tools/call", "{not valid json", "1");
    CHECK_STR_NULL(req, "invalid params_json should yield NULL");

    free(req);
    fprintf(stderr, "  [ok] test_build_request_invalid_params\n");
}

static void test_build_request_null_method(void) {
    char *req = jsonrpc_build_request(NULL, "{}", "1");
    CHECK_STR_NULL(req, "NULL method should yield NULL");

    free(req);
    fprintf(stderr, "  [ok] test_build_request_null_method\n");
}

static void test_id_increments(void) {
    char *r1 = NULL;
    char *r2 = NULL;
    CHECK_EQ(jsonrpc_build_list_tools(&r1), EXIT_SUCCESS, "first list_tools build");
    CHECK_EQ(jsonrpc_build_list_tools(&r2), EXIT_SUCCESS, "second list_tools build");

    char *id1 = dup_string_field(r1, "id");
    char *id2 = dup_string_field(r2, "id");
    CHECK(id1 != NULL && id2 != NULL, "both ids present");
    CHECK(id1 == NULL || id2 == NULL || strcmp(id1, id2) != 0, "ids must differ across calls");

    /* verify numeric ordering: id2 > id1 as integers */
    if (id1 != NULL && id2 != NULL) {
        long n1 = strtol(id1, NULL, 10);
        long n2 = strtol(id2, NULL, 10);
        CHECK(n2 > n1, "second id should be greater than first");
    }

    free(id1);
    free(id2);
    free(r1);
    free(r2);
    fprintf(stderr, "  [ok] test_id_increments\n");
}

static void test_parse_response_result_scalar(void) {
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":42}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_SUCCESS, "result response");
    CHECK_STR_EQ(result, "42", "result scalar serialized");
    CHECK_STR_NULL(err, "no error on result response");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_result_scalar\n");
}

static void test_parse_response_result_object(void) {
    const char *resp =
        "{\"jsonrpc\":\"2.0\",\"id\":\"2\",\"result\":{\"tools\":[{\"name\":\"fs.get\"}]}}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_SUCCESS, "object result response");

    cJSON *parsed = cJSON_Parse(result);
    CHECK(parsed != NULL, "result should re-parse as JSON");
    cJSON *tools = (parsed != NULL) ? cJSON_GetObjectItem(parsed, "tools") : NULL;
    CHECK(tools != NULL && cJSON_GetArraySize(tools) == 1, "tools array preserved");
    cJSON_Delete(parsed);

    CHECK_STR_NULL(err, "no error on object result");
    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_result_object\n");
}

static void test_parse_response_result_null_value(void) {
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"3\",\"result\":null}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_SUCCESS,
             "null result is still a result");
    CHECK_STR_EQ(result, "null", "null result serialized as \"null\"");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_result_null_value\n");
}

static void test_parse_response_error(void) {
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"4\",\"error\":{\"code\":-32601,\"message\":"
                       "\"Method not found\"}}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_MCP_ERR, "error response code");
    CHECK_STR_EQ(err, "Method not found", "error.message extracted");
    CHECK_STR_NULL(result, "no result on error response");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_error\n");
}

static void test_parse_response_error_no_message(void) {
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"5\",\"error\":{\"code\":-32700}}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_MCP_ERR, "error without message");
    CHECK_STR_EQ(err, "Unknown MCP error", "fallback message used");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_error_no_message\n");
}

static void test_parse_response_malformed(void) {
    const char *resp = "{not json at all";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_INTERNAL_ERR, "malformed JSON");
    CHECK_STR_NULL(result, "no result on malformed");
    CHECK_STR_NULL(err, "no error string on malformed");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_malformed\n");
}

static void test_parse_response_no_result_no_error(void) {
    const char *resp = "{\"jsonrpc\":\"2.0\",\"id\":\"6\"}";
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_INTERNAL_ERR,
             "response with neither result nor error");

    free(result);
    free(err);
    fprintf(stderr, "  [ok] test_parse_response_no_result_no_error\n");
}

static void test_parse_response_null_args(void) {
    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(NULL, &result, &err), EXIT_INTERNAL_ERR, "NULL response");
    CHECK_EQ(jsonrpc_parse_response("{}", NULL, &err), EXIT_INTERNAL_ERR, "NULL out_result");
    CHECK_EQ(jsonrpc_parse_response("{}", &result, NULL), EXIT_INTERNAL_ERR, "NULL out_error");

    fprintf(stderr, "  [ok] test_parse_response_null_args\n");
}

static void test_build_initialize(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_initialize(&req), EXIT_SUCCESS, "build_initialize");
    CHECK(req != NULL, "initialize request non-NULL");

    cJSON *root = cJSON_Parse(req);
    CHECK(root != NULL, "initialize parses");

    cJSON *method = (root != NULL) ? cJSON_GetObjectItem(root, "method") : NULL;
    CHECK(method != NULL && cJSON_IsString(method) &&
              strcmp(method->valuestring, "initialize") == 0,
          "method == initialize");

    cJSON *params = (root != NULL) ? cJSON_GetObjectItem(root, "params") : NULL;
    CHECK(params != NULL && cJSON_IsObject(params), "params object present");

    cJSON *pv = (params != NULL) ? cJSON_GetObjectItem(params, "protocolVersion") : NULL;
    CHECK(pv != NULL && cJSON_IsString(pv) && strcmp(pv->valuestring, MCP_PROTOCOL_VERSION) == 0,
          "protocolVersion matches");

    cJSON *caps = (params != NULL) ? cJSON_GetObjectItem(params, "capabilities") : NULL;
    CHECK(caps != NULL && cJSON_IsObject(caps), "capabilities object present");

    cJSON *info = (params != NULL) ? cJSON_GetObjectItem(params, "clientInfo") : NULL;
    CHECK(info != NULL && cJSON_IsObject(info), "clientInfo object present");
    cJSON *name = (info != NULL) ? cJSON_GetObjectItem(info, "name") : NULL;
    CHECK(name != NULL && cJSON_IsString(name) && strcmp(name->valuestring, "llmkit") == 0,
          "clientInfo.name == llmkit");
    cJSON *ver = (info != NULL) ? cJSON_GetObjectItem(info, "version") : NULL;
    CHECK(ver != NULL && cJSON_IsString(ver) && strcmp(ver->valuestring, LLMKIT_VERSION) == 0,
          "clientInfo.version matches LLMKIT_VERSION");

    cJSON *id = (root != NULL) ? cJSON_GetObjectItem(root, "id") : NULL;
    CHECK(id != NULL && cJSON_IsString(id), "id present as string");

    cJSON_Delete(root);
    free(req);
    fprintf(stderr, "  [ok] test_build_initialize\n");
}

static void test_build_list_tools(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_list_tools(&req), EXIT_SUCCESS, "build_list_tools");

    cJSON *root = cJSON_Parse(req);
    cJSON *method = (root != NULL) ? cJSON_GetObjectItem(root, "method") : NULL;
    CHECK(method != NULL && cJSON_IsString(method) &&
              strcmp(method->valuestring, "tools/list") == 0,
          "method == tools/list");
    cJSON_Delete(root);

    /* MCP canonical tools/list omits params entirely */
    CHECK(!has_field(req, "params"), "tools/list has no params field");

    free(req);
    fprintf(stderr, "  [ok] test_build_list_tools\n");
}

static void test_build_list_resources(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_list_resources(&req), EXIT_SUCCESS, "build_list_resources");
    char *method = dup_string_field(req, "method");
    CHECK_STR_EQ(method, "resources/list", "method == resources/list");

    free(method);
    free(req);
    fprintf(stderr, "  [ok] test_build_list_resources\n");
}

static void test_build_list_prompts(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_list_prompts(&req), EXIT_SUCCESS, "build_list_prompts");
    char *method = dup_string_field(req, "method");
    CHECK_STR_EQ(method, "prompts/list", "method == prompts/list");

    free(method);
    free(req);
    fprintf(stderr, "  [ok] test_build_list_prompts\n");
}

static void test_build_call_tool_with_args(void) {
    char *req = NULL;
    CHECK_EQ(
        jsonrpc_build_call_tool("get_weather", "{\"location\":\"Paris\",\"unit\":\"c\"}", &req),
        EXIT_SUCCESS, "build_call_tool");

    cJSON *root = cJSON_Parse(req);
    cJSON *method = (root != NULL) ? cJSON_GetObjectItem(root, "method") : NULL;
    CHECK(method != NULL && cJSON_IsString(method) &&
              strcmp(method->valuestring, "tools/call") == 0,
          "method == tools/call");

    cJSON *params = (root != NULL) ? cJSON_GetObjectItem(root, "params") : NULL;
    cJSON *name = (params != NULL) ? cJSON_GetObjectItem(params, "name") : NULL;
    CHECK(name != NULL && cJSON_IsString(name) && strcmp(name->valuestring, "get_weather") == 0,
          "params.name preserved");

    cJSON *args = (params != NULL) ? cJSON_GetObjectItem(params, "arguments") : NULL;
    CHECK(args != NULL && cJSON_IsObject(args), "params.arguments is object");
    cJSON *loc = (args != NULL) ? cJSON_GetObjectItem(args, "location") : NULL;
    CHECK(loc != NULL && cJSON_IsString(loc) && strcmp(loc->valuestring, "Paris") == 0,
          "arguments.location preserved");
    cJSON *unit = (args != NULL) ? cJSON_GetObjectItem(args, "unit") : NULL;
    CHECK(unit != NULL && cJSON_IsString(unit) && strcmp(unit->valuestring, "c") == 0,
          "arguments.unit preserved");

    cJSON_Delete(root);
    free(req);
    fprintf(stderr, "  [ok] test_build_call_tool_with_args\n");
}

static void test_build_call_tool_null_args(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_call_tool("noop", NULL, &req), EXIT_SUCCESS, "call_tool NULL args");

    cJSON *root = cJSON_Parse(req);
    cJSON *params = (root != NULL) ? cJSON_GetObjectItem(root, "params") : NULL;
    cJSON *args = (params != NULL) ? cJSON_GetObjectItem(params, "arguments") : NULL;
    CHECK(args != NULL && cJSON_IsObject(args), "NULL args -> empty arguments object");
    CHECK(args != NULL && cJSON_GetArraySize(args) == 0, "empty arguments has no keys");

    cJSON_Delete(root);
    free(req);
    fprintf(stderr, "  [ok] test_build_call_tool_null_args\n");
}

static void test_build_call_tool_invalid_args(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_call_tool("get_weather", "{broken", &req), EXIT_INTERNAL_ERR,
             "invalid args_json -> internal error");
    CHECK_STR_NULL(req, "no output on invalid args");

    free(req);
    fprintf(stderr, "  [ok] test_build_call_tool_invalid_args\n");
}

static void test_build_call_tool_null_name(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_call_tool(NULL, "{}", &req), EXIT_INTERNAL_ERR, "NULL name -> error");
    CHECK_STR_NULL(req, "no output on NULL name");

    free(req);
    fprintf(stderr, "  [ok] test_build_call_tool_null_name\n");
}

/* Deliverable: request/response round-trip. Build an initialize request, fake
 * a server result response, and confirm the parser extracts it cleanly. */
static void test_round_trip_initialize(void) {
    char *req = NULL;
    CHECK_EQ(jsonrpc_build_initialize(&req), EXIT_SUCCESS, "initialize build");
    char *id = dup_string_field(req, "id");
    CHECK(id != NULL, "initialize request has id");

    /* Construct a plausible initialize result using the id from the request */
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{\"protocolVersion\":\"%s\","
             "\"serverInfo\":{\"name\":\"test-server\",\"version\":\"1.0\"}}}",
             id != NULL ? id : "0", MCP_PROTOCOL_VERSION);

    char *result = NULL;
    char *err = NULL;
    CHECK_EQ(jsonrpc_parse_response(resp, &result, &err), EXIT_SUCCESS,
             "initialize response parse");
    CHECK_STR_NULL(err, "no error on initialize response");

    cJSON *parsed = cJSON_Parse(result);
    cJSON *sv = (parsed != NULL) ? cJSON_GetObjectItem(parsed, "serverInfo") : NULL;
    CHECK(sv != NULL && cJSON_IsObject(sv), "result.serverInfo present");
    cJSON_Delete(parsed);

    free(result);
    free(err);
    free(id);
    free(req);
    fprintf(stderr, "  [ok] test_round_trip_initialize\n");
}

int main(void) {
    fprintf(stderr, "=== test_jsonrpc ===\n");

    test_build_request_basic();
    test_build_request_no_params();
    test_build_request_no_id();
    test_build_request_invalid_params();
    test_build_request_null_method();
    test_id_increments();
    test_parse_response_result_scalar();
    test_parse_response_result_object();
    test_parse_response_result_null_value();
    test_parse_response_error();
    test_parse_response_error_no_message();
    test_parse_response_malformed();
    test_parse_response_no_result_no_error();
    test_parse_response_null_args();
    test_build_initialize();
    test_build_list_tools();
    test_build_list_resources();
    test_build_list_prompts();
    test_build_call_tool_with_args();
    test_build_call_tool_null_args();
    test_build_call_tool_invalid_args();
    test_build_call_tool_null_name();
    test_round_trip_initialize();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
