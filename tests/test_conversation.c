#include "conversation.h"
#include "util.h"
#include <assert.h>
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

/* ---- helpers ---- */

static FILE *tmp_file(char **out_path) {
    char tmpl[] = "/tmp/llmkit_test_convo_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        *out_path = NULL;
        return NULL;
    }
    *out_path = util_strdup(tmpl);
    return fdopen(fd, "a");
}

/* ---- declarations ---- */
static void test_open_new(void);
static void test_open_utf8_invalid(void);
static void test_write_meta(void);
static void test_write_user(void);
static void test_write_assistant(void);
static void test_write_assistant_with_reasoning(void);
static void test_write_assistant_empty_reasoning_omitted(void);
static void test_reconstruct_reasoning(void);
static void test_write_tool_call(void);
static void test_write_tool_result(void);
static void test_write_error(void);
static void test_reconstruct_empty(void);
static void test_reconstruct_simple(void);
static void test_reconstruct_with_tools(void);
static void test_reconstruct_roundtrip(void);
static void test_free_null(void);
static void test_read_last_assistant_empty(void);
static void test_read_last_assistant_simple(void);
static void test_read_last_assistant_multiple(void);
static void test_read_last_assistant_no_file(void);
static void test_read_last_assistant_no_assistant(void);
static void test_write_scoped_fields(void);
static void test_write_subagent_brackets(void);
static void test_reconstruct_scope_filtering(void);
static void test_reconstruct_v1_compat(void);
static void test_read_last_assistant_skips_scoped(void);

