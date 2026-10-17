#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* mime_of (below) - windows content type sniffing */
#ifdef _WIN32
#include <objbase.h>
#include <urlmon.h>
#endif

/* ================= file_analyze ================= */

/* per-language outline analyzers: file_analyze picks one by extension
   and renders its output - a tree of nodes, one per line, indented
   with two spaces per level and prefixed with the line number. adding
   a type is an analyze_<name>() function next to its siblings plus one
   { name, extensions, fn } entry. */

#define AN_NODES_MAX 1000 /* nodes before truncation */
#define AN_DEPTH_MAX 30   /* indent levels rendered */
#define AN_LABEL_MAX 200  /* bytes of one node label */
#define AN_ACC_MAX 1024   /* bytes of one c declaration head */

/* what an analyzer writes into: node_add() appends one rendered node
   line and enforces the shared node cap */
typedef struct {
    buf_t out;
    int count;
    int truncated;
} nodes_t;

typedef void (*analyze_fn)(const char *text, size_t len, nodes_t *ns);

typedef struct {
    const char *name;        /* labels the reply: "[markdown]" */
    const char *const *exts; /* lowercase, dot-prefixed, NULL-ended */
    analyze_fn fn;
} analyzer_t;

static int ncl(size_t n) {
    return n > AN_LABEL_MAX ? AN_LABEL_MAX : (int)n;
}

/* copy s[0..n) into dst; past AN_LABEL_MAX bytes the copy is cut on a
   utf-8 character boundary and closed with an ellipsis */
static void label_copy(char *dst, size_t dstn, const char *s, size_t n) {
    size_t take = n > AN_LABEL_MAX ? AN_LABEL_MAX : n;
    if (take < n) {
        /* back off partial utf-8 sequences at the cut */
        for (int back = 1; back <= 3 && take >= (size_t)back; back++) {
            unsigned char c = (unsigned char)s[take - back];
            int len = c < 0x80 ? 1
                        : c >= 0xc2 && c <= 0xdf ? 2
                        : c >= 0xe0 && c <= 0xef ? 3
                        : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (len > 0) {
                if (len > back) take -= (size_t)back;
                break;
            }
        }
    }
    if (take + 8 > dstn) take = dstn > 8 ? dstn - 8 : 0;
    memcpy(dst, s, take);
    if (take < n) memcpy(dst + take, "\xE2\x80\xA6", 4); /* ... */
    else dst[take] = '\0';
}

/* one node line: "%5ld", two spaces, two per depth, the label. Past
   AN_NODES_MAX only the truncation flag is set. */
static void node_add(nodes_t *ns, long lineno, int depth, const char *label) {
    if (ns->truncated) return;
    if (ns->count >= AN_NODES_MAX) {
        ns->truncated = true;
        return;
    }
    if (depth > AN_DEPTH_MAX) depth = AN_DEPTH_MAX;
    if (depth < 0) depth = 0;
    char num[24];
    snprintf(num, sizeof num, "%5ld", lineno);
    buf_append_str(&ns->out, num);
    buf_append_str(&ns->out, "  ");
    for (int i = 0; i < depth; i++) buf_append_str(&ns->out, "  ");
    buf_append_str(&ns->out, label);
    buf_append_byte(&ns->out, '\n');
    ns->count++;
}

/* ---------- markdown: the heading tree ---------- */

static void analyze_markdown(const char *text, size_t len, nodes_t *ns) {
    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;
    int fence = 0; /* inside a fenced code block */

    while (bm_next_line(text, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && k < 3 && isspace((unsigned char)line[k])) k++;

        /* a fence line opens or closes a code block (any 3+ run of `
           or ~ at up to three spaces of indentation) */
        if (k < ll && (line[k] == '`' || line[k] == '~')) {
            size_t run = 1;
            while (k + run < ll && line[k + run] == line[k]) run++;
            if (run >= 3) {
                fence = !fence;
                continue;
            }
        }
        if (fence) continue;

        /* an atx heading: 1-6 '#', whitespace, then the title */
        int level = 0;
        while (k + (size_t)level < ll && line[k + level] == '#') level++;
        if (level < 1 || level > 6) continue;
        k += (size_t)level;
        if (k >= ll || !isspace((unsigned char)line[k])) continue;
        while (k < ll && isspace((unsigned char)line[k])) k++;

        /* the title, minus trailing space and one trailing run of '#'s */
        size_t end = ll;
        while (end > k && isspace((unsigned char)line[end - 1])) end--;
        if (end > k && line[end - 1] == '#') {
            end--;
            while (end > k && line[end - 1] == '#') end--;
            while (end > k && isspace((unsigned char)line[end - 1])) end--;
        }
        char label[AN_LABEL_MAX + 8];
        label_copy(label, sizeof label, line + k, end > k ? end - k : 0);
        node_add(ns, lineno, level - 1, label);
    }
}

/* ---------- python: classes and defs ---------- */

/* no python parser here: nesting comes from indentation, so a def or
   class one level deeper than its enclosing def or class lands under
   it (one hidden inside an if-block shows up a level down instead of
   not at all) */

/* does `word` (lowercase, len wl) sit at line[pos..], followed by
   whitespace (or the end of the line)? */
static bool word_at(const char *line, size_t len, size_t pos,
                    const char *word, size_t wl) {
    if (pos + wl > len) return false; /* must fit before eol */
    if (strncmp(line + pos, word, wl)) return false;
    if (pos + wl == len) return true; /* keyword alone on the line */
    return line[pos + wl] == ' ' || line[pos + wl] == '\t';
}

