/* mdterm.c - markdown rendering for the terminal, in two layers over
   the vendored md4c parser (src/vendor/md4c):

   1. the whole-document renderer (md_render): md4c walks the parsed
      document, our callbacks write styled text to a dspy_t sink.
      Blocks (paragraph, heading, list, quote, code, table, rule) get
      their frame; spans (emphasis, strong, code, links) their colors.

   2. the streaming line machine (md_line_*): live streams arrive as
      deltas, so each completed line - the smallest unit this styling
      can decide on alone - goes through a lightweight scanner (fence
      detection, heading markers, list bullets, quote bars, tables)
      and a mini inline scanner. Whole documents go through md4c;
      single lines cannot (md4c needs document context to classify
      them), so the line machine owns a tiny span scanner for **bold**,
      *em*, `code`, [links](url).

   Design notes (docs/markdown-rendering.md):
   - the renderer only ever appends - it is a printer, never a
     component - so nothing needs scrollback control (opencode's
     TUI assumptions do not transfer; the styling ideas do).
   - style_probe's tty/TERM gate holds end to end: every attribute
     string is "" on a pipe and the line machine falls back to
     verbatim text, keeping `llmkit call | cat` byte-stable.
   - the palette is fixed 16-color ANSI; colors honor NO_COLOR while
     bold/italic survive it (they predate this file). */
#include "mdterm.h"

#include <stdlib.h>
#include <string.h>

#include "vendor/md4c/md4c.h"

/* ================= styles ================= */

static bool colors_allowed(void) {
    const char *nc = getenv("NO_COLOR");
    return !(nc && *nc);
}

void md_style_probe(md_style_t *st, FILE *out) {
    memset(st, 0, sizeof *st);
    style_t base;
    style_probe(&base, out);
    if (!*base.reset) return; /* not a tty, or styling off: plain only */
    snprintf(st->bold, sizeof st->bold, "\033[1m");
    snprintf(st->italic, sizeof st->italic, "\033[3m");
    snprintf(st->dim, sizeof st->dim, "\033[2m");
    snprintf(st->reset, sizeof st->reset, "\033[0m");
    snprintf(st->underline, sizeof st->underline, "\033[4m");
    if (!colors_allowed()) return;
    snprintf(st->cyan, sizeof st->cyan, "\033[36m");
    snprintf(st->green, sizeof st->green, "\033[32m");
    snprintf(st->yellow, sizeof st->yellow, "\033[33m");
    snprintf(st->blue, sizeof st->blue, "\033[34m");
    snprintf(st->magenta, sizeof st->magenta, "\033[35m");
    snprintf(st->red, sizeof st->red, "\033[31m");
}

int md_plain_only(const md_style_t *st) { return !*st->reset; }

/* ================= whole-document renderer ================= */

#define LIST_MAX 8

typedef struct md_doc {
    dspy_t *out;
    md_style_t style_store; /* own probe result, d.st points here */
    const md_style_t *st;
    const md_style_t *plain; /* all-empty: table cells collect plain */
    bool line_start;         /* cursor sits at column 0 */
    int quote_depth;
    int heading;             /* 0, or the open heading's level */
    bool in_code;            /* inside a fenced/indented code block */
    bool in_html;
    /* list frames, one per nesting level */
    struct {
        bool is_ol;
        unsigned no;  /* the running ordered-list number */
        char mark;    /* the bullet char of the source */
        bool tight;
    } lists[LIST_MAX];
    int list_depth;
    /* tables: cells collect into a buffer, rows separated by \0 */
    buf_t table;      /* all cell texts, '\0'-terminated each */
    size_t table_cap; /* total cells buffered */
    size_t row_cells; /* cells in the current row */
    size_t ncols;     /* the table's column count */
    bool in_cell;
    /* inline state, persistent across one document's text callbacks */
    struct {
        buf_t *out;           /* active span target (doc or scratch) */
        const md_style_t *st; /* active span styles */
        int em, strong, del, code, link; /* open counts, flat SGR */
        buf_t link_url;       /* the open link's href */
        bool autolink;
    } span;
} md_doc_t;

/* ---- low-level writes: the quote bar rides every newline ---- */

static void doc_write(md_doc_t *d, const char *t, size_t n) {
    if (!n) return;
    if (!d->quote_depth || d->in_code || d->in_html) {
        dspy_write(d->out, t, n);
        d->line_start = t[n - 1] == '\n';
        return;
    }
    /* insert the bar at every line start while inside a quote */
    while (n) {
        const char *nl = memchr(t, '\n', n);
        size_t seg = nl ? (size_t)(nl - t) : n;
        if (d->line_start) dspy_write_str(d->out, "| ");
        if (seg) {
            dspy_write(d->out, t, seg);
            d->line_start = false;
        }
        if (nl) {
            dspy_write_str(d->out, "\n");
            d->line_start = true;
        }
        t += seg + (nl ? 1 : 0);
        n -= seg + (nl ? 1 : 0);
    }
}

