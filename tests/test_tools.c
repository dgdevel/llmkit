#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tools.h"
#include "util.h"
#include "platform.h"

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
    CHECK(strcmp(TOOLS_BUILTIN_NAMES[2], "file_scan") == 0, "file_scan registered");
    CHECK(strcmp(TOOLS_BUILTIN_NAMES[3], "exec") == 0, "exec registered");
    CHECK(strcmp(TOOLS_BUILTIN_NAMES[4], "exec_status") == 0, "exec_status registered");
    CHECK(TOOLS_BUILTIN_NAMES[5] == NULL, "builtin list NULL-terminated");
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
/*  file_scan (runs against a throwaway tree under the temp dir)       */
/* ------------------------------------------------------------------ */

static char g_fixture_dir[600];
static char g_orig_cwd[600];

static void fixture_write(const char *rel, const char *data, size_t len) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, rel);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "fixture: cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
    if (len > 0) fwrite(data, 1, len, fp);
    fclose(fp);
}

static void fixture_mkdir(const char *rel) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, rel);
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "fixture: cannot mkdir %s\n", path);
        exit(EXIT_FAILURE);
    }
}

static void setup_fixture(void) {
    if (getcwd(g_orig_cwd, sizeof(g_orig_cwd)) == NULL) exit(EXIT_FAILURE);
    char uniq[64];
    util_uuid_v4(uniq);
    snprintf(g_fixture_dir, sizeof(g_fixture_dir), "%s/llmkit_fs_%s", platform_temp_dir(), uniq);
    if (mkdir(g_fixture_dir, 0755) != 0) {
        fprintf(stderr, "fixture: cannot mkdir %s\n", g_fixture_dir);
        exit(EXIT_FAILURE);
    }
    fixture_mkdir("sub");
    fixture_mkdir("sub/deep");
    fixture_mkdir("hits");

    fixture_write("a.txt", "alpha one\nbeta two\nalpha three\n", 31);
    fixture_write("notes.md", "hello\n", 6);
    fixture_write("sub/a.txt", "x\n", 2);
    fixture_write("sub/deep/d.txt", "needle\nplain\nneedle again\n", 26);
    fixture_write("b.log", "BIN\0ARY\n", 8); /* binary: NUL byte */

    char big[2500];
    memset(big, 'A', sizeof(big));
    fixture_write("big.bin", big, sizeof(big));

    char many[60 * 8];
    size_t many_len = 0;
    char fifty[(50 * 8) + 8];
    size_t fifty_len = 0;
    for (int i = 1; i <= 60; i++) {
        many_len += (size_t)snprintf(many + many_len, sizeof(many) - many_len, "hit %02d\n", i);
        if (i <= 50) {
            fifty_len +=
                (size_t)snprintf(fifty + fifty_len, sizeof(fifty) - fifty_len, "hit %02d\n", i);
        }
    }
    fifty_len += (size_t)snprintf(fifty + fifty_len, sizeof(fifty) - fifty_len, "no match here\n");
    fixture_write("manyhits.txt", many, many_len);
    fixture_write("fifty.txt", fifty, fifty_len);

    /* 60 files with one matching line each (the .dat extension keeps
     * them out of the *.txt globs below), plus 25 files to exercise the
     * 20-record cap. */
    char name[32];
    for (int i = 1; i <= 60; i++) {
        snprintf(name, sizeof(name), "hits/h%02d.dat", i);
        char body[32];
        int n = snprintf(body, sizeof(body), "hit %02d\n", i);
        fixture_write(name, body, (size_t)n);
    }
    for (int i = 1; i <= 25; i++) {
        snprintf(name, sizeof(name), "t%02d.tmp", i);
        char body[16];
        int n = snprintf(body, sizeof(body), "tmp %02d\n", i);
        fixture_write(name, body, (size_t)n);
    }

    if (chdir(g_fixture_dir) != 0) exit(EXIT_FAILURE);
}