int main(void) {
    printf("=== test_conversation ===\n");

    test_open_new();
    test_open_utf8_invalid();
    test_write_meta();
    test_write_user();
    test_write_assistant();
    test_write_assistant_with_reasoning();
    test_write_assistant_empty_reasoning_omitted();
    test_reconstruct_reasoning();
    test_write_tool_call();
    test_write_tool_result();
    test_write_error();
    test_reconstruct_empty();
    test_reconstruct_simple();
    test_reconstruct_with_tools();
    test_reconstruct_roundtrip();
    test_free_null();
    test_read_last_assistant_empty();
    test_read_last_assistant_simple();
    test_read_last_assistant_multiple();
    test_read_last_assistant_no_file();
    test_read_last_assistant_no_assistant();
    test_write_scoped_fields();
    test_write_subagent_brackets();
    test_reconstruct_scope_filtering();
    test_reconstruct_v1_compat();
    test_read_last_assistant_skips_scoped();

    printf("\n%d tests, %d failed\n", tests, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

void test_open_new(void) {
    TEST("open creates new file");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file created");
    if (fp) fclose(fp);

    FILE *fp2 = NULL;
    int rc = conversation_open(path, &fp2);
    ASSERT(rc == EXIT_SUCCESS, "open returns success");
    ASSERT(fp2 != NULL, "out_fp set");
    if (fp2) fclose(fp2);

    FILE *f = path ? fopen(path, "r") : NULL;
    ASSERT(f != NULL, "file exists");
    if (f) fclose(f);

    remove(path);
    free(path);
}

void test_open_utf8_invalid(void) {
    TEST("open rejects invalid UTF-8");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    fwrite("\xff\xfe\x00\x01", 4, 1, fp);
    if (fp) fclose(fp);

    FILE *fp2 = (FILE *)0x1;
    int rc = conversation_open(path, &fp2);
    ASSERT(rc == EXIT_FILE_ERR, "rejects invalid UTF-8");
    ASSERT(fp2 == NULL, "out_fp is NULL on error");

    remove(path);
    free(path);
}

void test_write_meta(void) {
    TEST("write_meta produces correct JSON");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_meta(fp, "sha256:abc123", "uuid-1234");
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    ASSERT(content != NULL, "file readable");
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    cJSON *t = cJSON_GetObjectItem(j, "type");
    ASSERT(t && cJSON_IsString(t) && strcmp(t->valuestring, "meta") == 0, "type=meta");
    cJSON *v = cJSON_GetObjectItem(j, "version");
    ASSERT(v && cJSON_IsNumber(v) && v->valueint == 2, "version=2");
    cJSON *ch = cJSON_GetObjectItem(j, "config_hash");
    ASSERT(ch && cJSON_IsString(ch) && strcmp(ch->valuestring, "sha256:abc123") == 0,
           "config_hash");
    cJSON *ri = cJSON_GetObjectItem(j, "run_id");
    ASSERT(ri && cJSON_IsString(ri) && strcmp(ri->valuestring, "uuid-1234") == 0, "run_id");
    cJSON *ts = cJSON_GetObjectItem(j, "timestamp");
    ASSERT(ts && cJSON_IsString(ts), "timestamp present");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_user(void) {
    TEST("write_entry(USER) produces correct JSON");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_entry(fp, ENTRY_USER, "Hello world", "cli");
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "user") == 0, "type=user");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "content")->valuestring, "Hello world") == 0, "content");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "source")->valuestring, "cli") == 0, "source=cli");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_assistant(void) {
    TEST("write_entry(ASSISTANT) with usage");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    usage_info usage = {.prompt_tokens = 10, .completion_tokens = 5, .total_tokens = 15};
    int rc = conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello back", "", "gpt-4o", &usage);
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "assistant") == 0, "type=assistant");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "content")->valuestring, "Hello back") == 0, "content");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "gpt-4o") == 0, "model");
    cJSON *u = cJSON_GetObjectItem(j, "usage");
    ASSERT(u != NULL, "usage present");
    ASSERT(cJSON_GetObjectItem(u, "prompt_tokens")->valueint == 10, "prompt_tokens");
    ASSERT(cJSON_GetObjectItem(u, "completion_tokens")->valueint == 5, "completion_tokens");
    ASSERT(cJSON_GetObjectItem(u, "total_tokens")->valueint == 15, "total_tokens");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_assistant_with_reasoning(void) {
    TEST("write_entry(ASSISTANT) with reasoning field");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    usage_info usage = {.prompt_tokens = 10, .completion_tokens = 5, .total_tokens = 15};
    int rc = conversation_write_entry(fp, ENTRY_ASSISTANT, "The answer is 42",
                                      "Let me think... 6*7=42.", "gpt-4o", &usage);
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "assistant") == 0, "type=assistant");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "content")->valuestring, "The answer is 42") == 0,
           "content");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "reasoning")->valuestring, "Let me think... 6*7=42.") == 0,
           "reasoning field present and correct");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "gpt-4o") == 0, "model");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_assistant_empty_reasoning_omitted(void) {
    TEST("write_entry(ASSISTANT) with empty reasoning omits the field");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello", "", "gpt-4o",
                                      (const usage_info *)NULL);
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(cJSON_GetObjectItem(j, "reasoning") == NULL, "reasoning field absent when empty");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_reconstruct_reasoning(void) {
    TEST("reconstruct loads reasoning into json_message");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_meta(fp, "h", "r");
    conversation_write_entry(fp, ENTRY_USER, "Hi", "cli");
    usage_info u = {.prompt_tokens = 5, .completion_tokens = 10, .total_tokens = 15};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello!", "Thinking about it...", "gpt-4o", &u);
    if (fp) fclose(fp);

    json_message *msgs = NULL;
    int count = 0;
    int rc = conversation_reconstruct(path, &msgs, &count);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct success");
    ASSERT(count == 2, "2 messages");
    ASSERT(strcmp(msgs[1].role, "assistant") == 0, "msg[1] role=assistant");
    ASSERT(strcmp(msgs[1].content, "Hello!") == 0, "msg[1] content");
    ASSERT(msgs[1].reasoning != NULL, "msg[1] reasoning non-NULL");
    ASSERT(strcmp(msgs[1].reasoning, "Thinking about it...") == 0, "msg[1] reasoning loaded");
    conversation_free_messages(msgs, count);
    remove(path);
    free(path);
}

