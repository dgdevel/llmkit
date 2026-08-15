#include "subagent.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Build a spec with three attributes (two required, one optional). */
static void build_spec(subagent_spec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->tool.name = util_strdup("calculator");
    spec->tool.description = util_strdup("A mathematic helper");
    spec->tool.attribute_count = 3;
    spec->tool.attributes = calloc(3, sizeof(subagent_attr));
    spec->tool.attributes[0].name = util_strdup("expression");
    spec->tool.attributes[0].type = util_strdup("string");
    spec->tool.attributes[0].description = util_strdup("the expression");
    spec->tool.attributes[0].required = 1;
    spec->tool.attributes[1].name = util_strdup("base");
    spec->tool.attributes[1].type = util_strdup("integer");
    spec->tool.attributes[1].required = 0;
    spec->tool.attributes[2].name = util_strdup("verbose");
    spec->tool.attributes[2].type = util_strdup("boolean");
    spec->tool.attributes[2].required = 1;
}

static void free_spec(subagent_spec *spec) {
    for (int i = 0; i < spec->tool.attribute_count; i++) {
        free(spec->tool.attributes[i].name);
        free(spec->tool.attributes[i].type);
        free(spec->tool.attributes[i].description);
    }
    free(spec->tool.attributes);
    free(spec->tool.name);
    free(spec->tool.description);
    memset(spec, 0, sizeof(*spec));
}

/* ------------------------------------------------------------------ */
/*  input schema                                                       */
/* ------------------------------------------------------------------ */

static void test_input_schema(void) {
    subagent_spec spec;
    build_spec(&spec);

    char *schema = subagent_build_input_schema(&spec.tool);
    CHECK(schema != NULL, "schema builds");

    const char *expected =
        "{\"type\":\"object\",\"properties\":{"
        "\"expression\":{\"type\":\"string\",\"description\":\"the expression\"},"
        "\"base\":{\"type\":\"integer\"},"
        "\"verbose\":{\"type\":\"boolean\"}},"
        "\"required\":[\"expression\",\"verbose\"]}";
    CHECK_STR_EQ(schema, expected, "schema content and ordering");

    /* Determinism: identical input yields identical bytes. */
    char *schema2 = subagent_build_input_schema(&spec.tool);
    CHECK_STR_EQ(schema2, schema, "schema is deterministic");
    free(schema2);
    free(schema);

    /* No attributes: empty properties and required arrays. */
    subagent_tool_def empty = {0};
    empty.name = util_strdup("noop");
    char *s3 = subagent_build_input_schema(&empty);
    CHECK_STR_EQ(s3, "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
                 "empty schema shape");
    free(s3);
    free(empty.name);

    free_spec(&spec);
    fprintf(stderr, "  [ok] test_input_schema\n");
}

/* ------------------------------------------------------------------ */
/*  interpolation                                                      */
/* ------------------------------------------------------------------ */

static void test_interp(void) {
    subagent_spec spec;
    build_spec(&spec);

    /* String attribute interpolates raw text. */
    char *r = subagent_interp("Resolve {expression} now", &spec, "{\"expression\":\"2+3\"}");
    CHECK_STR_EQ(r, "Resolve 2+3 now", "string attribute");
    free(r);

    /* Missing optional attribute interpolates empty; unknown untouched. */
    r = subagent_interp("[{base}] {unknown} {expression}", &spec, "{\"expression\":\"x\"}");
    CHECK_STR_EQ(r, "[] {unknown} x", "missing optional + unknown placeholder");
    free(r);

    /* Number and boolean interpolate JSON literals. */
    r = subagent_interp("base={base} verbose={verbose}", &spec,
                        "{\"base\":16,\"verbose\":true,\"expression\":\"e\"}");
    CHECK_STR_EQ(r, "base=16 verbose=true", "number and boolean literals");
    free(r);

    /* Multiple occurrences of the same attribute. */
    r = subagent_interp("{expression} and {expression}", &spec, "{\"expression\":\"7\"}");
    CHECK_STR_EQ(r, "7 and 7", "repeated placeholder");
    free(r);

    /* Unclosed brace is left as-is. */
    r = subagent_interp("a {expression b", &spec, "{\"expression\":\"x\"}");
    CHECK_STR_EQ(r, "a {expression b", "unclosed brace untouched");
    free(r);

    /* NULL arguments behaves like an empty object. */
    r = subagent_interp("x={expression}", &spec, NULL);
    CHECK_STR_EQ(r, "x=", "NULL args");
    free(r);

    /* NULL template yields NULL. */
    CHECK(subagent_interp(NULL, &spec, "{}") == NULL, "NULL template");

    free_spec(&spec);
    fprintf(stderr, "  [ok] test_interp\n");
}