static void teardown_fixture(void) {
    chdir(g_orig_cwd);
    /* Best-effort cleanup; failures are ignored. */
    char path[700];
    char name[700];
    /* exec/exec_status tests leave their full-output logs behind. */
    snprintf(path, sizeof(path), "%s/.output", g_fixture_dir);
    DIR *dot = opendir(path);
    if (dot != NULL) {
        struct dirent *ent;
        while ((ent = readdir(dot)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char log[1024];
            snprintf(log, sizeof(log), "%s/%s", path, ent->d_name);
            unlink(log);
        }
        closedir(dot);
    }
    rmdir(path);
    snprintf(path, sizeof(path), "%s/hits", g_fixture_dir);
    for (int i = 1; i <= 60; i++) {
        snprintf(name, sizeof(name), "%s/h%02d.dat", path, i);
        unlink(name);
    }
    rmdir(path);
    for (int i = 1; i <= 25; i++) {
        snprintf(name, sizeof(name), "%s/t%02d.tmp", g_fixture_dir, i);
        unlink(name);
    }
    static const char *const RELS[] = {"a.txt",     "notes.md",       "b.log",
                                       "big.bin",   "manyhits.txt",   "fifty.txt",
                                       "sub/a.txt", "sub/deep/d.txt", NULL};
    for (int i = 0; RELS[i] != NULL; i++) {
        snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, RELS[i]);
        unlink(path);
    }
    rmdir(path); /* sub/deep via the last rels entry's full path */
    snprintf(path, sizeof(path), "%s/sub", g_fixture_dir);
    rmdir(path);
    rmdir(g_fixture_dir);
}

/* Count occurrences of "Path: " records in a report. */
static int count_records(const char *report) {
    int n = 0;
    const char *p = report;
    while ((p = strstr(p, "Path: ")) != NULL) {
        n++;
        p += 6;
    }
    return n;
}

static void test_file_scan_globbing(void) {
    char *err = NULL;

    /* Plain star stays within one directory level. */
    char *out = tools_file_scan("*.txt", NULL, &err);
    CHECK(err == NULL, "glob accepted");
    CHECK(out != NULL, "top-level glob returns results");
    CHECK_STR_CONTAINS(out, "Path: a.txt\nSize: 31b\nLines: 3", "record with size and lines");
    CHECK_STR_NOT_CONTAINS(out, "sub/a.txt", "star does not cross directories");
    CHECK_STR_NOT_CONTAINS(out, g_fixture_dir, "absolute path never disclosed");
    CHECK_STR_NOT_CONTAINS(out, "Matching lines", "no matching-lines field without a regex");
    free(out);

    /* '**' spans any number of directories; output is sorted by path. */
    out = tools_file_scan("**/*.txt", NULL, &err);
    CHECK(out != NULL, "double-star glob returns results");
    CHECK_STR_CONTAINS(out, "Path: sub/deep/d.txt", "double-star reaches nested dirs");
    CHECK_STR_NOT_CONTAINS(out, "big.bin", "pattern extension still filters");
    const char *top = strstr(out, "Path: a.txt");
    const char *nested = strstr(out, "Path: sub/a.txt");
    CHECK(top != NULL && nested != NULL && top < nested, "records sorted by path");
    CHECK(count_records(out) == 5, "five txt files at every depth, under the cap");
    free(out);

    /* A trailing '**' collects everything below the prefix. */
    out = tools_file_scan("sub/**", NULL, &err);
    CHECK(out != NULL, "trailing double-star returns results");
    CHECK_STR_CONTAINS(out, "Path: sub/a.txt", "files directly under the prefix");
    CHECK_STR_CONTAINS(out, "Path: sub/deep/d.txt", "files in nested directories");
    CHECK_STR_NOT_CONTAINS(out, "Path: a.txt", "nothing above the prefix");
    CHECK_STR_NOT_CONTAINS(out, "more files matching", "no truncation note under the cap");
    free(out);

    /* Explicit intermediate directories and './' normalization. */
    out = tools_file_scan("./sub/deep/*.txt", NULL, &err);
    CHECK(out != NULL, "prefixed pattern returns results");
    CHECK_STR_CONTAINS(out, "Path: sub/deep/d.txt", "./ prefix normalized");
    CHECK(count_records(out) == 1, "only the deep file matches");
    free(out);

    /* Backslash separators work like slashes. */
    out = tools_file_scan("sub\\deep\\d.txt", NULL, &err);
    CHECK(out != NULL, "backslash pattern returns results");
    CHECK_STR_CONTAINS(out, "Path: sub/deep/d.txt", "backslashes treated as separators");
    free(out);

    out = tools_file_scan("*.zzz", NULL, &err);
    CHECK(out != NULL, "no-match returns a message, not NULL");
    CHECK_STR_CONTAINS(out, "No files matching: *.zzz", "no-match message names the pattern");
    free(out);
}