void test_write_tool_call(void) {
    TEST("write_entry(TOOL_CALL) produces correct JSON");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_entry(fp, ENTRY_TOOL_CALL, "call_1", "get_weather",
                                      "{\"loc\":\"Paris\"}", "weather-srv");
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "tool_call") == 0, "type=tool_call");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "id")->valuestring, "call_1") == 0, "id");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "name")->valuestring, "get_weather") == 0, "name");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "arguments")->valuestring, "{\"loc\":\"Paris\"}") == 0,
           "args");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_tool_result(void) {
    TEST("write_entry(TOOL_RESULT) produces correct JSON");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_entry(fp, ENTRY_TOOL_RESULT, "call_1", "get_weather",
                                      "{\"temp\":22}", 0, 0, "weather-srv");
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "tool_result") == 0, "type");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "call_id")->valuestring, "call_1") == 0, "call_id");
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(j, "is_error")) == 0, "is_error=false");
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(j, "is_timeout")) == 0, "is_timeout=false");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_write_error(void) {
    TEST("write_entry(ERROR) produces correct JSON");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    ASSERT(fp != NULL, "reopen");
    int rc = conversation_write_entry(fp, ENTRY_ERROR, 5, "Tool failed", 0);
    ASSERT(rc == EXIT_SUCCESS, "write success");
    if (fp) fclose(fp);

    char *content = util_read_file(path);
    cJSON *j = cJSON_Parse(content);
    ASSERT(j != NULL, "valid JSON");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "type")->valuestring, "error") == 0, "type=error");
    ASSERT(cJSON_GetObjectItem(j, "code")->valueint == 5, "code=5");
    ASSERT(strcmp(cJSON_GetObjectItem(j, "message")->valuestring, "Tool failed") == 0, "message");
    cJSON_Delete(j);
    free(content);
    remove(path);
    free(path);
}

void test_reconstruct_empty(void) {
    TEST("reconstruct from non-existent file returns empty");
    json_message *msgs = (json_message *)0x1;
    int count = -1;
    int rc = conversation_reconstruct("/tmp/nonexistent_xyz123", &msgs, &count);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    ASSERT(msgs == NULL, "out_msgs is NULL");
    ASSERT(count == 0, "count is 0");
}

void test_reconstruct_simple(void) {
    TEST("reconstruct simple user+assistant");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_meta(fp, "h", "r");
    conversation_write_entry(fp, ENTRY_USER, "Hi", "cli");
    usage_info u = {.prompt_tokens = 5, .completion_tokens = 10, .total_tokens = 15};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello!", "", "gpt-4o", &u);
    if (fp) fclose(fp);

    json_message *msgs = NULL;
    int count = 0;
    int rc = conversation_reconstruct(path, &msgs, &count);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct success");
    ASSERT(count == 2, "2 messages");
    ASSERT(strcmp(msgs[0].role, "user") == 0, "msg[0] role=user");
    ASSERT(strcmp(msgs[0].content, "Hi") == 0, "msg[0] content=Hi");
    ASSERT(strcmp(msgs[1].role, "assistant") == 0, "msg[1] role=assistant");
    ASSERT(strcmp(msgs[1].content, "Hello!") == 0, "msg[1] content");
    conversation_free_messages(msgs, count);
    remove(path);
    free(path);
}

void test_reconstruct_with_tools(void) {
    TEST("reconstruct with tool_calls and tool_results");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_entry(fp, ENTRY_USER, "Weather?", "cli");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "", "gpt-4o", (const usage_info *)NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "call_1", "get_weather", "{\"loc\":\"Paris\"}",
                             "wsrv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "call_1", "get_weather", "{\"temp\":22}", 0, 0,
                             "wsrv");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "It's 22C.", "", "gpt-4o",
                             (const usage_info *)NULL);
    if (fp) fclose(fp);

    json_message *msgs = NULL;
    int count = 0;
    int rc = conversation_reconstruct(path, &msgs, &count);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct success");
    ASSERT(count == 4, "4 messages");
    ASSERT(strcmp(msgs[0].role, "user") == 0, "msg[0] user");
    ASSERT(msgs[1].tool_call_count == 1, "msg[1] has 1 tool_call");
    ASSERT(strcmp(msgs[1].tool_calls[0].id, "call_1") == 0, "tool_call id");
    ASSERT(strcmp(msgs[2].role, "tool") == 0, "msg[2] tool");
    ASSERT(strcmp(msgs[2].content, "{\"temp\":22}") == 0, "tool result content");
    ASSERT(strcmp(msgs[3].role, "assistant") == 0, "msg[3] assistant final");
    conversation_free_messages(msgs, count);
    remove(path);
    free(path);
}

