#include "mcp.h"
#include "llmkit.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests = 0, failed = 0;

#define TEST(name)                     \
    do {                               \
        tests++;                       \
        printf("  [test] %s\n", name); \
    } while (0)
#define ASSERT(cond, msg)                                      \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("    FAIL: %s (line %d)\n", msg, __LINE__); \
            failed++;                                          \
        }                                                      \
    } while (0)

/* ---- test function declarations ---- */
static void test_connect_all_empty(void);
static void test_connect_all_null_ctx(void);
static void test_discover_tools_no_servers(void);
static void test_call_tool_unknown_server(void);
static void test_send_request_unknown_server(void);

int main(void) {
    printf("=== test_mcp ===\n");

    test_connect_all_empty();
    test_connect_all_null_ctx();
    test_discover_tools_no_servers();
    test_call_tool_unknown_server();
    test_send_request_unknown_server();

    printf("\n%d tests, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

void test_connect_all_empty(void) {
    TEST("connect_all with 0 servers succeeds");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mcp_count = 0;
    ctx.mcps = NULL;

    int rc = mcp_connect_all(&ctx);
    ASSERT(rc == EXIT_SUCCESS, "returns success with 0 servers");

    /* Should be safe to disconnect even with no connections. */
    mcp_disconnect_all(&ctx);
}

void test_connect_all_null_ctx(void) {
    TEST("connect_all with NULL ctx returns error");
    int rc = mcp_connect_all(NULL);
    ASSERT(rc == EXIT_INTERNAL_ERR, "returns internal error for NULL ctx");
}

void test_discover_tools_no_servers(void) {
    TEST("discover_tools with no connections returns empty");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.tools = NULL;
    ctx.tool_count = -1;

    /* No servers connected, so tools should be empty. */
    int rc = mcp_discover_tools(&ctx);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    ASSERT(ctx.tool_count == 0, "tool_count is 0");
    ASSERT(ctx.tools == NULL, "tools is NULL");
}

void test_call_tool_unknown_server(void) {
    TEST("call_tool with unknown server returns error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* No servers connected. */

    char *result = NULL;
    bool is_error = false;
    int rc = mcp_call_tool(&ctx, "nonexistent", "some_tool", "{}", &result, &is_error);
    ASSERT(rc == EXIT_MCP_ERR, "returns MCP error for unknown server");
    ASSERT(is_error == true, "is_error set to true");
    free(result);
}

void test_send_request_unknown_server(void) {
    TEST("send_request with unknown server returns error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    char *resp = NULL;
    int rc = mcp_send_request(&ctx, "nonexistent", "{}", &resp);
    ASSERT(rc == EXIT_MCP_ERR, "returns MCP error");
    ASSERT(resp == NULL, "response is NULL");
}