static void analyze_python(const char *text, size_t len, nodes_t *ns) {
    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;
    int indents[AN_DEPTH_MAX + 2]; /* enclosing def/class indents; [0]
                                      is the module level, always 0 */
    int depth_n = 1;
    indents[0] = 0;

    while (bm_next_line(text, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k >= ll || line[k] == '#') continue;

        const char *kw = NULL;
        size_t j = k;
        if (word_at(line, ll, j, "class", 5)) {
            kw = "class";
            j += 5;
        } else if (word_at(line, ll, j, "def", 3)) {
            kw = "def";
            j += 3;
        } else if (word_at(line, ll, j, "async", 5)) {
            size_t w = j + 5;
            while (w < ll && (line[w] == ' ' || line[w] == '\t')) w++;
            if (word_at(line, ll, w, "def", 3)) {
                kw = "async def";
                j = w + 3;
            }
        }
        if (!kw) continue;

        while (j < ll && (line[j] == ' ' || line[j] == '\t')) j++;
        size_t nend = j;
        while (nend < ll &&
               (isalnum((unsigned char)line[nend]) || line[nend] == '_'))
            nend++;
        if (nend == j) continue; /* no name after the keyword */

        /* the signature: the parenthesized group after the name - bases
           for a class, parameters for a def - when the line closes it
           (a multi-line parameter list keeps the bare name, parens
           empty) */
        size_t po = nend, pc = 0;
        while (po < ll && (line[po] == ' ' || line[po] == '\t')) po++;
        if (po < ll && line[po] == '(') {
            int pd = 0;
            for (size_t m = po; m < ll; m++) {
                if (line[m] == '(') pd++;
                else if (line[m] == ')' && --pd == 0) {
                    pc = m;
                    break;
                }
            }
        }

        /* dedent: pop the defs/classes this one closes */
        while (depth_n > 1 && indents[depth_n - 1] >= (int)k) depth_n--;
        char label[512];
        if (pc) {
            size_t a = po + 1, b = pc; /* the group, ws-trimmed */
            while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
            while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
            snprintf(label, sizeof label, "%s %.*s(%.*s)",
                     strcmp(kw, "class") == 0 ? "class" : kw,
                     ncl(nend - j), line + j, ncl(b - a), line + a);
        } else if (strcmp(kw, "class") == 0) {
            snprintf(label, sizeof label, "class %.*s", ncl(nend - j),
                     line + j);
        } else {
            snprintf(label, sizeof label, "%s %.*s()", kw, ncl(nend - j),
                     line + j);
        }
        node_add(ns, lineno, depth_n - 1, label);
        if (depth_n < (int)(sizeof indents / sizeof indents[0]))
            indents[depth_n++] = (int)k;
    }
}

/* ---------- the c family (c/c++/java/c#/js-ish) ---------- */

static const char *const C_KEYWORDS[] = {
    "abstract", "async", "await", "auto", "bool", "break", "case",
    "catch", "char", "class", "const", "constexpr", "continue",
    "default", "delete", "do", "double", "else", "enum", "export",
    "extends", "extern", "false", "final", "float", "for", "friend",
    "function", "goto", "if", "implements", "import", "inline",
    "instanceof", "int", "interface", "let", "long", "namespace",
    "native", "new", "noexcept", "nullptr", "operator", "override",
    "package", "private", "protected", "public", "register",
    "restrict", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "synchronized", "template", "this", "throw",
    "true", "try", "typedef", "typename", "typeof", "union",
    "unsigned", "using", "var", "virtual", "void", "volatile",
    "while", "yield",
};

static bool is_c_keyword(const char *s, size_t n) {
    for (size_t k = 0; k < sizeof C_KEYWORDS / sizeof C_KEYWORDS[0]; k++)
        if (strlen(C_KEYWORDS[k]) == n && !strncmp(C_KEYWORDS[k], s, n))
            return true;
    return false;
}

/* identifiers, [A-Za-z_~] then word characters */
static bool id_start(unsigned char c) {
    return c == '_' || c == '~' || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

typedef struct {
    const char *p;
    size_t n;
} tok_t;

/* the identifier runs in s[0..n): at most cap stored, the number
   stored returned */
static size_t scan_idents(const char *s, size_t n, tok_t *out,
                          size_t cap) {
    size_t count = 0;
    for (size_t i = 0; i < n;) {
        if (id_start((unsigned char)s[i])) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_'))
                j++;
            if (count < cap) out[count] = (tok_t){s + i, j - i};
            count++;
            i = j;
        } else {
            i++;
        }
    }
    return count < cap ? count : cap;
}

/* the last identifier run in s[0..n), if any */
static bool last_ident(const char *s, size_t n, tok_t *out) {
    tok_t id = {NULL, 0};
    size_t count = 0;
    for (size_t i = 0; i < n;) {
        if (id_start((unsigned char)s[i])) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_'))
                j++;
            id = (tok_t){s + i, j - i};
            count++;
            i = j;
        } else {
            i++;
        }
    }
    if (!count) return false;
    *out = id;
    return true;
}

/* the function name: the identifier just before the first '(' of a
   declaration head (any ')' anywhere in the head makes it look like a
   call or prototype, not a stray paren) */
static bool func_name(const char *head, tok_t *out) {
    const char *paren = strchr(head, '(');
    if (!paren || paren == head) return false;
    if (!strchr(head, ')')) return false;
    size_t before = (size_t)(paren - head);
    if (memchr(head, '=', before)) return false;
    tok_t id;
    if (!last_ident(head, before, &id)) return false;
    if (is_c_keyword(id.p, id.n)) return false;
    *out = id;
    return true;
}

/* the declared name: array brackets and initializers dropped, the last
   identifier that is not a keyword - copied into name (the scan runs on
   a scratch buffer, so the result cannot point into it) */
static bool decl_name(const char *head, char *name, size_t name_n) {
    char buf[AN_ACC_MAX];
    size_t w = 0;
    bool bracket = false;
    for (const char *q = head; *q && w + 1 < sizeof buf; q++) {
        if (*q == '[') {
            bracket = true;
            continue;
        }
        if (*q == ']') {
            bracket = false;
            continue;
        }
        if (bracket) continue;
        if (*q == '=') break;
        buf[w++] = *q;
    }
    buf[w] = '\0';
    tok_t ids[64];
    size_t n = scan_idents(buf, w, ids, sizeof ids / sizeof ids[0]);
    for (size_t k = n; k > 0; k--)
        if (!is_c_keyword(ids[k - 1].p, ids[k - 1].n)) {
            snprintf(name, name_n, "%.*s", ncl(ids[k - 1].n), ids[k - 1].p);
            return true;
        }
    return false;
}

