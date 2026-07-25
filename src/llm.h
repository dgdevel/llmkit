#ifndef LLM_H
#define LLM_H

#include "llmkit.h"

int llm_chat_complete(runtime_ctx *ctx, const void *messages, int msg_count, tool_def *tools,
                      int tool_count, char **out_content, char **out_model, void **out_calls,
                      int *out_call_count, void *usage);

#endif