static void doc_write_str(md_doc_t *d, const char *s) {
    doc_write(d, s, strlen(s));
}

/* end the current line unless it already ended */
static void doc_block_line(md_doc_t *d) {
    if (!d->line_start) doc_write_str(d, "\n");
}

/* ---- inline writes through d->span ---- */

static void span_write_text(md_doc_t *d, const char *t, size_t n) {
    if (!n) return;
    const md_style_t *st = d->span.st;
    if (d->span.code > 0) {
        /* code span: one green run per span (they never nest) */
        if (*st->green) buf_append_str(d->span.out, st->green);
        buf_append(d->span.out, t, n);
        if (*st->green) buf_append_str(d->span.out, st->reset);
        return;
    }
    buf_append(d->span.out, t, n);
}

static void span_apply_open(md_doc_t *d) {
    buf_t *dst = d->span.out;
    const md_style_t *st = d->span.st;
    if (d->span.em && *st->italic) buf_append_str(dst, st->italic);
    if (d->span.strong && *st->bold) buf_append_str(dst, st->bold);
    if (d->span.del && *st->red) buf_append_str(dst, st->red);
    if (d->span.link && *st->underline) buf_append_str(dst, st->underline);
}

static void span_apply_reset(md_doc_t *d) {
    const md_style_t *st = d->span.st;
    if (*st->reset &&
        (d->span.em || d->span.strong || d->span.del || d->span.link))
        buf_append_str(d->span.out, st->reset);
}

/* ---- the text callback: entities decoded, breaks flattened ---- */

static void doc_text(md_doc_t *d, MD_TEXTTYPE type, const char *t,
                     MD_SIZE size) {
    if (!size) return;
    switch (type) {
    case MD_TEXT_BR:
        span_write_text(d, "\n", 1);
        return;
    case MD_TEXT_SOFTBR:
        span_write_text(d, " ", 1);
        return;
    case MD_TEXT_ENTITY:
        if (size == 4 && !memcmp(t, "&lt;", 4)) span_write_text(d, "<", 1);
        else if (size == 4 && !memcmp(t, "&gt;", 4)) span_write_text(d, ">", 1);
        else if (size == 5 && !memcmp(t, "&amp;", 5)) span_write_text(d, "&", 1);
        else if (size == 6 && !memcmp(t, "&quot;", 6)) span_write_text(d, "\"", 1);
        else if (size == 6 && !memcmp(t, "&nbsp;", 6)) span_write_text(d, " ", 1);
        else if (size == 6 && !memcmp(t, "&copy;", 6)) span_write_text(d, "(c)", 3);
        else span_write_text(d, t, size);
        return;
    case MD_TEXT_NULLCHAR:
        span_write_text(d, "\357\277\275", 3); /* U+FFFD */
        return;
    case MD_TEXT_HTML:
        /* inline html: dim verbatim */
        if (*d->span.st->dim) buf_append_str(d->span.out, d->span.st->dim);
        buf_append(d->span.out, t, size);
        if (*d->span.st->dim) buf_append_str(d->span.out, d->span.st->reset);
        return;
    default:
        span_write_text(d, t, size);
        return;
    }
}

/* ---- the table flush: widths measured, then rows written ---- */

static void table_flush(md_doc_t *d) {
    if (!d->table.len) return;
    /* cells: '\0'-terminated, ncols per row */
    size_t total_cells = 0;
    for (size_t i = 0; i < d->table.len; i++)
        if (d->table.data[i] == '\0') total_cells++;
    if (d->table.len && d->table.data[d->table.len - 1] != '\0') {
        buf_append_byte(&d->table, '\0'); /* close a dangling cell */
        total_cells++;
    }
    if (!total_cells) return;
    size_t ncols = d->ncols ? d->ncols : 1;
    size_t nrows = total_cells / ncols;

    /* column widths */
    size_t *w = calloc(ncols, sizeof *w);
    if (!w) { /* fall back to unaligned output */
        for (size_t i = 0; i < total_cells; i++) ;
        dspy_write_str(d->out, "[table]\n");
        return;
    }
    const char *p = d->table.data;
    for (size_t r = 0; r < nrows; r++)
        for (size_t c = 0; c < ncols; c++) {
            size_t l = strlen(p);
            if (l > w[c]) w[c] = l;
            p += l + 1;
        }

    /* the header separator and every row, columns padded */
    p = d->table.data;
    for (size_t r = 0; r < nrows; r++) {
        doc_block_line(d);
        for (size_t c = 0; c < ncols; c++) {
            size_t l = strlen(p);
            if (r == 0) {
                /* header: bold */
                dspy_write_str(d->out, d->st->bold);
            }
            dspy_write(d->out, p, l);
            if (r == 0) dspy_write_str(d->out, d->st->reset);
            /* pad to the column width */
            for (size_t k = l; k < w[c] + 2; k++) dspy_write_str(d->out, " ");
            p += l + 1;
        }
        if (r == 0) {
            /* separator row under the header */
            doc_block_line(d);
            for (size_t c = 0; c < ncols; c++)
                for (size_t k = 0; k < w[c] + 2; k++)
                    dspy_write_str(d->out, "-");
            dspy_write_str(d->out, "\n");
        }
    }
    free(w);
    buf_clear(&d->table);
}

