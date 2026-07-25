#ifndef LLM_H
#define LLM_H

#include "llmkit.h"

/*
 * Send a chat completion request to the OpenAI-compatible API.
 *
 * Builds a JSON body from the given messages and tools, POSTs it to
 * {api_base}/chat/completions via libcurl, and parses the response.
 *
 * Parameters:
 *   ctx          - runtime context (llm.api_base, llm.api_key, llm.model, llm.headers)
 *   messages     - array of chat messages (may include system prompt in msg[0])
 *   msg_count    - number of messages
 *   tools        - tool definitions (may be NULL if tool_count == 0)
 *   tool_count   - number of tool definitions
 *   out_content  - set to the assistant's reply text (malloc'd, "" if empty/absent)
 *   out_model    - set to the model string from the response (malloc'd)
 *   out_calls    - set to an array of tool_call structs, or NULL if none
 *   out_call_count - set to the number of tool calls
 *   usage        - filled with token counts from the response (may be NULL)
 *
 * Returns EXIT_SUCCESS or EXIT_LLM_ERR (exit code 4) on error.
 * The caller must free *out_content, *out_model, and *out_calls (including
 * nested strings).
 */
int llm_chat_complete(runtime_ctx *ctx, const json_message *messages, int msg_count,
                      const tool_def *tools, int tool_count, char **out_content, char **out_model,
                      tool_call **out_calls, int *out_call_count, usage_info *usage);

#endif /* LLM_H */