/* a member or typedef's label: the declaration head as it stands - the
   type stays, so do array sizes and bitfield widths - minus any
   initializer */
static void decl_label(char *dst, size_t dstn, const char *head) {
    const char *eq = strchr(head, '=');
    size_t n = eq ? (size_t)(eq - head) : strlen(head);
    while (n > 0 && head[n - 1] == ' ') n--;
    label_copy(dst, dstn, head, n);
}

/* the next space-separated word of a collapsed head, or NULL */
static const char *next_word(const char **cursor, size_t *len) {
    const char *p = *cursor;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    const char *start = p;
    while (*p && *p != ' ') p++;
    *len = (size_t)(p - start);
    *cursor = p;
    return start;
}

static bool eq_word(const char *w, size_t n, const char *lit) {
    return strlen(lit) == n && !strncmp(w, lit, n);
}

/* an aggregate head: [typedef] (struct|union|enum|class|interface)
   [class] [name] [: bases] - and nothing else; the whole head must be
   exactly that */
static bool match_agg(const char *head, tok_t *kind, tok_t *name) {
    const char *cur = head;
    const char *w;
    size_t n;

    name->p = NULL;
    name->n = 0;

    w = next_word(&cur, &n);
    if (!w) return false;
    if (eq_word(w, n, "typedef")) {
        w = next_word(&cur, &n);
        if (!w) return false;
    }
    if (!eq_word(w, n, "struct") && !eq_word(w, n, "union") &&
        !eq_word(w, n, "enum") && !eq_word(w, n, "class") &&
        !eq_word(w, n, "interface"))
        return false;
    *kind = (tok_t){w, n};

    if ((w = next_word(&cur, &n)) && eq_word(w, n, "class"))
        w = next_word(&cur, &n);

    /* the pattern needs \s+ after the kind word: an anonymous
       "struct {" (nothing but the kind) is not an aggregate */
    if (!w) return false;

    if (w[0] != ':') {
        /* a plain identifier: [A-Za-z_]\w* */
        if (!(w[0] == '_' || (w[0] >= 'A' && w[0] <= 'Z') ||
              (w[0] >= 'a' && w[0] <= 'z')))
            return false;
        for (size_t k = 1; k < n; k++)
            if (!(isalnum((unsigned char)w[k]) || w[k] == '_'))
                return false;
        *name = (tok_t){w, n};
        w = next_word(&cur, &n);
    }
    /* nothing, or a ':' inheritance tail */
    return !w || w[0] == ':';
}

/* comments and string/char literals become spaces (newlines survive),
   so nothing inside them can look like structure. Returns a malloc'd
   same-length copy. */
static char *blank_comments(const char *text, size_t len) {
    char *out = malloc(len + 1);
    if (!out) {
        perror("llmkit: malloc");
        exit(EXIT_OUT_OF_CHANNEL);
    }
    size_t i = 0;
    int state = 0; /* 0 code 1 // 2 block 3 str 4 char */
    while (i < len) {
        char c = text[i];
        char nxt = i + 1 < len ? text[i + 1] : '\0';
        if (state == 0) {
            if (c == '/' && nxt == '/') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 1;
            } else if (c == '/' && nxt == '*') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 2;
            } else if (c == '"') {
                out[i++] = ' ';
                state = 3;
            } else if (c == '\'') {
                out[i++] = ' ';
                state = 4;
            } else {
                out[i++] = c;
            }
        } else if (state == 1) {
            out[i] = c == '\n' ? '\n' : ' ';
            if (c == '\n') state = 0;
            i++;
        } else if (state == 2) {
            if (c == '*' && nxt == '/') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 0;
            } else {
                out[i++] = c == '\n' ? '\n' : ' ';
            }
        } else { /* string or char literal */
            if (c == '\\' && nxt) {
                out[i++] = ' ';
                out[i++] = nxt == '\n' ? '\n' : ' ';
            } else if ((state == 3 && c == '"') ||
                       (state == 4 && c == '\'')) {
                out[i++] = ' ';
                state = 0;
            } else {
                out[i++] = c == '\n' ? '\n' : ' ';
            }
        }
    }
    out[len] = '\0';
    return out;
}

/* collapse a run of whitespace to single spaces */
static void squash_ws(char *dst, size_t dstn, const char *src) {
    size_t r = 0, w = 0;
    bool pending = false;
    while (src[r] && w + 1 < dstn) {
        if (src[r] == ' ' || src[r] == '\t') {
            pending = true;
            r++;
            continue;
        }
        if (pending && w) dst[w++] = ' ';
        pending = false;
        dst[w++] = src[r++];
    }
    dst[w] = '\0';
}

/* ---------- shell, batch, powershell: function outline ---------- */

/* one shared outline walker; the extractors do dialect work */

/* is line[pos..] a word end here: the next char is not an identifier
   character? */
static bool word_ends(const char *line, size_t len, size_t pos,
                      size_t wl) {
    size_t end = pos + wl;
    if (end >= len) return true;
    return !id_start((unsigned char)line[end]) &&
           !isalnum((unsigned char)line[end]) && line[end] != '_';
}

/* drop '  a:  b ' -> 'a b' into dst; two runs of whitespace squeeze to
   one space, trailing space goes */
static void squeeze_spaces(char *dst, size_t cap, const char *s,
                           size_t n) {
    size_t w = 0;
    bool pending = false;
    for (size_t r = 0; r < n && w + 1 < cap; r++) {
        if (s[r] == ' ' || s[r] == '\t') {
            pending = true;
            continue;
        }
        if (pending && w) dst[w++] = ' ';
        pending = false;
        dst[w++] = s[r];
    }
    dst[w] = '\0';
}