static void test_file_scan_content_regex(void) {
    char *err = NULL;

    char *out = tools_file_scan("a.txt", "alpha", &err);
    CHECK(out != NULL, "regex scan returns results");
    CHECK_STR_CONTAINS(out, "Matching lines: 1, 3", "matching line numbers listed");
    free(out);

    /* Files without a matching line are excluded entirely. */
    out = tools_file_scan("**/*.txt", "needle", &err);
    CHECK(out != NULL, "filtered scan returns results");
    CHECK_STR_CONTAINS(out, "Path: sub/deep/d.txt", "matching file kept");
    CHECK_STR_NOT_CONTAINS(out, "Path: a.txt", "non-matching file excluded");
    CHECK_STR_CONTAINS(out, "Matching lines: 1, 3", "lines 1 and 3 contain the needle");
    free(out);

    /* The line-number list is capped at 50 entries with a trailing '+'
     * when one file holds more matches than that. */
    out = tools_file_scan("manyhits.txt", "hit", &err);
    CHECK(out != NULL, "many-match scan returns results");
    CHECK_STR_CONTAINS(out, "49, 50+", "list capped at 50 with a plus sign");
    CHECK_STR_NOT_CONTAINS(out, "51", "no line numbers past the cap");
    CHECK_STR_CONTAINS(out, "Lines: 60", "all lines counted");
    CHECK(count_records(out) == 1, "single file, single record");
    free(out);

    /* Exactly 50 matches in one file: no plus sign. */
    out = tools_file_scan("fifty.txt", "hit", &err);
    CHECK(out != NULL, "fifty-match scan returns results");
    CHECK_STR_CONTAINS(out, "49, 50", "first fifty matching lines listed");
    CHECK_STR_NOT_CONTAINS(out, "+", "no plus sign at or below the cap");
    free(out);

    /* The 20-record cap also applies under a content filter. */
    out = tools_file_scan("hits/**", "hit", &err);
    CHECK(out != NULL, "many-file scan returns results");
    CHECK(count_records(out) == 20, "record cap hit for 60 matching files");
    CHECK_STR_CONTAINS(out, "40 more files matching", "truncation note counts the rest");
    CHECK_STR_NOT_CONTAINS(out, "+", "single match per file lists no plus sign");
    free(out);

    err = NULL;
    out = tools_file_scan("*.txt", "[", &err);
    CHECK(out == NULL && err != NULL, "invalid regex rejected");
    CHECK_STR_CONTAINS(err, "content_lines_regex", "invalid regex names the argument");
    free(err);
}

