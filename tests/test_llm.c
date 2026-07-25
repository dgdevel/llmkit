#include "llm.h"
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

/* ---- declarations ---- */
static void test_chat_null_ctx(void);
static void test_chat_null_output_ptrs(void);
static void test_chat_no_api_base(void);
static void test_chat_empty_messages(void);
static void test_chat_empty_tools(void);

int main(void) {
    printf("=== test_llm ===\n");

    test_chat_null_ctx();
    test_chat_null_output_ptrs();
    test_chat_no_api_base();
    test_chat_empty_messages();
    test_chat_empty_tools();

    printf("\n%d tests, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

void test_chat_null_ctx(void) {
    TEST("chat_complete with NULL ctx returns error");
    char *content = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc =
        llm_chat_complete(NULL, NULL, 0, NULL, 0, &content, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_INTERNAL_ERR, "returns internal error");
}

void test_chat_null_output_ptrs(void) {
    TEST("chat_complete with NULL output pointers returns error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.llm.api_base = util_strdup("http://localhost:9999/v1");

    int rc = llm_chat_complete(&ctx, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL);
    ASSERT(rc == EXIT_INTERNAL_ERR, "returns internal error");

    free(ctx.llm.api_base);
}

void test_chat_no_api_base(void) {
    TEST("chat_complete with missing api_base returns LLM error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* api_base is NULL */

    char *content = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc =
        llm_chat_complete(&ctx, NULL, 0, NULL, 0, &content, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "returns LLM error");

    free(content);
    free(model);
    free(calls);
}

void test_chat_empty_messages(void) {
    TEST("chat_complete with empty messages (no api call, api_base invalid)");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.llm.api_base = util_strdup("http://localhost:1/v1");
    ctx.llm.model = util_strdup("gpt-4o-mini");

    char *content = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    /* Will attempt HTTP call and fail with EXIT_LLM_ERR. */
    int rc =
        llm_chat_complete(&ctx, NULL, 0, NULL, 0, &content, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "fails with LLM error (no server)");

    free(content);
    free(model);
    free(calls);
    free(ctx.llm.api_base);
    free(ctx.llm.model);
}

void test_chat_empty_tools(void) {
    TEST("chat_complete with no tools (no api call, api_base invalid)");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.llm.api_base = util_strdup("http://localhost:2/v1");

    /* Build one user message. */
    json_message msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("user");
    msgs[0].content = util_strdup("Hi");

    char *content = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc =
        llm_chat_complete(&ctx, msgs, 1, NULL, 0, &content, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "fails with LLM error (no server)");

    free(content);
    free(model);
    free(calls);
    free(msgs[0].role);
    free(msgs[0].content);
    free(ctx.llm.api_base);
}
