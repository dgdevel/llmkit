# builtin-mcp

`llmkit builtin-mcp` is an mcp server on stdio. It exposes a set of generic-use tools: web search, web fetch, file download, seven file operations, four process operations, two agent-skill operations, and the current date and time. It takes no arguments and has no configuration file.

Test it with the mcp-repl feature:

```sh
$ llmkit mcp-repl --stdio 'llmkit builtin-mcp'
```

It speaks the same json-rpc surface as `agent-as-tool` and `mcp-proxy` (initialize / notifications/initialized / ping / tools/list / tools/call) in all current protocol revisions up to `2026-07-28`.

Everything is stateless except the process tools. The server keeps one record per spawned process (pid, exit state, and one output temp file) for its own lifetime. `process_status`, `process_wait`, and `process_send_signal` can therefore report on, or signal, any pid it spawned. Any other pid on the machine can be checked too, without the output.

## Attaching it to a conversation

Tools are mcp servers. The builtin server therefore attaches with an ordinary tools record (see [records.md](records.md)):

```json
{"type":"tools","tools":[{"type":"stdio","name":"builtin","command_line":"llmkit builtin-mcp"}]}
```

The tool names are then `builtin.web_search`, `builtin.file_edit`, and so on. Inside `llmkit agent` the bundled server flattens: the same tools list under their bare names (`web_search`, `file_edit`) - the agent attaches the server itself, one per conversation, and the prefix adds no signal. Inside `llmkit call` / `llmkit repl`, the binary spawns itself the same way it spawns `mcp-proxy` children.

## The tools

Every tool and argument description is one text file: tool descriptions in `src/prompts/mcp/<tool>/description.txt`, argument descriptions in `src/prompts/mcp/<tool>/arguments/<argument>.txt`. `tools/gen-prompts.sh` bundles them into the binary at build time (the generated `src/prompts.gen.c` / `src/prompts.gen.h` pair, rerun by the Makefile whenever a text or the script changes). Everything still ships as a single file. Edit the txt files and rebuild. An empty file is sent as an empty description.

A platform-dependent description ships in both variants. The extra `<argument>.windows.txt` or `description.windows.txt` generates a `..._windows` symbol, and the windows build aliases the base symbol to it (`#ifdef` in `src/builtin.c`). The posix and windows tools/list answers therefore advertise exactly what each platform delivers. For example, `process_send_signal` offers `hup`/`usr1`/`usr2` only on posix.

| tool | arguments | returns |
|---|---|---|
| `web_search` | `keywords: string` | search results block |
| `web_fetch` | `url: string` | the page as markdown |
| `download` | `url: string, outputfile: string` | confirmation |
| `files_list` | `path: string, regex: string, recurse_subdirectories: bool = false, show_hidden_files: bool = false` | one line per entry |
| `files_search` | `path: string, regex: string, show_hidden_files: bool = false` | entries plus matching line numbers |
| `file_read` | `path: string, lines_offset: int, lines_length: int` | the requested lines |
| `file_create` | `path: string, content: string, overwrite: bool = false` | confirmation |
| `file_edit` | `path: string, oldString: string, newString: string, line_number: int` | confirmation |
| `file_delete` | `path: string` | confirmation |
| `file_copy` | `sourcepath: string, destpath: string, overwrite: bool = false` | confirmation |
| `file_move` | `sourcepath: string, destpath: string, overwrite: bool = false` | confirmation |
| `file_analyze` | `path: string` | the file's structure, one line per element |
| `process_exec` | `cmdline: string` | exit code or pid + output tail |
| `process_status` | `pid: int` | same report for one spawned pid. Any other pid: running state |
| `process_wait` | `pid: int, timeout: int` | waits, then the same report |
| `process_send_signal` | `pid: int, signal: string` | delivery confirmation, or the exit status of a spawned pid |
| `skills_search` | `keywords: string` | matching skills as records |
| `skills_read` | `name: string` | the skill's SKILL.md text |
| `now` | none | current date and time, iso 8601 + utc offset + timezone name |

### web_search(keywords)

The tool queries the duckduckgo html endpoint (`html.duckduckgo.com/html/`). It answers with results separated by one blank line:

```
URL: ${URL}
Description: ${brief_description}
```

Duckduckgo redirect links are unwrapped to the real target url. At most 25 results are returned. Failures (transport, non-2xx) come back as `isError` replies.

### web_fetch(url)