/* ---------- shell (.sh, .bash): POSIX functions and subcommands ---------- */

/* the function name if `name() {` (or a newline) opens a POSIX shell
   function here, else NULL */
static const char *sh_fn_at(const char *line, size_t ll, size_t k) {
    size_t n = 0;
    while (k + n < ll && (isalnum((unsigned char)line[k + n]) ||
                          line[k + n] == '_'))
        n++;
    if (!n || k + n + 2 > ll || line[k + n] != '(' ||
        line[k + n + 1] != ')')
        return NULL;
    size_t a = k + n + 2;
    while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
    if (a >= ll || line[a] != '{') return NULL;
    return line + k;
}

/* the case keyword if `case WORD in` opens a case statement here, else
   NULL */
static const char *sh_case_at(const char *line, size_t ll, size_t k) {
    if (!word_at(line, ll, k, "case", 4)) return NULL;
    if (!word_ends(line, ll, k, 4)) return NULL;
    size_t a = k + 4;
    while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
    if (a >= ll || line[a] == ' ' || line[a] == '\t') return NULL;
    while (a < ll && line[a] != ' ' && line[a] != '\t') a++;
    while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
    if (!word_at(line, ll, a, "in", 2) || !word_ends(line, ll, a, 2))
        return NULL;
    return line + k + 5;
}

/* the case-arm label if the line is a pattern branch: `p) cmd ;;`,
   `p)` opening a multi-line arm, or `p) cmd # c`. The pattern (text
   before the closing paren) must be a plain word list: `=`, `(`, `;`
   or a backtick in it rejects the line (kills `x=$(...)` assignments) */
static const char *sh_arm_at(const char *line, size_t ll, size_t k) {
    if (line[k] == '(' || line[k] == '`' || line[k] == '\\' ||
        line[k] == '#')
        return NULL;
    int sub = 0, par = 0;
    for (size_t i = k; i < ll; i++) {
        char c = line[i];
        if (c == '$') {
            if (i + 1 < ll && (line[i + 1] == '(' || line[i + 1] == '{'))
                sub++;
            continue;
        }
        if (sub) {
            if (c == ')' || c == '}') sub--;
            continue;
        }
        if (c == '(') {
            par++;
            continue;
        }
        if (c != ')' || par) {
            if (c == ')') par--;
            continue;
        }
        /* the close: the pattern is line[k..i) */
        for (size_t j = k; j < i; j++) {
            char d = line[j];
            if (d == '=' || d == ';' || d == '(' || d == '`')
                return NULL;
        }
        size_t a = i + 1;
        while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
        if (a >= ll || line[a] == ';' || line[a] == '#')
            return line + k; /* arm over here */
        /* a one-liner: the `;;` must come before any further `)` */
        int s2 = 0;
        for (size_t j = a; j < ll; j++) {
            char d = line[j];
            if (d == '$') {
                if (j + 1 < ll && line[j + 1] == '(') s2++;
                continue;
            }
            if (s2) {
                if (d == ')' || d == '}') s2--;
                continue;
            }
            if (d == ';' && j + 1 < ll && line[j + 1] == ';')
                return line + k;
            if (d == ')') return NULL; /* that was a group, not a close */
        }
        return line + k;
    }
    return NULL;
}

static void analyze_shell(const char *text, size_t len, nodes_t *ns) {
    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;
    int depth = 0;

    while (bm_next_line(text, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k >= ll || line[k] == '#') continue;
        if (!strncmp(line + k, "esac", 4) && word_ends(line, ll, k, 4)) {
            if (depth > 0) depth--;
            continue;
        }
        if (!strncmp(line + k, "}", 1) && word_ends(line, ll, k, 1)) {
            if (depth > 0) depth--;
            continue;
        }
        const char *w;
        if ((w = sh_fn_at(line, ll, k))) {
            char label[AN_LABEL_MAX + 8];
            size_t wl = 0;
            while (w + wl < line + ll && w[wl] != '(') wl++;
            squeeze_spaces(label, sizeof label, w, wl);
            char lab2[AN_LABEL_MAX + 16];
            snprintf(lab2, sizeof lab2, "%s()", label);
            node_add(ns, lineno, depth, lab2);
            depth++;
            continue;
        }
        if ((w = sh_case_at(line, ll, k))) {
            char label[AN_LABEL_MAX + 8];
            squeeze_spaces(label, sizeof label, w,
                           (size_t)(line + ll - w));
            char lab2[AN_LABEL_MAX + 16];
            snprintf(lab2, sizeof lab2, "case %s", label);
            node_add(ns, lineno, depth, lab2);
            depth++;
            continue;
        }
        if ((w = sh_arm_at(line, ll, k))) {
            char label[AN_LABEL_MAX + 8];
            size_t wl = 0;
            while (w + wl < line + ll && w[wl] != ')') wl++;
            squeeze_spaces(label, sizeof label, w, wl);
            node_add(ns, lineno, depth, label);
        }
    }
}

/* ---------- batch (.bat, .cmd): labels and calls ---------- */

static bool bat_line_at(const char *line, size_t ll, size_t k,
                        const char *word, size_t wl) {
    if (!word_at(line, ll, k, word, wl)) return false;
    return word_ends(line, ll, k, wl);
}

