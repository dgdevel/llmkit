#include "compact.h"
#include "conversation.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static void test_estimate_tokens(void);
static void test_disabled_noop(void);
static void test_small_noop(void);
static void test_below_threshold_noop(void);
static void test_builds_projection(void);
static void test_sidecar_reuse(void);
static void test_appended_tail_reuse(void);
static void test_tamper_fail_closed(void);
static void test_model_change_fail_closed(void);
static void test_no_system_prompt(void);
static void test_tail_tool_boundary(void);

int main(void) {
    printf("=== test_compact ===\n");

    test_estimate_tokens();
    test_disabled_noop();
    test_small_noop();
    test_below_threshold_noop();
    test_builds_projection();
    test_sidecar_reuse();
    test_appended_tail_reuse();
    test_tamper_fail_closed();
    test_model_change_fail_closed();
    test_no_system_prompt();
    test_tail_tool_boundary();

    printf("\n%d tests, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Canonical conversation: msgs[0] = system, then alternating user/assistant
 * with long-ish content so the bytes/4 estimate exceeds a tiny budget. */
static json_message *mk_msgs(int n) {
    json_message *msgs = calloc((size_t)n, sizeof(json_message));
    if (msgs == NULL) return NULL;
    msgs[0].role = util_strdup("system");
    msgs[0].content = util_strdup("You are a helpful assistant.");
    for (int i = 1; i < n; i++) {
        msgs[i].role = util_strdup((i % 2) ? "user" : "assistant");
        char buf[128];
        snprintf(buf, sizeof(buf), "This is message number %d with some filler text.", i);
        msgs[i].content = util_strdup(buf);
    }
    return msgs;
}

static runtime_ctx mk_ctx(const char *convo_path, const char *model) {
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.agent.compact_enabled = 1;
    ctx.agent.compact_max_tokens = 10;
    ctx.agent.compact_threshold = 0.8;
    ctx.agent.compact_summarize = 0;
    ctx.llm.model = util_strdup(model);
    ctx.convo_path = util_strdup(convo_path);
    return ctx;
}

static void free_ctx(runtime_ctx *ctx) {
    free(ctx->llm.model);
    free(ctx->convo_path);
}

static char *sidecar_of(const char *convo_path) {
    size_t len = strlen(convo_path);
    char *p = malloc(len + sizeof(".context.json") + 1);
    if (p == NULL) return NULL;
    memcpy(p, convo_path, len);
    memcpy(p + len, ".context.json", sizeof(".context.json"));
    return p;
}

/* Remove the sidecar for a conversation, freeing the path. */
static void rm_sidecar(const char *convo_path) {
    char *sp = sidecar_of(convo_path);
    if (sp != NULL) {
        unlink(sp);
        free(sp);
    }
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

void test_estimate_tokens(void) {
    TEST("estimate_tokens uses bytes/4 heuristic (min 1)");
    ASSERT(compact_estimate_tokens(NULL, 0) == 1, "empty estimate is 1");
    json_message m;
    memset(&m, 0, sizeof(m));
    m.role = util_strdup("user");
    m.content = util_strdup("abcd"); /* 8 bytes total */
    ASSERT(compact_estimate_tokens(&m, 1) == 3, "8 bytes -> 8/4+1 = 3");
    free(m.role);
    free(m.content);
}

void test_disabled_noop(void) {
    TEST("compact disabled leaves the array untouched");
    json_message *msgs = mk_msgs(8);
    runtime_ctx ctx = mk_ctx("/tmp/llmkit_tc_disabled.jsonl", "m");
    ctx.agent.compact_enabled = 0;
    int count = 8, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(count == 8 && covered == 0, "unchanged");
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
}

void test_small_noop(void) {
    TEST("conversations under the minimum are not compacted");
    json_message *msgs = mk_msgs(5);
    runtime_ctx ctx = mk_ctx("/tmp/llmkit_tc_small.jsonl", "m");
    int count = 5, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(count == 5 && covered == 0, "unchanged");
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
}

void test_below_threshold_noop(void) {
    TEST("estimate below budget leaves the array untouched");
    json_message *msgs = mk_msgs(8);
    runtime_ctx ctx = mk_ctx("/tmp/llmkit_tc_below.jsonl", "m");
    ctx.agent.compact_max_tokens = 100000;
    int count = 8, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(count == 8 && covered == 0, "unchanged");
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
}

void test_builds_projection(void) {
    TEST("compaction builds projection (system + first user + summary + tail)");
    char convo[] = "/tmp/llmkit_tc_build.jsonl";
    rm_sidecar(convo);
    json_message *msgs = mk_msgs(8);
    runtime_ctx ctx = mk_ctx(convo, "deepseek-chat");
    int count = 8, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(covered == 6, "covered = 8 - 2 tail");
    ASSERT(count == 5, "merged = projection(3) + tail(2)");
    if (count == 5) {
        ASSERT(strcmp(msgs[0].content, "You are a helpful assistant.") == 0,
               "system pinned verbatim");
        ASSERT(strcmp(msgs[1].content, "This is message number 1 with some filler text.") == 0,
               "first user message pinned verbatim");
        ASSERT(strcmp(msgs[2].role, "user") == 0, "summary is a user message");
        ASSERT(strstr(msgs[2].content, "[Earlier conversation compacted") != NULL,
               "placeholder summary used");
        ASSERT(strcmp(msgs[3].content, "This is message number 6 with some filler text.") == 0,
               "tail[0] verbatim");
        ASSERT(strcmp(msgs[4].content, "This is message number 7 with some filler text.") == 0,
               "tail[1] verbatim");
    }
    char *sp = sidecar_of(convo);
    ASSERT(access(sp, F_OK) == 0, "sidecar written");
    free(sp);
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_sidecar_reuse(void) {
    TEST("second call reuses the persisted projection");
    char convo[] = "/tmp/llmkit_tc_reuse.jsonl";
    rm_sidecar(convo);
    runtime_ctx ctx = mk_ctx(convo, "deepseek-chat");

    json_message *msgs1 = mk_msgs(8);
    int count1 = 8, covered1 = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs1, &count1, &covered1);
    ASSERT(rc == EXIT_SUCCESS && covered1 == 6, "first call compacts");
    conversation_free_messages(msgs1, count1);

    /* Identical canonical history: must reuse the sidecar projection. */
    json_message *msgs2 = mk_msgs(8);
    int count2 = 8, covered2 = -1;
    rc = compact_apply(&ctx, ctx.convo_path, &msgs2, &count2, &covered2);
    ASSERT(rc == EXIT_SUCCESS && covered2 == 6, "second call reuses");
    ASSERT(count2 == 5, "same merged shape");
    if (count2 == 5) {
        ASSERT(strcmp(msgs2[2].role, "user") == 0, "summary role stable");
        ASSERT(strstr(msgs2[2].content, "[Earlier conversation compacted") != NULL,
               "summary from sidecar");
        ASSERT(strcmp(msgs2[3].content, "This is message number 6 with some filler text.") == 0,
               "tail preserved");
    }
    conversation_free_messages(msgs2, count2);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_appended_tail_reuse(void) {
    TEST("growing history reuses the projection and appends only");
    char convo[] = "/tmp/llmkit_tc_grow.jsonl";
    rm_sidecar(convo);
    runtime_ctx ctx = mk_ctx(convo, "deepseek-chat");

    json_message *msgs1 = mk_msgs(8);
    int count1 = 8, covered1 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs1, &count1, &covered1) == EXIT_SUCCESS &&
               covered1 == 6,
           "first call compacts");
    conversation_free_messages(msgs1, count1);

    /* 10 messages: same 8 plus two appended turns. */
    json_message *msgs2 = mk_msgs(10);
    int count2 = 10, covered2 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs2, &count2, &covered2) == EXIT_SUCCESS &&
               covered2 == 6,
           "covered stays fixed at 6");
    ASSERT(count2 == 7, "merged = projection(3) + tail(4)");
    if (count2 == 7) {
        ASSERT(strcmp(msgs2[6].content, "This is message number 9 with some filler text.") == 0,
               "newest message appended");
    }
    conversation_free_messages(msgs2, count2);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_tamper_fail_closed(void) {
    TEST("tampered sidecar hash is rejected and the sidecar rebuilt");
    char convo[] = "/tmp/llmkit_tc_tamper.jsonl";
    rm_sidecar(convo);
    runtime_ctx ctx = mk_ctx(convo, "deepseek-chat");

    json_message *msgs1 = mk_msgs(8);
    int count1 = 8, covered1 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs1, &count1, &covered1) == EXIT_SUCCESS,
           "first call compacts");
    conversation_free_messages(msgs1, count1);

    /* Corrupt the stored covered_prefix_hash. */
    char *sp = sidecar_of(convo);
    char *raw = util_read_file(sp);
    ASSERT(raw != NULL, "sidecar readable");
    char *hash_pos = strstr(raw, "covered_prefix_hash");
    char *good = NULL;
    if (hash_pos != NULL) {
        char *q1 = strchr(hash_pos, '"');
        char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 != NULL && q2 != NULL && q2 > q1) {
            good = strndup(q1 + 1, (size_t)(q2 - q1 - 1));
            memcpy(q1 + 1, "sha256:deadbeefdeadbeefdeadbeefdeadbeef", 40);
        }
    }
    FILE *fp = fopen(sp, "wb");
    if (fp) {
        fwrite(raw, 1, strlen(raw), fp);
        fclose(fp);
    }
    free(raw);

    json_message *msgs2 = mk_msgs(8);
    int count2 = 8, covered2 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs2, &count2, &covered2) == EXIT_SUCCESS &&
               covered2 == 6,
           "rebuilds after tamper");
    conversation_free_messages(msgs2, count2);

    /* The sidecar must now carry the correct hash again. */
    raw = util_read_file(sp);
    ASSERT(raw != NULL && strstr(raw, good ? good : "sha256:") != NULL &&
               strstr(raw, "deadbeef") == NULL,
           "sidecar hash corrected");
    free(raw);
    free(good);
    free(sp);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_model_change_fail_closed(void) {
    TEST("model change invalidates the projection (prompt_cache_key mismatch)");
    char convo[] = "/tmp/llmkit_tc_model.jsonl";
    rm_sidecar(convo);
    runtime_ctx ctx = mk_ctx(convo, "deepseek-chat");

    json_message *msgs1 = mk_msgs(8);
    int count1 = 8, covered1 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs1, &count1, &covered1) == EXIT_SUCCESS,
           "first call compacts");
    conversation_free_messages(msgs1, count1);

    free(ctx.llm.model);
    ctx.llm.model = util_strdup("deepseek-reasoner");

    json_message *msgs2 = mk_msgs(8);
    int count2 = 8, covered2 = -1;
    ASSERT(compact_apply(&ctx, ctx.convo_path, &msgs2, &count2, &covered2) == EXIT_SUCCESS &&
               covered2 == 6,
           "rebuilds under a different model");
    ASSERT(count2 == 5, "merged shape correct");
    conversation_free_messages(msgs2, count2);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_tail_tool_boundary(void) {
    TEST("tail boundary never splits a tool_call from its tool result");
    char convo[] = "/tmp/llmkit_tc_toolbound.jsonl";
    rm_sidecar(convo);
    /* [0]=system [1]=u1 [2]=a1(tool_calls) [3]=tool1 [4]=u2 [5]=a2(tool_calls)
     * [6]=tool2 [7]=u3 -- the naive tail start (index 6) is a tool result. */
    json_message *msgs = mk_msgs(8);
    for (int i = 0; i < 8; i++) {
        free(msgs[i].role);
        free(msgs[i].content);
        char role[16], content[256];
        snprintf(role, sizeof(role), "user");
        if (i == 0) snprintf(role, sizeof(role), "system");
        if (i == 2 || i == 5) snprintf(role, sizeof(role), "assistant");
        if (i == 3 || i == 6) snprintf(role, sizeof(role), "tool");
        snprintf(content, sizeof(content),
                 "Message %d with enough filler text to exceed the budget. ", i);
        msgs[i].role = util_strdup(role);
        msgs[i].content = util_strdup(content);
    }
    msgs[2].tool_call_count = 1;
    msgs[2].tool_calls = calloc(1, sizeof(tool_call));
    msgs[2].tool_calls[0].id = util_strdup("c1");
    msgs[2].tool_calls[0].name = util_strdup("ns.get_time");
    msgs[2].tool_calls[0].arguments = util_strdup("{}");
    msgs[3].tool_call_id = util_strdup("c1");
    msgs[5].tool_call_count = 1;
    msgs[5].tool_calls = calloc(1, sizeof(tool_call));
    msgs[5].tool_calls[0].id = util_strdup("c2");
    msgs[5].tool_calls[0].name = util_strdup("ns.get_time");
    msgs[5].tool_calls[0].arguments = util_strdup("{}");
    msgs[6].tool_call_id = util_strdup("c2");

    runtime_ctx ctx = mk_ctx(convo, "m");
    int count = 8, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(covered == 7, "tool result folded into covered span (covered=7)");
    ASSERT(count == 4, "merged = projection(3) + tail(1)");
    if (count == 4) {
        ASSERT(strcmp(msgs[3].role, "user") == 0, "first tail message is not a tool result");
        ASSERT(strstr(msgs[3].content, "Message 7") != NULL, "tail keeps the last user turn");
    }
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
    rm_sidecar(convo);
}

