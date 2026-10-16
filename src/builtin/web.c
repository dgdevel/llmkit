#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ================= html dom (readability input) ================= */

typedef struct hnode hnode_t;
struct hnode {
    hnode_t *parent, *child, *tail, *next;
    char tag[12]; /* lowercase element name; "#r" for the root */
    char *text;   /* text nodes (tag "") own decoded text */
    char *cls, *id, *href, *src, *alt;
    double score;
};

typedef struct hdom {
    hnode_t *root;
    hnode_t **all; /* every node for iteration and teardown */
    size_t n, cap;
    bool oom;
} hdom_t;

static hnode_t *hnew(hdom_t *d, hnode_t *parent) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        d->all = realloc(d->all, d->cap * sizeof *d->all);
    }
    hnode_t *n = calloc(1, sizeof *n);
    if (!n) {
        d->oom = true;
        return NULL;
    }
    n->parent = parent;
    d->all[d->n++] = n;
    if (parent) {
        if (parent->tail) parent->tail->next = n;
        else parent->child = n;
        parent->tail = n;
    }
    return n;
}

static hnode_t *hadd_text(hdom_t *d, hnode_t *parent, const char *s,
                          size_t len) {
    hnode_t *n = hnew(d, parent);
    if (!n) return NULL;
    buf_t b;
    buf_init(&b);
    bm_ent_decode(&b, s, len);
    n->text = buf_steal(&b, NULL);
    return n;
}

static bool tag_in(const char *tag, const char *const *set, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(tag, set[i])) return true;
    return false;
}

static const char *const VOID_TAGS[] = { "area", "base", "br",  "col",
                                         "embed", "hr",   "img", "input",
                                         "link",  "meta", "param", "source",
                                         "track", "wbr" };
static const char *const RAW_TAGS[] = { "script", "style", "noscript",
                                        "template" };
static const char *const JUNK_TAGS[] = {
    "script", "style",   "noscript", "template", "svg",   "iframe", "form",
    "button", "input",   "select",   "textarea", "option", "object", "embed",
    "link",   "meta",    "base",     "canvas",   "video", "audio",  "track",
    "source"
};
static const char *const BLOCK_TAGS[] = {
    "p",      "div",   "ul",        "ol",   "li",   "table", "thead", "tbody",
    "tfoot",  "tr",    "td",        "th",   "h1",   "h2",    "h3",    "h4",
    "h5",     "h6",    "blockquote", "pre", "section", "article", "header",
    "footer", "nav",   "aside",     "main", "form", "fieldset", "hr", "figure",
    "figcaption", "dl", "dt",       "dd",   "address", "details", "summary"
};

enum {
    VOID_N = sizeof VOID_TAGS / sizeof VOID_TAGS[0],
    RAW_N = sizeof RAW_TAGS / sizeof RAW_TAGS[0],
    JUNK_N = sizeof JUNK_TAGS / sizeof JUNK_TAGS[0],
    BLOCK_N = sizeof BLOCK_TAGS / sizeof BLOCK_TAGS[0]
};

/* should an open 'cur' close when 'new' opens? */
static bool closes_on(const char *cur, const char *nw) {
    if (!strcmp(cur, "p") && tag_in(nw, BLOCK_TAGS, BLOCK_N)) return true;
    if (!strcmp(cur, "li") && !strcmp(nw, "li")) return true;
    if ((!strcmp(cur, "dt") || !strcmp(cur, "dd")) &&
        (!strcmp(nw, "dt") || !strcmp(nw, "dd")))
        return true;
    if ((!strcmp(cur, "td") || !strcmp(cur, "th")) &&
        (!strcmp(nw, "td") || !strcmp(nw, "th") || !strcmp(nw, "tr")))
        return true;
    if (!strcmp(cur, "tr") && !strcmp(nw, "tr")) return true;
    if (!strcmp(cur, "option") && !strcmp(nw, "option")) return true;
    if ((!strcmp(cur, "thead") || !strcmp(cur, "tbody") || !strcmp(cur, "tfoot")) &&
        (!strcmp(nw, "thead") || !strcmp(nw, "tbody") || !strcmp(nw, "tfoot")))
        return true;
    return false;
}

static size_t ci_find_close(const char *s, size_t i, size_t n,
                            const char *tag) {
    /* find "</tag" case-insensitively at or after i; s is NUL-terminated */
    size_t tl = strlen(tag);
    for (size_t k = i; k + tl + 2 <= n && s[k]; k++) {
        if (s[k] == '<' && s[k + 1] == '/' &&
            !strncasecmp(s + k + 2, tag, tl))
            return k;
    }
    return (size_t)-1;
}