static void test_file_scan_binary_and_sizes(void) {
    char *err = NULL;

    /* Binary files are listed without a Lines field. */
    char *out = tools_file_scan("b.log", NULL, &err);
    CHECK(out != NULL, "binary file listed");
    CHECK_STR_CONTAINS(out, "Path: b.log\nSize: 8b", "binary record has path and size");
    CHECK_STR_NOT_CONTAINS(out, "Lines:", "binary record omits line count");
    free(out);

    /* ... and can never satisfy a content filter. */
    out = tools_file_scan("b.log", "BIN", &err);
    CHECK(out != NULL, "binary scan returns a message");
    CHECK_STR_CONTAINS(out, "No files matching: b.log", "binary excluded from regex scan");
    free(out);

    /* Size formatting crosses unit boundaries. */
    out = tools_file_scan("big.bin", NULL, &err);
    CHECK_STR_CONTAINS(out, "Size: 2.4Kb", "kilobyte formatting");
    CHECK_STR_CONTAINS(out, "Lines: 1", "final line without a newline still counted");
    free(out);

    /* The 20-record cap with the exact remaining count. */
    out = tools_file_scan("*.tmp", NULL, &err);
    CHECK(out != NULL, "tmp scan returns results");
    CHECK(count_records(out) == 20, "results capped at 20 records");
    CHECK_STR_CONTAINS(out, "5 more files matching", "truncation note with exact count");
    free(out);
}

static void test_file_scan_errors(void) {
    char *err = NULL;

    char *out = tools_file_scan("sub/../a.txt", NULL, &err);
    CHECK(out == NULL && err != NULL, "inner '..' rejected");
    CHECK_STR_CONTAINS(err, "'..'", "error names the '..' rule");
    free(err);

    err = NULL;
    out = tools_file_scan("../outside", NULL, &err);
    CHECK(out == NULL && err != NULL, "leading '..' rejected");
    free(err);

    err = NULL;
    out = tools_file_scan("/etc/passwd", NULL, &err);
    CHECK(out == NULL && err != NULL, "absolute pattern rejected");
    CHECK_STR_CONTAINS(err, "relative", "error names the relative-path rule");
    free(err);

    err = NULL;
    out = tools_file_scan("", NULL, &err);
    CHECK(out == NULL && err != NULL, "empty pattern rejected");
    free(err);
}

/* ------------------------------------------------------------------ */
/*  exec / exec_status (runs inside the fixture cwd)                   */
/* ------------------------------------------------------------------ */

static void test_exec_basic(void) {
    char *err = NULL;

    char *out = tools_exec("echo hello", &err);
    CHECK(err == NULL, "plain exec succeeds");
    CHECK(out != NULL, "exec returns a report");
    CHECK_STR_CONTAINS(out, "Exit code: 0\n", "zero exit code reported");
    CHECK_STR_CONTAINS(out, "Duration: ", "duration reported");
    CHECK_STR_CONTAINS(out, "Output:\nhello\n", "stdout captured");
    CHECK_STR_NOT_CONTAINS(out, "Output truncated", "small output not truncated");
    free(out);

    err = NULL;
    out = tools_exec("echo oops >&2; exit 3", &err);
    CHECK(err == NULL, "exec with stderr succeeds");
    CHECK_STR_CONTAINS(out, "Exit code: 3\n", "nonzero exit code reported");
    CHECK_STR_CONTAINS(out, "oops\n", "stderr captured with stdout");
    free(out);

    /* The cmdline is one shell string, not a tokenized argv: quotes,
     * pipes and multiple spaces must survive intact. */
    err = NULL;
    out = tools_exec("printf '%s\\n' 'a  b' | tr a-z A-Z", &err);
    CHECK_STR_CONTAINS(out, "A  B\n", "quotes and pipes work in the subshell");
    free(out);
}