/* ---- block/span callbacks ---- */

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    md_doc_t *d = ud;
    switch (type) {
    case MD_BLOCK_QUOTE:
        d->quote_depth++;
        doc_block_line(d);
        break;
    case MD_BLOCK_ADMONITION:
        d->quote_depth++;
        doc_block_line(d);
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL: {
        if (d->list_depth < LIST_MAX) {
            bool is_ol = type == MD_BLOCK_OL;
            const MD_BLOCK_OL_DETAIL *od = detail;
            const MD_BLOCK_UL_DETAIL *ud2 = detail;
            d->lists[d->list_depth].is_ol = is_ol;
            d->lists[d->list_depth].no = is_ol && od ? od->start : 1;
            d->lists[d->list_depth].mark =
                !is_ol && ud2 && ud2->mark ? (char)ud2->mark : '-';
            d->lists[d->list_depth].tight =
                is_ol ? (od ? od->is_tight != 0 : true)
                      : (ud2 ? ud2->is_tight != 0 : true);
            d->list_depth++;
        }
        doc_block_line(d);
        break;
    }
    case MD_BLOCK_LI: {
        const MD_BLOCK_LI_DETAIL *li = detail;
        if (d->list_depth) {
            doc_block_line(d);
            int ind = (d->list_depth - 1) * 2;
            for (int i = 0; i < ind; i++) doc_write_str(d, " ");
            bool is_ol = d->lists[d->list_depth - 1].is_ol;
            if (li && li->is_task) {
                doc_write_str(d, li->task_mark == ' ' ? "[ ] " : "[x] ");
            } else if (is_ol) {
                char m[24];
                int n = snprintf(m, sizeof m, "%u. ",
                                 d->lists[d->list_depth - 1].no);
                doc_write(d, m, n > 0 ? (size_t)n : 0);
                d->lists[d->list_depth - 1].no++;
            } else {
                char m[3] = {d->lists[d->list_depth - 1].mark, ' ', 0};
                doc_write_str(d, m);
            }
        }
        break;
    }
    case MD_BLOCK_HR:
        doc_block_line(d);
        dspy_rule(d->out, '-');
        d->line_start = true;
        break;
    case MD_BLOCK_H: {
        const MD_BLOCK_H_DETAIL *h = detail;
        d->heading = h ? h->level : 1;
        doc_block_line(d);
        break;
    }
    case MD_BLOCK_CODE: {
        const MD_BLOCK_CODE_DETAIL *cd = detail;
        doc_block_line(d);
        if (cd && cd->lang.size) {
            char banner[80];
            size_t n = 0;
            for (MD_SIZE i = 0; i < cd->lang.size && n < sizeof banner - 1; i++)
                banner[n++] = (char)cd->lang.text[i];
            if (n) {
                banner[n] = '\0';
                doc_write_str(d, "  ");
                if (*d->st->dim) doc_write_str(d, d->st->dim);
                doc_write_str(d, banner);
                if (*d->st->reset) doc_write_str(d, d->st->reset);
                doc_write_str(d, "\n");
            }
        }
        d->in_code = true;
        break;
    }
    case MD_BLOCK_HTML:
        doc_block_line(d);
        d->in_html = true;
        break;
    case MD_BLOCK_P:
        doc_block_line(d);
        break;
    case MD_BLOCK_TABLE: {
        const MD_BLOCK_TABLE_DETAIL *td = detail;
        d->ncols = td ? td->col_count : 0;
        d->row_cells = 0;
        doc_block_line(d);
        break;
    }
    case MD_BLOCK_TR:
        d->row_cells = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        /* cell text collects into the table buffer, plain styles */
        d->in_cell = true;
        d->span.out = &d->table;
        d->span.st = d->plain;
        break;
    default:
        break;
    }
    return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    (void)detail;
    md_doc_t *d = ud;
    switch (type) {
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_ADMONITION:
        d->quote_depth--;
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (d->list_depth) d->list_depth--;
        doc_block_line(d);
        break;
    case MD_BLOCK_H:
        d->heading = 0;
        break;
    case MD_BLOCK_CODE:
        d->in_code = false;
        break;
    case MD_BLOCK_HTML:
        d->in_html = false;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        buf_append_byte(&d->table, '\0'); /* terminate the cell */
        d->in_cell = false;
        d->span.out = NULL;
        d->span.st = d->st;
        d->row_cells++;
        break;
    case MD_BLOCK_TR:
        break;
    case MD_BLOCK_TABLE:
        table_flush(d);
        break;
    default:
        break;
    }
    return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *ud) {
    md_doc_t *d = ud;
    switch (type) {
    case MD_SPAN_CODE:
        d->span.code++;
        break;
    case MD_SPAN_EM:
    case MD_SPAN_U: /* the underline ext folds into emphasis look */
        d->span.em++;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_STRONG:
        d->span.strong++;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_DEL:
        d->span.del++;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_A: {
        const MD_SPAN_A_DETAIL *a = detail;
        d->span.link++;
        buf_clear(&d->span.link_url);
        if (a) {
            buf_append(&d->span.link_url, a->href.text, a->href.size);
            d->span.autolink = a->is_autolink != 0;
        }
        span_apply_reset(d);
        span_apply_open(d);
        break;
    }
    case MD_SPAN_IMG: {
        const MD_SPAN_IMG_DETAIL *img = detail;
        doc_write_str(d, "[image: ");
        if (img && img->src.size)
            doc_write(d, img->src.text, img->src.size);
        doc_write_str(d, "] ");
        break;
    }
    case MD_SPAN_WIKILINK:
        doc_write_str(d, "[[");
        break;
    default:
        break;
    }
    return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *ud) {
    (void)detail;
    md_doc_t *d = ud;
    switch (type) {
    case MD_SPAN_CODE:
        d->span.code--;
        break;
    case MD_SPAN_EM:
    case MD_SPAN_U:
        if (d->span.em) d->span.em--;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_STRONG:
        if (d->span.strong) d->span.strong--;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_DEL:
        if (d->span.del) d->span.del--;
        span_apply_reset(d);
        span_apply_open(d);
        break;
    case MD_SPAN_A: {
        d->span.link--;
        span_apply_reset(d);
        /* the url beside the label; autolinks already show it */
        if (!d->span.autolink && d->span.link_url.len) {
            doc_write_str(d, " (");
            if (*d->st->blue) doc_write_str(d, d->st->blue);
            doc_write(d, d->span.link_url.data ? d->span.link_url.data : "",
                      d->span.link_url.len);
            if (*d->st->reset) doc_write_str(d, d->st->reset);
            doc_write_str(d, ")");
        }
        d->span.autolink = false;
        buf_clear(&d->span.link_url);
        span_apply_open(d);
        break;
    }
    case MD_SPAN_WIKILINK:
        doc_write_str(d, "]]");
        break;
    default:
        break;
    }
    return 0;
}

