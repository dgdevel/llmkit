#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "htmlmd.h"
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

static char *convert(const char *html) {
    char *md = htmlmd_convert(html);
    if (md == NULL) {
        fprintf(stderr, "  FAIL: htmlmd_convert returned NULL\n");
        tests_failed++;
        return util_strdup("");
    }
    return md;
}

/* ------------------------------------------------------------------ */

static void test_empty_and_plain(void) {
    char *md = convert("");
    CHECK_STR_EQ(md, "", "empty input yields empty output");
    free(md);

    md = convert("just text, no html");
    CHECK_STR_CONTAINS(md, "just text, no html", "plain text is preserved");
    free(md);
}

static void test_headings_and_paragraphs(void) {
    char *md = convert("<h1>One</h1><h2>Two</h2><h3>Three</h3>"
                       "<p>first para</p><p>second para</p>");
    CHECK_STR_CONTAINS(md, "# One", "h1 becomes #");
    CHECK_STR_CONTAINS(md, "## Two", "h2 becomes ##");
    CHECK_STR_CONTAINS(md, "### Three", "h3 becomes ###");
    CHECK_STR_CONTAINS(md, "first para", "paragraph text kept");
    CHECK_STR_CONTAINS(md, "second para", "second paragraph kept");
    free(md);
}

static void test_inline_formatting_links_images(void) {
    char *md = convert("<p><strong>bold</strong> and <em>italic</em></p>"
                       "<p><a href=\"https://example.com/page?a=1&amp;b=2\">a link</a></p>"
                       "<p><img src=\"pic.png\" alt=\"a pic\"></p>");
    CHECK_STR_CONTAINS(md, "**bold**", "strong becomes **");
    CHECK_STR_CONTAINS(md, "_italic_", "em becomes _");
    CHECK_STR_CONTAINS(md, "[a link](https://example.com/page?a=1&b=2)", "link with decoded href");
    CHECK_STR_CONTAINS(md, "![a pic](pic.png)", "image with alt and src");
    free(md);

    md = convert("<a href=\"https://example.com\">https://example.com</a>");
    CHECK_STR_CONTAINS(md, "<https://example.com>", "bare-URL link becomes autolink");
    free(md);
}

static void test_lists(void) {
    char *md = convert("<ul><li>alpha</li><li>beta<ul><li>nested</li></ul></li>"
                       "<li>gamma</li></ul>");
    CHECK_STR_CONTAINS(md, "- alpha", "unordered item");
    CHECK_STR_CONTAINS(md, "- beta", "second unordered item");
    CHECK_STR_CONTAINS(md, "  - nested", "nested item is indented");
    CHECK_STR_CONTAINS(md, "- gamma", "list continues after nesting");
    free(md);

    md = convert("<ol><li>one</li><li>two</li><li>three</li></ol>");
    CHECK_STR_CONTAINS(md, "1. one", "ordered item 1");
    CHECK_STR_CONTAINS(md, "2. two", "ordered item 2");
    CHECK_STR_CONTAINS(md, "3. three", "ordered item 3");
    free(md);

    /* Implicit </li>: browsers tolerate a missing close tag. */
    md = convert("<ul><li>a<li>b</ul>");
    CHECK_STR_CONTAINS(md, "- a", "implicit li close item 1");
    CHECK_STR_CONTAINS(md, "- b", "implicit li close item 2");
    free(md);
}

static void test_code_and_pre(void) {
    char *md = convert("<p>use <code>malloc()</code> here</p>"
                       "<pre><code>int x = 1;\nint y = 2;\n</code></pre>");
    CHECK_STR_CONTAINS(md, "`malloc()`", "inline code gets backticks");
    CHECK_STR_CONTAINS(md, "```\nint x = 1;\nint y = 2;\n```", "pre becomes fenced block");
    free(md);

    md = convert("<pre>code with ``` fence inside</pre>");
    CHECK_STR_CONTAINS(md, "````\ncode with ``` fence inside\n````",
                       "fence grows past inner backticks");
    free(md);
}