void test_no_system_prompt(void) {
    TEST("compaction works without a system prompt (pin = first user)");
    char convo[] = "/tmp/llmkit_tc_nosys.jsonl";
    rm_sidecar(convo);
    json_message *msgs = mk_msgs(8);
    free(msgs[0].role);
    free(msgs[0].content);
    msgs[0].role = util_strdup("user");
    msgs[0].content = util_strdup("Original first prompt.");
    runtime_ctx ctx = mk_ctx(convo, "m");
    int count = 8, covered = -1;
    int rc = compact_apply(&ctx, ctx.convo_path, &msgs, &count, &covered);
    ASSERT(rc == EXIT_SUCCESS, "rc success");
    ASSERT(covered == 6, "covered = 8 - 2");
    ASSERT(count == 4, "merged = projection(2) + tail(2)");
    if (count == 4) {
        ASSERT(strcmp(msgs[0].content, "Original first prompt.") == 0, "first user pinned");
        ASSERT(strcmp(msgs[1].role, "user") == 0, "summary follows");
        ASSERT(strcmp(msgs[2].content, "This is message number 6 with some filler text.") == 0,
               "tail[0] verbatim");
    }
    conversation_free_messages(msgs, count);
    free_ctx(&ctx);
    rm_sidecar(convo);
}