static int cb_text(MD_TEXTTYPE type, const char *text, MD_SIZE size,
                   void *ud) {
    md_doc_t *d = ud;
    if (!size) return 0;
    if (d->in_code || d->in_html) {
        doc_write(d, text, size); /* verbatim */
        return 0;
    }
    if (d->in_cell) {
        doc_text(d, type, text, size); /* collect into the cell */
        return 0;
    }
    /* plain text with no open styling: straight out */
    if (type == MD_TEXT_NORMAL && !d->span.em && !d->span.strong &&
        !d->span.del && !d->span.link && !d->span.code && !d->heading &&
        type != MD_TEXT_ENTITY && type != MD_TEXT_BR &&
        type != MD_TEXT_SOFTBR) {
        doc_write(d, text, size);
        return 0;
    }
    /* styled context: render this run into scratch, then write once */
    buf_t tmp;
    buf_init(&tmp);
    buf_t *save_out = d->span.out;
    d->span.out = &tmp;
    doc_text(d, type, text, size);
    d->span.out = save_out;
    if (d->heading) {
        const char *on = d->heading <= 2 ? d->st->cyan : d->st->bold;
        if (*on) doc_write_str(d, on);
        if (d->heading <= 2 && *d->st->bold) doc_write_str(d, d->st->bold);
        doc_write(d, tmp.data ? tmp.data : "", tmp.len);
        if (*d->st->reset) doc_write_str(d, d->st->reset);
    } else {
        doc_write(d, tmp.data ? tmp.data : "", tmp.len);
    }
    buf_free(&tmp);
    return 0;
}