static void dom_parse(hdom_t *d, const char *html, size_t n) {
    memset(d, 0, sizeof *d);
    char *s = malloc(n + 1);
    if (!s) {
        d->oom = true;
        return;
    }
    memcpy(s, html, n);
    s[n] = '\0';
    size_t len = 0;
    while (len < n && s[len]) len++; /* stop at any embedded NUL */
    d->root = hnew(d, NULL);
    if (!d->root) {
        free(s);
        return;
    }
    strcpy(d->root->tag, "#r");
    hnode_t *cur = d->root;
    size_t i = 0;
    while (i < len && !d->oom) {
        if (s[i] != '<') {
            size_t j = i;
            while (j < len && s[j] != '<') j++;
            hadd_text(d, cur, s + i, j - i);
            i = j;
            continue;
        }
        if (len - i >= 4 && !strncmp(s + i, "<!--", 4)) {
            const char *e = strstr(s + i + 4, "-->");
            i = e ? (size_t)(e - s) + 3 : len;
            continue;
        }
        if (s[i + 1] == '!' || s[i + 1] == '?') {
            const char *e = strchr(s + i, '>');
            i = e ? (size_t)(e - s) + 1 : len;
            continue;
        }
        if (s[i + 1] == '/') {
            /* closing tag: unwind to the matching open element */
            size_t j = i + 2;
            char name[12];
            size_t nl = 0;
            while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-') &&
                   nl + 1 < sizeof name)
                name[nl++] = (char)tolower((unsigned char)s[j++]);
            name[nl] = '\0';
            const char *e = strchr(s + j, '>');
            i = e ? (size_t)(e - s) + 1 : len;
            if (!nl || !strcmp(name, "#r")) continue;
            for (hnode_t *p = cur; p && p != d->root; p = p->parent)
                if (!strcmp(p->tag, name)) {
                    cur = p->parent;
                    break;
                }
            continue;
        }
        if (!isalpha((unsigned char)s[i + 1])) {
            /* stray '<': text */
            hadd_text(d, cur, s + i, 1);
            i++;
            continue;
        }
        /* opening tag */
        size_t j = i + 1;
        char name[12];
        size_t nl = 0;
        while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-') &&
               nl + 1 < sizeof name)
            name[nl++] = (char)tolower((unsigned char)s[j++]);
        name[nl] = '\0';
        /* attributes until '>' */
        char *cls = NULL, *id = NULL, *href = NULL, *src = NULL, *alt = NULL;
        bool selfclose = false;
        for (;;) {
            while (j < len && bm_is_ws(s[j])) j++;
            if (j >= len) break;
            if (s[j] == '>') {
                j++;
                break;
            }
            if (s[j] == '/') {
                selfclose = true;
                j++;
                continue;
            }
            char an[16];
            size_t al = 0;
            while (j < len && !bm_is_ws(s[j]) && s[j] != '=' && s[j] != '>' &&
                   s[j] != '/' && al + 1 < sizeof an)
                an[al++] = (char)tolower((unsigned char)s[j++]);
            an[al] = '\0';
            if (al && s[j] == '=') {
                j++;
                while (j < len && bm_is_ws(s[j])) j++;
                char q = 0;
                if (j < len && (s[j] == '"' || s[j] == '\'')) {
                    q = s[j++];
                }
                size_t vs = j;
                while (j < len && (q ? s[j] != q
                                     : (!bm_is_ws(s[j]) && s[j] != '>')))
                    j++;
                buf_t vb;
                buf_init(&vb);
                bm_ent_decode(&vb, s + vs, j - vs);
                if (q && j < len) j++; /* closing quote */
                char *val = buf_steal(&vb, NULL);
                if (!strcmp(an, "class") && !cls) cls = val;
                else if (!strcmp(an, "id") && !id) id = val;
                else if (!strcmp(an, "href") && !href) href = val;
                else if (!strcmp(an, "src") && !src) src = val;
                else if (!strcmp(an, "alt") && !alt) alt = val;
                else free(val);
            }
        }
        i = j;
        if (tag_in(name, RAW_TAGS, RAW_N)) {
            /* raw text element: skip to its close, keep nothing */
            size_t cl = ci_find_close(s, i, len, name);
            if (cl == (size_t)-1) i = len;
            else {
                const char *e = strchr(s + cl, '>');
                i = e ? (size_t)(e - s) + 1 : len;
            }
            free(cls); free(id); free(href); free(src); free(alt);
            continue;
        }
        while (cur != d->root && closes_on(cur->tag, name)) cur = cur->parent;
        bool voidt = tag_in(name, VOID_TAGS, VOID_N);
        hnode_t *el = hnew(d, cur);
        if (!el) break;
        strcpy(el->tag, name);
        el->cls = cls;
        el->id = id;
        el->href = href;
        el->src = src;
        el->alt = alt;
        if (!selfclose && !voidt) cur = el;
    }
    free(s);
}

