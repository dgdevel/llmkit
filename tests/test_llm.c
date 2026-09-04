#include "llm.h"
#include "llmkit.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

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
static void test_anthropic_body_basic(void);
static void test_anthropic_body_tool_roundtrip(void);
static void test_anthropic_body_tool_merge(void);
static void test_anthropic_body_deterministic_prefix(void);
static void test_anthropic_parse_text_usage(void);
static void test_anthropic_parse_tool_thinking(void);
static void test_anthropic_parse_error(void);

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
    test_anthropic_body_basic();
    test_anthropic_body_tool_roundtrip();
    test_anthropic_body_tool_merge();
    test_anthropic_body_deterministic_prefix();
    test_anthropic_parse_text_usage();
    test_anthropic_parse_tool_thinking();
    test_anthropic_parse_error();

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

/* ------------------------------------------------------------------ */
/*  Anthropic Messages API                                             */
/* ------------------------------------------------------------------ */

static void free_calls(tool_call *calls, int count) {
    for (int i = 0; i < count; i++) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].arguments);
    }
    free(calls);
}

void test_anthropic_body_basic(void) {
    TEST("anthropic body hoists system, sets model/max_tokens, text-blocks user content");
    json_message msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("system");
    msgs[0].content = util_strdup("Be brief.");
    msgs[1].role = util_strdup("user");
    msgs[1].content = util_strdup("Hello");

    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = LLM_PROVIDER_ANTHROPIC;
    cfg.model = util_strdup("claude-mock");

    char *body = llm_build_request_body_anthropic(msgs, 2, NULL, 0, &cfg);
    ASSERT(body != NULL, "body serialized");
    if (body != NULL) {
        cJSON *root = cJSON_Parse(body);
        ASSERT(root != NULL, "body is valid JSON");
        if (root != NULL) {
            cJSON *sys = cJSON_GetObjectItem(root, "system");
            ASSERT(sys != NULL && cJSON_IsString(sys) && strcmp(sys->valuestring, "Be brief.") == 0,
                   "top-level system parameter");
            cJSON *mt = cJSON_GetObjectItem(root, "max_tokens");
            ASSERT(mt != NULL && cJSON_IsNumber(mt) && mt->valueint == 4096,
                   "max_tokens defaults to 4096 when unset");
            cJSON *model = cJSON_GetObjectItem(root, "model");
            ASSERT(model != NULL && cJSON_IsString(model) &&
                       strcmp(model->valuestring, "claude-mock") == 0,
                   "model passed through");
            cJSON *arr = cJSON_GetObjectItem(root, "messages");
            ASSERT(arr != NULL && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 1,
                   "system message not repeated in messages");
            cJSON *m0 = (arr != NULL) ? cJSON_GetArrayItem(arr, 0) : NULL;
            cJSON *role = (m0 != NULL) ? cJSON_GetObjectItem(m0, "role") : NULL;
            ASSERT(role != NULL && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0,
                   "only the user message remains");
            cJSON *content = (m0 != NULL) ? cJSON_GetObjectItem(m0, "content") : NULL;
            cJSON *block =
                (content != NULL && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : NULL;
            cJSON *type = (block != NULL) ? cJSON_GetObjectItem(block, "type") : NULL;
            cJSON *text = (block != NULL) ? cJSON_GetObjectItem(block, "text") : NULL;
            ASSERT(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
                       text != NULL && cJSON_IsString(text) &&
                       strcmp(text->valuestring, "Hello") == 0,
                   "user content is a text block");
            cJSON_Delete(root);
        }
        free(body);
    }

    /* Explicit max_tokens is passed through. */
    cfg.max_tokens = 1024;
    body = llm_build_request_body_anthropic(msgs, 2, NULL, 0, &cfg);
    ASSERT(body != NULL, "body with max_tokens serialized");
    if (body != NULL) {
        ASSERT(strstr(body, "\"max_tokens\":1024") != NULL, "explicit max_tokens honored");
        free(body);
    }

    free(cfg.model);
    free(msgs[0].role);
    free(msgs[0].content);
    free(msgs[1].role);
    free(msgs[1].content);
}

void test_anthropic_body_tool_roundtrip(void) {
    TEST("anthropic body maps tool_calls/tool messages to tool_use/tool_result blocks");
    json_message msgs[4];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("user");
    msgs[0].content = util_strdup("What time is it?");
    msgs[1].role = util_strdup("assistant");
    msgs[1].content = util_strdup("");
    msgs[1].tool_call_count = 1;
    msgs[1].tool_calls = calloc(1, sizeof(tool_call));
    msgs[1].tool_calls[0].id = util_strdup("toolu_1");
    msgs[1].tool_calls[0].name = util_strdup("ns.get_time");
    msgs[1].tool_calls[0].arguments = util_strdup("{\"tz\":\"UTC\"}");
    msgs[2].role = util_strdup("tool");
    msgs[2].content = util_strdup("12:00");
    msgs[2].tool_call_id = util_strdup("toolu_1");
    msgs[3].role = util_strdup("user");
    msgs[3].content = util_strdup("Thanks");

    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = LLM_PROVIDER_ANTHROPIC;
    cfg.model = util_strdup("claude-mock");

    char *body = llm_build_request_body_anthropic(msgs, 4, NULL, 0, &cfg);
    ASSERT(body != NULL, "body serialized");
    if (body != NULL) {
        cJSON *root = cJSON_Parse(body);
        ASSERT(root != NULL, "body is valid JSON");
        if (root != NULL) {
            cJSON *arr = cJSON_GetObjectItem(root, "messages");
            ASSERT(arr != NULL && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 3,
                   "three wire messages (user, assistant, user)");
            cJSON *asst = (arr != NULL) ? cJSON_GetArrayItem(arr, 1) : NULL;
            cJSON *role = (asst != NULL) ? cJSON_GetObjectItem(asst, "role") : NULL;
            ASSERT(role != NULL && cJSON_IsString(role) &&
                       strcmp(role->valuestring, "assistant") == 0,
                   "assistant message in place");
            cJSON *content = (asst != NULL) ? cJSON_GetObjectItem(asst, "content") : NULL;
            cJSON *use =
                (content != NULL && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : NULL;
            cJSON *type = (use != NULL) ? cJSON_GetObjectItem(use, "type") : NULL;
            cJSON *id = (use != NULL) ? cJSON_GetObjectItem(use, "id") : NULL;
            cJSON *name = (use != NULL) ? cJSON_GetObjectItem(use, "name") : NULL;
            cJSON *input = (use != NULL) ? cJSON_GetObjectItem(use, "input") : NULL;
            ASSERT(type != NULL && cJSON_IsString(type) &&
                       strcmp(type->valuestring, "tool_use") == 0,
                   "tool call becomes a tool_use block");
            ASSERT(id != NULL && cJSON_IsString(id) && strcmp(id->valuestring, "toolu_1") == 0 &&
                       name != NULL && cJSON_IsString(name) &&
                       strcmp(name->valuestring, "ns.get_time") == 0,
                   "tool_use id/name preserved");
            cJSON *tz =
                (input != NULL && cJSON_IsObject(input)) ? cJSON_GetObjectItem(input, "tz") : NULL;
            ASSERT(tz != NULL && cJSON_IsString(tz) && strcmp(tz->valuestring, "UTC") == 0,
                   "arguments string parsed into the input object");

            cJSON *res_user = (arr != NULL) ? cJSON_GetArrayItem(arr, 2) : NULL;
            role = (res_user != NULL) ? cJSON_GetObjectItem(res_user, "role") : NULL;
            content = (res_user != NULL) ? cJSON_GetObjectItem(res_user, "content") : NULL;
            cJSON *res =
                (content != NULL && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : NULL;
            type = (res != NULL) ? cJSON_GetObjectItem(res, "type") : NULL;
            cJSON *tool_use_id = (res != NULL) ? cJSON_GetObjectItem(res, "tool_use_id") : NULL;
            cJSON *res_content = (res != NULL) ? cJSON_GetObjectItem(res, "content") : NULL;
            ASSERT(role != NULL && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0 &&
                       type != NULL && cJSON_IsString(type) &&
                       strcmp(type->valuestring, "tool_result") == 0 && tool_use_id != NULL &&
                       cJSON_IsString(tool_use_id) &&
                       strcmp(tool_use_id->valuestring, "toolu_1") == 0 && res_content != NULL &&
                       cJSON_IsString(res_content) &&
                       strcmp(res_content->valuestring, "12:00") == 0,
                   "tool result becomes a tool_result block in a user message");
            /* The trailing user turn merges into the same user message (the
             * Messages API requires alternating roles). */
            cJSON *text2 = (content != NULL && cJSON_GetArraySize(content) > 1)
                               ? cJSON_GetArrayItem(content, 1)
                               : NULL;
            cJSON *t2_type = (text2 != NULL) ? cJSON_GetObjectItem(text2, "type") : NULL;
            cJSON *t2_text = (text2 != NULL) ? cJSON_GetObjectItem(text2, "text") : NULL;
            ASSERT(t2_type != NULL && cJSON_IsString(t2_type) &&
                       strcmp(t2_type->valuestring, "text") == 0 && t2_text != NULL &&
                       cJSON_IsString(t2_text) && strcmp(t2_text->valuestring, "Thanks") == 0,
                   "following user turn merged into the tool-result user message");
            cJSON_Delete(root);
        }
        free(body);
    }

    free(cfg.model);
    for (int i = 0; i < 4; i++) {
        free(msgs[i].role);
        free(msgs[i].content);
        free(msgs[i].tool_call_id);
    }
    free(msgs[1].tool_calls[0].id);
    free(msgs[1].tool_calls[0].name);
    free(msgs[1].tool_calls[0].arguments);
    free(msgs[1].tool_calls);
}

void test_anthropic_body_tool_merge(void) {
    TEST("anthropic body merges consecutive tool messages into one user message");
    json_message msgs[4];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = util_strdup("user");
    msgs[0].content = util_strdup("Go");
    msgs[1].role = util_strdup("assistant");
    msgs[1].content = util_strdup("");
    msgs[1].tool_call_count = 2;
    msgs[1].tool_calls = calloc(2, sizeof(tool_call));
    msgs[1].tool_calls[0].id = util_strdup("toolu_a");
    msgs[1].tool_calls[0].name = util_strdup("a");
    msgs[1].tool_calls[0].arguments = util_strdup("{}");
    msgs[1].tool_calls[1].id = util_strdup("toolu_b");
    msgs[1].tool_calls[1].name = util_strdup("b");
    msgs[1].tool_calls[1].arguments = util_strdup("{}");
    msgs[2].role = util_strdup("tool");
    msgs[2].content = util_strdup("result a");
    msgs[2].tool_call_id = util_strdup("toolu_a");
    msgs[3].role = util_strdup("tool");
    msgs[3].content = util_strdup("result b");
    msgs[3].tool_call_id = util_strdup("toolu_b");

    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = LLM_PROVIDER_ANTHROPIC;
    cfg.model = util_strdup("claude-mock");

    char *body = llm_build_request_body_anthropic(msgs, 4, NULL, 0, &cfg);
    ASSERT(body != NULL, "body serialized");
    if (body != NULL) {
        cJSON *root = cJSON_Parse(body);
        ASSERT(root != NULL, "body is valid JSON");
        if (root != NULL) {
            cJSON *arr = cJSON_GetObjectItem(root, "messages");
            ASSERT(arr != NULL && cJSON_GetArraySize(arr) == 3,
                   "two tool results merged into one user message");
            cJSON *last = (arr != NULL) ? cJSON_GetArrayItem(arr, 2) : NULL;
            cJSON *content = (last != NULL) ? cJSON_GetObjectItem(last, "content") : NULL;
            ASSERT(content != NULL && cJSON_IsArray(content) && cJSON_GetArraySize(content) == 2,
                   "both tool_result blocks present");
            cJSON *r0 = (content != NULL) ? cJSON_GetArrayItem(content, 0) : NULL;
            cJSON *r1 = (content != NULL) ? cJSON_GetArrayItem(content, 1) : NULL;
            cJSON *id0 = (r0 != NULL) ? cJSON_GetObjectItem(r0, "tool_use_id") : NULL;
            cJSON *id1 = (r1 != NULL) ? cJSON_GetObjectItem(r1, "tool_use_id") : NULL;
            ASSERT(id0 != NULL && cJSON_IsString(id0) && strcmp(id0->valuestring, "toolu_a") == 0 &&
                       id1 != NULL && cJSON_IsString(id1) &&
                       strcmp(id1->valuestring, "toolu_b") == 0,
                   "tool_result blocks reference both tool_use ids");
            cJSON_Delete(root);
        }
        free(body);
    }

    free(cfg.model);
    free(msgs[0].role);
    free(msgs[0].content);
    free(msgs[1].role);
    free(msgs[1].content);
    free(msgs[2].role);
    free(msgs[2].content);
    free(msgs[2].tool_call_id);
    free(msgs[3].role);
    free(msgs[3].content);
    free(msgs[3].tool_call_id);
    free(msgs[1].tool_calls[0].id);
    free(msgs[1].tool_calls[0].name);
    free(msgs[1].tool_calls[0].arguments);
    free(msgs[1].tool_calls[1].id);
    free(msgs[1].tool_calls[1].name);
    free(msgs[1].tool_calls[1].arguments);
    free(msgs[1].tool_calls);
}

void test_anthropic_body_deterministic_prefix(void) {
    TEST("anthropic body is deterministic and append-only (prefix-cache stability)");
    llm_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = LLM_PROVIDER_ANTHROPIC;
    cfg.model = util_strdup("claude-mock");

    json_message base[4];
    memset(base, 0, sizeof(base));
    base[0].role = util_strdup("system");
    base[0].content = util_strdup("Be brief.");
    base[1].role = util_strdup("user");
    base[1].content = util_strdup("What time is it?");
    base[2].role = util_strdup("assistant");
    base[2].content = util_strdup("");
    base[2].tool_call_count = 1;
    base[2].tool_calls = calloc(1, sizeof(tool_call));
    base[2].tool_calls[0].id = util_strdup("toolu_1");
    base[2].tool_calls[0].name = util_strdup("t");
    base[2].tool_calls[0].arguments = util_strdup("{}");
    base[3].role = util_strdup("tool");
    base[3].content = util_strdup("12:00");
    base[3].tool_call_id = util_strdup("toolu_1");

    char *a = llm_build_request_body_anthropic(base, 4, NULL, 0, &cfg);
    char *b = llm_build_request_body_anthropic(base, 4, NULL, 0, &cfg);
    ASSERT(a != NULL && b != NULL, "bodies serialized");
    if (a != NULL && b != NULL) {
        ASSERT(strcmp(a, b) == 0, "byte-identical across calls");
    }
    free(b);

    /* Grown history: same four messages plus one more user turn. Because
     * the new user turn merges into the trailing tool-result user message
     * (roles must alternate), the shared prefix ends at the last tool_result
     * block: both bodies must agree byte-for-byte up to that anchor. */
    json_message grown[5];
    memcpy(grown, base, sizeof(base));
    grown[4].role = util_strdup("user");
    grown[4].content = util_strdup("And tomorrow?");
    char *g = llm_build_request_body_anthropic(grown, 5, NULL, 0, &cfg);
    ASSERT(a != NULL && g != NULL, "grown body serialized");
    if (a != NULL && g != NULL) {
        const char *anchor = "\"content\":\"12:00\"}";
        const char *a_pos = strstr(a, anchor);
        const char *g_pos = strstr(g, anchor);
        ASSERT(a_pos != NULL && g_pos != NULL, "anchor present in both bodies");
        if (a_pos != NULL && g_pos != NULL) {
            size_t prefix_len = (size_t)(a_pos - a) + strlen(anchor);
            ASSERT(strncmp(g, a, prefix_len) == 0, "history prefix byte-identical in grown body");
        }
    }
    free(a);
    free(g);

    free(cfg.model);
    free(grown[4].role);
    free(grown[4].content);
    for (int i = 0; i < 4; i++) {
        free(base[i].role);
        free(base[i].content);
        free(base[i].tool_call_id);
    }
    free(base[2].tool_calls[0].id);
    free(base[2].tool_calls[0].name);
    free(base[2].tool_calls[0].arguments);
    free(base[2].tool_calls);
}

void test_anthropic_parse_text_usage(void) {
    TEST("anthropic parse_response reads text and maps usage with cache tokens");
    const char *body = "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
                       "\"model\":\"claude-mock\","
                       "\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
                       "\"stop_reason\":\"end_turn\","
                       "\"usage\":{\"input_tokens\":100,\"output_tokens\":5,"
                       "\"cache_read_input_tokens\":80,\"cache_creation_input_tokens\":20}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response_anthropic(body, &content, &reasoning, &model, &calls, &call_count,
                                          &usage);
    ASSERT(rc == EXIT_SUCCESS, "parses successfully");
    ASSERT(content != NULL && strcmp(content, "hi") == 0, "text block extracted");
    ASSERT(reasoning != NULL && strcmp(reasoning, "") == 0, "no reasoning");
    ASSERT(model != NULL && strcmp(model, "claude-mock") == 0, "model extracted");
    /* prompt side = input_tokens + cache_read + cache_creation */
    ASSERT(usage.prompt_tokens == 200 && usage.completion_tokens == 5 && usage.total_tokens == 205,
           "usage folded with cache tokens");
    ASSERT(usage.cached_tokens == 80, "cache_read_input_tokens -> cached_tokens");
    ASSERT(usage.cache_creation_tokens == 20, "cache_creation_input_tokens extracted");
    ASSERT(call_count == 0 && calls == NULL, "no tool calls");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}

void test_anthropic_parse_tool_thinking(void) {
    TEST("anthropic parse_response reads thinking and tool_use blocks");
    const char *body =
        "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\","
        "\"model\":\"claude-mock\","
        "\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"Let me check.\",\"signature\":\"sig==\"},"
        "{\"type\":\"text\",\"text\":\"Answer: 4\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"calc\",\"input\":{\"x\":2}}"
        "],"
        "\"stop_reason\":\"tool_use\","
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":7}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response_anthropic(body, &content, &reasoning, &model, &calls, &call_count,
                                          &usage);
    ASSERT(rc == EXIT_SUCCESS, "parses successfully");
    ASSERT(content != NULL && strcmp(content, "Answer: 4") == 0, "text block extracted");
    ASSERT(reasoning != NULL && strcmp(reasoning, "Let me check.") == 0,
           "thinking block becomes reasoning");
    ASSERT(call_count == 1 && calls != NULL, "tool_use extracted");
    if (calls != NULL && call_count == 1) {
        ASSERT(calls[0].id != NULL && strcmp(calls[0].id, "toolu_9") == 0 &&
                   calls[0].name != NULL && strcmp(calls[0].name, "calc") == 0,
               "tool_use id/name preserved");
        ASSERT(calls[0].arguments != NULL && strcmp(calls[0].arguments, "{\"x\":2}") == 0,
               "input object serialized as arguments string");
    }
    ASSERT(usage.prompt_tokens == 10 && usage.completion_tokens == 7 && usage.total_tokens == 17,
           "plain usage summed");
    ASSERT(usage.cached_tokens == 0 && usage.cache_creation_tokens == 0,
           "cache fields zero when absent");

    free(content);
    free(reasoning);
    free(model);
    free_calls(calls, call_count);
}

void test_anthropic_parse_error(void) {
    TEST("anthropic parse_response reports API errors");
    const char *body = "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                       "\"message\":\"max_tokens: field required\"}}";

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));

    int rc = llm_parse_response_anthropic(body, &content, &reasoning, &model, &calls, &call_count,
                                          &usage);
    ASSERT(rc == EXIT_LLM_ERR, "returns LLM error");
    ASSERT(content == NULL && model == NULL, "outputs untouched on error");

    /* Malformed: missing content array. */
    rc = llm_parse_response_anthropic("{\"id\":\"x\"}", &content, &reasoning, &model, &calls,
                                      &call_count, &usage);
    ASSERT(rc == EXIT_LLM_ERR, "missing content array is an error");

    free(content);
    free(reasoning);
    free(model);
    free(calls);
}
