/* fuzz/fuzz_regex.c - the builtin tools' posix-ERE translation layer
 * (src/builtin/util.c): \d \w \s and friends, (?:...) groups -> plain
 * ere, then regcomp + regexec via builtin_regex_match.
 */
#include "llmkit.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* input = pattern \0 text (no NUL -> all pattern, empty text).
       both halves are copied out: libFuzzer buffers are not
       NUL-terminated and builtin_regex_match calls strlen. */
    char pat[512], txt[512];
    const uint8_t *nul = memchr(data, 0, size);
    size_t plen = nul ? (size_t)(nul - data) : size;
    size_t tlen = nul ? size - plen - 1 : 0;
    if (plen >= sizeof pat) plen = sizeof pat - 1;
    if (tlen >= sizeof txt) tlen = sizeof txt - 1;
    memcpy(pat, data, plen);
    pat[plen] = '\0';
    if (tlen) memcpy(txt, nul + 1, tlen);
    txt[tlen] = '\0';
    char err[128];
    builtin_regex_match(pat, txt, err, sizeof err);
    return 0;
}