static void dom_free(hdom_t *d) {
    for (size_t i = 0; i < d->n; i++) {
        free(d->all[i]->text);
        free(d->all[i]->cls);
        free(d->all[i]->id);
        free(d->all[i]->href);
        free(d->all[i]->src);
        free(d->all[i]->alt);
        free(d->all[i]);
    }
    free(d->all);
    memset(d, 0, sizeof *d);
}

/* unlink children whose tag is in the junk set, recursively */
static void strip_tags(hnode_t *n, const char *const *set, size_t setn) {
    hnode_t **link = &n->child;
    n->tail = NULL;
    hnode_t *last = NULL;
    for (hnode_t *c = n->child; c;) {
        hnode_t *next = c->next;
        if (c->text ? false : tag_in(c->tag, set, setn)) {
            c->parent = NULL; /* subtree detached; freed at dom_free */
            c->next = NULL;
        } else {
            strip_tags(c, set, setn);
            *link = c;
            last = c;
            link = &c->next;
        }
        c = next;
    }
    *link = NULL;
    n->tail = last;
}

static hnode_t *find_tag(hdom_t *d, const char *tag) {
    for (size_t i = 0; i < d->n; i++)
        if (d->all[i]->text == NULL && !strcmp(d->all[i]->tag, tag))
            return d->all[i];
    return NULL;
}

/* concatenated text of a subtree (with a work budget); <br> counts as \n */
static void node_text(const hnode_t *n, buf_t *out, long *budget) {
    if (*budget <= 0) return;
    if (n->text) {
        buf_append(out, n->text, strlen(n->text));
        *budget -= (long)strlen(n->text);
    } else if (!strcmp(n->tag, "br")) {
        buf_append_byte(out, '\n');
    }
    for (hnode_t *c = n->child; c; c = c->next) {
        if (*budget <= 0) return;
        node_text(c, out, budget);
    }
}

/* ---- readability-lite scoring ---- */

static double pattern_bonus(const hnode_t *e) {
    static const char *const pos[] = { "article", "body",    "main",
                                       "content", "entry",   "post",
                                       "text",    "blog",    "story" };
    static const char *const neg[] = { "nav",     "footer",  "header",
                                       "sidebar", "comment", "menu",
                                       "advert",  "promo",   "social",
                                       "share",   "widget",  "meta",
                                       "hidden",  "related", "popup",
                                       "banner",  "sponsor" };
    double s = 0;
    for (size_t i = 0; i < sizeof pos / sizeof pos[0]; i++) {
        if (bm_ci_contains(e->cls, pos[i]) || bm_ci_contains(e->id, pos[i])) s += 25;
    }
    for (size_t i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        if (bm_ci_contains(e->cls, neg[i]) || bm_ci_contains(e->id, neg[i])) s -= 25;
    }
    if (s > 50) s = 50;
    if (s < -25) s = -25;
    return s;
}

static bool scoreable_tag(const char *t) {
    return !strcmp(t, "p") || !strcmp(t, "pre") || !strcmp(t, "blockquote") ||
           !strcmp(t, "td") || !strcmp(t, "div") || !strcmp(t, "section") ||
           !strcmp(t, "article") || !strcmp(t, "main") || !strcmp(t, "figure");
}

static hnode_t *best_candidate(hdom_t *d) {
    long budget = 4L * 1024 * 1024;
    /* paragraphs feed their ancestors */
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag)) continue;
        if (strcmp(e->tag, "p") && strcmp(e->tag, "pre") &&
            strcmp(e->tag, "blockquote") && strcmp(e->tag, "td"))
            continue;
        buf_t t;
        buf_init(&t);
        node_text(e, &t, &budget);
        long commas = 0;
        for (size_t k = 0; k < t.len; k++)
            if (t.data[k] == ',') commas++;
        long pts = 1 + commas + (t.len > 2000 ? 20 : (long)(t.len / 100));
        if (e->parent) e->parent->score += pts;
        if (e->parent && e->parent->parent) e->parent->parent->score += pts / 2.0;
        buf_free(&t);
        if (budget <= 0) break;
    }
    /* direct-text candidates */
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag)) continue;
        size_t dl = 0;
        long dc = 0;
        for (hnode_t *c = e->child; c; c = c->next) {
            if (c->text) {
                dl += strlen(c->text);
                for (const char *p = c->text; *p; p++)
                    if (*p == ',') dc++;
            }
        }
        if (dl > 20) e->score += 1 + dc + (long)(dl / 100);
        e->score += pattern_bonus(e);
    }
    hnode_t *best = NULL;
    for (size_t i = 0; i < d->n; i++) {
        hnode_t *e = d->all[i];
        if (e->text || !scoreable_tag(e->tag) || !strcmp(e->tag, "#r")) continue;
        if (e->score > 0 && (!best || e->score > best->score)) best = e;
    }
    if (!best) return d->root;
    /* a single paragraph is not a page: take its block parent */
    if (!strcmp(best->tag, "p") || !strcmp(best->tag, "pre") ||
        !strcmp(best->tag, "blockquote") || !strcmp(best->tag, "td"))
        best = (best->parent && strcmp(best->parent->tag, "#r"))
                   ? best->parent
                   : best;
    return best;
}

