/* fuzz/fuzz_html.c - the builtin web tool html pipeline: the
 * readability-style reducer and the duckduckgo results parser
 * (src/builtin/web.c). Both take (ptr,len): no NUL requirement.
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* first byte routes: even -> markdown, odd -> ddg results */
    if (size == 0) return 0;
    int mode = data[0] & 1;
    data++; size--;
    char *out = mode
        ? builtin_ddg_results((const char *)data, size)
        : builtin_html_to_markdown((const char *)data, size);
    free(out);
    return 0;
}
