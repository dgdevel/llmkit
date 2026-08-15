#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "util.h"

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

static int write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t n = fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return (int)n;
}

static void test_full_config(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://localhost:11434/v1\"\n"
                       "  api_key: \"sk-test-key\"\n"
                       "  model: \"llama3\"\n"
                       "  headers:\n"
                       "    X-Custom: \"value1\"\n"
                       "    X-Other: \"value2\"\n"
                       "  retain_reasoning: true\n"
                       "\n"
                       "mcps:\n"
                       "  - name: \"srv1\"\n"
                       "    type: stdio\n"
                       "    cmdline: \"node server.js\"\n"
                       "    init_timeout: \"10s\"\n"
                       "    call_timeout: \"5m\"\n"
                       "    call_timeout_behavior: continue\n"
                       "    namespace: \"fs\"\n"
                       "    rename:\n"
                       "      fs.old_tool: fs.new_tool\n"
                       "    redefine:\n"
                       "      fs.tool1: \"New description\"\n"
                       "    whitelist:\n"
                       "      - \"fs.tool1\"\n"
                       "    blacklist:\n"
                       "      - \"fs.tool2\"\n"
                       "\n"
                       "  - name: \"srv2\"\n"
                       "    type: http\n"
                       "    url: \"http://localhost:8080/mcp\"\n"
                       "    headers:\n"
                       "      Authorization: \"Bearer token123\"\n"
                       "\n"
                       "  - name: \"srv3\"\n"
                       "    type: sse\n"
                       "    url: \"http://localhost:9090/sse\"\n"
                       "    max_reconnect: 5\n"
                       "    reconnect_delay: \"2s\"\n"
                       "    hide: true\n"
                       "\n"
                       "agent:\n"
                       "  system_prompt: \"You are a helpful assistant\"\n"
                       "  compact:\n"
                       "    enabled: true\n"
                       "    max_tokens: 8192\n"
                       "    threshold: 0.6\n"
                       "    summarize: yes\n";

    const char *tmp = "/tmp/llmkit_test_full.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "full config should load successfully");

    /* Check LLM config */
    CHECK_STR_EQ(ctx.llm.api_base, "http://localhost:11434/v1", "llm.api_base");
    CHECK_STR_EQ(ctx.llm.api_key, "sk-test-key", "llm.api_key");
    CHECK_STR_EQ(ctx.llm.model, "llama3", "llm.model");
    CHECK_EQ(ctx.llm.retain_reasoning, 1, "llm.retain_reasoning");

    CHECK(ctx.llm.headers != NULL, "llm.headers should be set");
    CHECK_STR_EQ(ctx.llm.headers[0], "X-Custom=value1", "llm.headers[0]");
    CHECK_STR_EQ(ctx.llm.headers[1], "X-Other=value2", "llm.headers[1]");
    CHECK(ctx.llm.headers[2] == NULL, "llm.headers[2] should be NULL");

    /* Check MCP servers */
    CHECK_EQ(ctx.mcp_count, 3, "mcp_count should be 3");

    /* srv1 - stdio */
    CHECK_STR_EQ(ctx.mcps[0].name, "srv1", "mcps[0].name");
    CHECK_EQ(ctx.mcps[0].transport, MCP_STDIO, "mcps[0].transport");
    CHECK_STR_EQ(ctx.mcps[0].cmdline, "node server.js", "mcps[0].cmdline");
    CHECK(ctx.mcps[0].url == NULL, "mcps[0].url should be NULL");
    CHECK_EQ(ctx.mcps[0].init_timeout_ms, 10000, "mcps[0].init_timeout");
    CHECK_EQ(ctx.mcps[0].call_timeout_ms, 300000, "mcps[0].call_timeout");
    CHECK_EQ(ctx.mcps[0].call_timeout_beh, TIMEOUT_CONTINUE, "mcps[0].timeout_beh");
    CHECK_EQ(ctx.mcps[0].hide, false, "mcps[0].hide");
    CHECK_STR_EQ(ctx.mcps[0].namespace, "fs", "mcps[0].namespace");
    CHECK_STR_EQ(ctx.mcps[0].rename_keys[0], "fs.old_tool=fs.new_tool", "mcps[0].rename");
    CHECK(ctx.mcps[0].rename_keys[1] == NULL, "mcps[0].rename[1] NULL");
    CHECK_STR_EQ(ctx.mcps[0].redefine_keys[0], "fs.tool1=New description", "mcps[0].redefine");
    CHECK(ctx.mcps[0].redefine_keys[1] == NULL, "mcps[0].redefine[1] NULL");
    CHECK_STR_EQ(ctx.mcps[0].whitelist[0], "fs.tool1", "mcps[0].whitelist[0]");
    CHECK(ctx.mcps[0].whitelist[1] == NULL, "mcps[0].whitelist[1] NULL");
    CHECK_STR_EQ(ctx.mcps[0].blacklist[0], "fs.tool2", "mcps[0].blacklist[0]");
    CHECK(ctx.mcps[0].blacklist[1] == NULL, "mcps[0].blacklist[1] NULL");

    /* srv2 - http */
    CHECK_STR_EQ(ctx.mcps[1].name, "srv2", "mcps[1].name");
    CHECK_EQ(ctx.mcps[1].transport, MCP_HTTP, "mcps[1].transport");
    CHECK(ctx.mcps[1].cmdline == NULL, "mcps[1].cmdline NULL");
    CHECK_STR_EQ(ctx.mcps[1].url, "http://localhost:8080/mcp", "mcps[1].url");
    CHECK_STR_EQ(ctx.mcps[1].headers[0], "Authorization=Bearer token123", "mcps[1].headers[0]");
    CHECK(ctx.mcps[1].headers[1] == NULL, "mcps[1].headers[1] NULL");
    CHECK_STR_EQ(ctx.mcps[1].namespace, "srv2", "mcps[1].namespace (defaults to name)");
    CHECK_EQ(ctx.mcps[1].max_reconnect, 3, "mcps[1].max_reconnect (default)");

    /* srv3 - sse */
    CHECK_STR_EQ(ctx.mcps[2].name, "srv3", "mcps[2].name");
    CHECK_EQ(ctx.mcps[2].transport, MCP_SSE, "mcps[2].transport");
    CHECK_STR_EQ(ctx.mcps[2].url, "http://localhost:9090/sse", "mcps[2].url");
    CHECK_EQ(ctx.mcps[2].hide, true, "mcps[2].hide");
    CHECK_EQ(ctx.mcps[2].max_reconnect, 5, "mcps[2].max_reconnect");
    CHECK_EQ(ctx.mcps[2].reconnect_delay_ms, 2000, "mcps[2].reconnect_delay");

    /* Check agent config */
    CHECK_STR_EQ(ctx.agent.system_prompt, "You are a helpful assistant", "agent.system_prompt");
    CHECK_EQ(ctx.agent.compact_enabled, 1, "agent.compact.enabled");
    CHECK_EQ(ctx.agent.compact_max_tokens, 8192, "agent.compact.max_tokens");
    CHECK(ctx.agent.compact_threshold > 0.59 && ctx.agent.compact_threshold < 0.61,
          "agent.compact.threshold");
    CHECK_EQ(ctx.agent.compact_summarize, 1, "agent.compact.summarize");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_full_config\n");
}