void test_reconstruct_roundtrip(void) {
    TEST("full round-trip: write entries -> reconstruct -> verify order");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_meta(fp, "h", "r");
    conversation_write_entry(fp, ENTRY_USER, "Q1", "cli");
    usage_info u1 = {.prompt_tokens = 1, .completion_tokens = 1, .total_tokens = 2};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "A1", "", "m1", &u1);
    conversation_write_entry(fp, ENTRY_USER, "Q2", "cli");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "", "m2", (const usage_info *)NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c1", "tool1", "{}", "srv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c1", "tool1", "ok", 0, 0, "srv");
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c2", "tool2", "{}", "srv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c2", "tool2", "ok2", 0, 0, "srv");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "A3", "", "m2", &u1);
    if (fp) fclose(fp);

    json_message *msgs = NULL;
    int count = 0;
    int rc = conversation_reconstruct(path, &msgs, &count);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct");
    ASSERT(count == 7, "7 messages");

    const char *roles[] = {"user", "assistant", "user", "assistant", "tool", "tool", "assistant"};
    for (int i = 0; i < count && i < 7; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "msg[%d] role=%s", i, roles[i]);
        ASSERT(strcmp(msgs[i].role, roles[i]) == 0, buf);
    }

    ASSERT(msgs[3].tool_call_count == 2, "second assistant has 2 tool_calls");
    ASSERT(strcmp(msgs[3].tool_calls[0].id, "c1") == 0, "tc[0].id");
    ASSERT(strcmp(msgs[3].tool_calls[1].id, "c2") == 0, "tc[1].id");

    conversation_free_messages(msgs, count);
    remove(path);
    free(path);
}

void test_free_null(void) {
    TEST("free_messages handles NULL");
    conversation_free_messages(NULL, 0);
    ASSERT(1, "no crash");
}

/* ------------------------------------------------------------------ */
/*  conversation_read_last_assistant tests                              */
/* ------------------------------------------------------------------ */

void test_read_last_assistant_empty(void) {
    TEST("read_last_assistant from empty file returns empty string");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    char *content = NULL;
    int rc = conversation_read_last_assistant(path, &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "") == 0, "content is empty");
    }
    free(content);
    remove(path);
    free(path);
}

void test_read_last_assistant_simple(void) {
    TEST("read_last_assistant returns last assistant content");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_meta(fp, "h", "r");
    conversation_write_entry(fp, ENTRY_USER, "Hi", "cli");
    usage_info u = {.prompt_tokens = 5, .completion_tokens = 10, .total_tokens = 15};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello there!", "", "gpt-4o", &u);
    if (fp) fclose(fp);

    char *content = NULL;
    int rc = conversation_read_last_assistant(path, &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "Hello there!") == 0, "content matches last assistant");
    }
    free(content);
    remove(path);
    free(path);
}

void test_read_last_assistant_multiple(void) {
    TEST("read_last_assistant with multiple assistant entries returns last");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_entry(fp, ENTRY_USER, "Q1", "cli");
    usage_info u = {.prompt_tokens = 5, .completion_tokens = 10, .total_tokens = 15};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Answer 1", "", "gpt-4o", &u);
    conversation_write_entry(fp, ENTRY_USER, "Q2", "cli");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "", "gpt-4o", (const usage_info *)NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c1", "t1", "{}", "srv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c1", "t1", "ok", 0, 0, "srv");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Final answer", "", "gpt-4o", &u);
    if (fp) fclose(fp);

    char *content = NULL;
    int rc = conversation_read_last_assistant(path, &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "Final answer") == 0, "last assistant content");
    }
    free(content);
    remove(path);
    free(path);
}

void test_read_last_assistant_no_file(void) {
    TEST("read_last_assistant from non-existent file returns empty string");
    char *content = NULL;
    int rc = conversation_read_last_assistant("/tmp/nonexistent_xyz789", &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "") == 0, "content is empty");
    }
    free(content);
}

