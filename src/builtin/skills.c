#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>


/* ================= agent skills ================= */

/* a skill is a directory holding a SKILL.md (yaml frontmatter with at
   least name and description, then the body). two roots are scanned in
   order: .agents/skills under the current working directory, then under
   the home directory - the local one shadows the global one. */

/* the existing skill roots, search order (0..2 of them) */
size_t bm_skill_roots(char roots[][BM_PATH_MAX]) {
    size_t n = 0;
    char cwd[BM_PATH_MAX];
#ifdef _WIN32
    if (_getcwd(cwd, sizeof cwd))
#else
    if (getcwd(cwd, sizeof cwd))
#endif
        if (snprintf(roots[n], BM_PATH_MAX, "%s/.agents/skills", cwd) <
            (int)BM_PATH_MAX)
            n++;
#ifndef _WIN32
    const char *home = getenv("HOME");
#else
    const char *home = getenv("USERPROFILE");
#endif
    if (home && *home &&
        snprintf(roots[n], BM_PATH_MAX, "%s/.agents/skills", home) <
            (int)BM_PATH_MAX)
        n++;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        if (!stat(roots[i], &st) && S_ISDIR(st.st_mode)) {
            if (w != i) memmove(roots[w], roots[i], BM_PATH_MAX);
            w++;
        }
    }
    return w;
}

void bm_dir_names_free(char **names, size_t n) {
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* strcmp for qsort of names lives in util.c as bm_cmp_names */

/* sorted entry names of a directory (not "." / ".."); free with
   bm_dir_names_free */
char **bm_dir_names(const char *path, size_t *nout) {
    *nout = 0;
    DIR *d = opendir(path);
    if (!d) return NULL;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **grown = realloc(names, cap * sizeof *names);
            if (!grown) {
                bm_dir_names_free(names, n);
                closedir(d);
                return NULL;
            }
            names = grown;
        }
        names[n] = strdup(de->d_name);
        if (!names[n]) {
            bm_dir_names_free(names, n);
            closedir(d);
            return NULL;
        }
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof *names, bm_cmp_names);
    *nout = n;
    return names;
}

/* <root>/<entry>/SKILL.md when entry is a directory */
bool bm_skill_file(const char *root, const char *entry, char *out,
                       size_t sz) {
    char dir[BM_PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/%s", root, entry) >= (int)sizeof dir)
        return false;
    struct stat st;
    if (BM_LSTAT(dir, &st) || !S_ISDIR(st.st_mode)) return false;
    return snprintf(out, sz, "%s/SKILL.md", dir) < (int)sz;
}

/* the line starting at *i in s (len bytes): *ls and *ll point at it (cr
   stripped), *i moves past the newline; false at end of input */
bool bm_next_line(const char *s, size_t len, size_t *i, const char **ls,
                      size_t *ll) {
    if (*i >= len) return false;
    const char *p = s + *i;
    const char *nl = memchr(p, '\n', len - *i);
    *ll = nl ? (size_t)(nl - p) : len - *i;
    if (*ll && p[*ll - 1] == '\r') (*ll)--;
    *ls = p;
    *i += nl ? (size_t)(nl - p) + 1 : len - *i;
    return true;
}

/* the value of a top-level "key:" line inside a yaml frontmatter block
   (first line "---", closed by a second "---"). folded block scalars
   ("|" / ">") join their indented lines with spaces, so the result
   never holds a newline. malloc'd, or NULL when the block or key is
   missing. */
char *bm_fm_value(const char *text, size_t len, const char *key) {
    size_t i = 0;
    const char *ls;
    size_t ll;
    if (!bm_next_line(text, len, &i, &ls, &ll)) return NULL;
    if (ll != 3 || strncmp(ls, "---", 3)) return NULL;
    size_t kl = strlen(key);
    for (;;) {
        if (!bm_next_line(text, len, &i, &ls, &ll)) return NULL;
        if (ll == 3 && !strncmp(ls, "---", 3)) return NULL; /* block closed */
        if (ll > kl && !strncmp(ls, key, kl) && ls[kl] == ':') {
            size_t v = kl + 1;
            while (v < ll && bm_is_ws(ls[v])) v++;
            buf_t b;
            buf_init(&b);
            if (v >= ll || ls[v] == '|' || ls[v] == '>') {
                /* block scalar: the following indented lines, folded */
                while (bm_next_line(text, len, &i, &ls, &ll)) {
                    if (ll == 3 && !strncmp(ls, "---", 3)) break;
                    if (!ll) continue; /* blank lines fold away */
                    if (!bm_is_ws(ls[0])) break; /* dedent: scalar ended */
                    size_t a = 0;
                    while (a < ll && bm_is_ws(ls[a])) a++;
                    if (b.len) buf_append_byte(&b, ' ');
                    buf_append(&b, ls + a, ll - a);
                }
            } else {
                size_t e = ll;
                while (e > v && bm_is_ws(ls[e - 1])) e--;
                buf_append(&b, ls + v, e - v);
            }
            return buf_steal(&b, NULL);
        }
    }
}

/* case-insensitive non-overlapping occurrence count of needle in a
   byte range (ascii case folding, like bm_ci_contains) */
static unsigned long long ci_count(const char *hay, size_t haylen,
                                   const char *needle) {
    size_t nl = strlen(needle);
    if (!nl || nl > haylen) return 0;
    unsigned long long n = 0;
    for (size_t i = 0; i + nl <= haylen; i++) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)hay[i + k]) ==
                             tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) {
            n++;
            i += nl - 1;
        }
    }
    return n;
}