static void test_defaults(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://localhost:11434/v1\"\n"
                       "\n"
                       "mcps:\n"
                       "  - name: \"defaults\"\n"
                       "    cmdline: \"/bin/echo\"\n";

    const char *tmp = "/tmp/llmkit_test_defaults.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "minimal config should load");

    CHECK_STR_EQ(ctx.llm.model, "gpt-4o-mini", "default model");
    CHECK_STR_NULL(ctx.llm.api_key, "default api_key NULL");
    CHECK_EQ(ctx.llm.retain_reasoning, 0, "default retain_reasoning false");

    CHECK_EQ(ctx.mcp_count, 1, "one MCP server");
    CHECK_STR_EQ(ctx.mcps[0].name, "defaults", "server name");
    CHECK_EQ(ctx.mcps[0].transport, MCP_STDIO, "default transport stdio");
    CHECK_STR_EQ(ctx.mcps[0].cmdline, "/bin/echo", "cmdline");
    CHECK_EQ(ctx.mcps[0].init_timeout_ms, 30000, "default init_timeout");
    CHECK_EQ(ctx.mcps[0].call_timeout_ms, 600000, "default call_timeout");
    CHECK_EQ(ctx.mcps[0].call_timeout_beh, TIMEOUT_FAIL, "default timeout behavior");
    CHECK_EQ(ctx.mcps[0].hide, false, "default hide");
    CHECK_STR_EQ(ctx.mcps[0].namespace, "defaults", "default namespace (name)");
    CHECK_EQ(ctx.mcps[0].max_reconnect, 3, "default max_reconnect");
    CHECK_EQ(ctx.mcps[0].reconnect_delay_ms, 1000, "default reconnect_delay");

    CHECK_STR_NULL(ctx.agent.system_prompt, "default system_prompt NULL");
    CHECK_EQ(ctx.agent.compact_enabled, 0, "default compact.enabled off");
    CHECK_EQ(ctx.agent.compact_max_tokens, 16384, "default compact.max_tokens");
    CHECK(ctx.agent.compact_threshold > 0.79 && ctx.agent.compact_threshold < 0.81,
          "default compact.threshold 0.8");
    CHECK_EQ(ctx.agent.compact_summarize, 0, "default compact.summarize off");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_defaults\n");
}

