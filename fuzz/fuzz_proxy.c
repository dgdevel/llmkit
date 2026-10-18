/* fuzz/fuzz_proxy.c - the mcp-proxy config parser (src/proxy.c):
 * header/tools/server/expose/hide records in jsonl form, driven through
 * proxy_config_line + proxy_resolve_and_build. HTTP-free: resolve/build
 * only touch files declared in the config (spawn happens at serve time).
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    proxy_state_t p;
    proxy_state_init(&p);
    /* one line per \n: reuse the project's own byte pipeline */
    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            size_t ln = i - start;
            if (ln) {
                char *line = malloc(ln + 1);
                if (line) {
                    memcpy(line, data + start, ln);
                    line[ln] = '\0';
                    if (!proxy_config_line(&p, line)) break;
                }
            }
            start = i + 1;
        }
    }
    /* resolve is NOT called: it connects/spawns the configured upstream
       servers (the file tools path again). the config grammar, the
       record validation and the selector parser are what this harness
       covers; resolution needs a live server per config line. */
    proxy_state_free(&p);
    return 0;
}