The tool validates the url (http or https, nonempty host) and fetches it following redirects (20Mb cap). Then it runs a readability-style reduction: junk elements are dropped, the content subtree is picked by paragraph-driven scoring with class/id hints, and the result is converted to markdown (headings, links, images, lists, blockquotes, fenced code, tables). The tool response is the markdown of the page, title first as a `#` heading. Pages that are not html (by content type) or have no readable content are errors.

### download(url, outputfile)

The tool validates the url (http or https, nonempty host) and streams the response body to `outputfile` (10-minute budget, any content type, no size cap). The parent directory is created when missing. An `outputfile` that already exists is an error - the tool never overwrites. A failed transfer (transport error, non-2xx, disk write error) unlinks the partial file and is an error. The reply is `wrote ${bytes} bytes to ${outputfile}`.

### files_list(path, regex, recurse_subdirectories=false, show_hidden_files=false)

Lists the tree under `path` (alphabetical, pre-order: directories are listed before their contents), keeping only entries whose path matches the regex anywhere. A `path` that is a directory lists its own entries only. If `recurse_subdirectories` is true, the tool descends into the subdirectories too. Dot-prefixed (hidden) files and directories are left out unless `show_hidden_files` is true. A hidden directory is listed (with a ` (files inside)` note) but never descended into by recursion - naming one as `path` is the only way inside. Paths are answered relative when the given path is relative, absolute when it is absolute:

```
${permissions} ${size} ${path}${, lines}
```

- `permissions` is `rwx` with `-` for no permission (read/write/execute
  for the current user)
- `size` is human readable (`512b`, `5Kb`, `5.4Kb`, `15Kb`, `1Mb`)
- `lines` is the entry's line count, last on the line. Textual files
  carry one. A directory carries none:

```
rw- 5Kb ./path/to/file.txt, 6 lines
r-x 15Kb ./path/to/binary/executable
r-- 1Mb ./this/is/read/only/file.bin
rwx 4Kb ./path/to/directory
```

A file counts as textual when it holds no NUL bytes and is strict utf-8. Everything else (including oversized files, over 64Mb) is listed without the line count.

A walk that matches nothing (an unreadable `path` aside) is a plain success: `No files or directory match the requested filters`. `files_search` answers the same sentence when no line matched anywhere.

### files_search(path, regex, show_hidden_files=false)

Same walk as `files_list` with `recurse_subdirectories` set, but the regex matches line *content*. Every textual file under `path` (16Mb cap per file) whose lines match is listed with the matching line numbers, comma separated. Dot-prefixed (hidden) files are read when `show_hidden_files` is true; hidden directories are never descended into - naming one as `path` searches inside it:

```
${permissions} ${size} ${path}${, lines}
Matching lines: ${line numbers}
```

Line numbers are 1-based. Files without textual matches, directories, and binary files are omitted.

### file_read(path, lines_offset, lines_length)

The tool makes sure that the path is a regular textual file (same textual rule as above). It answers the requested lines joined with `\n`. `lines_offset` is the 0-based index of the first line. `lines_length` is the number of lines. An offset past the end of the file is an error.

### file_create(path, content, overwrite=false)

Writes `content` to `path` and creates parent directories as needed. An existing file is an error unless `overwrite` is true. Directories are never overwritten.

### file_edit(path, oldString, newString, line_number)

Searches `oldString` around `line_number` (1-based) with a tolerance of 5 lines. The closest match by distance wins. A `line_number` of 0 means "search the whole file". Matching compares each line with leading and trailing whitespace ignored. If that finds nothing, the search is retried with internal whitespace runs collapsed to a single space (a tab standing in for spaces, a doubled space for one). It is retried again with blank lines padded onto either end of the block dropped. A literal match anywhere wins over a relaxed one. The closest candidate wins inside one pass.

The replacement's indentation is adjusted to the replaced block. The new block keeps its internal relative indentation and is shifted so its first line lands on the column of the first replaced line. The text of `newString` itself is written verbatim. Line endings are preserved (`\n` and `\r\n`).

### file_delete(path)

Moves the file or directory at `path` into the user's trash instead of destroying it. The freedesktop trash dirs on Linux (`$XDG_DATA_HOME/Trash` or `~/.local/share/Trash`, per-mount `.Trash/$UID` or `.Trash-$UID`, created when missing and the mount is writable). `~/.Trash` on macOS. The recycle bin on Windows (`SHFileOperationW` with `FOF_ALLOWUNDO`).

Name collisions in the trash get a " 2", " 3" ... suffix. The freedesktop sidecar `.trashinfo` records the original path and the deletion date, so a file manager can restore the file. The tool still answers `deleted <path>`.

