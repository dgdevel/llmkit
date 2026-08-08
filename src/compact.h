#ifndef COMPACT_H
#define COMPACT_H

#include "llmkit.h"

/*
 * Prefix-cache-aware compaction.
 *
 * Long conversations are replaced (in the request only) by a projection:
 * the pinned leading messages (system prompt + first user message, verbatim)
 * plus one rolling summary, followed by the recent tail. The canonical
 * conversation JSONL is never rewritten - the projection is persisted in a
 * sidecar file (<conversation>.context.json) so every subsequent turn reuses
 * the exact same provider-visible prefix and only appends. Provider prefix
 * caches (e.g. DeepSeek's automatic prefix cache) therefore stay warm; the
 * first request after compaction is a deliberate cache-reset point.
 *
 * The sidecar records the number of canonical messages covered and a SHA256
 * of their serialized provider-visible bytes, so an edited or rewritten
 * history (or a model change) invalidates the projection fail-closed.
 */

/* Estimate the token count of a provider-visible message array using a
 * bytes/4 heuristic (returns >= 1 so it is never zero). */
int64_t compact_estimate_tokens(const json_message *msgs, int msg_count);

/*
 * Apply compaction to a provider-visible message array.
 *
 *   ctx         - runtime context (agent.compact.* settings, llm config)
 *   convo_path  - canonical conversation JSONL path (sidecar derives from it)
 *   in/out_msgs - on entry the reconstructed messages (system prompt already
 *                 prepended at index 0 when configured); on success possibly
 *                 replaced by projection + tail (caller frees with
 *                 conversation_free_messages)
 *   in/out_count - message count, updated to match
 *   out_covered - set to the number of canonical messages the projection
 *                 replaces (0 when compaction did not apply)
 *
 * Returns EXIT_SUCCESS or an EXIT_* error code. When compaction is disabled,
 * when no valid sidecar exists and the estimate is under the threshold, or
 * when the sidecar cannot be persisted, the array is left unchanged.
 */
int compact_apply(runtime_ctx *ctx, const char *convo_path, json_message **in_out_msgs,
                  int *in_out_count, int *out_covered);

#endif /* COMPACT_H */
