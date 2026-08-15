#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "llmkit.h"

/* Sentinel mcp_server value marking a tool_def backed by a subagent instead
 * of a real MCP backend. */
#define SUBAGENT_TOOL_SERVER "subagent"

/* Build the JSON input schema for a subagent tool definition:
 * {"type":"object","properties":{...},"required":[...]}. Deterministic:
 * properties and required entries follow YAML insertion order so the
 * serialized tool block stays byte-stable (provider prefix caches).
 * Returns a malloc'd JSON string; caller frees. NULL on allocation
 * failure. */
char *subagent_build_input_schema(const subagent_tool_def *tool);

/* Merge the given subagent list into ctx->tools as synthetic tool
 * definitions (mcp_server = SUBAGENT_TOOL_SERVER) and re-sort the merged
 * array. Returns EXIT_CONFIG_ERR on a name collision with an already
 * registered tool, EXIT_INTERNAL_ERR on allocation failure. */
int subagent_register_tools_list(runtime_ctx *ctx, const subagent_spec *specs, int count);

/* subagent_register_tools_list for the root-level subagents of ctx. */
int subagent_register_tools(runtime_ctx *ctx);

/* Find a subagent spec by tool name within a sibling list. */
subagent_spec *subagent_find(subagent_spec *arr, int count, const char *tool_name);

/* Replace {attribute} placeholders in a template with values from the tool
 * call arguments JSON: string attributes interpolate raw text, other JSON
 * types their literal, missing (optional) attributes the empty string.
 * Unknown placeholders are left untouched. Returns a malloc'd string, or
 * NULL on allocation failure. Exposed for tests. */
char *subagent_interp(const char *tmpl, const subagent_spec *spec, const char *args_json);

/* Run a subagent (agent-as-tool): interpolate {attribute} placeholders in
 * its system_prompt/user_prompt from the tool-call arguments JSON, lazily
 * connect its private MCP servers (name-only references reuse the running
 * top-level connections), run a nested conversation loop with the same LLM
 * config as the parent agent, and return the subagent's final assistant
 * content in *out_result (malloc'd).
 *
 * The nested loop is silent on stdout (diagnostics go to stderr via
 * log_activity), uses the parent's retry policy (Fibonacci backoff) and a
 * 50-turn safety limit, and may dispatch to the subagent's own subagents
 * (depth + 1, capped by SUBAGENT_MAX_DEPTH).
 *
 * Returns EXIT_SUCCESS with *out_is_error set for soft failures (LLM or
 * tool errors delivered to the caller as a tool result), EXIT_MCP_ERR to
 * propagate a fatal inner tool failure, or EXIT_INTERNAL_ERR. */
int subagent_call(const runtime_ctx *parent, subagent_spec *spec, const char *args_json, int depth,
                  int max_retries, char **out_result, bool *out_is_error);

#endif