/* split on whitespace, in place; at most max pointers */
static size_t split_kw(char *s, char **out, size_t max) {
    size_t n = 0;
    char *p = s;
    while (*p && n < max) {
        while (*p && bm_is_ws(*p)) p++;
        if (!*p) break;
        out[n++] = p;
        while (*p && !bm_is_ws(*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* ---- skills tools ---- */

typedef struct skill_hit {
    char *name, *desc;
    unsigned long long count;
    size_t seq;
} skill_hit_t;

static int cmp_hits(const void *a, const void *b) {
    const skill_hit_t *x = a, *y = b;
    if (x->count != y->count) return x->count > y->count ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

void tool_skills_search(const cJSON *args, buf_t *out, bool *is_error) {
    const char *keywords = NULL;
    if (!bm_need_str(args, "keywords", &keywords, out)) {
        *is_error = true;
        return;
    }
    char *kcopy = strdup(keywords);
    char *kws[64];
    size_t nkws = kcopy ? split_kw(kcopy, kws, 64) : 0;
    if (!nkws) {
        free(kcopy);
        buf_clear(out);
        buf_append_str(out, "keywords must not be empty");
        *is_error = true;
        return;
    }
    char roots[2][BM_PATH_MAX];
    size_t nroots = bm_skill_roots(roots);
    if (!nroots) {
        free(kcopy);
        buf_clear(out);
        buf_append_str(out, "no skills directory found (looked in "
                            "./.agents/skills and ~/.agents/skills)");
        *is_error = true;
        return;
    }
    skill_hit_t *hits = NULL;
    size_t nh = 0, caph = 0, seq = 0;
    for (size_t r = 0; r < nroots; r++) {
        size_t nn = 0;
        char **names = bm_dir_names(roots[r], &nn);
        for (size_t e = 0; names && e < nn; e++) {
            char sk[BM_PATH_MAX];
            if (bm_skill_file(roots[r], names[e], sk, sizeof sk)) {
                buf_t b;
                buf_init(&b);
                if (bm_read_whole(sk, &b, BM_SEARCH_CAP) == 0 &&
                    !memchr(b.data ? b.data : "", 0, b.len)) {
                    unsigned long long total = 0;
                    for (size_t k = 0; k < nkws; k++)
                        total += ci_count(b.data ? b.data : "", b.len, kws[k]);
                    if (total) {
                        char *nm = bm_fm_value(b.data, b.len, "name");
                        char *ds = bm_fm_value(b.data, b.len, "description");
                        if (!nm) nm = strdup(names[e]);
                        if (!ds) ds = strdup("");
                        if (nh == caph) {
                            size_t ncaph = caph ? caph * 2 : 16;
                            skill_hit_t *grown =
                                realloc(hits, ncaph * sizeof *hits);
                            if (grown) {
                                hits = grown;
                                caph = ncaph;
                            }
                        }
                        if (nh < caph) {
                            hits[nh].name = nm;
                            hits[nh].desc = ds;
                            hits[nh].count = total;
                            hits[nh].seq = seq++;
                            nh++;
                        } else {
                            free(nm);
                            free(ds);
                        }
                    }
                }
                buf_free(&b);
            }
        }
        bm_dir_names_free(names, nn);
    }
    free(kcopy);
    qsort(hits, nh, sizeof *hits, cmp_hits);
    for (size_t i = 0; i < nh; i++) {
        if (i) buf_append_byte(out, '\n');
        buf_appendf(out, "name: %s\ndescription: %s\n", hits[i].name,
                    hits[i].desc);
        free(hits[i].name);
        free(hits[i].desc);
    }
    free(hits);
    if (!nh) buf_append_str(out, "no matching skills");
    bm_sanitize_utf8(out);
}

void tool_skills_read(const cJSON *args, buf_t *out, bool *is_error) {
    const char *name = NULL;
    if (!bm_need_str(args, "name", &name, out)) {
        *is_error = true;
        return;
    }
    if (!strlen(name)) {
        buf_clear(out);
        buf_append_str(out, "name must not be empty");
        *is_error = true;
        return;
    }
    char roots[2][BM_PATH_MAX];
    size_t nroots = bm_skill_roots(roots);
    char path[BM_PATH_MAX] = "";
    for (size_t r = 0; r < nroots && !path[0]; r++) {
        size_t nn = 0;
        char **names = bm_dir_names(roots[r], &nn);
        for (size_t e = 0; names && e < nn && !path[0]; e++) {
            char sk[BM_PATH_MAX];
            if (bm_skill_file(roots[r], names[e], sk, sizeof sk)) {
                if (!strcmp(names[e], name)) {
                    snprintf(path, sizeof path, "%s", sk);
                } else {
                    /* no dir-name match: the frontmatter name decides */
                    buf_t fb;
                    buf_init(&fb);
                    if (bm_read_whole(sk, &fb, BM_SEARCH_CAP) == 0) {
                        char *nm = bm_fm_value(fb.data, fb.len, "name");
                        if (nm && !strcmp(nm, name))
                            snprintf(path, sizeof path, "%s", sk);
                        free(nm);
                    }
                    buf_free(&fb);
                }
            }
        }
        bm_dir_names_free(names, nn);
    }
    if (!path[0]) {
        buf_clear(out);
        buf_appendf(out, "no skill named '%s'", name);
        *is_error = true;
        return;
    }
    buf_t b;
    buf_init(&b);
    int rc = bm_read_whole(path, &b, BM_FILE_CAP);
    if (rc != 0 || memchr(b.data ? b.data : "", 0, b.len) ||
        !utf8_valid((const uint8_t *)(b.data ? b.data : ""), b.len)) {
        buf_free(&b);
        buf_clear(out);
        if (rc == -2)
            buf_appendf(out, "skill '%s' is too large", name);
        else if (rc == -1)
            buf_appendf(out, "cannot read skill '%s'", name);
        else
            buf_append_str(out, "the skill file is not textual");
        *is_error = true;
        return;
    }
    buf_clear(out);
    buf_append(out, b.data, b.len);
    buf_free(&b);
    bm_sanitize_utf8(out);
}