static void test_blockquote_and_hr(void) {
    char *md = convert("<p>before</p><blockquote><p>quoted line</p></blockquote><hr>");
    CHECK_STR_CONTAINS(md, "> quoted line", "blockquote gets > prefix");
    CHECK_STR_CONTAINS(md, "---", "hr becomes ---");
    free(md);
}

static void test_entities(void) {
    char *md = convert("<p>&amp; &lt; &gt; &quot; &nbsp; &#65; &#x2713; &copy;</p>");
    CHECK_STR_CONTAINS(md, "& < > \"", "basic entities decoded");
    CHECK_STR_CONTAINS(md, "A", "numeric entity decoded");
    CHECK_STR_CONTAINS(md, "\xe2\x9c\x93", "hex entity decoded (check mark)");
    CHECK_STR_CONTAINS(md, "\xc2\xa9", "copy entity decoded");
    free(md);

    md = convert("<p>&unknownentity; &ampcopy</p>");
    CHECK_STR_CONTAINS(md, "&unknownentity;", "unknown entity left literal");
    CHECK_STR_CONTAINS(md, "&ampcopy", "entity without semicolon left literal");
    free(md);
}

static void test_boilerplate_stripped(void) {
    char *md = convert("<html><head><title>T</title><style>body{color:red}</style></head>"
                       "<body><nav>menu</nav><script>alert(1)</script>"
                       "<article><h1>Content</h1><p>real content</p></article>"
                       "<footer>copyright</footer></body></html>");
    CHECK_STR_CONTAINS(md, "real content", "article content kept");
    CHECK_STR_NOT_CONTAINS(md, "alert(1)", "script content removed");
    CHECK_STR_NOT_CONTAINS(md, "color:red", "style content removed");
    CHECK_STR_NOT_CONTAINS(md, "menu", "nav removed");
    CHECK_STR_NOT_CONTAINS(md, "copyright", "footer removed");
    free(md);
}

static void test_title_becomes_h1(void) {
    char *md = convert("<html><head><title>  My   Page  </title></head>"
                       "<body><p>hello</p></body></html>");
    CHECK_STR_CONTAINS(md, "# My Page", "collapsed title becomes h1");
    free(md);
}

static void test_table(void) {
    char *md = convert("<table><tr><th>Name</th><th>Value</th></tr>"
                       "<tr><td>a|b</td><td>1</td></tr>"
                       "<tr><td>c</td><td>2</td></tr></table>");
    CHECK_STR_CONTAINS(md, "| Name | Value |", "header row rendered");
    CHECK_STR_CONTAINS(md, "| --- | --- |", "header separator rendered");
    CHECK_STR_CONTAINS(md, "| a\\|b | 1 |", "pipes in cells escaped");
    CHECK_STR_CONTAINS(md, "| c | 2 |", "second row rendered");
    free(md);
}

static void test_find_by_class(void) {
    const char *html =
        "<div class=\"item first\"><a class=\"inner\" href=\"x?a=1&amp;b=2\">t</a></div>"
        "<div class=\"item\">second &amp; last</div>";
    int count = 0;
    htmlmd_match *m = htmlmd_find_by_class(html, "item", NULL, &count);
    CHECK(m != NULL, "find_by_class finds matches");
    CHECK_EQ(count, 2, "find_by_class count");
    CHECK_STR_EQ(m[0].text, "t", "nested text collected");
    CHECK_STR_EQ(m[1].text, "second & last", "entity decoded in text");
    htmlmd_matches_free(m, count);

    count = 0;
    m = htmlmd_find_by_class(html, "inner", "href", &count);
    CHECK(m != NULL, "inner class found");
    CHECK_STR_EQ(m[0].attr, "x?a=1&b=2", "attribute entity decoded");
    htmlmd_matches_free(m, count);

    count = 0;
    m = htmlmd_find_by_class(html, "missing", NULL, &count);
    CHECK(m == NULL, "no match returns NULL");
    CHECK_EQ(count, 0, "no match count is 0");
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_empty_and_plain();
    test_headings_and_paragraphs();
    test_inline_formatting_links_images();
    test_lists();
    test_code_and_pre();
    test_blockquote_and_hr();
    test_entities();
    test_boilerplate_stripped();
    test_title_becomes_h1();
    test_table();
    test_find_by_class();

    printf("test_htmlmd: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
