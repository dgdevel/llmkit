#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "llmkit.h"
#include <stdarg.h>

/*
 * Open (or create) the conversation JSONL file for appending.
 * Validates UTF-8 on any existing content.  Returns EXIT_SUCCESS
 * or EXIT_FILE_ERR / EXIT_INTERNAL_ERR.
 */
int conversation_open(const char *path, FILE **out_fp);

/*
 * Write the mandatory first-line meta entry (type "meta").
 */
int conversation_write_meta(FILE *fp, const char *config_hash, const char *run_id);

/*
 * Write one entry of the given type.  Variadic arguments depend on type:
 *
 *   ENTRY_USER:       content (const char*), source (const char*)
 *   ENTRY_ASSISTANT:  content (const char*), model (const char*),
 *                     usage (const usage_info* - may be NULL)
 *   ENTRY_TOOL_CALL:  id (const char*), name (const char*),
 *                     arguments (const char*), mcp_server (const char*)
 *   ENTRY_TOOL_RESULT: call_id (const char*), name (const char*),
 *                      result (const char*), is_error (int),
 *                      is_timeout (int), mcp_server (const char*)
 *   ENTRY_ERROR:      code (int), message (const char*),
 *                     recoverable (int)
 *
 * Returns EXIT_SUCCESS or EXIT_FILE_ERR.  Flushes after each write.
 */
int conversation_write_entry(FILE *fp, entry_type type, ...);

/*
 * Reconstruct the LLM message array from JSONL history.
 * Skips meta and error entries.  Groups tool_call entries with
 * their preceding assistant message and creates tool messages
 * from matching tool_result entries.
 *
 * The caller must free the returned array with conversation_free_messages().
 * Returns EXIT_SUCCESS or an error code.
 */
int conversation_reconstruct(const char *path, json_message **out_msgs, int *out_count);

/*
 * Free a message array returned by conversation_reconstruct().
 */
void conversation_free_messages(json_message *msgs, int count);

/*
 * Read a conversation JSONL file and find the last "assistant" entry.
 * Returns its "content" field in *out_content (caller must free).
 * If no assistant entry exists, *out_content is set to an empty string.
 * Returns EXIT_SUCCESS or EXIT_FILE_ERR / EXIT_INTERNAL_ERR.
 */
int conversation_read_last_assistant(const char *path, char **out_content, char **out_model,
                                     usage_info *out_usage);

#endif /* CONVERSATION_H */