/* ---- markdown rendering ---- */

static void md_inline(const hnode_t *n, buf_t *out);
static void md_one(const hnode_t *n, buf_t *out);
static void md_li(const hnode_t *li, buf_t *out);
static void md_trim(buf_t *b);

/* append one rendered block: a single blank line between blocks */
static void md_join_block(buf_t *out, const buf_t *sub) {
    if (!sub->len) return;
    if (out->len) {
        size_t nl = 0;
        for (size_t i = out->len; i > 0 && out->data[i - 1] == '\n'; i--) nl++;
        if (nl < 2) buf_append_str(out, "\n\n");
    }
    buf_append(out, sub->data, sub->len);
}

static void md_inline_children(const hnode_t *n, buf_t *out) {
    for (const hnode_t *c = n->child; c; c = c->next) md_inline(c, out);
}

static void md_inline(const hnode_t *n, buf_t *out) {
    if (!n) return;
    if (n->text) {
        /* collapse internal runs, keep edge spaces: the block assembly
           trims the finished run once */
        size_t len = strlen(n->text), i = 0;
        while (i < len) {
            if (bm_is_ws(n->text[i])) {
                while (i < len && bm_is_ws(n->text[i])) i++;
                buf_append_byte(out, ' ');
            } else {
                buf_append_byte(out, n->text[i++]);
            }
        }
        return;
    }
    const char *t = n->tag;
    if (!strcmp(t, "br")) {
        buf_append_byte(out, '\n');
    } else if (!strcmp(t, "img")) {
        buf_appendf(out, "![%s](%s)", n->alt ? n->alt : "",
                    n->src ? n->src : "");
    } else if (!strcmp(t, "a")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        if (n->href && n->href[0] && inner.len)
            buf_appendf(out, "[%s](%s)", inner.data, n->href);
        else
            buf_append(out, inner.data, inner.len);
        buf_free(&inner);
    } else if (!strcmp(t, "strong") || !strcmp(t, "b")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "**%s**", inner.data);
        buf_free(&inner);
    } else if (!strcmp(t, "em") || !strcmp(t, "i")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "*%s*", inner.data);
        buf_free(&inner);
    } else if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") ||
               !strcmp(t, "tt") || !strcmp(t, "var")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) buf_appendf(out, "`%s`", inner.data);
        buf_free(&inner);
    } else {
        md_inline_children(n, out);
    }
}

/* trim whitespace at both ends of a buf */
static void md_trim(buf_t *b) {
    size_t r = 0;
    while (r < b->len && bm_is_ws(b->data[r])) r++; /* leading */
    size_t w = 0;
    while (r < b->len) b->data[w++] = b->data[r++];
    while (w && bm_is_ws(b->data[w - 1])) w--;
    b->len = w;
    if (b->cap) b->data[w] = '\0';
}

/* prefix every nonempty line (all lines when all=true) with prefix */
static void prefix_lines(buf_t *b, const char *prefix, bool all) {
    buf_t o;
    buf_init(&o);
    size_t start = 0;
    for (size_t i = 0; i <= b->len; i++) {
        if (i == b->len || b->data[i] == '\n') {
            bool emit_nl = i < b->len;
            bool empty = i == start;
            if (all || !empty) buf_append_str(&o, prefix);
            buf_append(&o, b->data + start, i - start);
            if (emit_nl) buf_append_byte(&o, '\n');
            start = i + 1;
        }
    }
    buf_clear(b);
    buf_append(b, o.data ? o.data : "", o.len);
    buf_free(&o);
}

/* indent every continuation line (after the first) by n spaces */
static void indent_continuation(buf_t *b, size_t n) {
    buf_t o;
    buf_init(&o);
    for (size_t i = 0; i < b->len; i++) {
        buf_append_byte(&o, b->data[i]);
        if (b->data[i] == '\n')
            for (size_t k = 0; k < n; k++) buf_append_byte(&o, ' ');
    }
    buf_clear(b);
    buf_append(b, o.data ? o.data : "", o.len);
    buf_free(&o);
}

static bool is_list(const hnode_t *n) {
    return !n->text && (!strcmp(n->tag, "ul") || !strcmp(n->tag, "ol"));
}

static bool is_heading(const hnode_t *n) {
    return !n->text && n->tag[0] == 'h' && isdigit((unsigned char)n->tag[1]) &&
           !n->tag[2];
}