void md_render(dspy_t *s, const char *text, size_t len) {
    static const md_style_t plain_style; /* all "": cell text plain */
    md_doc_t d;
    memset(&d, 0, sizeof d);
    d.out = s;
    md_style_probe(&d.style_store, s->out);
    d.st = &d.style_store;
    d.plain = &plain_style;
    d.line_start = true;
    buf_init(&d.table);
    buf_init(&d.span.link_url);
    d.span.st = d.st;

    MD_PARSER p;
    memset(&p, 0, sizeof p);
    p.flags = MD_DIALECT_GITHUB;
    p.enter_block = cb_enter_block;
    p.leave_block = cb_leave_block;
    p.enter_span = cb_enter_span;
    p.leave_span = cb_leave_span;
    p.text = cb_text;
    md_parse(text, (MD_SIZE)len, &p, &d);

    buf_free(&d.table);
    buf_free(&d.span.link_url);
}

/* ================= streaming line machine ================= */

/* md4c cannot classify a lone line (list items need the list, table
   rows the delimiter row), so the stream machine scans each completed
   line itself. What it knows:
   - fence lines (```/~~~ toggles; the language banner at open)
   - ATX headings (# ...)
   - list markers (- * + and n.), bullet styled dim
   - quote bars (> ...)
   - table rows: collected until the block closes, then aligned like
     the whole-doc renderer writes them (collect-all is the only way
     to know the widths; streaming tables replay through this buffer)
   - everything else: paragraph text through the inline scanner

   The inline scanner is small on purpose: **bold**, *em*, _em_,
   `code`, [label](url) and raw http(s) links. Entities pass verbatim
   (md4c decodes them only in whole documents; terminals show &amp;
   fine). The tail of an unterminated line stays buffered until its
   \n arrives or the block closes. */

int md_line_init(md_line_t *m, dspy_t *out) {
    memset(m, 0, sizeof *m);
    md_style_probe(&m->st_store, out->out);
    m->st = &m->st_store;
    m->out = out;
    buf_init(&m->line);
    buf_init(&m->table_row);
    buf_init(&m->pending);
    m->active = true;
    return 0;
}

/* forward: the blank-line path flushes a collected table */
static void line_table_flush(md_line_t *m);

/* the inline scanner: render one text run (already stripped of its
   block prefix) with the span styles. Writes through a scratch buffer
   so a single styled run is one write. */
static void scan_inline(md_line_t *m, buf_t *dst, const char *t, size_t n) {
    const md_style_t *st = m->st;
    size_t i = 0;
    while (i < n) {
        char c = t[i];
        if (c == '`') {
            /* code span: to the closing backtick */
            size_t j = i + 1;
            while (j < n && t[j] != '`') j++;
            if (j < n) {
                if (*st->green) buf_append_str(dst, st->green);
                buf_append(dst, t + i + 1, j - i - 1);
                if (*st->green) buf_append_str(dst, st->reset);
                i = j + 1;
                continue;
            }
            buf_append_byte(dst, c);
            i++;
            continue;
        }
        if (c == '*' && i + 1 < n && t[i + 1] == '*') {
            /* bold toggle; flanked runs only */
            size_t j = i + 2;
            while (j + 1 < n && !(t[j] == '*' && t[j + 1] == '*')) j++;
            if (j + 1 < n) {
                if (*st->bold) buf_append_str(dst, st->bold);
                buf_append(dst, t + i + 2, j - i - 2);
                if (*st->reset) buf_append_str(dst, st->reset);
                i = j + 2;
                continue;
            }
            buf_append(dst, t + i, 2);
            i += 2;
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < n &&
            t[i + 1] != ' ' && t[i + 1] != c) {
            size_t j = i + 1;
            while (j < n && t[j] != c) j++;
            if (j < n && j > i + 1) {
                if (*st->italic) buf_append_str(dst, st->italic);
                buf_append(dst, t + i + 1, j - i - 1);
                if (*st->reset) buf_append_str(dst, st->reset);
                i = j + 1;
                continue;
            }
        }
        if (c == '[') {
            /* [label](url) */
            size_t close = i + 1;
            while (close < n && t[close] != ']') close++;
            if (close < n && close + 1 < n && t[close + 1] == '(') {
                size_t end = close + 2;
                while (end < n && t[end] != ')') end++;
                if (end < n) {
                    if (*st->underline) buf_append_str(dst, st->underline);
                    buf_append(dst, t + i + 1, close - i - 1);
                    if (*st->reset) buf_append_str(dst, st->reset);
                    buf_append_str(dst, " (");
                    if (*st->blue) buf_append_str(dst, st->blue);
                    buf_append(dst, t + close + 2, end - close - 2);
                    if (*st->reset) buf_append_str(dst, st->reset);
                    buf_append_byte(dst, ')');
                    i = end + 1;
                    continue;
                }
            }
        }
        buf_append_byte(dst, c);
        i++;
    }
}

