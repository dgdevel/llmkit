#include "llm.h"

int llm_chat_complete(runtime_ctx *ctx,
                      const void *messages, int msg_count,
                      tool_def *tools, int tool_count,
                      char **out_content, char **out_model,
                      void **out_calls, int *out_call_count,
                      void *usage) {
    (void)ctx; (void)messages; (void)msg_count;
    (void)tools; (void)tool_count;
    (void)out_content; (void)out_model;
    (void)out_calls; (void)out_call_count; (void)usage;
    return EXIT_SUCCESS;
}