void test_read_last_assistant_no_assistant(void) {
    TEST("read_last_assistant with no assistant entries returns empty");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    if (fp) fclose(fp);

    fp = path ? fopen(path, "a") : NULL;
    conversation_write_meta(fp, "h", "r");
    conversation_write_entry(fp, ENTRY_USER, "Hi", "cli");
    if (fp) fclose(fp);

    char *content = NULL;
    int rc = conversation_read_last_assistant(path, &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "") == 0, "content is empty");
    }
    free(content);
    remove(path);
    free(path);
}

/* ------------------------------------------------------------------ */
/*  Scoped (subagent) entries                                          */
/* ------------------------------------------------------------------ */

/* Read the raw JSONL lines as parsed objects (NULL-terminated array of
 * cJSON*; caller deletes each and frees the array). */
static cJSON **read_lines(const char *path, int *out_n) {
    *out_n = 0;
    char *content = util_read_file(path);
    if (content == NULL) return NULL;
    cJSON **arr = calloc(64, sizeof(cJSON *));
    if (arr == NULL) {
        free(content);
        return NULL;
    }
    char *line = content;
    while (line != NULL && *line != '\0' && *out_n < 64) {
        char *next = strchr(line, '\n');
        if (next != NULL) *next = '\0';
        cJSON *j = cJSON_Parse(line);
        if (j != NULL) arr[(*out_n)++] = j;
        line = next ? next + 1 : NULL;
    }
    free(content);
    return arr;
}

static void free_lines(cJSON **arr, int n) {
    if (arr == NULL) return;
    for (int i = 0; i < n; i++) cJSON_Delete(arr[i]);
    free(arr);
}

void test_write_scoped_fields(void) {
    TEST("scoped entry carries depth/subagent/run_id");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");

    conv_scope scope = {2, "inner", "runid-inner"};
    int rc = conversation_write_scoped(fp, &scope, ENTRY_USER, "hello", "subagent");
    ASSERT(rc == EXIT_SUCCESS, "write success");
    rc = conversation_write_scoped(fp, &scope, ENTRY_ASSISTANT, "hi", "", "m", NULL);
    ASSERT(rc == EXIT_SUCCESS, "write assistant success");
    fclose(fp);

    int n = 0;
    cJSON **lines = read_lines(path, &n);
    ASSERT(n == 2, "two lines");
    if (lines != NULL && n == 2) {
        cJSON *d = cJSON_GetObjectItem(lines[0], "depth");
        ASSERT(d && cJSON_IsNumber(d) && d->valueint == 2, "depth=2");
        cJSON *s = cJSON_GetObjectItem(lines[0], "subagent");
        ASSERT(s && cJSON_IsString(s) && strcmp(s->valuestring, "inner") == 0, "subagent name");
        cJSON *r = cJSON_GetObjectItem(lines[0], "run_id");
        ASSERT(r && cJSON_IsString(r) && strcmp(r->valuestring, "runid-inner") == 0, "run_id");
        cJSON *c = cJSON_GetObjectItem(lines[0], "content");
        ASSERT(c && cJSON_IsString(c) && strcmp(c->valuestring, "hello") == 0, "content intact");
        cJSON *r1 = cJSON_GetObjectItem(lines[1], "run_id");
        ASSERT(r1 && cJSON_IsString(r1) && strcmp(r1->valuestring, "runid-inner") == 0,
               "assistant scoped too");
    }
    free_lines(lines, n);
    remove(path);
    free(path);
}