static void test_invalid_utf8(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://localhost:11434/v1\"\n"
                       "mcps:\n"
                       "  - name: \"test\xFF\xFE\"\n"
                       "    cmdline: \"/bin/echo\"\n";

    const char *tmp = "/tmp/llmkit_test_invalid_utf8.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "invalid UTF-8 in name should fail");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_invalid_utf8\n");
}

static void test_missing_required(void) {
    /* MCP server without name */
    const char *yaml1 = "mcps:\n"
                        "  - cmdline: \"/bin/echo\"\n";

    const char *tmp = "/tmp/llmkit_test_missing.yml";
    write_file(tmp, yaml1);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "missing mcp name should fail");
    config_free(&ctx);

    /* MCP server without cmdline for stdio */
    const char *yaml2 = "mcps:\n"
                        "  - name: \"test\"\n";

    write_file(tmp, yaml2);
    memset(&ctx, 0, sizeof(ctx));

    ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "missing cmdline for stdio should fail");
    config_free(&ctx);

    /* MCP server without url for http */
    const char *yaml3 = "mcps:\n"
                        "  - name: \"test\"\n"
                        "    type: http\n";

    write_file(tmp, yaml3);
    memset(&ctx, 0, sizeof(ctx));

    ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "missing url for http should fail");
    config_free(&ctx);

    unlink(tmp);

    fprintf(stderr, "  [ok] test_missing_required\n");
}

static void test_empty_mcps(void) {
    const char *yaml = "mcps:\n";

    const char *tmp = "/tmp/llmkit_test_empty_mcps.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "empty mcps should fail");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_empty_mcps\n");
}

static void test_invalid_types(void) {
    const char *yaml = "mcps:\n"
                       "  - name: \"test\"\n"
                       "    type: invalid_type\n"
                       "    cmdline: \"/bin/echo\"\n";

    const char *tmp = "/tmp/llmkit_test_invalid_type.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "invalid transport type should fail");

    config_free(&ctx);
    unlink(tmp);

    /* invalid timeout behavior */
    const char *yaml2 = "mcps:\n"
                        "  - name: \"test\"\n"
                        "    cmdline: \"/bin/echo\"\n"
                        "    call_timeout_behavior: invalid\n";

    write_file(tmp, yaml2);
    memset(&ctx, 0, sizeof(ctx));

    ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "invalid timeout behavior should fail");

    config_free(&ctx);
    unlink(tmp);

    /* invalid duration */
    const char *yaml3 = "mcps:\n"
                        "  - name: \"test\"\n"
                        "    cmdline: \"/bin/echo\"\n"
                        "    init_timeout: \"abc\"\n";

    write_file(tmp, yaml3);
    memset(&ctx, 0, sizeof(ctx));

    ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, EXIT_CONFIG_ERR, "invalid duration should fail");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_invalid_types\n");
}

static void test_config_free_null(void) {
    config_free(NULL);
    config_free_mcp(NULL);
    fprintf(stderr, "  [ok] test_config_free_null\n");
}