/* one block element rendered into out (no leading/trailing newlines) */
static void md_one(const hnode_t *n, buf_t *out) {
    if (!n) return;
    if (n->text) {
        buf_t t;
        buf_init(&t);
        buf_append_str(&t, n->text);
        bm_collapse_ws(&t);
        if (t.len) buf_append(out, t.data, t.len);
        buf_free(&t);
        return;
    }
    const char *t = n->tag;
    if (is_heading(n)) {
        int lvl = t[1] - '0';
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        if (inner.len) {
            for (int i = 0; i < lvl; i++) buf_append_byte(out, '#');
            buf_appendf(out, " %s", inner.data);
        }
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "p") || !strcmp(t, "figcaption")) {
        buf_t inner;
        buf_init(&inner);
        md_inline_children(n, &inner);
        bm_collapse_ws(&inner);
        md_trim(&inner);
        buf_append(out, inner.data, inner.len);
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "ul") || !strcmp(t, "ol")) {
        bool ordered = t[0] == 'o';
        long idx = 1;
        bool first = true;
        for (const hnode_t *li = n->child; li; li = li->next) {
            if (li->text) continue; /* stray text between items */
            if (strcmp(li->tag, "li")) {
                /* unexpected child: render as its own block */
                buf_t sub;
                buf_init(&sub);
                md_one(li, &sub);
                if (sub.len) {
                    if (!first) buf_append_byte(out, '\n');
                    buf_append(out, sub.data, sub.len);
                    first = false;
                }
                buf_free(&sub);
                continue;
            }
            buf_t lb;
            buf_init(&lb);
            md_li(li, &lb);
            char marker[24];
            long shown = idx > 999999 ? 999999 : idx;
            if (ordered) snprintf(marker, sizeof marker, "%ld. ", shown);
            else snprintf(marker, sizeof marker, "- ");
            idx++;
            indent_continuation(&lb, strlen(marker));
            if (!first) buf_append_byte(out, '\n');
            buf_append_str(out, marker);
            buf_append(out, lb.data ? lb.data : "", lb.len);
            buf_free(&lb);
            first = false;
        }
        return;
    }
    if (!strcmp(t, "blockquote")) {
        buf_t inner;
        buf_init(&inner);
        for (const hnode_t *c = n->child; c; c = c->next) {
            buf_t sub;
            buf_init(&sub);
            md_one(c, &sub);
            if (sub.len) md_join_block(&inner, &sub);
            buf_free(&sub);
        }
        prefix_lines(&inner, "> ", true);
        buf_append(out, inner.data ? inner.data : "", inner.len);
        buf_free(&inner);
        return;
    }
    if (!strcmp(t, "pre")) {
        long budget = 1L << 20;
        buf_t raw;
        buf_init(&raw);
        node_text(n, &raw, &budget);
        while (raw.len && (raw.data[raw.len - 1] == '\n' ||
                           bm_is_ws(raw.data[raw.len - 1])))
            raw.len--;
        if (raw.cap) raw.data[raw.len] = '\0';
        buf_append_str(out, "```\n");
        buf_append(out, raw.data ? raw.data : "", raw.len);
        buf_append_str(out, "\n```");
        buf_free(&raw);
        return;
    }
    if (!strcmp(t, "hr")) {
        buf_append_str(out, "---");
        return;
    }
    if (!strcmp(t, "table")) {
        bool first = true;
        for (const hnode_t *sec = n->child; sec; sec = sec->next) {
            bool is_sec =
                !sec->text && (!strcmp(sec->tag, "thead") ||
                               !strcmp(sec->tag, "tbody") ||
                               !strcmp(sec->tag, "tfoot"));
            if (!is_sec && (sec->text || strcmp(sec->tag, "tr"))) continue;
            /* thead/tbody wrap their rows; a bare tr is its own row */
            const hnode_t *rows = is_sec ? sec->child : sec;
            for (const hnode_t *tr = rows; tr; tr = tr->next) {
                if (tr->text || strcmp(tr->tag, "tr")) continue;
                buf_t row;
                buf_init(&row);
                size_t c = 0;
                for (const hnode_t *cell = tr->child; cell; cell = cell->next) {
                    if (cell->text || (strcmp(cell->tag, "td") &&
                                       strcmp(cell->tag, "th")))
                        continue;
                    buf_t ct;
                    buf_init(&ct);
                    md_inline_children(cell, &ct);
                    bm_collapse_ws(&ct);
                    md_trim(&ct);
                    if (c) buf_append_str(&row, " | ");
                    buf_append_str(&row, ct.data ? ct.data : "");
                    buf_free(&ct);
                    c++;
                }
                if (row.len || c) {
                    if (!first) buf_append_byte(out, '\n');
                    buf_appendf(out, "| %s |", row.data ? row.data : "");
                    if (first) { /* separator under the header row */
                        buf_append_byte(out, '\n');
                        buf_append_byte(out, '|');
                        for (size_t k = 0; k < c; k++)
                            buf_append_str(out, " --- |");
                    }
                    first = false;
                }
                buf_free(&row);
                if (!is_sec) break; /* the single tr was sec itself */
            }
        }
        return;
    }
    if (!strcmp(t, "img")) {
        buf_appendf(out, "![%s](%s)", n->alt ? n->alt : "",
                    n->src ? n->src : "");
        return;
    }
    /* structural containers and unknown tags: children as blocks */
    for (const hnode_t *c = n->child; c; c = c->next) {
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        md_join_block(out, &sub);
        buf_free(&sub);
    }
}