/* leading-width helpers */
static size_t line_indent(const char *t, size_t n) {
    size_t i = 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    return i;
}

static bool is_fence(const char *t, size_t n, char *ch, size_t *len) {
    size_t ind = line_indent(t, n);
    if (ind >= 4) return false;
    size_t i = ind;
    if (i >= n) return false;
    char c = t[i];
    if (c != '`' && c != '~') return false;
    size_t l = 0;
    while (i < n && t[i] == c) { l++; i++; }
    if (l < 3) return false;
    *ch = c;
    *len = l;
    return true;
}

/* render one complete line (no trailing \n) */
static void line_render(md_line_t *m, const char *t, size_t n) {
    const md_style_t *st = m->st;
    dspy_t *out = m->out;

    /* a closing fence: test before the verbatim branch below - the
       fence's own lines never render, only its toggle does */
    char fch;
    size_t flen_c;
    if (m->in_fence && is_fence(t, n, &fch, &flen_c) &&
        fch == m->fence_char && flen_c >= m->fence_len) {
        m->in_fence = false;
        dspy_write_str(out, "\n");
        return;
    }

    /* inside a fence: everything verbatim */
    if (m->in_fence) {
        dspy_write(out, t, n);
        dspy_write_str(out, "\n");
        return;
    }

    size_t ind = line_indent(t, n);
    if (ind >= 4 && !m->table_mode) {
        /* indented code: verbatim */
        dspy_write(out, t, n);
        dspy_write_str(out, "\n");
        return;
    }

    /* blank line */
    if (ind == n) {
        if (m->have_pending) {
            /* a held header line above a blank: plain text, flushed */
            buf_t body;
            buf_init(&body);
            scan_inline(m, &body, m->pending.data, m->pending.len - 1);
            dspy_write(out, body.data ? body.data : "", body.len);
            buf_free(&body);
            buf_clear(&m->pending);
            m->have_pending = false;
        }
        if (m->table_row.len) line_table_flush(m); /* blank ends a table */
        dspy_write_str(out, "\n");
        m->table_mode = false; /* a blank closes a table */
        return;
    }

    char ch;
    size_t flen;
    if (is_fence(t, n, &ch, &flen)) {
        m->in_fence = true; /* reached only outside a fence now */
        m->fence_char = ch;
        m->fence_len = flen;
        m->fence_saw_lang = false;
        /* the language banner: the info string on the opening fence */
        {
            size_t li = ind + flen;
            while (li < n && t[li] == ' ') li++;
            size_t le = li;
            while (le < n && t[le] != ' ' && t[le] != '\t') le++;
            if (le > li) {
                char banner[80];
                size_t bn = le - li < sizeof banner - 1 ? le - li
                                                        : sizeof banner - 1;
                memcpy(banner, t + li, bn);
                banner[bn] = '\0';
                dspy_write_str(out, "  ");
                dspy_styled(out, st->dim, banner);
                dspy_write_str(out, "\n");
                m->fence_saw_lang = true;
            }
        }
        return;
    }

    /* ATX heading */
    if (ind < n && t[ind] == '#') {
        size_t h = ind;
        int level = 0;
        while (h < n && t[h] == '#' && level < 6) { h++; level++; }
        if (h < n && t[h] == ' ') {
            size_t b = h + 1;
            while (b < n && t[b] == ' ') b++;
            size_t e = n;
            while (e > b && (t[e - 1] == ' ' || t[e - 1] == '#')) e--;
            buf_t body;
            buf_init(&body);
            scan_inline(m, &body, t + b, e - b);
            const char *on = level <= 2 ? st->cyan : st->bold;
            if (level <= 2 && *st->bold) dspy_write_str(out, st->bold);
            if (*on) dspy_write_str(out, on);
            dspy_write(out, body.data ? body.data : "", body.len);
            if (*st->reset) dspy_write_str(out, st->reset);
            dspy_write_str(out, "\n");
            buf_free(&body);
            m->table_mode = false;
            return;
        }
    }

    /* tables: a pipe line is held as a candidate header; the delimiter
       next promotes it, anything else flushes it as plain text and the
       current line takes the normal paths */
    bool has_pipe = memchr(t, '|', n) != NULL;
    bool delim = false;
    if (has_pipe && !m->table_mode) {
        /* a delimiter row: only -, :, |, space */
        delim = true;
        for (size_t i = 0; i < n; i++) {
            char c = t[i];
            if (c != '-' && c != ':' && c != '|' && c != ' ' && c != '\t') {
                delim = false;
                break;
            }
        }
        delim = delim && memchr(t, '-', n) != NULL;
    }
    if (m->table_mode) {
        /* a body row: collect, the close flushes aligned */
        buf_append(&m->table_row, t + ind, n - ind);
        buf_append_byte(&m->table_row, '\n');
        return;
    }
    if (delim) {
        if (m->have_pending) {
            /* the held header joins the table, the delimiter vanishes */
            buf_append(&m->table_row, m->pending.data, m->pending.len);
            buf_clear(&m->pending);
            m->have_pending = false;
            m->table_mode = true;
        } else {
            /* a stray delimiter: paragraph text */
            buf_t body;
            buf_init(&body);
            scan_inline(m, &body, t + ind, n - ind);
            dspy_write(out, body.data ? body.data : "", body.len);
            dspy_write_str(out, "\n");
            buf_free(&body);
        }
        return;
    }
    if (m->have_pending) {
        /* not a delimiter: the held line was plain text, not a header */
        buf_t body;
        buf_init(&body);
        scan_inline(m, &body, m->pending.data, m->pending.len - 1);
        dspy_write(out, body.data ? body.data : "", body.len);
        dspy_write_str(out, "\n");
        buf_free(&body);
        buf_clear(&m->pending);
        m->have_pending = false;
        /* fall through: the current line renders below */
    } else if (has_pipe && !m->table_mode) {
        /* candidate header: hold it for one line */
        buf_append(&m->pending, t + ind, n - ind);
        buf_append_byte(&m->pending, '\n');
        m->have_pending = true;
        return;
    }

    /* quote bar */
    if (ind < n && t[ind] == '>') {
        size_t b = ind + 1;
        if (b < n && t[b] == ' ') b++;
        dspy_write_str(out, "| ");
        buf_t body;
        buf_init(&body);
        scan_inline(m, &body, t + b, n - b);
        dspy_write(out, body.data ? body.data : "", body.len);
        dspy_write_str(out, "\n");
        buf_free(&body);
        m->table_mode = false;
        return;
    }

    /* list marker */
    bool is_ol = false;
    size_t mk = ind;
    if (mk < n && (t[mk] == '-' || t[mk] == '*' || t[mk] == '+') &&
        mk + 1 < n && t[mk + 1] == ' ') {
        dspy_write(out, t + ind, 2); /* the marker, source shape */
        mk += 2;
    } else {
        size_t dig = mk;
        size_t ndig = 0;
        while (dig < n && t[dig] >= '0' && t[dig] <= '9') { dig++; ndig++; }
        if (ndig && ndig <= 9 && dig < n &&
            (t[dig] == '.' || t[dig] == ')') && dig + 1 < n &&
            t[dig + 1] == ' ') {
            is_ol = true;
            dspy_write(out, t + ind, dig + 2 - ind);
            mk = dig + 2;
        }
    }
    if (mk > ind) {
        /* the body after the marker */
        buf_t body;
        buf_init(&body);
        scan_inline(m, &body, t + mk, n - mk);
        dspy_write(out, body.data ? body.data : "", body.len);
        dspy_write_str(out, "\n");
        buf_free(&body);
        m->table_mode = false;
        return;
    }
    (void)is_ol;

    /* paragraph text */
    buf_t body;
    buf_init(&body);
    scan_inline(m, &body, t + ind, n - ind);
    dspy_write(out, body.data ? body.data : "", body.len);
    dspy_write_str(out, "\n");
    buf_free(&body);
    m->table_mode = false;
}