static void test_unknown_keys(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://localhost:11434/v1\"\n"
                       "  unknown_field: \"should be ignored\"\n"
                       "\n"
                       "mcps:\n"
                       "  - name: \"test\"\n"
                       "    cmdline: \"/bin/echo\"\n"
                       "    unknown_key: \"should be ignored\"\n"
                       "\n"
                       "agent:\n"
                       "  system_prompt: \"prompt\"\n"
                       "  unknown_agent_key: \"should be ignored\"\n"
                       "\n"
                       "unknown_root_key:\n"
                       "  sub: \"should be ignored\"\n";

    const char *tmp = "/tmp/llmkit_test_unknown_keys.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "unknown keys should be ignored");

    CHECK_STR_EQ(ctx.llm.api_base, "http://localhost:11434/v1", "api_base");
    CHECK_STR_EQ(ctx.mcps[0].cmdline, "/bin/echo", "cmdline");
    CHECK_STR_EQ(ctx.agent.system_prompt, "prompt", "system_prompt");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_unknown_keys\n");
}

/* ------------------------------------------------------------------ */
/*  subagents                                                          */
/* ------------------------------------------------------------------ */

static void test_subagents_valid(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://1.2.3.4/v1\"\n"
                       "agent:\n"
                       "  system_prompt: \"You are a helpful assistant.\"\n"
                       "subagents:\n"
                       "  - tool_definition:\n"
                       "      name: calculator\n"
                       "      description: A mathematic helper\n"
                       "      attributes:\n"
                       "        expression:\n"
                       "          type: string\n"
                       "          description: the expression to be evaluated\n"
                       "        base:\n"
                       "          type: integer\n"
                       "          required: false\n"
                       "    system_prompt: \"You help doing math.\"\n"
                       "    user_prompt: \"Resolve {expression}\"\n"
                       "    mcps:\n"
                       "      - name: real_calculator\n"
                       "        cmdline: uvx mcp-server-calculator\n";

    const char *tmp = "/tmp/llmkit_test_subagents_valid.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "valid subagents config loads");

    CHECK_EQ(ctx.subagent_count, 1, "one root subagent");
    subagent_spec *s = &ctx.subagents[0];
    CHECK_STR_EQ(s->tool.name, "calculator", "tool name");
    CHECK_STR_EQ(s->tool.description, "A mathematic helper", "tool description");
    CHECK_STR_EQ(s->system_prompt, "You help doing math.", "system_prompt");
    CHECK_STR_EQ(s->user_prompt, "Resolve {expression}", "user_prompt");

    CHECK_EQ(s->tool.attribute_count, 2, "two attributes");
    CHECK_STR_EQ(s->tool.attributes[0].name, "expression", "attr 0 name");
    CHECK_STR_EQ(s->tool.attributes[0].type, "string", "attr 0 type");
    CHECK_EQ(s->tool.attributes[0].required, 1, "attr 0 required defaults to true");
    CHECK_STR_EQ(s->tool.attributes[1].name, "base", "attr 1 name");
    CHECK_STR_EQ(s->tool.attributes[1].type, "integer", "attr 1 type");
    CHECK_EQ(s->tool.attributes[1].required, 0, "attr 1 required: false");

    CHECK_EQ(s->mcp_count, 1, "one private mcp");
    CHECK_STR_EQ(s->mcps[0].name, "real_calculator", "mcp name");
    CHECK_STR_EQ(s->mcps[0].cmdline, "uvx mcp-server-calculator", "mcp cmdline");
    CHECK_EQ(s->mcps[0].reference, 0, "mcp is a full entry, not a reference");
    CHECK_EQ(s->subagent_count, 0, "no nested subagents");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_subagents_valid\n");
}

static void test_subagents_nested(void) {
    const char *yaml = "llm:\n"
                       "  api_base: \"http://1.2.3.4/v1\"\n"
                       "subagents:\n"
                       "  - tool_definition:\n"
                       "      name: parent\n"
                       "    subagents:\n"
                       "      - tool_definition:\n"
                       "          name: child\n"
                       "        mcps:\n"
                       "          - name: inner\n"
                       "            cmdline: run-inner\n";

    const char *tmp = "/tmp/llmkit_test_subagents_nested.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "nested subagents config loads");

    CHECK_EQ(ctx.subagent_count, 1, "one root subagent");
    subagent_spec *s = &ctx.subagents[0];
    CHECK_STR_EQ(s->tool.name, "parent", "root tool name");
    CHECK_EQ(s->subagent_count, 1, "one nested subagent");
    CHECK_STR_EQ(s->subagents[0].tool.name, "child", "nested tool name");
    CHECK_EQ(s->subagents[0].mcp_count, 1, "nested private mcp");
    CHECK_STR_EQ(s->subagents[0].mcps[0].cmdline, "run-inner", "nested mcp cmdline");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_subagents_nested\n");
}

