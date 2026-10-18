/* fuzz/fuzz_sse.c - the shared SSE parser, fed in random chunks.
 *
 * targets sse_init/sse_feed/sse_eof/sse_free with arbitrary byte splits
 * to exercise the chunk-safety guarantee (sse.c header comment).
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void cb(void *ctx, const char *event, const char *data, size_t n) {
    (void)ctx; (void)event; (void)data; (void)n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* split point from the first bytes: keeps runs deterministic */
    size_t cut = 0;
    if (size > 1) {
        cut = data[0] % size;
        data++; size--;
    }
    sse_parser_t p;
    sse_init(&p, cb, NULL);
    sse_feed(&p, (const char *)data, cut);
    sse_feed(&p, (const char *)data + cut, size - cut);
    sse_eof(&p);
    sse_free(&p);
    return 0;
}
