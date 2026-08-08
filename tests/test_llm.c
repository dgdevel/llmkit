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
static void test_request_body_deterministic(void);
static void test_request_body_prefix_stable(void);
static void test_request_body_tools_stable(void);
static void test_parse_deepseek_cache_usage(void);
static void test_parse_openai_cached_tokens(void);
static void test_parse_no_cache_fields(void);

int main(void) {
    printf("=== test_llm ===\n");

    test_chat_null_ctx();
    test_chat_null_output_ptrs();
    test_chat_no_api_base();
    test_chat_empty_messages();
    test_chat_empty_tools();
    test_request_body_deterministic();
    test_request_body_prefix_stable();
    test_request_body_tools_stable();
    test_parse_deepseek_cache_usage();
    test_parse_openai_cached_tokens();
    test_parse_no_cache_fields();

    printf("\n%d tests, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

void test_chat_null_ctx(void) {
    TEST("chat_complete with NULL ctx returns error");
    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc = llm_chat_complete(NULL, NULL, 0, NULL, 0, &content, &reasoning, &model, &calls,
                               &call_count, &usage);
    ASSERT(rc == EXIT_INTERNAL_ERR, "returns internal error");
}

void test_chat_null_output_ptrs(void) {
    TEST("chat_complete with NULL output pointers returns error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.llm.api_base = util_strdup("http://localhost:9999/v1");

    int rc = llm_chat_complete(&ctx, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL);
    ASSERT(rc == EXIT_INTERNAL_ERR, "returns internal error");

    free(ctx.llm.api_base);
}

void test_chat_no_api_base(void) {
    TEST("chat_complete with missing api_base returns LLM error");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* api_base is NULL */

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc = llm_chat_complete(&ctx, NULL, 0, NULL, 0, &content, &reasoning, &model, &calls,
                               &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "returns LLM error");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}

void test_chat_empty_messages(void) {
    TEST("chat_complete with empty messages (no api call, api_base invalid)");
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.llm.api_base = util_strdup("http://localhost:1/v1");
    ctx.llm.model = util_strdup("gpt-4o-mini");

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    /* Will attempt HTTP call and fail with EXIT_LLM_ERR. */
    int rc = llm_chat_complete(&ctx, NULL, 0, NULL, 0, &content, &reasoning, &model, &calls,
                               &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "fails with LLM error (no server)");

    free(content);
    free(reasoning);
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

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;

    int rc = llm_chat_complete(&ctx, msgs, 1, NULL, 0, &content, &reasoning, &model, &calls,
                               &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "fails with LLM error (no server)");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
    free(msgs[0].role);
    free(msgs[0].content);
    free(ctx.llm.api_base);
}

/* ------------------------------------------------------------------ */
/*  Request-body serialization (prefix-cache stability)                */
/* ------------------------------------------------------------------ */

/* Return pointer to the '[' opening the array for a JSON key (e.g. the
 * "messages" or "tools" array), or NULL. The test payloads are flat JSON
 * (no nested arrays), so the matching close is the next ']'. */
static const char *find_array(const char *body, const char *key) {
    const char *p = strstr(body, key);
    if (p == NULL) return NULL;
    return strchr(p + strlen(key), '[');
}

void test_request_body_deterministic(void) {
    TEST("request body serialization is deterministic (same input -> same bytes)");
    json_message msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("system");
    msgs[0].content = util_strdup("You are a helpful assistant.");
    msgs[1].role = util_strdup("user");
    msgs[1].content = util_strdup("Hello");

    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = util_strdup("deepseek-chat");

    char *a = llm_build_request_body(msgs, 2, NULL, 0, &cfg);
    char *b = llm_build_request_body(msgs, 2, NULL, 0, &cfg);
    ASSERT(a != NULL && b != NULL, "body serialized");
    if (a != NULL && b != NULL) {
        ASSERT(strcmp(a, b) == 0, "byte-identical across calls");
    }
    free(a);
    free(b);
    free(cfg.model);
    free(msgs[0].role);
    free(msgs[0].content);
    free(msgs[1].role);
    free(msgs[1].content);
}

void test_request_body_prefix_stable(void) {
    TEST("grown history keeps earlier message bytes identical (append-only prefix)");
    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = util_strdup("deepseek-chat");

    /* Base history: system + user + assistant + tool result (flat JSON). */
    json_message base[4];
    memset(base, 0, sizeof(base));
    base[0].role = util_strdup("system");
    base[0].content = util_strdup("You are a helpful assistant.");
    base[1].role = util_strdup("user");
    base[1].content = util_strdup("What time is it?");
    base[2].role = util_strdup("assistant");
    base[2].content = util_strdup("");
    base[3].role = util_strdup("tool");
    base[3].content = util_strdup("12:00");
    base[3].tool_call_id = util_strdup("call_1");

    char *body_base = llm_build_request_body(base, 4, NULL, 0, &cfg);

    /* Grown history: same four messages plus one more user turn. */
    json_message grown[5];
    memcpy(grown, base, sizeof(base));
    grown[4].role = util_strdup("user");
    grown[4].content = util_strdup("And tomorrow?");

    char *body_grown = llm_build_request_body(grown, 5, NULL, 0, &cfg);

    ASSERT(body_base != NULL && body_grown != NULL, "bodies serialized");
    if (body_base != NULL && body_grown != NULL) {
        /* The serialized messages array of the base history must be a strict
         * byte-prefix of the grown array (up to the base's closing ']'). */
        const char *base_open = find_array(body_base, "\"messages\":");
        const char *grown_open = find_array(body_grown, "\"messages\":");
        ASSERT(base_open != NULL && grown_open != NULL, "messages arrays found");
        if (base_open != NULL && grown_open != NULL) {
            const char *base_close = strchr(base_open, ']');
            const char *grown_close = strchr(grown_open, ']');
            ASSERT(base_close != NULL && grown_close != NULL, "messages arrays closed");
            if (base_close != NULL && grown_close != NULL) {
                size_t base_len = (size_t)(base_close - base_open); /* exclude ']' */
                ASSERT(strncmp(grown_open, base_open, base_len) == 0,
                       "earlier messages byte-identical in grown body");
                ASSERT(grown_open[base_len] == ',', "grown body appends after base prefix");
            }
        }
        /* The system prompt block at the head of the body is identical. */
        const char *sys_text = "You are a helpful assistant.";
        const char *sys_base = strstr(body_base, sys_text);
        const char *sys_grown = strstr(body_grown, sys_text);
        ASSERT(sys_base != NULL && sys_grown != NULL, "system prompt present in both");
        if (sys_base != NULL && sys_grown != NULL) {
            size_t sys_len = strlen(sys_text);
            ASSERT(strncmp(body_grown, body_base, (size_t)(sys_base - body_base) + sys_len) == 0,
                   "body head up to system prompt identical");
        }
    }

    free(body_base);
    free(body_grown);
    free(cfg.model);
    free(grown[4].role);
    free(grown[4].content);
    for (int i = 0; i < 4; i++) {
        free(base[i].role);
        free(base[i].content);
        free(base[i].tool_call_id);
    }
}

void test_parse_deepseek_cache_usage(void) {
    TEST("parse_response reads DeepSeek prompt_cache_hit/miss_tokens");
    const char *body =
        "{\"id\":\"x\",\"model\":\"deepseek-chat\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":5,\"total_tokens\":125,"
        "\"prompt_cache_hit_tokens\":100,\"prompt_cache_miss_tokens\":20}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response(body, &content, &reasoning, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_SUCCESS, "parses successfully");
    ASSERT(content != NULL && strcmp(content, "hi") == 0, "content extracted");
    ASSERT(model != NULL && strcmp(model, "deepseek-chat") == 0, "model extracted");
    ASSERT(usage.prompt_tokens == 120 && usage.completion_tokens == 5 && usage.total_tokens == 125,
           "base usage extracted");
    ASSERT(usage.prompt_cache_hit_tokens == 100, "cache hit tokens extracted");
    ASSERT(usage.prompt_cache_miss_tokens == 20, "cache miss tokens extracted");
    ASSERT(usage.cached_tokens == 0, "OpenAI cached_tokens untouched");
    ASSERT(call_count == 0 && calls == NULL, "no tool calls");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}

void test_parse_openai_cached_tokens(void) {
    TEST("parse_response reads OpenAI prompt_tokens_details.cached_tokens");
    const char *body =
        "{\"id\":\"y\",\"model\":\"gpt-4o\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"yo\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":7,\"total_tokens\":57,"
        "\"prompt_tokens_details\":{\"cached_tokens\":42}}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response(body, &content, &reasoning, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_SUCCESS, "parses successfully");
    ASSERT(usage.cached_tokens == 42, "cached_tokens extracted");
    ASSERT(usage.prompt_cache_hit_tokens == 0 && usage.prompt_cache_miss_tokens == 0,
           "DeepSeek fields untouched");
    ASSERT(content != NULL && strcmp(content, "yo") == 0, "content extracted");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}

void test_parse_no_cache_fields(void) {
    TEST("parse_response leaves cache fields zero when provider reports none");
    const char *body =
        "{\"id\":\"z\",\"model\":\"m\","
        "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":1,\"total_tokens\":10}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response(body, &content, &reasoning, &model, &calls, &call_count, &usage);
    ASSERT(rc == EXIT_SUCCESS, "parses successfully");
    ASSERT(usage.prompt_cache_hit_tokens == 0 && usage.prompt_cache_miss_tokens == 0 &&
               usage.cached_tokens == 0,
           "all cache fields zero");
    ASSERT(usage.total_tokens == 10, "base usage still parsed");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}

void test_request_body_tools_stable(void) {
    TEST("tools block is serialized deterministically and reflects changes");
    json_message msgs[1];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("user");
    msgs[0].content = util_strdup("Hi");

    tool_def tools[2];
    memset(tools, 0, sizeof(tools));
    tools[0].name = util_strdup("a.get_time");
    tools[0].description = util_strdup("Get the current time");
    tools[0].input_schema = util_strdup("{\"type\":\"object\",\"properties\":{}}");
    tools[1].name = util_strdup("b.read_file");
    tools[1].description = util_strdup("Read a file");
    tools[1].input_schema = util_strdup("{\"type\":\"object\",\"properties\":{}}");

    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.model = util_strdup("deepseek-chat");

    char *a = llm_build_request_body(msgs, 1, tools, 2, &cfg);
    char *b = llm_build_request_body(msgs, 1, tools, 2, &cfg);
    ASSERT(a != NULL && b != NULL, "bodies serialized");
    if (a != NULL && b != NULL) {
        ASSERT(strcmp(a, b) == 0, "same tools -> identical bytes");
    }
    free(b);

    /* Change one tool's description: the body must differ. */
    free(tools[1].description);
    tools[1].description = util_strdup("Read a file (updated)");
    char *c = llm_build_request_body(msgs, 1, tools, 2, &cfg);
    ASSERT(c != NULL, "changed body serialized");
    if (a != NULL && c != NULL) {
        ASSERT(strcmp(a, c) != 0, "changed tools -> different bytes");
    }

    free(a);
    free(c);
    free(cfg.model);
    free(msgs[0].role);
    free(msgs[0].content);
    for (int i = 0; i < 2; i++) {
        free(tools[i].name);
        free(tools[i].description);
        free(tools[i].input_schema);
    }
}