/* the table collected: measure and write aligned, the same shapes as
   the whole-doc renderer's table_flush */
static void line_table_flush(md_line_t *m) {
    if (!m->table_row.len) return;
    /* count columns from the first row, split on '|'; the boundary
       pipes (a leading/trailing '|') are not cells */
    const char *fp = m->table_row.data;
    size_t fn = 0;
    while (fn < m->table_row.len && m->table_row.data[fn] != '\n') fn++;
    size_t cols = 1;
    {
        if (fn && fp[0] == '|') fn--;
        if (fn && fp[fn - 1] == '|') fn--;
        for (size_t i = 0; i < fn; i++)
            if (fp[i] == '|') cols++;
    }
    /* header + delimiter + body rows are all in the buffer; the
       delimiter row is skipped at write time (it only shaped cols) */
    char *save = m->table_row.data;
    size_t nrows = 0;
    char *rows[64];
    size_t row_lens[64];
    for (size_t off = 0; off < m->table_row.len;) {
        char *nl = memchr(m->table_row.data + off, '\n',
                          m->table_row.len - off);
        size_t l = nl ? (size_t)(nl - (m->table_row.data + off))
                      : m->table_row.len - off;
        if (nrows < 64) {
            rows[nrows] = m->table_row.data + off;
            row_lens[nrows] = l;
        }
        nrows++;
        if (!nl) break;
        off += l + 1;
    }
    if (nrows > 64) nrows = 64; /* cap: pathological tables degrade */
    if (!nrows) { buf_clear(&m->table_row); free(save); return; }

    /* column widths from every row (skipping the delimiter) */
    size_t *w = calloc(cols, sizeof *w);
    if (!w) {
        for (size_t r = 0; r < nrows; r++) {
            dspy_write(m->out, rows[r], row_lens[r]);
            dspy_write_str(m->out, "\n");
        }
        buf_clear(&m->table_row);
        free(save);
        return;
    }
    for (size_t r = 0; r < nrows; r++) {
        /* split the row into cells */
        const char *p = rows[r];
        size_t n = row_lens[r];
        /* strip a leading and trailing pipe */
        if (n && p[0] == '|') { p++; n--; }
        if (n && p[n - 1] == '|') n--;
        size_t ci = 0;
        size_t start = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '|') {
                size_t l = i - start;
                /* trim spaces around the cell */
                while (l && p[start] == ' ') { start++; l--; }
                while (l && p[start + l - 1] == ' ') l--;
                if (ci < cols && l > w[ci]) w[ci] = l;
                ci++;
                start = i + 1;
            }
        }
    }
    /* write rows: the header (r==0) is bold, a separator follows it */
    for (size_t r = 0; r < nrows; r++) {
        const char *p = rows[r];
        size_t n = row_lens[r];
        if (n && p[0] == '|') { p++; n--; }
        if (n && p[n - 1] == '|') n--;
        size_t ci = 0, start = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '|') {
                size_t l = i - start;
                while (l && p[start] == ' ') { start++; l--; }
                while (l && p[start + l - 1] == ' ') l--;
                if (r == 0 && *m->st->bold) dspy_write_str(m->out, m->st->bold);
                dspy_write(m->out, p + start, l);
                if (r == 0 && *m->st->reset)
                    dspy_write_str(m->out, m->st->reset);
                if (ci + 1 < cols) {
                    size_t pad = w[ci] + 2 - l;
                    for (size_t k = 0; k < pad; k++)
                        dspy_write_str(m->out, " ");
                }
                ci++;
                start = i + 1;
            }
        }
        dspy_write_str(m->out, "\n");
        if (r == 0) {
            /* the separator under the header, the table's one echo of
               the source's delimiter row */
            for (size_t c = 0; c < cols; c++)
                for (size_t k = 0; k < w[c] + 2; k++)
                    dspy_write_str(m->out, "-");
            dspy_write_str(m->out, "\n");
        }
    }
    free(w);
    buf_clear(&m->table_row);
}