void test_write_subagent_brackets(void) {
    TEST("subagent_start/end brackets carry trace metadata");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");

    conv_scope scope = {1, "calc", "runid-calc"};
    int rc = conversation_write_subagent_start(fp, &scope, "call_9", "{\"a\":1}");
    ASSERT(rc == EXIT_SUCCESS, "start write success");
    rc = conversation_write_subagent_end(fp, &scope, 3, 1);
    ASSERT(rc == EXIT_SUCCESS, "end write success");
    fclose(fp);

    int n = 0;
    cJSON **lines = read_lines(path, &n);
    ASSERT(n == 2, "two lines");
    if (lines != NULL && n == 2) {
        cJSON *t = cJSON_GetObjectItem(lines[0], "type");
        ASSERT(t && strcmp(t->valuestring, "subagent_start") == 0, "start type");
        cJSON *cid = cJSON_GetObjectItem(lines[0], "call_id");
        ASSERT(cid && strcmp(cid->valuestring, "call_9") == 0, "start call_id");
        cJSON *args = cJSON_GetObjectItem(lines[0], "arguments");
        ASSERT(args && strcmp(args->valuestring, "{\"a\":1}") == 0, "start arguments");
        cJSON *d = cJSON_GetObjectItem(lines[1], "depth");
        ASSERT(d && cJSON_IsNumber(d) && d->valueint == 1, "end depth");
        cJSON *tn = cJSON_GetObjectItem(lines[1], "turns");
        ASSERT(tn && cJSON_IsNumber(tn) && tn->valueint == 3, "end turns");
        cJSON *ie = cJSON_GetObjectItem(lines[1], "is_error");
        ASSERT(ie && cJSON_IsBool(ie) && cJSON_IsTrue(ie), "end is_error");
    }
    free_lines(lines, n);
    remove(path);
    free(path);
}

/* Write a small conversation with two subagent scopes (one nested inside
 * the other) mimicking the agent runtime layout. */
static void write_nested_fixture(const char *path) {
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    conv_scope outer = {1, "calc", "runid-outer"};
    conv_scope inner = {2, "inner", "runid-inner"};

    conversation_write_meta(fp, "sha256:h", "runid-top");
    conversation_write_entry(fp, ENTRY_USER, "question", "cli");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "", "m", NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c1", "calc", "{}", "subagent");
    conversation_write_subagent_start(fp, &outer, "c1", "{}");
    conversation_write_scoped(fp, &outer, ENTRY_USER, "outer prompt", "subagent");
    conversation_write_scoped(fp, &outer, ENTRY_ASSISTANT, "", "", "m", NULL);
    conversation_write_scoped(fp, &outer, ENTRY_TOOL_CALL, "c2", "inner", "{}", "subagent");
    conversation_write_subagent_start(fp, &inner, "c2", "{}");
    conversation_write_scoped(fp, &inner, ENTRY_USER, "inner prompt", "subagent");
    conversation_write_scoped(fp, &inner, ENTRY_ASSISTANT, "inner final", "", "m", NULL);
    conversation_write_subagent_end(fp, &inner, 1, 0);
    conversation_write_scoped(fp, &outer, ENTRY_TOOL_RESULT, "c2", "inner", "inner final", 0, 0,
                              "subagent");
    conversation_write_scoped(fp, &outer, ENTRY_ASSISTANT, "outer final", "", "m", NULL);
    conversation_write_subagent_end(fp, &outer, 2, 0);
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c1", "calc", "outer final", 0, 0, "subagent");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "top final", "", "m", NULL);
    fclose(fp);
}

