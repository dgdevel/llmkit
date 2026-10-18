/* fuzz/fuzz_jsonl.c - the jsonl byte pipeline: CR-drop, line split,
 * strict utf-8, embedded-NUL rejection, then cJSON parse + record
 * classification + validators on each complete line.
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void on_line(void *ctx, char *line) {
    (void)ctx;
    cJSON *t = jsonl_parse_line(line);
    free(line);
    if (!t) return;
    int k = rec_classify(t);
    switch (k) {
    case R_LLM:
        free(validate_llm(t));
        break;
    case R_TOOLS:
        free(validate_tools(t));
        break;
    case R_OPTIONS:
        free(validate_options(t));
        break;
    case R_SYSTEM:
    case R_USER:
        free(validate_content(
            cJSON_GetObjectItemCaseSensitive(t, "content")));
        break;
    default:
        break;
    }
    cJSON_Delete(t);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, on_line, NULL);
    /* chunk the input at a data-derived offset: the hold-buffer
       boundary logic sees arbitrary splits */
    size_t cut = 0;
    if (size > 1) {
        cut = data[0] % size;
        data++; size--;
    }
    jsonl_feed(&p, (const char *)data, cut);
    jsonl_feed(&p, (const char *)data + cut, size - cut);
    jsonl_eof(&p);
    jsonl_pusher_free(&p);
    return 0;
}