static void analyze_batch(const char *text, size_t len, nodes_t *ns) {
    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;
    const char *last = NULL; /* the call the following label answers */

    while (bm_next_line(text, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k + 1 >= ll) continue;         /* ':' or empty */
        if (line[k + 1] == ':') continue;  /* :: comment */
        if (line[k] == ':') {
            if (last && !strncmp(line + k + 1, last, strlen(last)) &&
                line[k + 1 + strlen(last)] == '\0') {
                node_add(ns, lineno, 1, line + k + 1);
            } else {
                node_add(ns, lineno, 0, line + k + 1);
            }
            last = NULL;
            continue;
        }
        const char *word = NULL;
        if (bat_line_at(line, ll, k, "call", 4) ||
            bat_line_at(line, ll, k, "goto", 4)) {
            word = "call/goto";
        } else if (bat_line_at(line, ll, k, "for", 3)) {
            word = "for";
        } else if (bat_line_at(line, ll, k, "if", 2)) {
            word = "if";
        }
        if (!word) continue;
        if (!strcmp(word, "call/goto")) {
            size_t a = k + 4;
            while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
            if (bat_line_at(line, ll, a, "call", 4)) a += 4;
            while (a < ll && (line[a] == ' ' || line[a] == '\t')) a++;
            if (a < ll && line[a] == ':')
                last = line + a + 1; /* expect this label next */
        }
        char label[AN_LABEL_MAX / 2 + 4];
        squeeze_spaces(label, sizeof label, line + k, ll - k);
        char lab2[AN_LABEL_MAX + 16];
        snprintf(lab2, sizeof lab2, "%s %s", word, label);
        node_add(ns, lineno, 0, lab2);
    }
}

/* ---------- powershell (.ps1): functions, filters, classes ---------- */

/* the keyword if the line opens a nested function/filter/enum here,
   else NULL */
static const char *ps_sub_at(const char *line, size_t ll, size_t k) {
    static const char *const words[] = {"function", "filter", "enum",
                                        NULL};
    for (int i = 0; words[i]; i++) {
        size_t wl = strlen(words[i]);
        if (word_at(line, ll, k, words[i], wl) &&
            word_ends(line, ll, k, wl))
            return words[i];
    }
    return NULL;
}

static void analyze_pwsh(const char *text, size_t len, nodes_t *ns) {
    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;
    int depth = 0;

    while (bm_next_line(text, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k >= ll || line[k] == '#') continue;
        size_t tl = ll; /* the label ends at the opening '{' */
        for (size_t i = k; i < tl; i++)
            if (line[i] == '{') { tl = i; break; }
        while (tl > k && (line[tl - 1] == ' ' || line[tl - 1] == '\t'))
            tl--;
        if (line[k] == '}' && tl == k + 1) {
            if (depth > 0) depth--;
            continue;
        }
        const char *kw;
        if ((kw = ps_sub_at(line, ll, k))) {
            char label[AN_LABEL_MAX + 8];
            squeeze_spaces(label, sizeof label, line + k, tl - k);
            node_add(ns, lineno, depth, label);
            depth++;
            continue;
        }
        if (word_at(line, ll, k, "class", 5) &&
            word_ends(line, ll, k, 5)) {
            char label[AN_LABEL_MAX + 8];
            squeeze_spaces(label, sizeof label, line + k, tl - k);
            node_add(ns, lineno, depth, label);
            depth++;
        }
    }
}

/* ---------- php: namespaces, uses, classes, functions ---------- */

/* php blanking: html outside <?php ?> tags, // # and slash-star
   comments, quoted literals and heredoc/nowdoc bodies all become
   spaces (newlines survive), so none of it can look like structure.
   malloc'd same-length copy */
static char *php_blank(const char *text, size_t len) {
    char *out = malloc(len + 1);
    if (!out) {
        perror("llmkit: malloc");
        exit(EXIT_OUT_OF_CHANNEL);
    }
    bool php = false; /* in code (<?php opened) vs plain html */
    int state = 0; /* 0 code 1 line comment 2 block comment 3 str
                      4 char 5 heredoc body */
    char hdelim[40]; /* the heredoc's delimiter word */
    size_t hdn = 0;
    size_t i = 0;
    while (i < len) {
        char c = text[i];
        char nxt = i + 1 < len ? text[i + 1] : '\0';
        if (!php) { /* html until a php open tag */
            if (c == '<' && nxt == '?') {
                php = true;
                /* <?php / <?= openers: blank the whole word */
                size_t wl = 2;
                if (i + 4 <= len && !strncmp(text + i, "<?php", 5))
                    wl = 5;
                else if (i + 3 <= len && !strncmp(text + i, "<?=", 3))
                    wl = 3;
                while (wl-- && i < len) out[i++] = ' ';
                continue;
            }
            out[i++] = c == '\n' ? '\n' : ' ';
            continue;
        }
        if (state == 0) {
            if (c == '?' && nxt == '>') {
                php = false;
                out[i++] = ' ';
                out[i++] = ' ';
            } else if (c == '/' && nxt == '/') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 1;
            } else if (c == '#') {
                out[i++] = ' ';
                state = 1;
            } else if (c == '/' && nxt == '*') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 2;
            } else if (c == '"') {
                out[i++] = ' ';
                state = 3;
            } else if (c == '\'') {
                out[i++] = ' ';
                state = 4;
            } else if (c == '<' && nxt == '<' && i + 2 < len &&
                       text[i + 2] == '<') {
                /* heredoc/nowdoc: remember the delimiter, the body
                   ends at a line holding it alone */
                size_t j = i + 3;
                while (j < len && (text[j] == ' ' || text[j] == '\t'))
                    j++;
                if (j < len && (text[j] == '"' || text[j] == '\''))
                    j++; /* nowdoc's quotes are decoration */
                size_t dn = 0;
                while (j + dn < len &&
                       (isalnum((unsigned char)text[j + dn]) ||
                        text[j + dn] == '_') &&
                       dn + 1 < sizeof hdelim)
                    dn++;
                hdn = dn;
                memcpy(hdelim, text + j, hdn);
                for (size_t k = i; k < j + dn; k++)
                    out[k] = text[k] == '\n' ? '\n' : ' ';
                i = j + dn;
                state = hdn ? 5 : 0;
            } else {
                out[i++] = c;
            }
        } else if (state == 1) {
            out[i] = c == '\n' ? '\n' : ' ';
            if (c == '\n') state = 0;
            i++;
        } else if (state == 2) {
            if (c == '*' && nxt == '/') {
                out[i++] = ' ';
                out[i++] = ' ';
                state = 0;
            } else {
                out[i++] = c == '\n' ? '\n' : ' ';
            }
        } else if (state == 5) { /* heredoc body, ends at a line
                                    starting with the delimiter */
            out[i] = c == '\n' ? '\n' : ' ';
            if (c == '\n' && hdn) {
                size_t j = i + 1;
                size_t dn = 0;
                while (j + dn < len && dn < hdn &&
                       text[j + dn] == hdelim[dn])
                    dn++;
                if (dn == hdn) {
                    i++;
                    while (i < j + hdn) out[i++] = ' ';
                    state = 0;
                    hdn = 0;
                    continue;
                }
            }
            i++;
        } else { /* string or char literal, backslash escapes */
            if (c == '\\' && nxt) {
                out[i++] = ' ';
                if (i < len) out[i++] = ' ';
            } else if ((state == 3 && c == '"') ||
                       (state == 4 && c == '\'')) {
                out[i++] = ' ';
                state = 0;
            } else {
                out[i] = c == '\n' ? '\n' : ' ';
                i++;
            }
        }
    }
    out[len] = '\0';
    return out;
}