static void test_subagents_reference(void) {
    /* A name-only entry inside subagents.mcps references the main list. */
    const char *yaml = "llm:\n"
                       "  api_base: \"http://1.2.3.4/v1\"\n"
                       "mcps:\n"
                       "  - name: calc\n"
                       "    cmdline: uvx mcp-server-calculator\n"
                       "subagents:\n"
                       "  - tool_definition:\n"
                       "      name: c1\n"
                       "    mcps:\n"
                       "      - name: calc\n"
                       "      - name: own\n"
                       "        cmdline: run-own\n";

    const char *tmp = "/tmp/llmkit_test_subagents_reference.yml";
    write_file(tmp, yaml);

    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int ret = config_load(tmp, &ctx);
    CHECK_EQ(ret, 0, "reference config loads");

    subagent_spec *s = &ctx.subagents[0];
    CHECK_EQ(s->mcp_count, 2, "two mcp entries");
    CHECK_EQ(s->mcps[0].reference, 1, "name-only entry is a reference");
    CHECK_STR_NULL(s->mcps[0].cmdline, "reference has no cmdline");
    CHECK_EQ(s->mcps[1].reference, 0, "full entry is not a reference");
    CHECK_STR_EQ(s->mcps[1].cmdline, "run-own", "full entry cmdline");

    config_free(&ctx);
    unlink(tmp);

    fprintf(stderr, "  [ok] test_subagents_reference\n");
}

static void test_subagents_errors(void) {
    struct {
        const char *name;
        const char *yaml;
    } cases[] = {
        {"unresolved reference",
         "llm:\n  api_base: x\nmcps:\n  - name: other\n    cmdline: c\nsubagents:\n"
         "  - tool_definition:\n      name: c1\n    mcps:\n      - name: nosuch\n"},
        {"missing tool name", "llm:\n  api_base: x\nsubagents:\n  - system_prompt: s\n"},
        {"duplicate sibling tool names",
         "llm:\n  api_base: x\nsubagents:\n  - tool_definition:\n      name: c1\n"
         "  - tool_definition:\n      name: c1\n"},
        {"bad attribute type",
         "llm:\n  api_base: x\nsubagents:\n  - tool_definition:\n      name: c1\n"
         "      attributes:\n        a:\n          type: float\n"},
        {"mcp name collides with top-level",
         "llm:\n  api_base: x\nmcps:\n  - name: calc\n    cmdline: c\nsubagents:\n"
         "  - tool_definition:\n      name: c1\n    mcps:\n      - name: calc\n"
         "        cmdline: d\n"},
        {"reference without top-level mcps",
         "llm:\n  api_base: x\nsubagents:\n  - tool_definition:\n      name: c1\n"
         "    mcps:\n      - name: calc\n"},
        {"subagents not a sequence", "llm:\n  api_base: x\nsubagents: oops\n"},
        {"depth beyond cap",
         "llm:\n  api_base: x\nsubagents:\n  - tool_definition:\n      name: a\n"
         "    subagents:\n      - tool_definition:\n          name: b\n"
         "        subagents:\n          - tool_definition:\n              name: c\n"
         "            subagents:\n            - tool_definition:\n                name: d\n"
         "              subagents:\n              - tool_definition:\n                  name: e\n"
         "                subagents:\n                - tool_definition:\n                    "
         "name: f\n"
         "                  subagents:\n                  - tool_definition:\n                     "
         " name: g\n"
         "                    subagents:\n                    - tool_definition:\n"
         "                        name: h\n"
         "                      subagents:\n                      - tool_definition:\n"
         "                          name: i\n"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/llmkit_test_subagents_err_%zu.yml", i);
        write_file(path, cases[i].yaml);

        runtime_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        int ret = config_load(path, &ctx);
        CHECK_EQ(ret, EXIT_CONFIG_ERR, cases[i].name);
        config_free(&ctx);
        unlink(path);
    }

    fprintf(stderr, "  [ok] test_subagents_errors\n");
}

int main(void) {
    fprintf(stderr, "=== test_config ===\n");

    test_full_config();
    test_defaults();
    test_invalid_utf8();
    test_missing_required();
    test_empty_mcps();
    test_invalid_types();
    test_config_free_null();
    test_unknown_keys();
    test_subagents_valid();
    test_subagents_nested();
    test_subagents_reference();
    test_subagents_errors();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
