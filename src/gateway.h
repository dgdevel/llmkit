#ifndef GATEWAY_H
#define GATEWAY_H

#include "llmkit.h"

/*
 * llmkit gateway -- MCP server that fronts one or more backend MCP servers
 * but exposes exactly two tools:
 *
 *   discover(query)      -- uses the configured LLM to select the backend
 *                          tools relevant to a natural-language query and
 *                          returns their full specs (name, description,
 *                          input schema, backend server). Falls back to
 *                          keyword matching when the LLM call fails.
 *   invoke(name, args)   -- forwards the call to the backend MCP tool
 *                          identified by its namespaced name.
 *
 * Serves MCP over stdio (default) or HTTP (`-l host:port`), like the proxy.
 * Requires `llm` (for discover) and `mcps` sections in the config.
 */
int gateway_run(runtime_ctx *ctx, const char *listen_addr);

#endif /* GATEWAY_H */