/* does s[0..n) hold the opener (optionally prefixed with modifiers)
   of a php aggregate: class/interface/trait/enum? */
static bool php_agg_head(const char *s, size_t n) {
    static const char *const words[] = {
        "class", "interface", "trait", "enum", NULL};
    for (int w = 0; words[w]; w++) {
        size_t wl = strlen(words[w]);
        size_t k = 0;
        /* optional modifiers may stack before the keyword */
        for (;;) {
            if (k + 8 <= n && !strncmp(s + k, "abstract", 8) &&
                (s[k + 8] == ' ' || s[k + 8] == '\t'))
                k += 8;
            else if (k + 8 <= n && !strncmp(s + k, "readonly", 8) &&
                     (s[k + 8] == ' ' || s[k + 8] == '\t'))
                k += 8;
            else if (k + 5 <= n && !strncmp(s + k, "final", 5) &&
                     (s[k + 5] == ' ' || s[k + 5] == '\t'))
                k += 5;
            else
                break;
        }
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k + wl <= n && !strncmp(s + k, words[w], wl) &&
            word_ends(s, n, k, wl))
            return true;
    }
    return false;
}

/* does the head open with the word w? */
static bool starts_word(const char *s, const char *w) {
    size_t wl = strlen(w);
    return !strncmp(s, w, wl) && word_ends(s, strlen(s), 0, wl);
}

/* a named function head: php spells it out, [modifiers] function
   name (...), so the word itself is the test (anonymous closures and
   arrow fns have no name to report) */
static bool php_func_head(const char *head, tok_t *out) {
    size_t n = strlen(head);
    for (size_t i = 0; i + 8 <= n; i++) {
        if (!word_at(head, n, i, "function", 8)) continue;
        size_t j = i + 8;
        while (j < n && (head[j] == ' ' || head[j] == '\t')) j++;
        size_t e = j;
        while (e < n && (isalnum((unsigned char)head[e]) || head[e] == '_'))
            e++;
        if (e == j) return false; /* function ( : anonymous */
        *out = (tok_t){head + j, e - j};
        return true;
    }
    return false;
}

static void analyze_php(const char *text, size_t len, nodes_t *ns) {
    char *src = php_blank(text, len);

    int stack[64];        /* brace depth at each open aggregate */
    size_t stack_n = 0;
    int depth = 0;
    char acc[AN_ACC_MAX]; /* the declaration head being built */
    size_t acc_len = 0;
    long acc_line = -1;
    bool acc_blank = true; /* acc still all whitespace */
    bool acc_bad = false;  /* head overran the cap: no node from it */

    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;

    while (bm_next_line(src, len, &pos, &line, &ll)) {
        lineno++;

        for (size_t c = 0; c < ll; c++) {
            char ch = line[c];

            if (ch != '{' && ch != '}' && ch != ';') {
                if (!acc_bad) {
                    if (acc_blank && ch != ' ' && ch != '\t') {
                        acc_line = lineno;
                        acc_blank = false;
                    }
                    if (acc_len + 1 < sizeof acc)
                        acc[acc_len++] = ch;
                    else
                        acc_bad = true;
                }
                continue;
            }

            acc[acc_len] = '\0';
            char head[AN_ACC_MAX];
            squeeze_spaces(head, sizeof head, acc, acc_len);

            if (ch == '{') {
                tok_t fn;
                bool agg = !acc_bad && head[0] &&
                           php_agg_head(head, strlen(head));
                /* a function head one brace inside an aggregate is a
                   method; at the top level it is a plain function */
                bool meth = !agg && !acc_bad && head[0] && stack_n &&
                            depth == stack[stack_n - 1] + 1 &&
                            php_func_head(head, &fn);
                bool top = !agg && !acc_bad && head[0] && !stack_n &&
                           php_func_head(head, &fn);
                if (agg || meth || top) {
                    char label[AN_LABEL_MAX + 8];
                    label_copy(label, sizeof label, head, strlen(head));
                    node_add(ns, acc_line, (int)stack_n, label);
                    if (agg && stack_n < sizeof stack / sizeof stack[0])
                        stack[stack_n++] = depth;
                }
                depth++;
            } else if (ch == '}') {
                if (depth > 0) depth--;
                if (stack_n && stack[stack_n - 1] == depth) stack_n--;
            } else { /* ';' */
                tok_t fn;
                if (!acc_bad && head[0] && !stack_n) {
                    /* top level: namespaces and imports */
                    if (starts_word(head, "namespace") ||
                        starts_word(head, "use")) {
                        char label[AN_LABEL_MAX + 8];
                        label_copy(label, sizeof label, head,
                                   strlen(head));
                        node_add(ns, acc_line, 0, label);
                    }
                } else if (!acc_bad && head[0] && stack_n &&
                           depth == stack[stack_n - 1] + 1 &&
                           php_func_head(head, &fn)) {
                    /* a bodyless method: abstract, or an interface's */
                    char label[AN_LABEL_MAX + 8];
                    label_copy(label, sizeof label, head, strlen(head));
                    node_add(ns, acc_line, (int)stack_n, label);
                }
            }

            acc_len = 0;
            acc_bad = false;
            acc_blank = true;
            acc_line = -1;
        }
    }
    free(src);
}