/* ------------------------------------------------------------------ */
/*  registration / merge                                               */
/* ------------------------------------------------------------------ */

static tool_def make_tool(const char *name, const char *server) {
    tool_def t;
    memset(&t, 0, sizeof(t));
    t.name = util_strdup(name);
    t.original = util_strdup(name);
    t.description = util_strdup("");
    t.input_schema = util_strdup("{}");
    t.mcp_server = util_strdup(server);
    return t;
}

static void free_tools(tool_def *tools, int count) {
    for (int i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].original);
        free(tools[i].description);
        free(tools[i].input_schema);
        free(tools[i].mcp_server);
    }
    free(tools);
}

static void test_register_merge(void) {
    runtime_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.tools = calloc(2, sizeof(tool_def));
    ctx.tool_count = 2;
    ctx.tools[0] = make_tool("zeta.add", "zeta");
    ctx.tools[1] = make_tool("alpha.get", "alpha");

    subagent_spec specs[2];
    build_spec(&specs[0]); /* calculator */
    memset(&specs[1], 0, sizeof(specs[1]));
    specs[1].tool.name = util_strdup("beta_helper");

    int rc = subagent_register_tools_list(&ctx, specs, 2);
    CHECK_EQ(rc, EXIT_SUCCESS, "merge succeeds");
    CHECK_EQ(ctx.tool_count, 4, "tool count after merge");

    /* Deterministic ordering across the merged array. */
    const char *want[] = {"alpha.get", "beta_helper", "calculator", "zeta.add"};
    int order_ok = 1;
    for (int i = 0; i < 4; i++) {
        if (strcmp(ctx.tools[i].name, want[i]) != 0) order_ok = 0;
    }
    CHECK(order_ok, "merged array sorted deterministically");

    /* Subagent entries carry the sentinel server and a parsed schema. */
    int found = 0;
    for (int i = 0; i < ctx.tool_count; i++) {
        if (strcmp(ctx.tools[i].name, "calculator") == 0) {
            found = 1;
            CHECK_STR_EQ(ctx.tools[i].mcp_server, SUBAGENT_TOOL_SERVER, "sentinel server");
            CHECK_STR_EQ(ctx.tools[i].description, "A mathematic helper", "description kept");
            CHECK(strstr(ctx.tools[i].input_schema, "\"required\":[\"expression\",\"verbose\"]") !=
                      NULL,
                  "schema attached");
        }
    }
    CHECK(found, "calculator merged");

    /* Collision with an existing tool name is rejected. */
    subagent_spec bad;
    memset(&bad, 0, sizeof(bad));
    bad.tool.name = util_strdup("alpha.get");
    rc = subagent_register_tools_list(&ctx, &bad, 1);
    CHECK_EQ(rc, EXIT_CONFIG_ERR, "collision rejected");
    CHECK_EQ(ctx.tool_count, 4, "tool count unchanged after rejected merge");
    free(bad.tool.name);

    /* subagent_find locates by tool name. */
    CHECK(subagent_find(specs, 2, "beta_helper") == &specs[1], "subagent_find hit");
    CHECK(subagent_find(specs, 2, "nosuch") == NULL, "subagent_find miss");

    free_spec(&specs[0]);
    free(specs[1].tool.name);
    free_tools(ctx.tools, ctx.tool_count);
    fprintf(stderr, "  [ok] test_register_merge\n");
}

int main(void) {
    fprintf(stderr, "=== test_subagent ===\n");

    test_input_schema();
    test_interp();
    test_register_merge();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
