/* mdterm.h - markdown to terminal: the whole-document renderer and the
   streaming line machine over the vendored md4c (src/vendor/md4c).

   Two entry points, one style probe:

   - md_render(dspy_t, text, len): parse a complete markdown document
     and write it styled to the sink. Used where the text is whole at
     once: the call front-end's final record, prettyprint of stored
     transcript blocks.

   - md_line_*: a stateful per-line machine for live streams. The
     wires deliver text as deltas; a completed line is the smallest
     unit this styling can decide on alone (a heading, marker, fence
     line or paragraph line), so the machine buffers until \n and
     renders each finished line through a lightweight scanner. Fences
     toggle verbatim mode, tables collect until close, the tail is
     flushed when the block ends.

   Styling gates on the same tty/TERM probe as style_probe: every
   attribute string is "" when the output is not a tty, so both layers
   degrade to plain text with no special cases anywhere else. Colors
   honor NO_COLOR (bold/italic survive it, as before this file). */
#ifndef LLMKIT_MDTERM_H
#define LLMKIT_MDTERM_H

#include <stdio.h>

#include "llmkit.h"

/* the markdown styles: attribute codes the probe fills; "" = off.
   the palette is fixed 16-color ANSI: no truecolor escape is ever
   emitted, every terminal claiming ANSI conformance renders it */
typedef struct md_style {
    char bold[24], italic[24], dim[24], reset[24], underline[24];
    char cyan[24], green[24], yellow[24];
    char blue[24], magenta[24], red[24];
} md_style_t;

/* fill st through style_probe's gates ("" reset = plain only) */
void md_style_probe(md_style_t *st, FILE *out);

/* true when st carries no styling at all */
int md_plain_only(const md_style_t *st);

/* render a whole markdown document (text, len bytes) to s. the output
   never ends with a newline: the caller adds the block's closing one */
void md_render(dspy_t *s, const char *text, size_t len);

/* ---- the streaming line machine ---- */

typedef struct md_line {
    dspy_t *out;        /* rendered lines go here */
    md_style_t st_store;
    const md_style_t *st;
    buf_t line;         /* the incomplete line being buffered */
    bool active;
    bool in_fence;
    char fence_char;
    size_t fence_len;
    bool fence_saw_lang;
    bool table_mode;    /* delimiter row passed; rows collect */
    buf_t table_row;    /* rows collected until close */
    buf_t pending;      /* a candidate table header, held one line */
    bool have_pending;
} md_line_t;

/* arm the machine: probe styles against out; 0 ok */
int md_line_init(md_line_t *m, dspy_t *out);

/* feed a streaming delta: complete lines render now, the tail stays
   buffered; 0 ok, -1 not armed */
int md_line_feed(md_line_t *m, const char *text, size_t n);

/* the block closed: flush the tail, reset fences/tables; 0 ok */
int md_line_close(md_line_t *m);

/* teardown: free the buffers */
void md_line_free(md_line_t *m);

#endif