/* does a declaration head open with the `typedef` word? */
static bool starts_typedef(const char *head) {
    return !strncmp(head, "typedef", 7) &&
           (head[7] == '\0' || head[7] == ' ');
}

static void analyze_c(const char *text, size_t len, nodes_t *ns) {
    char *src = blank_comments(text, len);

    int stack[64];        /* brace depth at each open aggregate */
    size_t stack_n = 0;
    int depth = 0;
    char acc[AN_ACC_MAX]; /* the declaration head being built */
    size_t acc_len = 0;
    long acc_line = -1;
    bool acc_blank = true; /* acc still all whitespace */
    bool acc_bad = false;  /* head overran the cap: no node from it */

    size_t pos = 0;
    const char *line;
    size_t ll;
    long lineno = 0;

    while (bm_next_line(src, len, &pos, &line, &ll)) {
        lineno++;
        size_t k = 0;
        while (k < ll && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k < ll && line[k] == '#') { /* preprocessor line */
            acc_len = 0;
            acc_bad = false;
            acc_blank = true;
            acc_line = -1;
            continue;
        }

        for (size_t c = 0; c < ll; c++) {
            char ch = line[c];

            if (ch != '{' && ch != '}' && ch != ';') {
                if (!acc_bad) {
                    if (acc_blank && ch != ' ' && ch != '\t') {
                        acc_line = lineno;
                        acc_blank = false;
                    }
                    if (acc_len + 1 < sizeof acc)
                        acc[acc_len++] = ch;
                    else
                        acc_bad = true;
                }
                continue;
            }

            acc[acc_len] = '\0';
            char head[AN_ACC_MAX];
            squash_ws(head, sizeof head, acc);

            if (ch == '{') {
                if (!acc_bad && head[0]) {
                    tok_t kind, name;
                    if (match_agg(head, &kind, &name)) {
                        char label[AN_LABEL_MAX + 16];
                        if (name.n)
                            snprintf(label, sizeof label, "%.*s %.*s",
                                     ncl(kind.n), kind.p, ncl(name.n),
                                     name.p);
                        else
                            snprintf(label, sizeof label, "%.*s {{...}}",
                                     ncl(kind.n), kind.p);
                        node_add(ns, acc_line, (int)stack_n, label);
                        if (stack_n < sizeof stack / sizeof stack[0])
                            stack[stack_n++] = depth;
                    } else {
                        tok_t fn;
                        if (stack_n == 0 && func_name(head, &fn)) {
                            char label[AN_LABEL_MAX + 8];
                            size_t hlen = strlen(head);
                            if (hlen && head[hlen - 1] == ')')
                                /* the whole head is the signature */
                                label_copy(label, sizeof label, head, hlen);
                            else
                                snprintf(label, sizeof label, "%.*s()",
                                         ncl(fn.n), fn.p);
                            node_add(ns, acc_line, 0, label);
                        }
                    }
                }
                depth++;
            } else if (ch == '}') {
                if (depth > 0) depth--;
                if (stack_n && stack[stack_n - 1] == depth) stack_n--;
            } else { /* ';' */
                if (!acc_bad) {
                    char name[AN_LABEL_MAX + 8];
                    char label[AN_LABEL_MAX + 8];
                    if (stack_n && depth == stack[stack_n - 1] + 1) {
                        /* a member: its declaration, type included */
                        if (decl_name(head, name, sizeof name)) {
                            decl_label(label, sizeof label, head);
                            node_add(ns, acc_line, (int)stack_n, label);
                        }
                    } else if (depth == 0 && starts_typedef(head)) {
                        if (decl_name(head, name, sizeof name)) {
                            decl_label(label, sizeof label, head);
                            node_add(ns, acc_line, 0, label);
                        }
                    }
                }
            }

            acc_len = 0;
            acc_bad = false;
            acc_blank = true;
            acc_line = -1;
        }
    }
    free(src);
}

/* ---------- the registry ---------- */

static const char *const EXTS_MARKDOWN[] = {
    ".md", ".markdown", ".mdown", NULL};
static const char *const EXTS_C[] = {
    ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx",
    ".java", ".cs", ".js", ".jsx", ".ts", ".tsx", NULL};
static const char *const EXTS_PYTHON[] = {
    ".py", ".pyi", NULL};
static const char *const EXTS_SHELL[] = {
    ".sh", ".bash", NULL};
static const char *const EXTS_BATCH[] = {
    ".bat", ".cmd", NULL};
static const char *const EXTS_PWSH[] = {
    ".ps1", NULL};
static const char *const EXTS_PHP[] = {
    ".php", ".phtml", ".phps", NULL};

static const analyzer_t ANALYZERS[] = {
    {"markdown", EXTS_MARKDOWN, analyze_markdown},
    {"c",        EXTS_C,        analyze_c},
    {"python",   EXTS_PYTHON,   analyze_python},
    {"shell",    EXTS_SHELL,    analyze_shell},
    {"batch",    EXTS_BATCH,    analyze_batch},
    {"powershell", EXTS_PWSH,   analyze_pwsh},
    {"php",      EXTS_PHP,      analyze_php},
};
enum { ANALYZERS_N = sizeof ANALYZERS / sizeof ANALYZERS[0] };

/* the analyzer owning path's extension (case-insensitive), NULL when
   the file type is unknown */
