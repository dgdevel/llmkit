# Markdown Rendering for repl/agent - Research & Plan

> STATUS: implemented. Phases 1-3 complete:
> - `src/vendor/md4c/` (v0.6.0, pristine, excluded from check-ascii)
> - `src/mdterm.{h,c}`: whole-doc renderer (md4c SAX) + streaming line
>   machine (fence/heading/list/quote/table states, inline scanner)
> - hooks: pretty.c `dspy_md_feed/close/free` (pretty_record R_RESPONSE,
>   pretty_live = replay/proxy share it), repl.c live sink R_RESPONSE,
>   call.c tty-gated whole-doc render (pipes byte-exact plain)
> - styling: `md_style_probe` extends style_probe; 16-color ANSI; colors
>   honor NO_COLOR, bold/italic survive; all "" on pipes = plain bytes
> - tests: `test_mdterm` (whole-doc goldens, streamed goldens, tables,
>   quotes, links, entities) - make check green
> - fuzz: `fuzz/fuzz_mdterm.c` (both passes, split + byte-by-byte)

## 1. What opencode does

opencode TUI does **not** own a markdown renderer. All rendering (full TUI *and*
`mini`/scrollback mode) goes through `@opentui/core`'s `MarkdownRenderable`
(Tree-sitter based, styled spans, full layout). `packages/tui/src/plugin/markdown.ts`
only wires per-language code-block handlers. So: **no code to port - only design.**

Portable design ideas extracted (file refs in opencode clone):

| Idea | Source | Plain-console translation |
|---|---|---|
| Reasoning = muted color + 1-char left border (`|`), border = darkened bg not accent | `message-parts.tsx:61-107` | `| ` prefix per line, dim/italic ANSI |
| Reasoning collapse: one line while streaming, header `Thought: title :  duration` | `message-parts.tsx:121-147` | `Thinking: ...` italic line, optional expansion flag |
| Title from leading `**bold**` of reasoning text (OpenAI Responses summaries) | `context/thinking.ts:15-21` | tiny scan for `^\*\*(...)\*\*\n\n` |
| Unicode->ASCII fallback table (dash/quotes/bullets/arrows->ASCII), `?` for rest | `mini/mono.ts:13-56,229-232` | ~30-entry codepoint switch, gated on TERM/mono |
| Border charset swap `+--+`->`+-\|`, tables->space-aligned columns no borders | `mini/mono.ts:42-63` | trivial for plain output |
| Unknown tool output -> pretty JSON truncated 4096 + `... [truncated]` | `mini/tool.ts:1290-1305` | fits llmkit tool response trim queue |
| Semantic token set: heading/link/code/codeBlock/emphasis/strong/listItem/quote | `theme/assets/v2/opencode.json:133-147` | map to ANSI 256 (no config system needed) |

## 2. llmkit current state

- No markdown rendering anywhere. `style_probe` (pretty.c:27) gates only
  bold/italic/reset via isatty+TERM. `tty_cols` width available (platform.c).
- Print sites for model output:
  - repl live sink: repl.c:466 `dspy_styled(..., italic, tx)` - R_THINKING + R_RESPONSE, per-delta.
  - pretty record replay: pretty.c:412 - same path, used by store replay + proxy live.
  - call: call.c:434 `fwrite` raw, pipe-friendly, thinking discarded.
  - agent helper mirror: agent.c:26-39 stderr, plain.
- Records carry `partial` flag; `partial==false` = block closed (repl.c:464,
  pretty.c:415). Block boundaries detectable -> per-block render feasible.
- Engine emits records; rendering lives in sinks. Clean.

## 3. Portability verdict

- OpenTUI renderer: **not portable** (Tree-sitter wasm, JS, full scene graph).
- Correct engine choice: **md4c** (github.com/mity/md4c):
  - MIT license, vendored as `md4c.c`+`md4c.h` (zero deps, ~4k LOC) - matches
    existing `src/vendor/linenoise`, `src/vendor/regex` pattern.
  - CommonMark 0.31.2 compliant, OSS-Fuzzed, Blender/Qt/LibreOffice use it.
  - SAX push model (`md_parse` + enter/leave/text callbacks) -> renderer is our
    own ~400-line ANSI writer. No tree, no DOM, fits buf.c.
  - GFM flags: `MD_FLAG_TABLES`, `MD_FLAG_STRIKETHROUGH`, `MD_FLAG_TASKLISTS`,
    `MD_FLAG_LATEXMATHSPAN` (skip), permissive autolinks.
  - md4c-html not needed.
- Term wrapping/soft-wrap: our problem, not md4c's; use `tty_cols` + simple
  greedy wrap (ASCII only; wide-char wrapping can stay v2).

## 4. Implementation plan

### Phase 1 - vendor + renderer (core)
1. Vendor `src/vendor/md4c/{md4c.c,md4c.h}` + Makefile obj.
2. New `src/mdterm.c` + `mdterm.h`:
   - `md_render_ansi(dspy_t*, const char *md, md_style_t*)` via md4c callbacks.
   - Styles (ANSI 256, degrade to none when non-tty): heading=bold+cyan,
     code_block=neutral box or dim fg, inline_code=green, emphasis=italic,
     strong=bold, link=blue+underline, blockquote=left `| ` dim, list markers
     kept as text, hr=`tty_cols` rule, tables=space-aligned (mono.ts style).
   - Unicode fallback table (from mono.ts) applied when not utf-8 terminal.
3. Extend `style_t`: add colors, keep `style_probe` gating (non-tty -> all empty
   -> renderer degrades to plain text automatically). Honor `NO_COLOR`.

### Phase 2 - hook points (streaming-safe)
4. Streaming UX (no repaint possible, not a TUI):
   - Buffer per-block in sink while `partial==true`; **line-flush** incremental:
     render a completed line through a lightweight line-state machine (fence
     tracking, list, heading) so user sees text as it streams.
   - On `partial==false` close block.
   - Pragmatic v1 alternative: plain streaming during generation (as today),
     full md4c render only for non-streaming paths (call, replay, prettyprint).
     Choose after trying line-flush; fallback documented.
5. Hook sites:
   - repl.c:466 - thinking: collapse line `Thinking: title...` while partial,
     on close print rendered body if `--thinking full` (config knob).
   - pretty.c:412 - same treatment (covers replay, proxy live).
   - call.c:434 - render only when `isatty(fileno(out))` (pipes stay raw).
   - agent.c:26 - optional, own style_probe on stderr; v2.

### Phase 3 - tests
6. `test/`: golden-file tests (md in -> ANSI/plain out), tty+pipe modes, NO_COLOR,
   mono fallback, GFM tables/strikethrough/tasks.
7. Fuzz: `fuzz/` harness on md_render_ansi (md4c itself OSS-Fuzzed already).

### Estimates
- mdterm renderer: ~400-500 LOC + table/wrap edge cases.
- sink changes: ~150 LOC across repl/pretty/call.
- tests: ~200 LOC.

## 5. Risks / decisions needed
- Line-flush streaming renderer vs render-on-close: UX tradeoff, decide empirically.
- Width-aware soft wrap of paragraphs: adds complexity; v1 = wrap only code blocks?
- Syntax coloring *inside* code fences: skip (no tree-sitter in C without pain);
  dim/box treatment enough. Revisit later.
- `llmkit call` markdown changes byte-output contract: gate strictly on tty.
