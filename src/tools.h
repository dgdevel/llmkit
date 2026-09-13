#ifndef TOOLS_H
#define TOOLS_H

#include "llmkit.h"

/*
 * llmkit mcp -- MCP server exposing llmkit's built-in tools.
 *
 * Unlike `proxy` and `gateway`, no backend MCP servers or LLM config are
 * involved: the tools are implemented inside llmkit itself. The set of
 * exposed tools is chosen on the command line with a comma-separated list,
 * e.g. `llmkit mcp online_search,online_fetch`. Serves MCP over stdio
 * (default) or HTTP (`-l host:port`), like proxy/gateway.
 *
 * Built-in tools:
 *   online_search  -- web search via the free DuckDuckGo HTML endpoint
 *   online_fetch   -- fetch a URL and return its content as markdown
 *                     (readability-style stripping + HTML-to-markdown)
 *   file_scan      -- match files against a name glob relative to the
 *                     working directory, optionally filtered by a
 *                     per-line POSIX regex
 *   exec           -- run a shell command line in a subshell, reporting
 *                     exit code, duration and the tail of the combined
 *                     output; commands still running after 10 seconds
 *                     keep running in the background and are polled by
 *                     pid through exec_status (enabled together with
 *                     exec)
 *   exec_status    -- report a background exec command's exit code,
 *                     duration and output once finished, or how long it
 *                     has been running so far
 */

/* NULL-terminated list of the available built-in tool names. */
extern const char *const TOOLS_BUILTIN_NAMES[];

/* Parse a DuckDuckGo HTML results page and render the results in the
 * user-visible list format (Title/URL/Description entries separated by
 * blank lines), unwrapping DDG's redirect links. query is used only for
 * the "no results" message. Returns a malloc'd string, or NULL on OOM. */
char *tools_parse_search_results(const char *html, const char *query);

/* Run a file_scan query against the process working directory.
 * glob_pattern is a mandatory relative glob ('**' spans directories,
 * '*' and '?' are single-segment wildcards; absolute paths and any ".."
 * segment are rejected). lines_regex is an optional POSIX extended
 * regex: when given, only files with at least one matching line are
 * returned, each with its matching line numbers. Returns a malloc'd
 * report (records separated by blank lines, capped at 20 records with a
 * "<N> more files matching" note), or NULL with *out_err set when the
 * pattern or regex is invalid. Exported for tests. */
char *tools_file_scan(const char *glob_pattern, const char *lines_regex, char **out_err);

/* Run cmdline in a subshell ("/bin/sh -c" on POSIX, "cmd.exe /c" on
 * Windows) with stdin detached and stdout+stderr captured to a file
 * under .output/ in the working directory. Returns a malloc'd report:
 * "Exit code / Duration / Output" plus the last 3KB of output (cut at
 * the first newline) once the command finished within 10 seconds, or a
 * "PID: N / still running" note leaving it in the background. Returns
 * NULL with *out_err set for an empty cmdline or a spawn failure.
 * Exported for tests. */
char *tools_exec(const char *cmdline, char **out_err);

/* Report on a command previously started by tools_exec: the same
 * "Exit code / Duration / Output" report once it has terminated (a
 * signal death reports 128+signal), or "PID N still running, started
 * <duration> ago." while it has not. An unknown pid gets a plain "No
 * exec process with pid N." answer. Returns a malloc'd string, NULL
 * only on OOM. Exported for tests. */
char *tools_exec_status(int pid, char **out_err);

/* Run the built-in tools MCP server. tool_list is a comma-separated list
 * of names from TOOLS_BUILTIN_NAMES (required; unknown names are rejected
 * with EXIT_ARGS_ERR). listen_addr follows the srv_serve convention: NULL
 * or empty serves stdio, "host:port" serves HTTP. */
int tools_run(const char *tool_list, const char *listen_addr);

#endif /* TOOLS_H */
