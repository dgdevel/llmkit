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
 *   sleep          -- wait up to 60 seconds (fractions allowed) before
 *                     replying
 *   file_read      -- read a line range from a text file relative to
 *                     the working directory; binary files, '..',
 *                     absolute paths and symbolic links are refused
 *   file_create    -- create or overwrite a text file with the given
 *                     content, same path rules as file_read
 *   file_edit      -- replace old_string with new_string within 3
 *                     lines of a hint line, matching tolerantly across
 *                     tab/space indentation differences and
 *                     re-indenting the replacement from the file's own
 *                     indentation
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

/* Sleep for the given number of seconds (0..60, fractions allowed).
 * Returns a malloc'd "Slept for N seconds." reply, or NULL with *out_err
 * set for an out-of-range request. Exported for tests. */
char *tools_sleep(double seconds, char **out_err);

/* Read a text file relative to the working directory and render the
 * requested 1-based line range (line_offset <= 0 means line 1,
 * lines_length <= 0 means 2000; at most 2000 lines and 100000
 * characters are returned per call). Paths escaping the working
 * directory ('..', absolute paths, Windows drive prefixes) and symbolic
 * links are refused, as are binary files. Returns a malloc'd report
 * ("File path / Total lines / ----- lines from X to Y ----- / content",
 * with a truncation note when a cap hit), or NULL with *out_err set.
 * Exported for tests. */
char *tools_file_read(const char *filepath, long line_offset, long lines_length, char **out_err);

/* Create or overwrite a text file (same path rules as tools_file_read)
 * with content written verbatim. Returns a malloc'd "File created/
 * overwritten: <path> (N bytes, M lines)" reply, or NULL with *out_err
 * set. Exported for tests. */
char *tools_file_create(const char *filepath, const char *content, char **out_err);

/* Replace old_string with new_string in a text file (same path rules),
 * searching within 3 lines of the 1-based linefrom hint. Matching is
 * whitespace-tolerant: leading/trailing per-line whitespace and
 * internal whitespace runs are ignored, and the replacement is
 * re-indented by the difference between the file's and old_string's
 * first-line indentation. Returns "Edit accepted" on success; refusals
 * come back as text ("Edit refused: old_string not found" / "Edit
 * refused: old_string found at line N"). Returns NULL with *out_err set
 * for path guards and I/O errors. Exported for tests. */
char *tools_file_edit(const char *filepath, long linefrom, const char *old_string,
                      const char *new_string, char **out_err);

/* Run the built-in tools MCP server. tool_list is a comma-separated list
 * of names from TOOLS_BUILTIN_NAMES (required; unknown names are rejected
 * with EXIT_ARGS_ERR). listen_addr follows the srv_serve convention: NULL
 * or empty serves stdio, "host:port" serves HTTP. */
int tools_run(const char *tool_list, const char *listen_addr);

#endif /* TOOLS_H */
