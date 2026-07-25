#include "conversation.h"
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
static void test_write_tool_call(void);
static void test_write_tool_result(void);
static void test_write_error(void);
static void test_reconstruct_empty(void);
static void test_reconstruct_simple(void);
static void test_reconstruct_with_tools(void);
static void test_reconstruct_roundtrip(void);
static void test_free_null(void);

int main(void) {
    printf("=== test_conversation ===\n");

    test_open_new();
    test_open_utf8_invalid();
    test_write_meta();
    test_write_user();
    test_write_assistant();
    test_write_tool_call();
    test_write_tool_result();
    test_write_error();
    test_reconstruct_empty();
    test_reconstruct_simple();
    test_reconstruct_with_tools();
    test_reconstruct_roundtrip();
    test_free_null();

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
    ASSERT(v && cJSON_IsNumber(v) && v->valueint == 1, "version=1");
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
    usage_info usage = {10, 5, 15};
    int rc = conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello back", "gpt-4o", &usage);
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
    usage_info u = {5, 10, 15};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "Hello!", "gpt-4o", &u);
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
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "gpt-4o", (const usage_info *)NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "call_1", "get_weather", "{\"loc\":\"Paris\"}",
                             "wsrv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "call_1", "get_weather", "{\"temp\":22}", 0, 0,
                             "wsrv");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "It's 22C.", "gpt-4o", (const usage_info *)NULL);
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
    usage_info u1 = {1, 1, 2};
    conversation_write_entry(fp, ENTRY_ASSISTANT, "A1", "m1", &u1);
    conversation_write_entry(fp, ENTRY_USER, "Q2", "cli");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "", "m2", (const usage_info *)NULL);
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c1", "tool1", "{}", "srv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c1", "tool1", "ok", 0, 0, "srv");
    conversation_write_entry(fp, ENTRY_TOOL_CALL, "c2", "tool2", "{}", "srv");
    conversation_write_entry(fp, ENTRY_TOOL_RESULT, "c2", "tool2", "ok2", 0, 0, "srv");
    conversation_write_entry(fp, ENTRY_ASSISTANT, "A3", "m2", &u1);
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
