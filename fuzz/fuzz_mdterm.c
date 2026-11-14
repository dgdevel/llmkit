/* fuzz/fuzz_mdterm.c - the markdown terminal renderer, fed arbitrary
 * bytes.
 *
 * targets md_render (whole-document, md4c callbacks) and the streaming
 * line machine (md_line_feed/close) with random chunk splits, the two
 * paths the repl/pretty/call sinks render LLM output through. the
 * sink writes into a memory stream; NO_COLOR is forced so the runs
 * exercise the plain-degradation path alike (styled probing needs a
 * tty the fuzzer has none of anyway).
 */
#include "llmkit.h"
#include "mdterm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    setenv("NO_COLOR", "1", 1);
    static style_t st; /* zeroed: plain, deterministic */
    memset(&st, 0, sizeof st);

    /* pass 1: the whole-document renderer over the input */
    {
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        if (f) {
            dspy_t s;
            memset(&s, 0, sizeof s);
            s.out = f;
            s.st = &st;
            s.cur = -1;
            md_render(&s, (const char *)data, (size_t)size);
            fclose(f);
            free(buf);
        }
    }

    /* pass 2: the line machine, split at a data-derived point, then
       the rest byte-by-byte (the wires' worst delta pattern) */
    {
        char *buf = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&buf, &len);
        if (f) {
            dspy_t s;
            memset(&s, 0, sizeof s);
            s.out = f;
            s.st = &st;
            s.cur = -1;
            md_line_t m;
            md_line_init(&m, &s);
            size_t cut = size > 1 ? data[0] % size : 0;
            md_line_feed(&m, (const char *)data, cut);
            for (size_t i = cut; i < size; i++)
                md_line_feed(&m, (const char *)data + i, 1);
            md_line_close(&m);
            md_line_free(&m);
            fclose(f);
            free(buf);
        }
    }
    return 0;
}
