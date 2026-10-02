# builtin-mcp

`llmkit builtin-mcp` is a stateless mcp server on stdio exposing a set of
generic-use tools: web search, web fetch and five file operations. No
arguments, no config file:

```sh
$ llmkit builtin-mcp < jsonrpc-on-stdin   # serves until eof
```

It speaks the same json-rpc surface as `agent-as-tool` and `mcp-proxy`
(initialize / notifications/initialized / ping / tools/list / tools/call)
in all current protocol revisions up to `2026-07-28`.

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

All descriptions (tool and argument) ship as empty strings by design.
Every one of them lives in one block at the top of `src/builtin.c`
(`DESCRIPTIONS - the one place to edit them`): change the string after the
`=` sign and rebuild. An empty string is sent as an empty description.

| tool | arguments | returns |
|---|---|---|
| `web_search` | `keywords: string` | search results block |
| `web_fetch` | `url: string` | the page as markdown |
| `files_list` | `path: string, regex: string` | one line per entry |
| `files_search` | `path: string, regex: string` | entries plus matching line numbers |
| `file_read` | `path: string, lines_offset: int, lines_length: int` | the requested lines |
| `file_create` | `path: string, content: string, overwrite: bool = false` | confirmation |
| `file_edit` | `path: string, oldString: string, newString: string, line_number: int` | confirmation |

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

## Regex flavor

`files_list` / `files_search` use a small byte-oriented engine:
literals, `.`, character classes with ranges and negation, `^`, `$`,
alternation `|`, groups `(...)` / `(?:...)`, greedy `*`, `+`, `?`,
`{n}`, `{n,}`, `{n,m}`, and the classes `\d \w \s` with negations.
Matching is case-sensitive and unanchored (a match anywhere in the
subject counts). No backreferences or lookahead; a step budget keeps
pathological patterns from hanging the server (they answer no match).
