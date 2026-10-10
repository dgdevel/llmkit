# builtin-mcp

`llmkit builtin-mcp` is an mcp server on stdio exposing a set of
generic-use tools: web search, web fetch, five file operations and two
process operations. No arguments, no config file:

```sh
$ llmkit builtin-mcp < jsonrpc-on-stdin   # serves until eof
```

It speaks the same json-rpc surface as `agent-as-tool` and `mcp-proxy`
(initialize / notifications/initialized / ping / tools/list / tools/call)
in all current protocol revisions up to `2026-07-28`.

Everything is stateless except the process tools: the server keeps one
record per spawned process (pid, exit state and one output temp file)
for its own lifetime, so `process_status` can report on any pid it
spawned.

## Attaching it to a conversation

Tools are mcp servers, so the builtin server attaches with an ordinary
tools record (see [records.md](records.md)):

```json
{"type":"tools","tools":[{"type":"stdio","name":"builtin","command_line":"llmkit builtin-mcp"}]}
```

The tool names are then `builtin.web_search`, `builtin.file_edit`, and so
on. Inside `llmkit call` / `llmkit repl` the binary spawns itself the same
way it spawns `mcp-proxy` children.

## The tools

Every tool and argument description is one text file: tool descriptions
in `src/prompts/mcp/<tool>/description.txt`, argument descriptions in
`src/prompts/mcp/<tool>/arguments/<argument>.txt`. `tools/gen-prompts.sh`
bundles them into the binary at build time (the generated
`src/prompts.gen.c` / `src/prompts.gen.h` pair, rerun by the Makefile
whenever a text or the script changes), so everything still ships as a
single file. Edit the txt files and rebuild; an empty file is sent as an
empty description.

| tool | arguments | returns |
|---|---|---|
| `web_search` | `keywords: string` | search results block |
| `web_fetch` | `url: string` | the page as markdown |
| `files_list` | `path: string, regex: string` | one line per entry |
| `files_search` | `path: string, regex: string` | entries plus matching line numbers |
| `file_read` | `path: string, lines_offset: int, lines_length: int` | the requested lines |
| `file_create` | `path: string, content: string, overwrite: bool = false` | confirmation |
| `file_edit` | `path: string, oldString: string, newString: string, line_number: int` | confirmation |
| `process_exec` | `cmdline: string` | exit code or pid + output tail |
| `process_status` | `pid: int` | same report for one spawned pid |

### web_search(keywords)

Queries the duckduckgo html endpoint (`html.duckduckgo.com/html/`) and
answers with results separated by one blank line:

```
URL: ${URL}
Description: ${brief_description}
```

duckduckgo redirect links are unwrapped to the real target url, at most 25
results are returned. Failures (transport, non-2xx) come back as `isError`
replies.

### web_fetch(url)

Validates the url (http or https, nonempty host), fetches it following
redirects (20Mb cap), then runs a readability-style reduction: junk
elements are dropped, the content subtree is picked by paragraph-driven
scoring with class/id hints, and the result is converted to markdown
(headings, links, images, lists, blockquotes, fenced code, tables). The
tool response is the markdown of the page, title first as a `#` heading.
Pages that are not html (by content type) or have no readable content are
errors.

### files_list(path, regex)

Lists the tree under `path` recursively (alphabetical, pre-order:
directories are listed before their contents), keeping only entries whose
path matches the regex anywhere. Paths are answered relative when the
given path is relative, absolute when it is absolute:

```
${permissions} ${size} ${path}
```

- `permissions` is `rwx` with `-` for no permission (read/write/execute
  for the current user)
- `size` is human readable (`512b`, `5Kb`, `5.4Kb`, `15Kb`, `1Mb`);
  textual files also carry their line count:

```
rw- 5Kb, 6 lines ./path/to/file.txt
r-x 15Kb ./path/to/binary/executable
r-- 1Mb ./this/is/read/only/file.bin
```

A file counts as textual when it holds no NUL bytes and is strict utf-8;
everything else (including oversized files, over 64Mb) is listed without
the line count.

### files_search(path, regex)

Same walk as `files_list`, but the regex matches line *content*: every
textual file under `path` (16Mb cap per file) whose lines match is listed
with the matching line numbers, comma separated:

```
${permissions} ${size}${, lines} ${path}
Matching lines: ${line numbers}
```

Line numbers are 1-based. Files without textual matches, directories and
binary files are omitted.

### file_read(path, lines_offset, lines_length)

Checks the path is a regular textual file (same textual rule as above)
and answers the requested lines joined with `\n`. `lines_offset` is the
0-based index of the first line, `lines_length` the number of lines.
An offset past the end of the file is an error.

### file_create(path, content, overwrite=false)

Writes `content` to `path`, creating parent directories as needed. An
existing file is an error unless `overwrite` is true (directories are
never overwritten).

### file_edit(path, oldString, newString, line_number)

Searches `oldString` around `line_number` (1-based) with a tolerance of
3 lines - the closest match by distance wins, and a `line_number` of 0
means "search the whole file". Matching compares each line with leading
and trailing whitespace ignored. The replacement's indentation is
adjusted to the replaced block: the new block keeps its internal relative
indentation and is shifted so its first line lands on the column of the
first replaced line. Line endings are preserved (`\n` and `\r\n`).

### process_exec(cmdline)

Runs `cmdline` through the shell (posix: `/bin/sh -c`, windows:
`%ComSpec% /c`) with stdin connected to the null device, and waits up
to 10 seconds for it to finish. stdout and stderr arrive merged into
one stream and every line is stamped with a wall-clock timestamp as it
is read - the `annotate-output.sh` style, line races between the two
streams accepted:

```
HH:MM:SS: the line
```

The reply for a finished process:

```
Exit code: ${code}
Last three output lines:
${stamped line}
${stamped line}
${stamped line}
```

The reply for one still running after the wait:

```
PID ${pid} is still running.
Last three output lines:
...
Use process_status to monitor it.
```

`Last three output lines` carries whatever the process printed so far,
at most three of them. The full stamped output is appended to one temp
file per process (`$TMPDIR`, else `/tmp`, on posix; the user temp dir
on windows) for the lifetime of the server, and once more than three
lines exist the reply names it:

```
Process output available in ${path} (currently ${n} lines)
```

`currently`, because the count keeps growing until the process ends
and its output drains. Exit codes follow the shell convention:
a normal exit reports its status, a signal death reports `128 + signal`,
a command the shell cannot find reports 127.

### process_status(pid)

The same report for a pid spawned by this server - running or finished,
whichever its current state is. A pid this server did not spawn is an
error and reports no output.

## Regex flavor

`files_list` / `files_search` patterns are posix extended regular
expressions, matched with the libc `regcomp` (the windows build compiles
openbsd's implementation, vendored in `src/vendor/regex/`). As sugar,
`\d`, `\w`, `\s` and their negations and the escapes `\n \t \r \f \v`
are translated to plain ere classes, and `(?:...)` groups become
ordinary groups. Matching is case-sensitive and unanchored (a match
anywhere in the subject counts). No backreferences or lookahead, and no
step budget: a pathological pattern can be slow. `{n,m}` bounds are
capped at 255 on windows (the libc limit there).
