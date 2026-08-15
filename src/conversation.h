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
 *   ENTRY_ASSISTANT:  content (const char*), reasoning (const char*),
 *                     model (const char*), usage (const usage_info* - may be NULL)
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
 * conversation_write_entry with a subagent scope: the entry is stamped with
 * "depth", "subagent" and "run_id" fields (see conv_scope). Same variadic
 * arguments as conversation_write_entry for the entry types that may appear
 * inside a subagent trace (user, assistant, tool_call, tool_result, error).
 */
int conversation_write_scoped(FILE *fp, const conv_scope *scope, entry_type type, ...);

/*
 * Write a subagent_start / subagent_end bracket entry for a subagent run
 * identified by scope. start carries the originating call_id and the raw
 * tool-call arguments; end carries the number of turns executed and the
 * final is_error flag. Both are skipped during reconstruction.
 */
int conversation_write_subagent_start(FILE *fp, const conv_scope *scope, const char *call_id,
                                      const char *arguments);
int conversation_write_subagent_end(FILE *fp, const conv_scope *scope, int turns, int is_error);

/*
 * Reconstruct the LLM message array from JSONL history.
 * Skips meta, error, subagent_start/end and any scoped (subagent) entries -
 * only top-level entries contribute messages. Groups tool_call entries with
 * their preceding assistant message and creates tool messages from matching
 * tool_result entries.
 *
 * The caller must free the returned array with conversation_free_messages().
 * Returns EXIT_SUCCESS or an error code.
 */
int conversation_reconstruct(const char *path, json_message **out_msgs, int *out_count);

/*
 * Reconstruct the LLM message array for one conversation scope.
 * run_id == NULL: top-level entries only (subagent traces excluded) -
 * identical to conversation_reconstruct. run_id != NULL: only the entries
 * of that subagent run (its interpolated user prompt, assistant replies,
 * tool calls and results), so a nested agent rebuilds exactly its own
 * history from the shared conversation file.
 */
int conversation_reconstruct_scope(const char *path, const char *run_id, json_message **out_msgs,
                                   int *out_count);

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