When no trash is reachable (cross-device move, read-only or absent trash dir that cannot be created, no home), the old irreversible removal runs. It is depth-first post-order: each child is removed before its parent. Directories go through `rmdir(3)` only once empty. A failure therefore leaves the untouched remainder in place and reports the deepest path that failed with its `strerror` reason. Dangling symlinks are unlinked rather than followed. Depth is capped at 64.

### file_copy(sourcepath, destpath, overwrite=false)

Copies the file or directory tree at `sourcepath` to `destpath`. `destpath` names the target itself; when it is an existing directory, the copy lands inside it as `destpath/<source basename>`. An existing target is an error unless `overwrite` is true. A file never replaces a directory nor the other way round. Copying a directory into itself is refused. Directories are recreated and descended (depth cap 64), files are copied byte for byte; the first failure stops the copy and reports the path and reason, leaving what was already written in place. Only regular files are copied (special nodes error out).

### file_move(sourcepath, destpath, overwrite=false)

Moves the file or directory tree at `sourcepath` to `destpath`. Same `destpath` and `overwrite` rules as `file_copy`. Within one filesystem the move is a single `rename(2)`. Across two it is a full copy followed by an irreversible removal of the source (the trash is not involved); a failure to remove the source after a successful copy is an error naming what stayed behind.

### file_analyze(path)

Shows the structure of a text file of a known type instead of its whole content. This is cheaper than `file_read` when only the shape of a file matters. The type comes from the extension: markdown files (`.md`, `.markdown`, `.mdown`) come back as their heading tree, c-family sources (`.c .h .cpp .cc .cxx .hpp .hh .hxx .java .cs .js .jsx .ts .tsx`) as their structs with typed members, full function signatures and typedefs, python files (`.py`, `.pyi`) as their classes and defs with parameter lists, php files (`.php`, `.phtml`, `.phps`) as their namespaces, imports, classes/interfaces/traits/enums and function or method signatures. One line per element: its line number, an indent per nesting level, then the element, under a header naming the path, the kind and the line count:

```
src/builtin.c  [c]  4052 lines
   65  struct arg_def
   66    const char *name
   67    const char *type
   68    const char *desc
   69    bool required
  798  static bool next_line(const char *s, size_t len, size_t *i, const char **ls, size_t *ll)
```

Comments and string literals are ignored, so nothing inside them can look like structure. A file with no elements answers `(nothing found)`. At most 1000 elements are returned, marked `... [truncated]` past that. Directories, non regular files, non textual files of a known type and files over the 64Mb read cap are errors. An unknown type is not an error: the reply reports its human readable size, the content type when the OS can name one (the `file` utility on posix, FindMimeFromData on windows), the line count when the file is textual, and the list of analyzable types:

```
notes.txt: unknown file type (known: markdown, c, python, shell, batch, powershell, php)
size 17b, type text/plain, 3 lines
```

### process_exec(cmdline)

Runs `cmdline` through the shell (posix: `/bin/sh -c`, windows: `%ComSpec% /c`) with stdin connected to the null device, and waits up to 10 seconds for it to finish. stdout and stderr arrive on two separate pipes. Every line is stamped with a wall-clock timestamp and its stream, in the `annotate-output.sh` style with the stream named. A line is attributed when it is read. The order between the two streams at their boundary is therefore the reader's order, not a global one:

```
HH:MM:SS: STDOUT: the line
HH:MM:SS: STDERR: the line
```

The reply for a finished process:

```
Exit code: ${code}
Duration: ${seconds, three decimals}
First and last three output lines:
HH:MM:SS: STDOUT: ${line}
HH:MM:SS: STDERR: ${line}
HH:MM:SS: STDOUT: ${line}
[...]
HH:MM:SS: STDOUT: ${line}
HH:MM:SS: STDERR: ${line}
HH:MM:SS: STDOUT: ${line}
```

The reply for one still running after the wait (no duration line: the process has not ended, so it has none yet):

```
PID ${pid} is still running.
First and last three output lines:
...
Use process_status or process_wait.
```

`First and last three output lines` carries whatever the process printed so far: the first three lines, `[...]`, the last three. A process with six lines or fewer is quoted whole. Head and tail overlap there, so each line prints once and no `[...]` appears. Each line is stamped with its stream. A quoted line longer than 150 bytes is cut at that width (never mid-utf-8-character) and one note follows the quote:

```
Output was cut at 150 characters, see full output file for full data
```

