#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools.h"
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

#define CHECK_STR_CONTAINS(hay, needle, msg)                                               \
    do {                                                                                   \
        tests_run++;                                                                       \
        if ((hay) == NULL || strstr((hay), (needle)) == NULL) {                            \
            fprintf(stderr, "  FAIL (%s:%d): %s — \"%s\" not found in \"%s\"\n", __FILE__, \
                    __LINE__, msg, (needle) ? (needle) : "NULL", (hay) ? (hay) : "NULL");  \
            tests_failed++;                                                                \
        }                                                                                  \
    } while (0)

#define CHECK_STR_NOT_CONTAINS(hay, needle, msg)                                              \
    do {                                                                                      \
        tests_run++;                                                                          \
        if ((hay) != NULL && strstr((hay), (needle)) != NULL) {                               \
            fprintf(stderr, "  FAIL (%s:%d): %s — \"%s\" unexpectedly in \"%s\"\n", __FILE__, \
                    __LINE__, msg, (needle) ? (needle) : "NULL", (hay) ? (hay) : "NULL");     \
            tests_failed++;                                                                   \
        }                                                                                     \
    } while (0)

/* A DuckDuckGo HTML results page fixture shaped like the real markup:
 * result__a anchors (with //duckduckgo.com/l/?uddg= redirect hrefs) and
 * result__snippet elements, plus boilerplate that must be ignored. */
static const char *ddg_page =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\"><head><title>test at DuckDuckGo</title></head>\n"
    "<body>\n"
    "<form action=\"/html/\"><input type=\"text\" name=\"q\"></form>\n"
    "<div class=\"result results_links results_links_deep web-result\">\n"
    "  <h2 class=\"result__title\">\n"
    "    <a rel=\"nofollow\" class=\"result__a\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2F&amp;rut=abc123\">"
    "Example <b>Domain</b></a>\n"
    "  </h2>\n"
    "  <div class=\"links_main links_deep result__body\">\n"
    "    <a class=\"result__snippet\" href=\"https://example.com/\">Example "
    "Domain &amp; resources. This domain is for <b>use</b> in examples.</a>\n"
    "  </div>\n"
    "</div>\n"
    "<div class=\"result\">\n"
    "  <h2 class=\"result__title\"><a rel=\"nofollow\" class=\"result__a\" "
    "href=\"/l/?uddg=https%3A%2F%2Fexample.org%2Fpage%3Fa%3D1%26b%3D2&amp;rut=xyz\">"
    "Second result</a></h2>\n"
    "  <a class=\"result__snippet\" href=\"//example.org/snippet\">Snippet with "
    "&#39;quotes&#39; and &#x2713; check</a>\n"
    "</div>\n"
    "</body></html>\n";

static void test_registry_names(void) {
    CHECK(TOOLS_BUILTIN_NAMES[0] != NULL, "builtin list non-empty");
    CHECK(strcmp(TOOLS_BUILTIN_NAMES[0], "online_search") == 0, "online_search registered");
    CHECK(strcmp(TOOLS_BUILTIN_NAMES[1], "online_fetch") == 0, "online_fetch registered");
    CHECK(TOOLS_BUILTIN_NAMES[2] == NULL, "builtin list NULL-terminated");
}

static void test_parse_results(void) {
    char *text = tools_parse_search_results(ddg_page, "test");
    CHECK(text != NULL, "search results parsed");

    /* Entry 1: redirect URL unwrapped, tags stripped, entities decoded. */
    CHECK_STR_CONTAINS(text, "Title: Example Domain", "entry 1 title (no <b> tags)");
    CHECK_STR_CONTAINS(text, "URL: https://example.com/", "entry 1 redirect unwrapped");
    CHECK_STR_CONTAINS(
        text, "Description: Example Domain & resources. This domain is for use in examples.",
        "entry 1 snippet decoded");
    CHECK_STR_NOT_CONTAINS(text, "uddg=", "no redirect wrappers left");
    CHECK_STR_NOT_CONTAINS(text, "<b>", "no inner tags left");

    /* Entry 2: root-relative redirect, encoded query params, entities. */
    CHECK_STR_CONTAINS(text, "Title: Second result", "entry 2 title");
    CHECK_STR_CONTAINS(text, "URL: https://example.org/page?a=1&b=2",
                       "entry 2 query params decoded");
    CHECK_STR_CONTAINS(text, "Snippet with 'quotes' and \xe2\x9c\x93 check",
                       "entry 2 entities decoded");

    /* Entry format: exactly one blank line between the two entries. */
    CHECK_STR_CONTAINS(text, "https://example.com/\nDescription:", "field layout");
    CHECK_STR_CONTAINS(text, "use in examples.\n\nTitle: Second result",
                       "entries separated by blank line");
    free(text);
}

static void test_parse_no_results(void) {
    /* DDG challenge/anomaly pages have no result anchors. */
    char *text = tools_parse_search_results("<html><body>No anchors here</body></html>", "my q");
    CHECK(text != NULL, "no-results page handled");
    CHECK_STR_CONTAINS(text, "No results found for: my q", "no-results message");
    free(text);
}

static void test_run_rejects_bad_lists(void) {
    CHECK_EQ(tools_run(NULL, NULL), 2, "missing tool list exits 2");
    CHECK_EQ(tools_run("", NULL), 2, "empty tool list exits 2");
    CHECK_EQ(tools_run("no_such_tool", NULL), 2, "unknown tool exits 2");
    CHECK_EQ(tools_run("online_search,bogus", NULL), 2, "partially unknown list exits 2");
    CHECK_EQ(tools_run("online_search,online_search", NULL), 2, "duplicate tool exits 2");
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_registry_names();
    test_parse_results();
    test_parse_no_results();
    test_run_rejects_bad_lists();

    printf("test_tools: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