static const analyzer_t *analyzer_for_ext(const char *path) {
    if (!path) return NULL;
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return NULL; /* no (real) extension */

    char ext[32];
    size_t n = strlen(dot);
    if (n >= sizeof ext) return NULL;
    for (size_t i = 0; i <= n; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);

    for (size_t i = 0; i < ANALYZERS_N; i++)
        for (size_t k = 0; ANALYZERS[i].exts[k]; k++)
            if (!strcmp(ANALYZERS[i].exts[k], ext)) return &ANALYZERS[i];
    return NULL;
}

/* single-quote a path for /bin/sh: '...' with each embedded ' closed,
   escaped, reopened ('\'' - nothing inside single quotes is special
   to the shell) */
static void esc_sq(buf_t *out, const char *path) {
    buf_append_byte(out, '\'');
    for (const char *p = path; *p; p++) {
        if (*p == '\'') buf_append_str(out, "'\\''");
        else buf_append_byte(out, *p);
    }
    buf_append_byte(out, '\'');
}

/* malloc'd content type of path, NULL when the os cannot name one.
   posix asks the file(1) utility; windows sniffs the head bytes with
   FindMimeFromData (which needs no com init) */
static char *mime_of(const char *path) {
#ifdef _WIN32
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char head[256];
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    if (n == 0) return NULL;
    LPWSTR wmime = NULL;
    if (FAILED(FindMimeFromData(NULL, NULL, head, (DWORD)n, NULL,
                                FMFD_DEFAULT, &wmime, 0)))
        return NULL;
    if (!wmime) return NULL;
    int wl = WideCharToMultiByte(CP_UTF8, 0, wmime, -1, NULL, 0,
                                 NULL, NULL);
    if (wl <= 0) {
        CoTaskMemFree(wmime);
        return NULL;
    }
    char *mime = malloc((size_t)wl);
    if (!mime) {
        CoTaskMemFree(wmime);
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, wmime, -1, mime, wl, NULL, NULL);
    CoTaskMemFree(wmime);
    return mime;
#else
    buf_t cmd;
    buf_init(&cmd);
    buf_append_str(&cmd, "file -b --mime-type -- ");
    esc_sq(&cmd, path);
    FILE *p = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!p) return NULL;
    char line[128];
    if (!fgets(line, sizeof line, p)) {
        pclose(p);
        return NULL;
    }
    int rc = pclose(p);
    if (rc != 0) return NULL;
    for (char *e = line + strlen(line);
         e > line && (e[-1] == '\n' || e[-1] == '\r' ||
                      e[-1] == ' ' || e[-1] == '\t');)
        *--e = '\0';
    if (!line[0]) return NULL;
    char *mime = strdup(line);
    return mime;
#endif
}

void tool_file_analyze(const cJSON *args, buf_t *out,
                              bool *is_error) {
    char *path;
    if (!(path = bm_need_path(args, "path", out))) {
        *is_error = true;
        goto done;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        goto done;
    }
    struct stat st;
    if (stat(path, &st)) {
        buf_clear(out);
        buf_appendf(out, "cannot stat '%s': %s", path, strerror(errno));
        *is_error = true;
        goto done;
    }
    if (S_ISDIR(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is a directory (files_list lists those)",
                    path);
        *is_error = true;
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is not a regular file", path);
        *is_error = true;
        goto done;
    }
    const analyzer_t *an = analyzer_for_ext(path);
    if (!an) {
        char *mime = mime_of(path);
        char hs[32];
        bm_human_size((unsigned long long)st.st_size, hs, sizeof hs);
        buf_clear(out);
        buf_appendf(out, "'%s': unknown file type (known:", path);
        for (size_t i = 0; i < ANALYZERS_N; i++)
            buf_appendf(out, "%s %s", i ? "," : "", ANALYZERS[i].name);
        buf_append_str(out, ")\n");
        buf_appendf(out, "size %s", hs);
        if (mime) buf_appendf(out, ", type %s", mime);
        buf_t b;
        buf_init(&b);
        int rc = bm_read_whole(path, &b, BM_FILE_CAP);
        bool textual = rc == 0 && !memchr(b.data ? b.data : "", 0, b.len) &&
                       utf8_valid((const uint8_t *)(b.data ? b.data : ""),
                                  b.len);
        if (rc == 0 && textual) {
            unsigned long long nlines = 0;
            for (size_t i = 0; i < b.len; i++)
                if (b.data[i] == '\n') nlines++;
            if (b.len && b.data[b.len - 1] != '\n') nlines++;
            buf_appendf(out, ", %llu lines", nlines);
        } else if (rc == -2) {
            buf_append_str(out, ", over 64Mb read cap");
        }
        buf_append_byte(out, '\n');
        buf_free(&b);
        free(mime);
        goto done; /* *is_error stays false: the reply is a report */
    }
    buf_t b;
    buf_init(&b);
    int rc = bm_read_whole(path, &b, BM_FILE_CAP);
    if (rc == -2) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "file too large (over %llu bytes)", BM_FILE_CAP);
        *is_error = true;
        goto done;
    }
    if (rc == -1) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "cannot read '%s'", path);
        *is_error = true;
        goto done;
    }
    if (memchr(b.data, 0, b.len) ||
        !utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "'%s' is not a textual file", path);
        *is_error = true;
        goto done;
    }

    unsigned long long nlines = 0;
    for (size_t i = 0; i < b.len; i++)
        if (b.data[i] == '\n') nlines++;
    if (b.len && b.data[b.len - 1] != '\n') nlines++;

    nodes_t ns = {0};
    an->fn(b.data ? b.data : "", b.len, &ns);
    buf_free(&b);

    buf_clear(out);
    buf_appendf(out, "%s  [%s]  %llu lines\n", path, an->name, nlines);
    if (ns.count || ns.truncated)
        buf_append(out, ns.out.data, ns.out.len);
    else
        buf_append_str(out, "  (nothing found)\n");
    if (ns.truncated) buf_append_str(out, "... [truncated]\n");
    buf_free(&ns.out);
done:
    free(path);
}