int md_line_feed(md_line_t *m, const char *text, size_t n) {
    if (!m->active) return -1;
    if (!n) return 0;
    size_t off = 0;
    while (off < n) {
        const char *nl = memchr(text + off, '\n', n - off);
        if (!nl) {
            buf_append(&m->line, text + off, n - off);
            break;
        }
        size_t l = (size_t)(nl - (text + off));
        /* the buffered tail joins this line */
        buf_append(&m->line, text + off, l);
        line_render(m, m->line.data ? m->line.data : "", m->line.len);
        buf_clear(&m->line);
        off += l + 1;
    }
    return 0;
}

int md_line_close(md_line_t *m) {
    if (!m->active) return -1;
    if (m->line.len) {
        line_render(m, m->line.data, m->line.len);
        buf_clear(&m->line);
    }
    if (m->have_pending) {
        /* a held header never met its delimiter: plain text */
        buf_t body;
        buf_init(&body);
        scan_inline(m, &body, m->pending.data, m->pending.len - 1);
        dspy_write(m->out, body.data ? body.data : "", body.len);
        dspy_write_str(m->out, "\n");
        buf_free(&body);
        buf_clear(&m->pending);
        m->have_pending = false;
    }
    if (m->table_row.len) line_table_flush(m);
    m->in_fence = false;
    m->fence_char = 0;
    m->fence_len = 0;
    m->fence_saw_lang = false;
    m->table_mode = false;
    return 0;
}

void md_line_free(md_line_t *m) {
    buf_free(&m->line);
    buf_free(&m->table_row);
    buf_free(&m->pending);
    m->active = false;
}
