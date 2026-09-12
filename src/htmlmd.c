#include "htmlmd.h"
#include "llmkit.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Minimal DOM                                                        */
/* ------------------------------------------------------------------ */

typedef struct html_node {
    char *tag; /* lowercased tag name; NULL for document root and text */
    char *text;
    char **attr_name; /* lowercased attribute names */
    char **attr_val;  /* raw values with entities decoded; "" if valueless */
    int attr_count;
    struct html_node *parent;
    struct html_node **children;
    int child_count;
} html_node;

static html_node *node_new(const char *tag) {
    html_node *n = calloc(1, sizeof(html_node));
    if (n == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    if (tag != NULL) n->tag = util_strdup(tag);
    return n;
}

static void node_free(html_node *n) {
    if (n == NULL) return;
    free(n->tag);
    free(n->text);
    for (int i = 0; i < n->attr_count; i++) {
        free(n->attr_name[i]);
        free(n->attr_val[i]);
    }
    free(n->attr_name);
    free(n->attr_val);
    for (int i = 0; i < n->child_count; i++) node_free(n->children[i]);
    free(n->children);
    free(n);
}

static void node_add_child(html_node *parent, html_node *child) {
    html_node **tmp = realloc(parent->children, (size_t)(parent->child_count + 1) * sizeof(*tmp));
    if (tmp == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    parent->children = tmp;
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

static void node_add_attr(html_node *n, const char *name, const char *val) {
    char **tn = realloc(n->attr_name, (size_t)(n->attr_count + 1) * sizeof(*tn));
    char **tv = realloc(n->attr_val, (size_t)(n->attr_count + 1) * sizeof(*tv));
    if (tn == NULL || tv == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    n->attr_name = tn;
    n->attr_val = tv;
    n->attr_name[n->attr_count] = util_strdup(name);
    n->attr_val[n->attr_count] = util_strdup(val ? val : "");
    n->attr_count++;
}

static const char *node_get_attr(const html_node *n, const char *name) {
    for (int i = 0; i < n->attr_count; i++) {
        if (strcmp(n->attr_name[i], name) == 0) return n->attr_val[i];
    }
    return NULL;
}

/* Case-insensitive check whether the class attribute contains `cls` as a
 * whitespace-separated token. */
static int node_has_class(const html_node *n, const char *cls) {
    const char *val = node_get_attr(n, "class");
    if (val == NULL) return 0;
    size_t clen = strlen(cls);
    const char *p = val;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if ((size_t)(p - start) == clen && strncasecmp(start, cls, clen) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Entity decoding                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *utf8;
} html_entity;

static const html_entity ENTITIES[] = {
    {"amp", "&"},
    {"lt", "<"},
    {"gt", ">"},
    {"quot", "\""},
    {"apos", "'"},
    {"nbsp", "\xc2\xa0"},
    {"copy", "\xc2\xa9"},
    {"reg", "\xc2\xae"},
    {"trade", "\xe2\x84\xa2"},
    {"hellip", "\xe2\x80\xa6"},
    {"mdash", "\xe2\x80\x94"},
    {"ndash", "\xe2\x80\x93"},
    {"lsquo", "\xe2\x80\x98"},
    {"rsquo", "\xe2\x80\x99"},
    {"ldquo", "\xe2\x80\x9c"},
    {"rdquo", "\xe2\x80\x9d"},
    {"laquo", "\xc2\xab"},
    {"raquo", "\xc2\xbb"},
    {"deg", "\xc2\xb0"},
    {"plusmn", "\xc2\xb1"},
    {"times", "\xc3\x97"},
    {"divide", "\xc3\xb7"},
    {"cent", "\xc2\xa2"},
    {"pound", "\xc2\xa3"},
    {"yen", "\xc2\xa5"},
    {"euro", "\xe2\x82\xac"},
    {"sect", "\xc2\xa7"},
    {"para", "\xc2\xb6"},
    {"middot", "\xc2\xb7"},
    {"bull", "\xe2\x80\xa2"},
    {"micro", "\xc2\xb5"},
    {"frac12", "\xc2\xbd"},
    {"frac14", "\xc2\xbc"},
    {"frac34", "\xc2\xbe"},
    {"sup1", "\xc2\xb9"},
    {"sup2", "\xc2\xb2"},
    {"sup3", "\xc2\xb3"},
    {"larr", "\xe2\x86\x90"},
    {"uarr", "\xe2\x86\x91"},
    {"rarr", "\xe2\x86\x92"},
    {"darr", "\xe2\x86\x93"},
    {"harr", "\xe2\x86\x94"},
    {NULL, NULL},
};

/* Encode one Unicode code point as UTF-8 (1-4 bytes) into out. */
static int utf8_encode(unsigned int cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode HTML entities (&amp; &#65; &#x41;) in `s` into a malloc'd string. */
static char *decode_entities(const char *s) {
    util_growbuf out = {0};
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') {
            util_growbuf_append(&out, s + i, 1);
            i++;
            continue;
        }
        /* Find the terminating ';' within a reasonable entity length. */
        size_t j = i + 1;
        while (j < n && s[j] != ';' && s[j] != '&' && j - i < 12) j++;
        int decoded = 0;
        if (j < n && s[j] == ';') {
            size_t name_len = j - i - 1;
            const char *name = s + i + 1;
            if (name_len >= 2 && name[0] == '#') {
                unsigned int cp = 0;
                int ok = 1;
                if (name[1] == 'x' || name[1] == 'X') {
                    for (size_t k = 2; k < name_len && ok; k++) {
                        char c = name[k];
                        int d;
                        if (c >= '0' && c <= '9') {
                            d = c - '0';
                        } else if (c >= 'a' && c <= 'f') {
                            d = c - 'a' + 10;
                        } else if (c >= 'A' && c <= 'F') {
                            d = c - 'A' + 10;
                        } else {
                            d = -1;
                        }
                        if (d < 0) {
                            ok = 0;
                        } else {
                            cp = (cp * 16) + (unsigned int)d;
                        }
                    }
                } else {
                    for (size_t k = 1; k < name_len && ok; k++) {
                        char c = name[k];
                        if (c < '0' || c > '9') {
                            ok = 0;
                        } else {
                            cp = (cp * 10) + (unsigned int)(c - '0');
                        }
                    }
                }
                if (ok && cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                    char enc[4];
                    int enc_len = utf8_encode(cp, enc);
                    util_growbuf_append(&out, enc, (size_t)enc_len);
                    decoded = 1;
                }
            } else {
                for (int e = 0; ENTITIES[e].name != NULL; e++) {
                    if (strlen(ENTITIES[e].name) == name_len &&
                        strncasecmp(name, ENTITIES[e].name, name_len) == 0) {
                        util_growbuf_append_str(&out, ENTITIES[e].utf8);
                        decoded = 1;
                        break;
                    }
                }
            }
            if (decoded) {
                i = j + 1;
                continue;
            }
        }
        /* Not a recognized entity: emit the '&' literally. */
        util_growbuf_append(&out, s + i, 1);
        i++;
    }
    return util_growbuf_release(&out);
}

/* ------------------------------------------------------------------ */
/*  Parser                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    html_node *root;
    html_node **stack; /* open elements; stack[0] == root */
    int stack_len;
    int stack_cap;
} html_parser;

static const char *const VOID_TAGS[] = {
    "area", "base", "br",    "col",    "embed", "hr",  "img", "input",
    "link", "meta", "param", "source", "track", "wbr", NULL,
};

static int is_void_tag(const char *tag) {
    for (int i = 0; VOID_TAGS[i] != NULL; i++) {
        if (strcmp(VOID_TAGS[i], tag) == 0) return 1;
    }
    return 0;
}

static html_node *parser_current(html_parser *p) {
    return p->stack[p->stack_len - 1];
}

static void parser_push(html_parser *p, html_node *n) {
    if (p->stack_len + 1 > p->stack_cap) {
        int new_cap = p->stack_cap ? p->stack_cap * 2 : 32;
        html_node **tmp = realloc(p->stack, (size_t)new_cap * sizeof(*tmp));
        if (tmp == NULL) {
            log_activity("[error] OOM in htmlmd");
            exit(EXIT_INTERNAL_ERR);
        }
        p->stack = tmp;
        p->stack_cap = new_cap;
    }
    p->stack[p->stack_len++] = n;
}

/* Pop elements until (and including) the nearest open `tag`. */
static void parser_close_through(html_parser *p, const char *tag) {
    while (p->stack_len > 1) {
        html_node *n = p->stack[--p->stack_len];
        if (n->tag != NULL && strcmp(n->tag, tag) == 0) break;
    }
}

/* Is `tag` open at or below the top of the stack without encountering any
 * of the stopper tags first (stop1/stop2 may be NULL)? */
static html_node *parser_find_open(html_parser *p, const char *tag, const char *stop1,
                                   const char *stop2) {
    for (int i = p->stack_len - 1; i >= 1; i--) {
        const char *t = p->stack[i]->tag;
        if (t == NULL) continue;
        if (strcmp(t, tag) == 0) return p->stack[i];
        if (stop1 != NULL && strcmp(t, stop1) == 0) return NULL;
        if (stop2 != NULL && strcmp(t, stop2) == 0) return NULL;
    }
    return NULL;
}

static void parser_append_text(html_parser *p, const char *start, size_t len) {
    if (len == 0) return;
    html_node *parent = parser_current(p);
    char *decoded = malloc(len + 1);
    if (decoded == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    memcpy(decoded, start, len);
    decoded[len] = '\0';
    char *text = decode_entities(decoded);
    free(decoded);

    /* Merge with a trailing text node so whitespace collapsing at render
     * time sees one continuous run. */
    html_node *last = parent->child_count > 0 ? parent->children[parent->child_count - 1] : NULL;
    if (last != NULL && last->tag == NULL) {
        size_t old_len = strlen(last->text);
        char *merged = realloc(last->text, old_len + strlen(text) + 1);
        if (merged == NULL) {
            log_activity("[error] OOM in htmlmd");
            exit(EXIT_INTERNAL_ERR);
        }
        last->text = merged;
        memcpy(last->text + old_len, text, strlen(text) + 1);
        free(text);
    } else {
        html_node *n = node_new(NULL);
        n->text = text;
        node_add_child(parent, n);
    }
}

/* Parse an attribute value: quoted ("..."/'...') or unquoted (up to
 * whitespace or '>'). Appends the decoded value to *out_val. */
static void parse_attr_value(html_parser *p, char **out_val) {
    util_growbuf raw = {0};
    if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
        char quote = p->src[p->pos];
        p->pos++;
        while (p->pos < p->len && p->src[p->pos] != quote) {
            util_growbuf_append(&raw, p->src + p->pos, 1);
            p->pos++;
        }
        if (p->pos < p->len) p->pos++; /* closing quote */
    } else {
        while (p->pos < p->len && p->src[p->pos] != '>' &&
               !isspace((unsigned char)p->src[p->pos])) {
            util_growbuf_append(&raw, p->src + p->pos, 1);
            p->pos++;
        }
    }
    char *raw_str = util_growbuf_release(&raw);
    char *decoded = raw_str != NULL ? decode_entities(raw_str) : util_strdup("");
    free(raw_str);
    free(*out_val);
    *out_val = decoded;
}

/* Parse an open tag at p->pos (just past '<'). Creates the node, applies
 * implicit closes, attaches it, and pushes it unless void/self-closing.
 * Returns the parsed node or NULL for malformed input (e.g. "<3"). */
static html_node *parse_open_tag(html_parser *p) {
    size_t name_start = p->pos;
    while (p->pos < p->len && (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '-' ||
                               p->src[p->pos] == ':')) {
        p->pos++;
    }
    if (p->pos == name_start) {
        /* "<" followed by junk: treat the '<' as text. */
        parser_append_text(p, p->src + name_start - 1, 1);
        return NULL;
    }
    size_t name_len = p->pos - name_start;
    char *tag = malloc(name_len + 1);
    if (tag == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    for (size_t i = 0; i < name_len; i++) {
        tag[i] = (char)tolower((unsigned char)p->src[name_start + i]);
    }
    tag[name_len] = '\0';

    html_node *n = node_new(tag);

    /* Attributes. */
    int self_closing = 0;
    while (p->pos < p->len) {
        while (p->pos < p->len && isspace((unsigned char)p->src[p->pos])) p->pos++;
        if (p->pos >= p->len) break;
        if (p->src[p->pos] == '>') {
            p->pos++;
            break;
        }
        if (p->src[p->pos] == '/') {
            self_closing = 1;
            p->pos++;
            continue;
        }
        size_t an_start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != '=' && p->src[p->pos] != '>' &&
               p->src[p->pos] != '/' && !isspace((unsigned char)p->src[p->pos])) {
            p->pos++;
        }
        size_t an_len = p->pos - an_start;
        if (an_len == 0) {
            p->pos++; /* stray character; skip */
            continue;
        }
        char *aname = malloc(an_len + 1);
        if (aname == NULL) {
            log_activity("[error] OOM in htmlmd");
            exit(EXIT_INTERNAL_ERR);
        }
        for (size_t i = 0; i < an_len; i++) {
            aname[i] = (char)tolower((unsigned char)p->src[an_start + i]);
        }
        aname[an_len] = '\0';

        char *aval = util_strdup("");
        /* Look ahead past whitespace for '='. */
        size_t la = p->pos;
        while (la < p->len && isspace((unsigned char)p->src[la])) la++;
        if (la < p->len && p->src[la] == '=') {
            p->pos = la + 1;
            parse_attr_value(p, &aval);
        }
        node_add_attr(n, aname, aval);
        free(aname);
        free(aval);
    }

    /* Implicit closes for common malformed HTML. */
    if (strcmp(tag, "li") == 0 && parser_find_open(p, "li", "ul", "ol") != NULL) {
        parser_close_through(p, "li");
    } else if ((strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0) &&
               parser_find_open(p, "dt", "dl", NULL) != NULL) {
        parser_close_through(p, "dt");
    } else if (strcmp(tag, "dd") == 0 && parser_find_open(p, "dd", "dl", NULL) != NULL) {
        parser_close_through(p, "dd");
    } else if ((strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0) &&
               (parser_find_open(p, "td", "tr", "table") != NULL ||
                parser_find_open(p, "th", "tr", "table") != NULL)) {
        parser_close_through(p, "td");
        parser_close_through(p, "th");
    } else if (strcmp(tag, "tr") == 0 && parser_find_open(p, "tr", "table", NULL) != NULL) {
        parser_close_through(p, "tr");
    } else {
        static const char *const BLOCK_TAGS[] = {
            "p",  "div", "ul", "ol", "table", "blockquote", "pre", "section", "article", "h1",
            "h2", "h3",  "h4", "h5", "h6",    "hr",         "dl",  "figure",  NULL};
        for (int i = 0; BLOCK_TAGS[i] != NULL; i++) {
            if (strcmp(tag, BLOCK_TAGS[i]) == 0) {
                if (parser_current(p)->tag != NULL && strcmp(parser_current(p)->tag, "p") == 0) {
                    parser_close_through(p, "p");
                }
                break;
            }
        }
    }

    node_add_child(parser_current(p), n);
    if (!self_closing && !is_void_tag(tag)) parser_push(p, n);
    free(tag);
    return n;
}

/* Parse a close tag at p->pos (just past '</'). */
static void parse_close_tag(html_parser *p) {
    size_t name_start = p->pos;
    while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
    size_t name_len = p->pos - name_start;
    if (p->pos < p->len) p->pos++; /* skip '>' */

    char *tag = malloc(name_len + 1);
    if (tag == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    size_t t = 0;
    for (size_t i = 0; i < name_len; i++) {
        char c = p->src[name_start + i];
        if (isspace((unsigned char)c)) continue;
        tag[t++] = (char)tolower((unsigned char)c);
    }
    tag[t] = '\0';

    if (parser_find_open(p, tag, NULL, NULL) != NULL) {
        parser_close_through(p, tag);
    } else {
        /* Stray close tag: ignore. */
    }
    free(tag);
}

/* Skip a comment (<!-- -->), doctype (<! >) or processing instruction (<? >). */
static void skip_declaration(html_parser *p) {
    if (p->pos + 3 < p->len && strncmp(p->src + p->pos, "<!--", 4) == 0) {
        const char *end = strstr(p->src + p->pos + 4, "-->");
        p->pos = (end != NULL) ? (size_t)(end - p->src) + 3 : p->len;
        return;
    }
    while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
    if (p->pos < p->len) p->pos++;
}

static html_node *html_parse(const char *html) {
    html_parser p = {0};
    p.src = html ? html : "";
    p.len = strlen(p.src);
    p.root = node_new(NULL);
    p.stack_cap = 32;
    p.stack = malloc((size_t)p.stack_cap * sizeof(*p.stack));
    if (p.stack == NULL) {
        log_activity("[error] OOM in htmlmd");
        exit(EXIT_INTERNAL_ERR);
    }
    p.stack[p.stack_len++] = p.root;

    while (p.pos < p.len) {
        const char *lt = strchr(p.src + p.pos, '<');
        if (lt == NULL) {
            parser_append_text(&p, p.src + p.pos, p.len - p.pos);
            break;
        }
        size_t lt_off = (size_t)(lt - p.src);
        if (lt_off > p.pos) parser_append_text(&p, p.src + p.pos, lt_off - p.pos);
        p.pos = lt_off;

        if (p.pos + 1 < p.len && p.src[p.pos + 1] == '/') {
            p.pos += 2;
            parse_close_tag(&p);
        } else if (p.pos + 1 < p.len && p.src[p.pos + 1] == '!') {
            p.pos += 2;
            skip_declaration(&p);
        } else if (p.pos + 1 < p.len && p.src[p.pos + 1] == '?') {
            p.pos += 2;
            skip_declaration(&p);
        } else {
            p.pos++;
            parse_open_tag(&p);
        }
    }

    free(p.stack);
    return p.root;
}

/* ------------------------------------------------------------------ */
/*  Readability: boilerplate pruning + content root selection          */
/* ------------------------------------------------------------------ */

static const char *const BOILERPLATE_TAGS[] = {
    "script", "style", "noscript", "template", "svg",      "canvas", "iframe", "object", "embed",
    "form",   "input", "button",   "select",   "textarea", "option", "label",  "nav",    "header",
    "footer", "aside", "dialog",   "head",     "meta",     "link",   "base",   "title",  NULL,
};

static int is_boilerplate_tag(const char *tag) {
    if (tag == NULL) return 0;
    for (int i = 0; BOILERPLATE_TAGS[i] != NULL; i++) {
        if (strcmp(BOILERPLATE_TAGS[i], tag) == 0) return 1;
    }
    return 0;
}

/* Remove boilerplate children (recursively). */
static void prune_boilerplate(html_node *n) {
    int keep = 0;
    for (int i = 0; i < n->child_count; i++) {
        html_node *c = n->children[i];
        if (is_boilerplate_tag(c->tag)) {
            node_free(c);
        } else {
            prune_boilerplate(c);
            n->children[keep++] = c;
        }
    }
    n->child_count = keep;
}

/* First descendant (depth-first) with the given tag, or NULL. */
static html_node *find_first_by_tag(html_node *n, const char *tag) {
    if (n->tag != NULL && strcmp(n->tag, tag) == 0) return n;
    for (int i = 0; i < n->child_count; i++) {
        html_node *hit = find_first_by_tag(n->children[i], tag);
        if (hit != NULL) return hit;
    }
    return NULL;
}

/* Whitespace-collapsed text of a subtree appended to a shared buffer, so
 * word boundaries across nested elements are preserved ("a<b>b</b> c"
 * keeps its spaces). */
static void collapsed_text_accum(const html_node *n, util_growbuf *out) {
    if (n->tag == NULL) {
        if (n->text == NULL) return;
        for (const char *s = n->text; *s != '\0'; s++) {
            if (isspace((unsigned char)*s)) {
                if (out->len > 0 && out->buf[out->len - 1] != ' ') {
                    util_growbuf_append(out, " ", 1);
                }
            } else {
                util_growbuf_append(out, s, 1);
            }
        }
        return;
    }
    for (int i = 0; i < n->child_count; i++) {
        collapsed_text_accum(n->children[i], out);
    }
}

static char *node_collapsed_text(const html_node *n) {
    util_growbuf out = {0};
    collapsed_text_accum(n, &out);
    return util_growbuf_release(&out);
}

static char *extract_page_title(const html_node *root) {
    html_node *t = find_first_by_tag((html_node *)root, "title");
    if (t == NULL) return NULL;
    char *text = node_collapsed_text(t);
    if (text[0] == '\0') {
        free(text);
        return NULL;
    }
    return text;
}

/* ------------------------------------------------------------------ */
/*  Markdown renderer                                                  */
/* ------------------------------------------------------------------ */

#define HTMLMD_MAX_LIST_DEPTH 16

typedef struct {
    util_growbuf *out;
    int list_depth;
    int ordered[HTMLMD_MAX_LIST_DEPTH + 1];
    int counters[HTMLMD_MAX_LIST_DEPTH + 1];
    int in_pre;
    int in_cell;
} render_ctx;

static void render_node(render_ctx *ctx, const html_node *node);

static int out_ends_with(util_growbuf *out, const char *suffix) {
    size_t n = strlen(suffix);
    if (out->len < n) return 0;
    return memcmp(out->buf + out->len - n, suffix, n) == 0;
}

static void ensure_blank_line(render_ctx *ctx) {
    if (ctx->out->len == 0) return;
    if (out_ends_with(ctx->out, "\n\n")) return;
    util_growbuf_append_str(ctx->out, out_ends_with(ctx->out, "\n") ? "\n" : "\n\n");
}

static void end_line(render_ctx *ctx) {
    if (ctx->out->len > 0 && !out_ends_with(ctx->out, "\n")) {
        util_growbuf_append_str(ctx->out, "\n");
    }
}

/* Append text with whitespace runs collapsed to single spaces; leading
 * whitespace is dropped when the output is at a line start. */
static void append_collapsed(render_ctx *ctx, const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        char c = *p;
        if (isspace((unsigned char)c)) {
            if (ctx->out->len > 0 && !out_ends_with(ctx->out, "\n") &&
                !out_ends_with(ctx->out, " ")) {
                util_growbuf_append_str(ctx->out, " ");
            }
        } else {
            util_growbuf_append(ctx->out, &c, 1);
        }
    }
}

/* Render children into a fresh buffer. */
static util_growbuf render_to_temp(render_ctx *ctx, const html_node *node) {
    util_growbuf tmp = {0};
    render_ctx sub = *ctx;
    sub.out = &tmp;
    for (int i = 0; i < node->child_count; i++) {
        render_node(&sub, node->children[i]);
    }
    return tmp;
}

/* Render the children of `node` wrapped in `pre`/`post` markers, but only
 * when they contain visible (non-whitespace) content; pure-whitespace
 * content is appended collapsed so words on both sides stay separated. */
static void render_wrapped(render_ctx *ctx, const html_node *node, const char *pre,
                           const char *post) {
    util_growbuf tmp = render_to_temp(ctx, node);
    int visible = 0;
    for (size_t i = 0; i < tmp.len; i++) {
        if (!isspace((unsigned char)tmp.buf[i])) {
            visible = 1;
            break;
        }
    }
    if (visible) {
        util_growbuf_append_str(ctx->out, pre);
        util_growbuf_append(ctx->out, tmp.buf, tmp.len);
        util_growbuf_append_str(ctx->out, post);
    } else {
        char *s = util_growbuf_release(&tmp);
        append_collapsed(ctx, s);
        free(s);
        util_growbuf_free(&tmp);
        return;
    }
    util_growbuf_free(&tmp);
}

/* Concatenate the text of a subtree preserving whitespace/newlines (used
 * for <pre> content). Tags are stripped; entities are already decoded. */
static void collect_raw_text(const html_node *n, util_growbuf *out) {
    if (n->tag == NULL) {
        util_growbuf_append_str(out, n->text);
        return;
    }
    for (int i = 0; i < n->child_count; i++) collect_raw_text(n->children[i], out);
}

static void render_code_block(render_ctx *ctx, const html_node *node) {
    util_growbuf raw = {0};
    collect_raw_text(node, &raw);
    /* Trim leading/trailing blank lines that HTML source layout adds. */
    while (raw.len > 0 && raw.buf[0] == '\n') {
        memmove(raw.buf, raw.buf + 1, raw.len);
        raw.len--;
    }
    while (raw.len > 0 && raw.buf[raw.len - 1] == '\n') raw.len--;
    if (raw.len == 0) {
        util_growbuf_free(&raw);
        return;
    }
    /* Pick a fence longer than any backtick run inside the code. */
    int longest = 0;
    int run = 0;
    for (size_t i = 0; i < raw.len; i++) {
        if (raw.buf[i] == '`') {
            run++;
            if (run > longest) longest = run;
        } else {
            run = 0;
        }
    }
    char fence[16];
    int flen = longest + 1;
    if (flen < 3) flen = 3;
    if (flen > 15) flen = 15;
    for (int i = 0; i < flen; i++) fence[i] = '`';
    fence[flen] = '\0';

    ensure_blank_line(ctx);
    util_growbuf_append_str(ctx->out, fence);
    util_growbuf_append_str(ctx->out, "\n");
    util_growbuf_append(ctx->out, raw.buf, raw.len);
    util_growbuf_append_str(ctx->out, "\n");
    util_growbuf_append_str(ctx->out, fence);
    ensure_blank_line(ctx);
    util_growbuf_free(&raw);
}

/* Prefix every line of out->buf[start..] with `prefix` (blockquote "> "). */
static void prefix_segment(util_growbuf *out, size_t start, const char *prefix) {
    if (start >= out->len) return;
    util_growbuf nb = {0};
    util_growbuf_append(&nb, out->buf, start);
    util_growbuf_append_str(&nb, prefix);
    for (size_t i = start; i < out->len; i++) {
        util_growbuf_append(&nb, out->buf + i, 1);
        if (out->buf[i] == '\n' && i + 1 < out->len) {
            util_growbuf_append_str(&nb, prefix);
        }
    }
    util_growbuf_free(out);
    *out = nb;
}

/* ------------------------------------------------------------------ */
/*  Tables                                                             */
/* ------------------------------------------------------------------ */

#define TABLE_MAX_COLS 64

typedef struct {
    char *cells[TABLE_MAX_COLS];
    int col_count;
} table_row;

/* Render one table cell inline: blocks are flattened, newlines become
 * spaces, '|' is escaped. Result is malloc'd. */
static char *render_cell_text(render_ctx *ctx, const html_node *cell) {
    render_ctx sub = *ctx;
    util_growbuf tmp = {0};
    sub.out = &tmp;
    sub.in_cell = 1;
    for (int i = 0; i < cell->child_count; i++) {
        render_node(&sub, cell->children[i]);
    }
    util_growbuf flat = {0};
    for (size_t i = 0; i < tmp.len; i++) {
        char c = tmp.buf[i];
        if (c == '\n') {
            c = ' ';
        }
        if (c == '|') {
            util_growbuf_append_str(&flat, "\\|");
        } else {
            util_growbuf_append(&flat, &c, 1);
        }
    }
    util_growbuf_free(&tmp);
    /* Collapse duplicated spaces introduced by flattening. */
    util_growbuf out = {0};
    for (size_t i = 0; i < flat.len; i++) {
        if (flat.buf[i] == ' ' && out.len > 0 && out.buf[out.len - 1] == ' ') continue;
        if (flat.buf[i] == ' ' && (out.len == 0 || out.buf[out.len - 1] == '\n')) continue;
        util_growbuf_append(&out, flat.buf + i, 1);
    }
    util_growbuf_free(&flat);
    while (out.len > 0 && out.buf[out.len - 1] == ' ') out.len--;
    return util_growbuf_release(&out);
}

/* Depth-first collection of descendant nodes with `tag`, not descending
 * into nested tables or table cells (for `tr` / `td` / `th` lookups). */
static void collect_rows(const html_node *n, const char *tag, const html_node ***out, int *count) {
    if (n->tag != NULL) {
        if (strcmp(n->tag, "table") == 0 || strcmp(n->tag, "td") == 0 ||
            strcmp(n->tag, "th") == 0) {
            return; /* nested table: belongs to its own cell */
        }
        if (strcmp(n->tag, tag) == 0) {
            const html_node **tmp = realloc(*out, (size_t)(*count + 1) * sizeof(*tmp));
            if (tmp == NULL) {
                log_activity("[error] OOM in htmlmd");
                exit(EXIT_INTERNAL_ERR);
            }
            *out = tmp;
            (*out)[(*count)++] = n;
            return;
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        collect_rows(n->children[i], tag, out, count);
    }
}

static void render_table(render_ctx *ctx, const html_node *table) {
    const html_node **tr_nodes = NULL;
    int tr_count = 0;
    /* Start from the children: collect_rows skips the subtree rooted at
     * a table node (nested-table guard), so the table itself must not be
     * the search root. */
    for (int i = 0; i < table->child_count; i++) {
        collect_rows(table->children[i], "tr", &tr_nodes, &tr_count);
    }

    table_row *rows = calloc((size_t)(tr_count > 0 ? tr_count : 1), sizeof(table_row));
    if (rows == NULL) {
        free(tr_nodes);
        return;
    }

    int ncols = 0;
    for (int r = 0; r < tr_count; r++) {
        const html_node **td_nodes = NULL;
        int td_count = 0;
        collect_rows(tr_nodes[r], "td", &td_nodes, &td_count);
        /* <th> cells: collect with the same helper by scanning children. */
        for (int c = 0; c < tr_nodes[r]->child_count; c++) {
            const html_node *cell = tr_nodes[r]->children[c];
            const char *t = cell->tag;
            if (t == NULL || (strcmp(t, "td") != 0 && strcmp(t, "th") != 0)) continue;
            if (rows[r].col_count >= TABLE_MAX_COLS) break;
            int idx = rows[r].col_count++;
            rows[r].cells[idx] = render_cell_text(ctx, cell);
        }
        (void)td_nodes;
        free(td_nodes);
        if (rows[r].col_count > ncols) ncols = rows[r].col_count;
    }
    free(tr_nodes);

    if (ncols == 0) {
        for (int r = 0; r < tr_count; r++) {
            for (int c = 0; c < rows[r].col_count; c++) free(rows[r].cells[c]);
        }
        free(rows);
        return;
    }

    ensure_blank_line(ctx);
    for (int r = 0; r < tr_count; r++) {
        util_growbuf_append_str(ctx->out, "|");
        for (int c = 0; c < ncols; c++) {
            util_growbuf_append_str(ctx->out, " ");
            if (c < rows[r].col_count && rows[r].cells[c] != NULL) {
                util_growbuf_append_str(ctx->out, rows[r].cells[c]);
            }
            util_growbuf_append_str(ctx->out, " |");
        }
        end_line(ctx);
        /* After the first row emit the markdown header separator. */
        if (r == 0) {
            util_growbuf_append_str(ctx->out, "|");
            for (int c = 0; c < ncols; c++) util_growbuf_append_str(ctx->out, " --- |");
            end_line(ctx);
        }
    }
    ensure_blank_line(ctx);

    for (int r = 0; r < tr_count; r++) {
        for (int c = 0; c < rows[r].col_count; c++) free(rows[r].cells[c]);
    }
    free(rows);
}

/* ------------------------------------------------------------------ */
/*  Element dispatch                                                   */
/* ------------------------------------------------------------------ */

static void render_children(render_ctx *ctx, const html_node *node) {
    for (int i = 0; i < node->child_count; i++) {
        render_node(ctx, node->children[i]);
    }
}

static void render_heading(render_ctx *ctx, const html_node *node, int level) {
    ensure_blank_line(ctx);
    for (int i = 0; i < level; i++) util_growbuf_append_str(ctx->out, "#");
    util_growbuf_append_str(ctx->out, " ");
    render_children(ctx, node);
    ensure_blank_line(ctx);
}

static void render_list(render_ctx *ctx, const html_node *node, int ordered) {
    ctx->list_depth++;
    if (ctx->list_depth > HTMLMD_MAX_LIST_DEPTH) ctx->list_depth = HTMLMD_MAX_LIST_DEPTH;
    int d = ctx->list_depth;
    ctx->ordered[d] = ordered;
    ctx->counters[d] = 0;
    if (d == 1) {
        ensure_blank_line(ctx);
    } else {
        end_line(ctx);
    }
    render_children(ctx, node);
    ctx->list_depth--;
    end_line(ctx);
    if (ctx->list_depth == 0) ensure_blank_line(ctx);
}

static void render_list_item(render_ctx *ctx, const html_node *node) {
    int d = ctx->list_depth;
    if (d == 0) {
        /* Stray <li> outside any list: render transparently. */
        render_children(ctx, node);
        return;
    }
    end_line(ctx);
    for (int i = 1; i < d; i++) util_growbuf_append_str(ctx->out, "  ");
    if (ctx->ordered[d]) {
        ctx->counters[d]++;
        char buf[24];
        snprintf(buf, sizeof(buf), "%d. ", ctx->counters[d]);
        util_growbuf_append_str(ctx->out, buf);
    } else {
        util_growbuf_append_str(ctx->out, "- ");
    }
    render_children(ctx, node);
    end_line(ctx);
}

static void render_link(render_ctx *ctx, const html_node *node) {
    const char *href = node_get_attr(node, "href");
    util_growbuf tmp = render_to_temp(ctx, node);
    /* Trim the link text. */
    while (tmp.len > 0 && isspace((unsigned char)tmp.buf[0])) {
        memmove(tmp.buf, tmp.buf + 1, tmp.len);
        tmp.len--;
    }
    while (tmp.len > 0 && isspace((unsigned char)tmp.buf[tmp.len - 1])) tmp.len--;
    if (href != NULL && href[0] != '\0') {
        /* Empty text, or text identical to the target, renders as a
         * markdown autolink. */
        int same = (tmp.len == strlen(href) && strncmp(tmp.buf, href, tmp.len) == 0);
        if (tmp.len == 0 || same) {
            util_growbuf_append_str(ctx->out, "<");
            util_growbuf_append_str(ctx->out, href);
            util_growbuf_append_str(ctx->out, ">");
        } else {
            util_growbuf_append_str(ctx->out, "[");
            util_growbuf_append(ctx->out, tmp.buf, tmp.len);
            util_growbuf_append_str(ctx->out, "](");
            util_growbuf_append_str(ctx->out, href);
            util_growbuf_append_str(ctx->out, ")");
        }
    } else {
        util_growbuf_append(ctx->out, tmp.buf, tmp.len);
    }
    util_growbuf_free(&tmp);
}

static void render_image(render_ctx *ctx, const html_node *node) {
    const char *src = node_get_attr(node, "src");
    if (src == NULL || src[0] == '\0') return;
    const char *alt = node_get_attr(node, "alt");
    util_growbuf_append_str(ctx->out, "![");
    append_collapsed(ctx, alt != NULL ? alt : "");
    util_growbuf_append_str(ctx->out, "](");
    util_growbuf_append_str(ctx->out, src);
    util_growbuf_append_str(ctx->out, ")");
}

/* Tags that only group content and never add markup of their own. */
static int is_transparent_tag(const char *tag) {
    static const char *const TRANSPARENT[] = {
        "html",    "body",   "section", "article", "main",  "figure", "figcaption", "details",
        "summary", "center", "address", "span",    "small", "big",    "u",          "mark",
        "abbr",    "time",   "cite",    "q",       "dfn",   "var",    "samp",       "kbd",
        "acronym", "bdi",    "bdo",     "ruby",    "rt",    "rp",     "font",       NULL,
    };
    for (int i = 0; TRANSPARENT[i] != NULL; i++) {
        if (strcmp(TRANSPARENT[i], tag) == 0) return 1;
    }
    return 0;
}

/* Generic block container: guarantees paragraph separation around content. */
static void render_block_container(render_ctx *ctx, const html_node *node) {
    ensure_blank_line(ctx);
    render_children(ctx, node);
    ensure_blank_line(ctx);
}

void render_node(render_ctx *ctx, const html_node *node) {
    /* Text node. The document root also has tag == NULL but never any
     * text of its own; its children must still be rendered. */
    if (node->tag == NULL) {
        if (node->text != NULL) {
            if (ctx->in_pre) {
                util_growbuf_append_str(ctx->out, node->text);
            } else {
                append_collapsed(ctx, node->text);
            }
        }
        render_children(ctx, node);
        return;
    }

    const char *tag = node->tag;

    if (strcmp(tag, "p") == 0) {
        if (ctx->in_cell) {
            render_children(ctx, node);
        } else if (ctx->list_depth > 0) {
            /* Keep list items intact: soft separation instead of a blank
             * line, which would split the markdown list. */
            end_line(ctx);
            render_children(ctx, node);
            end_line(ctx);
        } else {
            render_block_container(ctx, node);
        }
    } else if (strcmp(tag, "div") == 0) {
        if (ctx->in_cell || ctx->list_depth > 0) {
            render_children(ctx, node);
        } else {
            render_block_container(ctx, node);
        }
    } else if (strlen(tag) == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        if (ctx->in_cell) {
            render_children(ctx, node);
        } else {
            render_heading(ctx, node, tag[1] - '0');
        }
    } else if (strcmp(tag, "ul") == 0) {
        render_list(ctx, node, 0);
    } else if (strcmp(tag, "ol") == 0) {
        render_list(ctx, node, 1);
    } else if (strcmp(tag, "li") == 0) {
        render_list_item(ctx, node);
    } else if (strcmp(tag, "blockquote") == 0) {
        ensure_blank_line(ctx);
        size_t start = ctx->out->len;
        render_children(ctx, node);
        prefix_segment(ctx->out, start, "> ");
        ensure_blank_line(ctx);
    } else if (strcmp(tag, "pre") == 0) {
        if (ctx->in_cell) {
            render_children(ctx, node);
        } else {
            ctx->in_pre++;
            render_code_block(ctx, node);
            ctx->in_pre--;
        }
    } else if (strcmp(tag, "code") == 0) {
        if (ctx->in_pre || ctx->in_cell) {
            render_children(ctx, node);
        } else {
            render_wrapped(ctx, node, "`", "`");
        }
    } else if (strcmp(tag, "a") == 0) {
        render_link(ctx, node);
    } else if (strcmp(tag, "img") == 0) {
        render_image(ctx, node);
    } else if (strcmp(tag, "br") == 0) {
        if (ctx->in_cell) {
            util_growbuf_append_str(ctx->out, " ");
        } else {
            end_line(ctx);
        }
    } else if (strcmp(tag, "hr") == 0) {
        ensure_blank_line(ctx);
        util_growbuf_append_str(ctx->out, "---");
        ensure_blank_line(ctx);
    } else if (strcmp(tag, "strong") == 0 || strcmp(tag, "b") == 0) {
        render_wrapped(ctx, node, "**", "**");
    } else if (strcmp(tag, "em") == 0 || strcmp(tag, "i") == 0) {
        render_wrapped(ctx, node, "_", "_");
    } else if (strcmp(tag, "del") == 0 || strcmp(tag, "s") == 0 || strcmp(tag, "strike") == 0) {
        render_wrapped(ctx, node, "~~", "~~");
    } else if (strcmp(tag, "table") == 0) {
        if (ctx->in_cell) {
            /* Nested table: flatten as text. */
            render_children(ctx, node);
        } else {
            render_table(ctx, node);
        }
    } else if (strcmp(tag, "dt") == 0 || strcmp(tag, "dd") == 0) {
        end_line(ctx);
        render_children(ctx, node);
        end_line(ctx);
    } else if (strcmp(tag, "thead") == 0 || strcmp(tag, "tbody") == 0 ||
               strcmp(tag, "tfoot") == 0 || strcmp(tag, "caption") == 0 ||
               strcmp(tag, "colgroup") == 0 || strcmp(tag, "col") == 0) {
        /* Table internals are handled by render_table. */
    } else if (is_transparent_tag(tag)) {
        render_children(ctx, node);
    } else {
        /* Unknown element: treat as a block-level container. */
        render_block_container(ctx, node);
    }
}

/* ------------------------------------------------------------------ */
/*  Top-level conversion                                               */
/* ------------------------------------------------------------------ */

char *htmlmd_convert(const char *html) {
    html_node *root = html_parse(html);
    char *title = extract_page_title(root);
    prune_boilerplate(root);

    html_node *content = find_first_by_tag(root, "article");
    if (content == NULL) content = find_first_by_tag(root, "main");
    if (content == NULL) content = find_first_by_tag(root, "body");
    if (content == NULL) content = root;

    util_growbuf out = {0};
    render_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = &out;
    render_node(&ctx, content);

    /* Normalize: cap runs of blank lines at one, trim edges. */
    {
        util_growbuf clean = {0};
        char *raw = util_growbuf_release(&out);
        size_t n = raw != NULL ? strlen(raw) : 0;
        for (size_t i = 0; i < n; i++) {
            util_growbuf_append(&clean, raw + i, 1);
            if (raw[i] == '\n') {
                while (i + 1 < n && raw[i + 1] == '\n' && clean.len > 0 &&
                       clean.buf[clean.len - 1] == '\n') {
                    i++;
                }
            }
        }
        free(raw);
        out = clean;
        while (out.len > 0 && (out.buf[out.len - 1] == '\n' || out.buf[out.len - 1] == ' ')) {
            out.len--;
        }
        size_t lead = 0;
        while (lead < out.len && (out.buf[lead] == '\n' || out.buf[lead] == ' ')) lead++;
        if (lead > 0) memmove(out.buf, out.buf + lead, out.len - lead);
        out.len -= lead;
    }

    if (title != NULL && title[0] != '\0' && out.len > 0) {
        util_growbuf full = {0};
        util_growbuf_append_str(&full, "# ");
        util_growbuf_append_str(&full, title);
        util_growbuf_append_str(&full, "\n\n");
        util_growbuf_append(&full, out.buf, out.len);
        util_growbuf_free(&out);
        out = full;
    }
    free(title);

    node_free(root);
    char *result = util_growbuf_release(&out);
    if (result == NULL) result = util_strdup("");
    return result;
}

/* ------------------------------------------------------------------ */
/*  Class matching (used by the online_search DDG result parser)        */
/* ------------------------------------------------------------------ */

static void collect_by_class(const html_node *n, const char *class_name, const char *want_attr,
                             htmlmd_match **out, int *count, int *cap) {
    if (n->tag != NULL && node_has_class(n, class_name)) {
        if (*count + 1 > *cap) {
            int new_cap = *cap ? *cap * 2 : 8;
            htmlmd_match *tmp = realloc(*out, (size_t)new_cap * sizeof(**out));
            if (tmp == NULL) {
                log_activity("[error] OOM in htmlmd");
                exit(EXIT_INTERNAL_ERR);
            }
            *out = tmp;
            *cap = new_cap;
        }
        htmlmd_match *m = &(*out)[(*count)++];
        m->text = node_collapsed_text(n);
        const char *val = want_attr != NULL ? node_get_attr(n, want_attr) : NULL;
        m->attr = val != NULL ? util_strdup(val) : NULL;
        /* Do not descend into a matched element: nested matches of the
         * same class (result blocks containing themselves) would
         * duplicate entries. */
        return;
    }
    for (int i = 0; i < n->child_count; i++) {
        collect_by_class(n->children[i], class_name, want_attr, out, count, cap);
    }
}

htmlmd_match *htmlmd_find_by_class(const char *html, const char *class_name, const char *want_attr,
                                   int *out_count) {
    *out_count = 0;
    if (html == NULL || class_name == NULL) return NULL;

    html_node *root = html_parse(html);
    htmlmd_match *matches = NULL;
    int count = 0;
    int cap = 0;
    collect_by_class(root, class_name, want_attr, &matches, &count, &cap);
    node_free(root);

    if (count == 0) return NULL;
    *out_count = count;
    return matches;
}

void htmlmd_matches_free(htmlmd_match *matches, int count) {
    if (matches == NULL) return;
    for (int i = 0; i < count; i++) {
        free(matches[i].text);
        free(matches[i].attr);
    }
    free(matches);
}