static void test_exec_truncation(void) {
    char *err = NULL;

    /* 20000 numbered lines (~210KB): far past the 3KB tail window. */
    char *out = tools_exec("awk 'BEGIN{for(i=1;i<=20000;i++) print \"line\",i}'", &err);
    CHECK(err == NULL, "big exec succeeds");
    CHECK_STR_CONTAINS(out, "Exit code: 0\n", "big exec exit code reported");
    CHECK_STR_CONTAINS(out, "Output truncated, full output in file .output/exec_",
                       "truncation note names the .output file");
    CHECK_STR_CONTAINS(out, "Kb, 20000 lines)", "note carries size and line count");
    CHECK_STR_CONTAINS(out, "line 20000\n", "tail reaches the last line");
    CHECK(strstr(out, "Output:\nline 1\n") == NULL, "head of the output is not included");
    free(out);

    /* Binary output: no line count in the note, and the report stays a
     * valid C string despite NUL bytes in the tail. */
    err = NULL;
    out = tools_exec("head -c 100000 /dev/zero", &err);
    CHECK_STR_CONTAINS(out, "Output truncated, full output in file .output/exec_",
                       "binary truncation note present");
    CHECK_STR_CONTAINS(out, "b)", "binary note carries a plain byte size");
    CHECK(strstr(out, " lines)") == NULL, "binary output has no line count");
    free(out);
}

static void test_exec_status_lifecycle(void) {
    char *err = NULL;

    /* A command past the 10s wait is left running and reported by pid. */
    char *out = tools_exec("sleep 30", &err);
    CHECK(err == NULL, "background exec accepted");
    CHECK_STR_CONTAINS(out, "PID: ", "background reply names the pid");
    CHECK_STR_CONTAINS(out, "Process still running, use exec_status(",
                       "background reply points at exec_status");
    CHECK_STR_NOT_CONTAINS(out, "Exit code:", "no exit code while running");
    int pid = (int)strtol(strstr(out, "PID: ") + 5, NULL, 10);
    CHECK(pid > 0, "pid parseable from the reply");
    free(out);

    err = NULL;
    char *st = tools_exec_status(pid, &err);
    CHECK_STR_CONTAINS(st, "still running, started ", "status reports the running pid");
    CHECK(strstr(st, "Exit code:") == NULL, "still no exit code while running");
    free(st);

    /* Kill it: the next status reaps it and reports the signal death as
     * 128+signal, with the (empty) output section. SIGKILL is async, so
     * poll like a real caller until the exit shows up. */
    err = NULL;
    CHECK(kill(pid, SIGKILL) == 0, "background command killed");
    st = NULL;
    for (int i = 0; i < 100; i++) {
        free(st);
        err = NULL;
        st = tools_exec_status(pid, &err);
        if (strstr(st, "Exit code: ") != NULL) break;
        usleep(10 * 1000);
    }
    CHECK_STR_CONTAINS(st, "Exit code: 137", "signal death reported as 128+signal");
    CHECK_STR_CONTAINS(st, "Duration: ", "duration reported after termination");
    CHECK_STR_CONTAINS(st, "Output:\n", "output section present after termination");
    /* The same answer repeats on a second poll. */
    err = NULL;
    char *st2 = tools_exec_status(pid, &err);
    CHECK_STR_CONTAINS(st2, "Exit code: 137", "status repeatable after termination");
    free(st2);
    free(st);

    err = NULL;
    st = tools_exec_status(999999, &err);
    CHECK_STR_CONTAINS(st, "No exec process with pid 999999", "unknown pid answered plainly");
    free(st);
}

static void test_exec_errors(void) {
    char *err = NULL;

    CHECK(tools_exec("", &err) == NULL, "empty cmdline rejected");
    CHECK(err != NULL && strstr(err, "cmdline") != NULL, "empty cmdline error names the argument");
    free(err);
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_registry_names();
    test_parse_results();
    test_parse_no_results();
    test_run_rejects_bad_lists();
    setup_fixture();
    test_file_scan_globbing();
    test_file_scan_content_regex();
    test_file_scan_binary_and_sizes();
    test_file_scan_errors();
    test_exec_basic();
    test_exec_truncation();
    test_exec_status_lifecycle();
    test_exec_errors();
    teardown_fixture();

    printf("test_tools: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