/* render an li: an inline run plus any nested blocks, joined into one buf */
static void md_li(const hnode_t *li, buf_t *out) {
    buf_t inline_acc;
    buf_init(&inline_acc);
    for (const hnode_t *c = li->child; c; c = c->next) {
        bool block_child =
            !c->text && (is_list(c) || !strcmp(c->tag, "p") ||
                         !strcmp(c->tag, "pre") || !strcmp(c->tag, "table") ||
                         !strcmp(c->tag, "blockquote") || is_heading(c));
        if (!block_child) {
            md_inline(c, &inline_acc);
            continue;
        }
        /* block child: flush the inline run accumulated so far */
        bm_collapse_ws(&inline_acc);
        md_trim(&inline_acc);
        if (inline_acc.len) {
            md_join_block(out, &inline_acc);
            buf_clear(&inline_acc);
        }
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        md_join_block(out, &sub);
        buf_free(&sub);
    }
    bm_collapse_ws(&inline_acc);
    md_trim(&inline_acc);
    if (inline_acc.len) md_join_block(out, &inline_acc);
    buf_free(&inline_acc);
}

char *builtin_html_to_markdown(const char *html, size_t n) {
    hdom_t d;
    dom_parse(&d, html, n);
    if (d.oom || !d.root) {
        dom_free(&d);
        return strdup("");
    }
    strip_tags(d.root, JUNK_TAGS, JUNK_N);

    buf_t out;
    buf_init(&out);

    /* title */
    hnode_t *title = find_tag(&d, "title");
    if (title) {
        long budget = 4096;
        buf_t tb;
        buf_init(&tb);
        node_text(title, &tb, &budget);
        bm_collapse_ws(&tb);
        md_trim(&tb);
        if (tb.len) buf_appendf(&out, "# %s\n\n", tb.data);
        buf_free(&tb);
    }

    hnode_t *best = best_candidate(&d);
    {
        static const char *const chrome[] = { "nav", "aside", "footer" };
        strip_tags(best, chrome, sizeof chrome / sizeof chrome[0]);
    }

    for (const hnode_t *c = best->child; c; c = c->next) {
        buf_t sub;
        buf_init(&sub);
        md_one(c, &sub);
        if (sub.len) md_join_block(&out, &sub);
        buf_free(&sub);
    }

    while (out.len && bm_is_ws(out.data[out.len - 1])) out.len--;
    if (out.cap) out.data[out.len] = '\0';

    char *res = strdup(out.data ? out.data : "");
    buf_free(&out);
    dom_free(&d);
    return res;
}


/* ================= duckduckgo results parsing ================= */

/* normalize a result href: unwrap the duckduckgo redirect, absolutize */
static char *ddg_url(const char *href) {
    if (!href) return NULL;
    buf_t out;
    buf_init(&out);
    const char *u = strstr(href, "uddg=");
    if (u) {
        u += 5;
        const char *e = strchr(u, '&');
        bm_pct_decode(&out, u, e ? (size_t)(e - u) : strlen(u));
    } else if (!strncmp(href, "//", 2)) {
        buf_appendf(&out, "https:%s", href);
    } else if (href[0] == '/') {
        buf_appendf(&out, "https://duckduckgo.com%s", href);
    } else {
        buf_append_str(&out, href);
    }
    return buf_steal(&out, NULL);
}

/* walk the parsed page in document order: an element whose class holds
   result__a opens a pending result (its text is the title), the next
   result__snippet element completes it (its text is the description);
   a new result__a flushes the pending one with the title alone. the
   d.all array is in document order, which is the pairing order. */
