/* fuzz/fuzz_mcp_tools.c - the builtin-mcp tools/call path with the
 * REAL tool functions (files, process, web, skills) invoked on fuzzer
 * bytes.
 *
 * !! DOCKER/VM ONLY !!  this harness executes whatever the fuzzer
 * crafts: file writes/deletes, arbitrary shell command lines, http
 * fetches. run it only inside the throwaway container from
 * fuzz/docker-fuzz.sh (no network, private mounts, cgroup caps).
 *
 * input: 1 selector byte, then a json object spread over the rest:
 *   {"tool":"<name>","arguments":{...}}   (compact, no padding needed)
 * the object is parsed with cJSON; on parse failure the input is
 * skipped (the rpc framing itself is fuzz_mcp's business).
 */
#include "llmkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return 0;
    static const char *TOOLS[] = {
        "web_search", "web_fetch", "download",
        "files_list", "files_search", "file_read", "file_create",
        "file_edit", "file_delete", "file_copy", "file_move",
        "file_analyze",
        "process_exec", "process_status", "process_wait",
        "process_send_signal",
        "skills_search", "skills_read", "now",
    };
    const char *tool = TOOLS[data[0] % 19];
    data++; size--;

    /* build params around the fuzzer-owned arguments object */
    buf_t req;
    buf_init(&req);
    buf_appendf(&req, "{\"name\":\"%s\",\"arguments\":", tool);
    buf_append(&req, (const char *)data, size);
    buf_append_str(&req, "}");

    cJSON *params = cJSON_ParseWithLength(req.data ? req.data : "{}",
                                          req.len + 1);
    buf_free(&req);
    if (!params || !cJSON_IsObject(params)) {
        cJSON_Delete(params);
        return 0;
    }

    cJSON *result = NULL;
    char *errmsg = NULL;
    cJSON *id = cJSON_CreateNumber(1);
    builtin_handle(NULL, "tools/call", params, id, &result, &errmsg);
    cJSON_Delete(params);
    cJSON_Delete(id);
    if (result) cJSON_Delete(result);
    free(errmsg);
    return 0;
}
