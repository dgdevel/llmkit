# llmkit ceilings

Known limits and their upgrade paths, extracted from `docs/design.md`
(sec. numbers below) before the design and requirements documents were
retired from the tree. Each entry is a deliberate ceiling, not a bug.

The full specs live at the `docs-archive` tag:
`git show docs-archive:docs/design.md`, `docs-archive:docs/requirements.md`.

- **llm read timeout (sec.5)** - `llm_read_timeout` is
  `CURLOPT_LOW_SPEED_LIMIT=1` + `CURLOPT_LOW_SPEED_TIME`, so a >=1 byte/s
  trickle never triggers. Replace with a write-callback watchdog if a
  pathological endpoint exists.
- **mcp spawn (sec.6)** - stdio servers spawn through
  `/bin/sh -c <command_line>` (posix) or `%ComSpec% /c` (windows); the
  caller owns quoting (`call_shell_quote` picks the dialect). No
  shell-less spawn mode; add one if a server needs argv-precise control.
- **repl history (sec.12)** - in-memory, 128 lines, arrow recall only, no
  persistence or search. Grow it when someone complains.
- **repl styling (sec.12)** - terminfo is read through `tput` rather than
  an in-tree terminfo database parser.
- **repl editor tests (sec.15)** - the raw-mode editor and the two ctrl-c
  stages at the prompt need a pty; they stay a manual smoke pass, no
  in-tree pty harness.
- **network tests (sec.15)** - no network integration tests in-tree; a
  `test/live.sh` hitting a real endpoint is added when the first endpoint
  bug shows up.
- **mcp-repl (sec.16)** - no per-server timeout flag (the rpc default 30s
  applies), no re-list on `tools/list_changed` (restart the console), no
  pty harness for the completion.
- **llm-proxy (sec.18)** - connections are sequential; a slow streamed
  response blocks the next connection. Per-connection threads the day a
  real client needs it. No ipv6 listen, no tls terminate, no
  request-body rewrite ever.
- **agent store (sec.19)** - one append-only jsonl file, no schema
  version, no lock file, no compaction. `prettyprint` reads it and a
  resumed session replays it, which is the whole feature.