char *builtin_ddg_results(const char *html, size_t n) {
    hdom_t d;
    dom_parse(&d, html, n);
    buf_t out;
    buf_init(&out);
    size_t results = 0;
    char *pending_url = NULL, *pending_title = NULL;
    for (size_t i = 0; i < d.n && results < 25; i++) {
        hnode_t *e = d.all[i];
        if (e->text) continue;
        bool is_link = bm_ci_contains(e->cls, "result__a");
        bool is_snip = bm_ci_contains(e->cls, "result__snippet");
        if (!is_link && !is_snip) continue;
        buf_t t;
        buf_init(&t);
        long budget = 1 << 20;
        node_text(e, &t, &budget);
        bm_collapse_ws(&t);
        char *text = buf_steal(&t, NULL);
        if (is_link) {
            if (pending_url) { /* flush the pending result without snippet */
                if (results) buf_append_byte(&out, '\n');
                buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                            pending_title ? pending_title : "");
                results++;
                free(pending_url);
                free(pending_title);
            }
            pending_url = ddg_url(e->href);
            pending_title = text;
        } else if (pending_url) {
            if (results) buf_append_byte(&out, '\n');
            buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                        text && text[0] ? text
                                        : (pending_title ? pending_title
                                                         : ""));
            results++;
            free(pending_url);
            free(pending_title);
            pending_url = NULL;
            pending_title = NULL;
            free(text);
        } else {
            free(text); /* snippet without a preceding result */
        }
    }
    if (pending_url && results < 25) {
        if (results) buf_append_byte(&out, '\n');
        buf_appendf(&out, "URL: %s\nDescription: %s\n", pending_url,
                    pending_title ? pending_title : "");
        free(pending_url);
        free(pending_title);
    }
    dom_free(&d);
    return buf_steal(&out, NULL);
}


/* ================= web fetch helpers ================= */

static const char *BROWSER_UA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

static bool valid_http_url(const char *u) {
    size_t n = strlen(u);
    if (!n || n > 2048) return false;
    for (const char *p = u; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c == 0x7f) return false;
    }
    /* structure (http/https scheme, nonempty host) via libcurl's parser */
    CURLU *h = curl_url();
    bool ok = h && curl_url_set(h, CURLUPART_URL, u, 0) == CURLUE_OK;
    if (ok) {
        char *scheme = NULL, *host = NULL;
        ok = curl_url_get(h, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
             scheme && (!strcasecmp(scheme, "http") ||
                        !strcasecmp(scheme, "https"));
        if (ok)
            ok = curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 host && host[0];
        curl_free(scheme);
        curl_free(host);
    }
    curl_url_cleanup(h);
    return ok;
}

/* GET a url; returns the body (malloc'd, *len_out set) or NULL with err */
static char *http_get(const char *url, size_t *len_out, char *ct_out,
                      size_t ctsz, long *status_out, char *err, size_t errsz) {
    http_req_t r;
    memset(&r, 0, sizeof r);
    buf_init(&r.resp);
    buf_init(&r.content_type);
    r.url = url;
    r.is_get = true;
    r.ua = BROWSER_UA;
    r.connect_to = 10;
    r.read_to = 30;
    r.total_to = 60;
    struct curl_slist *hdrs = NULL;
    http_hdr_add(&hdrs, "Accept", "text/html,application/xhtml+xml");
    r.hdrs = hdrs;
    int rc = http_perform(&r);
    char *body = NULL;
    if (rc == 0) {
        size_t l = r.resp.len;
        body = buf_steal(&r.resp, &l);
        *len_out = l;
        snprintf(ct_out, ctsz, "%s", r.content_type.data ? r.content_type.data
                                                         : "");
        *status_out = r.status;
    } else if (rc == 1) {
        snprintf(err, errsz, "http status %ld returned", r.status);
        *status_out = r.status;
    } else {
        snprintf(err, errsz, "transport error: %s",
                 curl_easy_strerror(r.curl_res));
    }
    buf_free(&r.resp);
    buf_free(&r.content_type);
    curl_slist_free_all(hdrs);
    return body;
}

/* ---- web tools ---- */

void tool_web_search(const cJSON *args, buf_t *out, bool *is_error) {
    const char *keywords = NULL;
    if (!bm_need_str(args, "keywords", &keywords, out)) {
        *is_error = true;
        return;
    }
    buf_t url;
    buf_init(&url);
    buf_append_str(&url, "https://html.duckduckgo.com/html/?q=");
    bm_url_encode(&url, keywords);
    size_t blen = 0;
    char ct[128] = "", err[256] = "";
    long status = 0;
    char *body = http_get(url.data, &blen, ct, sizeof ct, &status, err,
                          sizeof err);
    buf_free(&url);
    if (!body) {
        buf_clear(out);
        buf_appendf(out, "web_search failed: %s", err);
        *is_error = true;
        return;
    }
    char *res = builtin_ddg_results(body, blen);
    free(body);
    buf_clear(out);
    if (res) {
        buf_t t;
        buf_init(&t);
        buf_append_str(&t, res);
        bm_sanitize_utf8(&t);
        if (t.len) buf_append(out, t.data, t.len);
        else buf_append_str(out, "no results");
        buf_free(&t);
    } else {
        buf_append_str(out, "no results");
    }
    free(res);
}

