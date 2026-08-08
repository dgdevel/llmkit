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
 *   out_reasoning - set to the assistant's reasoning text (malloc'd, "" if empty/absent)
 *   out_model    - set to the model string from the response (malloc'd)
 *   out_calls    - set to an array of tool_call structs, or NULL if none
 *   out_call_count - set to the number of tool calls
 *   usage        - filled with token counts from the response (may be NULL)
 *
 * Returns EXIT_SUCCESS or EXIT_LLM_ERR (exit code 4) on error.
 * The caller must free *out_content, *out_reasoning, *out_model, and *out_calls
 * (including nested strings).
 */
int llm_chat_complete(runtime_ctx *ctx, const json_message *messages, int msg_count,
                      const tool_def *tools, int tool_count, char **out_content,
                      char **out_reasoning, char **out_model, tool_call **out_calls,
                      int *out_call_count, usage_info *usage);

/*
 * Serialize a chat-completion request body (JSON) from messages and tools.
 *
 * Exposed for tests: the serialization must be deterministic (identical input
 * always yields identical bytes) and append-only (a grown message history must
 * keep the serialization of the earlier messages byte-for-byte identical), so
 * provider prefix caches stay warm across turns.
 *
 * Returns a malloc'd JSON string, or NULL on allocation failure.
 * The caller must free the result.
 */
char *llm_build_request_body(const json_message *msgs, int msg_count, const tool_def *tools,
                             int tool_count, const llm_cfg *cfg);

/*
 * Parse a chat-completions response body into content, reasoning, model,
 * tool calls and usage.
 *
 * Exposed for tests: also fills the prefix-cache telemetry fields of usage
 * (DeepSeek top-level prompt_cache_hit_tokens / prompt_cache_miss_tokens and
 * OpenAI prompt_tokens_details.cached_tokens), leaving them zero when absent.
 *
 * Returns EXIT_SUCCESS, EXIT_LLM_ERR on a malformed/error response, or
 * EXIT_INTERNAL_ERR on allocation failure. The caller must free *out_content,
 * *out_reasoning, *out_model and *out_calls (including nested strings).
 */
int llm_parse_response(const char *body, char **out_content, char **out_reasoning, char **out_model,
                       tool_call **out_calls, int *out_call_count, usage_info *usage);

/*
 * Serialize just the "messages" array of a chat-completion request body.
 *
 * Uses the same deterministic serialization as llm_build_request_body, so the
 * bytes are exactly what a provider would see for those messages. Used by the
 * prefix-cache-aware compactor to hash the covered prefix and to persist the
 * projection in the sidecar.
 *
 * Returns a malloc'd JSON array string, or NULL on failure.
 * The caller must free the result.
 */
char *llm_serialize_messages(const json_message *msgs, int msg_count, const llm_cfg *cfg);

#endif /* LLM_H */