The full stamped output is appended to one temp file per process (`$TMPDIR`, else `/tmp`, on posix. The user temp dir on windows) for the lifetime of the server. Once more than six lines exist, or any quoted line was cut, the reply names it:

```
Process output available in ${path} (currently ${n} lines)
```

The count keeps growing until the process ends and its output drains. Exit codes follow the shell convention: a normal exit reports its status, a signal death reports `128 + signal`, a command the shell cannot find reports 127.

### process_status(pid)

The same report for a pid spawned by this server, running or finished, whichever its current state is. Any other pid on the machine is checked against the os instead (posix: `kill(pid, 0)`, windows: `OpenProcess`). It answers without the output parts:

```
PID ${pid} is still running.
Use process_wait to wait for it.
No exit code or output: this pid was not spawned by this server.
```

or `PID ${pid} is not running.` with the same last line when it is gone. Exit code and output belong to the parent of the process, so neither exists here. A pid that died but was never reaped still counts as running. A pid that never existed looks the same as a dead one. A `pid` of 0 or less is an error.

### process_wait(pid, timeout)

Waits for a pid spawned by this server to terminate, or for `timeout` whole seconds to pass, whichever happens first. Then it answers the same report as `process_status`: the exit code and output tail when the process ended, the running state otherwise. Validation clamps `timeout` to 0..600. The value 0 means one immediate check. A pid this server did not spawn gets the state-only report from `process_status` after the same bounded wait.

### process_send_signal(pid, signal)

Sends `signal` to `pid`. The name is named, not numbered: the names are a small portable subset, not the full posix zoo. Own children (spawned by this server) get the full treatment. Other pids are addressed by id alone.

Posix delivers the real signals: `int` SIGINT, `term` SIGTERM, `hup` SIGHUP, `usr1`/`usr2`, and `kill` SIGKILL (uncatchable). A spawned pid that dies on the spot has its exit code ride along in the confirmation line. Anything slower is the caller's next `process_status` call.

Windows has no cross-process signals. `kill` force-terminates via `TerminateProcess`: no cleanup handlers run, and the caller learns only the delivery, not a trustworthy exit reason. `int`/`term` deliver a `CTRL_BREAK_EVENT` to the child's process group. This is possible only for processes this server spawned (they start with `CREATE_NEW_PROCESS_GROUP`, so the group id is the pid) and only when they share this console. Some programs ignore it, so the reply points to `kill` as the fallback. All other names are rejected up front, per the windows tool description, which differs from the posix one on purpose. Each build bundles both text variants and `#ifdef _WIN32` picks the true one at compile time. `tools/list` therefore never advertises options the platform cannot honor.

### skills_search(keywords)

Scans the agent skills, `<cwd>/.agents/skills` first, then `$HOME/.agents/skills` (`USERPROFILE` on windows). It matches every `<skill>/SKILL.md` whose text contains any of the space-separated keywords, case-insensitively. A skill's score is the total number of keyword occurrences in its file. Results are sorted by score, highest first. Ties keep scan order (local root before the global one, each alphabetical). Every match is one record taken from the frontmatter, separated by a blank line:

```
name: Name of the skill
description: Description of the skill
```

A missing frontmatter name falls back to the skill's directory name. No match answers `no matching skills`. When neither root exists, the tool is an error.

### skills_read(name)

Searches the same roots in the same order. It answers the full text of the first `<skill>/SKILL.md` whose directory name or frontmatter name equals `name`. The local root shadows the global one. An unknown name is an error.

### now()

No arguments. It answers the machine's current wall-clock date and time: iso 8601 to the second, the utc offset of that instant, and the timezone name:

```
2025-06-01T12:00:00+02:00 at Europe/Rome timezone
```

On posix the timezone name comes from the `TZ` variable when it names a zone, else from the `/etc/localtime` symlink; a posix name is always served. On windows with no zoneinfo data the fallbacks are `UTC`-relative names such as `UTC` or the `TZ` abbreviation (`CET`); the offset always reflects the actual clock.

## Regex flavor

`files_list` / `files_search` patterns are posix extended regular expressions, matched with the libc `regcomp` (the windows build compiles openbsd's implementation, vendored in `src/vendor/regex/`). As sugar, `\d`, `\w`, `\s` and their negations, and the escapes `\n \t \r \f \v`, are translated to plain ere classes, and `(?:...)` groups become ordinary groups. Matching is case-sensitive and unanchored: a match anywhere in the subject counts. There are no backreferences, no lookahead, and no step budget. A pathological pattern can be slow. `{n,m}` bounds are capped at 255 on windows (the libc limit there).