void tool_web_fetch(const cJSON *args, buf_t *out, bool *is_error) {
    const char *url = NULL;
    if (!bm_need_str(args, "url", &url, out)) {
        *is_error = true;
        return;
    }
    if (!valid_http_url(url)) {
        buf_clear(out);
        buf_appendf(out, "invalid url: %s", url);
        *is_error = true;
        return;
    }
    size_t blen = 0;
    char ct[128] = "", err[256] = "";
    long status = 0;
    char *body = http_get(url, &blen, ct, sizeof ct, &status, err, sizeof err);
    if (!body) {
        buf_clear(out);
        buf_appendf(out, "web_fetch failed: %s", err);
        *is_error = true;
        return;
    }
    if (blen > 20ull * 1024 * 1024) {
        free(body);
        buf_clear(out);
        buf_append_str(out, "web_fetch failed: page too large (over 20Mb)");
        *is_error = true;
        return;
    }
    if (ct[0] && strncasecmp(ct, "text/html", 9) &&
        !strstr(ct, "application/xhtml")) {
        free(body);
        buf_clear(out);
        buf_appendf(out, "web_fetch failed: not an html page (content type %s)",
                    ct);
        *is_error = true;
        return;
    }
    char *md = builtin_html_to_markdown(body, blen);
    free(body);
    buf_clear(out);
    if (md && md[0]) {
        buf_t t = { md, strlen(md), 0 };
        bm_sanitize_utf8(&t);
        buf_append_str(out, md);
        free(md);
    } else {
        free(md);
        buf_append_str(out, "no readable content found");
        *is_error = true;
    }
}

/* write side of tool_download: bytes from http_perform into the FILE*,
   failures latched in ok (a short fwrite means disk full or similar) */
typedef struct dl_sink {
    FILE *f;
    bool ok;
    unsigned long long n;
} dl_sink_t;

static void dl_write(void *ctx, const char *bytes, size_t n) {
    dl_sink_t *s = ctx;
    if (!s->ok) return;
    s->ok = fwrite(bytes, 1, n, s->f) == n;
    if (s->ok) s->n += n;
}

void tool_download(const cJSON *args, buf_t *out, bool *is_error) {
    const char *url = NULL, *outputfile = NULL;
    if (!bm_need_str(args, "url", &url, out) ||
        !bm_need_str(args, "outputfile", &outputfile, out)) {
        *is_error = true;
        return;
    }
    if (!strlen(outputfile)) {
        buf_clear(out);
        buf_append_str(out, "outputfile must not be empty");
        *is_error = true;
        return;
    }
    struct stat st;
    if (!stat(outputfile, &st)) {
        buf_clear(out);
        buf_appendf(out, "'%s' already exists", outputfile);
        *is_error = true;
        return;
    }
    if (!valid_http_url(url)) {
        buf_clear(out);
        buf_appendf(out, "invalid url: %s", url);
        *is_error = true;
        return;
    }
    if (!bm_ensure_parent_dirs(outputfile)) {
        buf_clear(out);
        buf_appendf(out, "cannot create parent directories of '%s'",
                    outputfile);
        *is_error = true;
        return;
    }
    FILE *f = fopen(outputfile, "wb");
    if (!f) {
        buf_clear(out);
        buf_appendf(out, "cannot write '%s': %s", outputfile,
                    strerror(errno));
        *is_error = true;
        return;
    }
    dl_sink_t sink = { f, true, 0 };
    http_req_t r;
    memset(&r, 0, sizeof r);
    buf_init(&r.resp);
    buf_init(&r.content_type);
    r.url = url;
    r.is_get = true;
    r.ua = BROWSER_UA;
    r.connect_to = 10;
    r.read_to = 30;
    r.total_to = 600; /* big downloads get 10 minutes */
    r.on_data = dl_write;
    r.cb_ctx = &sink;
    int rc = http_perform(&r);
    if (rc == 0 && sink.ok && r.resp.len) {
        /* json content types stay in resp (platform.c): flush them out */
        sink.ok = fwrite(r.resp.data, 1, r.resp.len, f) == r.resp.len;
        if (sink.ok) sink.n += r.resp.len;
    }
    fclose(f);
    if (rc != 0 || !sink.ok) {
        remove(outputfile); /* unlink the partial file */
        buf_clear(out);
        if (rc == 1)
            buf_appendf(out, "download failed: http status %ld", r.status);
        else if (rc < 0)
            buf_appendf(out, "download failed: transport error: %s",
                        curl_easy_strerror(r.curl_res));
        else
            buf_appendf(out, "download failed: write error on '%s'",
                        outputfile);
        buf_free(&r.resp);
        buf_free(&r.content_type);
        *is_error = true;
        return;
    }
    buf_free(&r.resp);
    buf_free(&r.content_type);
    buf_clear(out);
    buf_appendf(out, "wrote %llu bytes to %s", sink.n, outputfile);
}