void test_reconstruct_scope_filtering(void) {
    TEST("reconstruct filters by scope (top-level and by run_id)");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    fclose(fp);
    remove(path);
    write_nested_fixture(path);

    /* Top level: user, assistant(tool_call), tool result, final assistant. */
    json_message *msgs = NULL;
    int n = 0;
    int rc = conversation_reconstruct(path, &msgs, &n);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct success");
    if (rc == EXIT_SUCCESS) {
        ASSERT(n == 4, "top-level message count");
        if (n == 4) {
            ASSERT(strcmp(msgs[0].role, "user") == 0, "msg0 user");
            ASSERT(strcmp(msgs[0].content, "question") == 0, "msg0 content");
            ASSERT(strcmp(msgs[1].role, "assistant") == 0, "msg1 assistant");
            ASSERT(msgs[1].tool_call_count == 1, "msg1 has the tool call");
            ASSERT(strcmp(msgs[1].tool_calls[0].name, "calc") == 0, "msg1 tool name");
            ASSERT(strcmp(msgs[2].role, "tool") == 0, "msg2 tool");
            ASSERT(strcmp(msgs[2].content, "outer final") == 0, "msg2 content");
            ASSERT(strcmp(msgs[3].content, "top final") == 0, "msg3 content");
        }
        conversation_free_messages(msgs, n);
    }

    /* Outer subagent scope: its own prompt, assistant + inner tool pair,
     * final. The nested run's entries must not leak in. */
    msgs = NULL;
    n = 0;
    rc = conversation_reconstruct_scope(path, "runid-outer", &msgs, &n);
    ASSERT(rc == EXIT_SUCCESS, "scope reconstruct success");
    if (rc == EXIT_SUCCESS) {
        ASSERT(n == 4, "outer scope message count");
        if (n == 4) {
            ASSERT(strcmp(msgs[0].role, "user") == 0, "outer msg0 user");
            ASSERT(strcmp(msgs[0].content, "outer prompt") == 0, "outer msg0 content");
            ASSERT(strcmp(msgs[1].role, "assistant") == 0, "outer msg1 assistant");
            ASSERT(msgs[1].tool_call_count == 1, "outer msg1 has inner tool call");
            ASSERT(strcmp(msgs[1].tool_calls[0].name, "inner") == 0, "outer msg1 tool name");
            ASSERT(strcmp(msgs[2].role, "tool") == 0, "outer msg2 tool");
            ASSERT(strcmp(msgs[2].content, "inner final") == 0, "outer msg2 content");
            ASSERT(strcmp(msgs[3].content, "outer final") == 0, "outer msg3 content");
        }
        conversation_free_messages(msgs, n);
    }

    /* Inner (depth 2) scope: only its two entries. */
    msgs = NULL;
    n = 0;
    rc = conversation_reconstruct_scope(path, "runid-inner", &msgs, &n);
    ASSERT(rc == EXIT_SUCCESS, "inner reconstruct success");
    if (rc == EXIT_SUCCESS) {
        ASSERT(n == 2, "inner scope message count");
        if (n == 2) {
            ASSERT(strcmp(msgs[0].content, "inner prompt") == 0, "inner msg0 content");
            ASSERT(strcmp(msgs[1].content, "inner final") == 0, "inner msg1 content");
        }
        conversation_free_messages(msgs, n);
    }

    /* Unknown run_id: empty history, not an error. */
    msgs = NULL;
    n = -1;
    rc = conversation_reconstruct_scope(path, "runid-nobody", &msgs, &n);
    ASSERT(rc == EXIT_SUCCESS, "unknown run success");
    ASSERT(n == 0, "unknown run empty");
    conversation_free_messages(msgs, n);

    remove(path);
    free(path);
}

void test_reconstruct_v1_compat(void) {
    TEST("version-1 files (no scope fields) reconstruct fully");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    /* Hand-written v1-style entries: no depth/subagent/run_id anywhere. */
    fputs("{\"type\":\"meta\",\"version\":1,\"timestamp\":\"2026-01-01T00:00:00Z\","
          "\"config_hash\":\"sha256:abc\",\"run_id\":\"r1\"}\n"
          "{\"type\":\"user\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"content\":\"hi\","
          "\"source\":\"cli\"}\n"
          "{\"type\":\"assistant\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"content\":\"yo\","
          "\"model\":\"m\"}\n",
          fp);
    fclose(fp);

    json_message *msgs = NULL;
    int n = 0;
    int rc = conversation_reconstruct(path, &msgs, &n);
    ASSERT(rc == EXIT_SUCCESS, "reconstruct success");
    if (rc == EXIT_SUCCESS) {
        ASSERT(n == 2, "both v1 entries are top-level");
        if (n == 2) ASSERT(strcmp(msgs[1].content, "yo") == 0, "assistant content");
        conversation_free_messages(msgs, n);
    }
    remove(path);
    free(path);
}

void test_read_last_assistant_skips_scoped(void) {
    TEST("read_last_assistant skips scoped assistants");
    char *path = NULL;
    FILE *fp = tmp_file(&path);
    ASSERT(fp != NULL, "tmp file");
    fclose(fp);
    remove(path);
    write_nested_fixture(path);

    char *content = NULL;
    int rc = conversation_read_last_assistant(path, &content, NULL, NULL);
    ASSERT(rc == EXIT_SUCCESS, "returns success");
    if (content != NULL) {
        ASSERT(strcmp(content, "top final") == 0, "top-level final wins over scoped ones");
    }
    free(content);
    remove(path);
    free(path);
}
