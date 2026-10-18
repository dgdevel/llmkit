/* fuzz/fuzz_mcp.c - the json-rpc dispatcher shared by builtin-mcp,
 * mcp-proxy and agent-as-tool, fed method/params/id directly.
 *
 * SECURITY: restricted to pure methods (initialize, tools/list,
 * ping, unknown methods, malformed params). tools/call would hand
 * fuzzer bytes to the file/process/web tools - never fuzz that
 * without a sandbox.
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    /* first byte: method selector. everything after is the params json */
    static const char *methods[] = { "initialize", "ping", "tools/list",
                                     "resources/list", "no/such/method" };
    const char *method = methods[data[0] % 5];
    data++; size--;
    cJSON *params = NULL;
    if (size) {
        params = cJSON_ParseWithLength((const char *)data, size);
        if (!params) return 0;
        if (!cJSON_IsObject(params) && !cJSON_IsNull(params)) {
            cJSON_Delete(params);
            return 0;
        }
    }
    cJSON *result = NULL;
    char *errmsg = NULL;
    cJSON *id = cJSON_CreateNumber(1);
    builtin_handle(NULL, method, params, id, &result, &errmsg);
    cJSON_Delete(params);
    cJSON_Delete(id);
    if (result) cJSON_Delete(result);
    free(errmsg);
    return 0;
}
