#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

static int cmp_names(const void *a, const void *b) {
    return bm_cmp_names(a, b);
}

/* ---- listing walk ---- */

typedef struct walk_ctx {
    buf_t *out;
    regex_t rx;   /* compiled pattern */
    bool content; /* files_search: pattern matches content lines */
    bool recurse; /* descend into subdirectories */
    bool hidden;  /* show_hidden_files: keep dot-prefixed entries */
} walk_ctx_t;

static void entry_line(buf_t *out, const char *path, const struct stat *st,
                       unsigned long long lines, bool has_lines) {
    char perm[4], hs[48];
    bm_perms_of(path, perm);
    bm_human_size((unsigned long long)st->st_size, hs, sizeof hs);
    buf_appendf(out, "%s %s %s%s", perm, hs, path,
                S_ISDIR(st->st_mode) ? "/" : "");
    if (has_lines) buf_appendf(out, ", %llu lines", lines);
    else if (S_ISDIR(st->st_mode)) {
        /* a hidden directory the walk will not descend into unless the
           caller names it: its line says so */
        const char *base = path_basename(path);
        if (*base == '.' && base[1])
            buf_append_str(out, " (files inside)");
    }
    buf_append_byte(out, '\n');
}

static void emit_entry(walk_ctx_t *w, const char *path,
                       const struct stat *st) {
    if (!w->content && !bm_rx_match(&w->rx, path, strlen(path))) return;
    if (S_ISDIR(st->st_mode)) {
        if (!w->content) entry_line(w->out, path, st, 0, false);
        return;
    }
    if (w->content) {
        /* content search: read, validate, collect matching line numbers */
        buf_t b;
        buf_init(&b);
        int rc = bm_read_whole(path, &b, BM_SEARCH_CAP);
        if (rc != 0 || memchr(b.data ? b.data : "", 0, b.len) ||
            !utf8_valid((const uint8_t *)(b.data ? b.data : ""), b.len)) {
            buf_free(&b);
            return;
        }
        /* count lines + test each */
        unsigned long long lno = 0, nlines = 0;
        buf_t nums;
        buf_init(&nums);
        bool any = false;
        size_t start = 0;
        for (size_t i = 0; i < b.len; i++) {
            if (b.data[i] == '\n') {
                size_t len = i - start;
                if (len && b.data[start + len - 1] == '\r') len--;
                lno++;
                if (bm_rx_match(&w->rx, b.data + start, len)) {
                    if (any) buf_append_str(&nums, ", ");
                    buf_appendf(&nums, "%llu", lno);
                    any = true;
                }
                nlines++;
                start = i + 1;
            }
        }
        if (start < b.len) { /* final line without terminator */
            size_t len = b.len - start;
            if (len && b.data[start + len - 1] == '\r') len--;
            lno++;
            if (bm_rx_match(&w->rx, b.data + start, len)) {
                if (any) buf_append_str(&nums, ", ");
                buf_appendf(&nums, "%llu", lno);
                any = true;
            }
            nlines++;
        }
        if (any) {
            entry_line(w->out, path, st, nlines, true);
            buf_appendf(w->out, "Matching lines: %s\n", nums.data ? nums.data : "");
        }
        buf_free(&nums);
        buf_free(&b);
        return;
    }
    tscan_t ts = bm_scan_text(path, BM_FILE_CAP);
    entry_line(w->out, path, st, ts.lines, ts.ok && ts.textual);
}

static void walk_dir(walk_ctx_t *w, const char *dirpath, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dirpath);
    if (!d) return;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!w->hidden && de->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_names);
    for (size_t i = 0; i < n; i++) {
        char child[BM_PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", dirpath, names[i]) >=
            (int)sizeof child) {
            free(names[i]);
            continue;
        }
        struct stat st;
        if (!BM_LSTAT(child, &st)) {
            emit_entry(w, child, &st);
            /* recursion never opens a hidden directory: an explicit
               root naming one is the only way inside */
            if (w->recurse && S_ISDIR(st.st_mode) && names[i][0] != '.')
                walk_dir(w, child, depth + 1);
        }
        free(names[i]);
    }
    free(names);
}

/* top-level entry: root dir walks, root file lists once */
static bool walk_start(walk_ctx_t *w, const char *root, char *err,
                       size_t errsz) {
    struct stat st;
    if (stat(root, &st)) {
        snprintf(err, errsz, "cannot access '%s': %s", root, strerror(errno));
        return false;
    }
    if (S_ISDIR(st.st_mode)) walk_dir(w, root, 0);
    else emit_entry(w, root, &st);
    return true;
}

/* the agent's head start: the first listing the model would otherwise
   call for - files_list(".", ".*", false, true), the working
   directory's entries, hidden ones included (their marker says which
   hold more). malloc'd text, NULL when the directory cannot be read */
char *builtin_headstart_listing(void) {
    buf_t out;
    buf_init(&out);
    regex_t rx;
    char rxerr[128] = ""; /* ".*" cannot fail, the slot is a safety net */
    if (!bm_rx_compile(&rx, ".*", rxerr, sizeof rxerr)) {
        buf_free(&out);
        return NULL;
    }
    walk_ctx_t w = { &out, rx, false, false, true };
    char err[256] = "";
    bool ok = walk_start(&w, ".", err, sizeof err);
    regfree(&rx);
    if (!ok || !out.len) {
        buf_free(&out);
        return ok ? strdup("(empty directory)") : NULL;
    }
    return out.data; /* the buf's heap block, NUL-terminated */
}

/* mkdir -p for the parent directory of a path */
bool bm_ensure_parent_dirs(const char *path) {
    char tmp[BM_PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return false;
    char *slash = strrchr(tmp, '/');
    if (!slash) return true; /* cwd */
    *slash = '\0';
    if (!tmp[0]) return true; /* "/file" root */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            BM_MKDIR(tmp);
            *p = '/';
        }
    }
    return BM_MKDIR(tmp) == 0 || errno == EEXIST;
}

/* ================= agent skills ================= */

/* a skill is a directory holding a SKILL.md (yaml frontmatter with at
   least name and description, then the body). two roots are scanned in
   order: .agents/skills under the current working directory, then under
   the home directory - the local one shadows the global one. */

static void tool_files(const cJSON *args, buf_t *out, bool *is_error,
                       bool content, bool recurse, bool hidden) {
    char *path = NULL;
    const char *regex = NULL;
    if (!(path = bm_need_path(args, "path", out)) ||
        !bm_need_str(args, "regex", &regex, out)) {
        free(path);
        *is_error = true;
        return;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        free(path);
        *is_error = true;
        return;
    }
    char err[192] = "";
    regex_t rx;
    if (!bm_rx_compile(&rx, regex, err, sizeof err)) {
        buf_clear(out);
        buf_appendf(out, "%s", err);
        *is_error = true;
        return;
    }
    walk_ctx_t w = { out, rx, content, recurse, hidden };
    char werr[256] = "";
    bool ok = walk_start(&w, path, werr, sizeof werr);
    regfree(&rx);
    free(path);
    if (!ok) {
        buf_clear(out);
        buf_appendf(out, "%s", werr);
        *is_error = true;
    }
}

void tool_files_list(const cJSON *a, buf_t *o, bool *e) {
    tool_files(a, o, e, false, rec_bool(a, "recurse_subdirectories", false),
               rec_bool(a, "show_hidden_files", false));
}

void tool_files_search(const cJSON *a, buf_t *o, bool *e) {
    tool_files(a, o, e, true, true, rec_bool(a, "show_hidden_files", false));
}

void tool_file_read(const cJSON *args, buf_t *out, bool *is_error) {
    char *path;
    long long off = 0, count = -1;
    if (!(path = bm_need_path(args, "path", out)) ||
        !bm_need_int(args, "lines_offset", &off, out) ||
        !bm_need_int(args, "lines_length", &count, out)) {
        free(path);
        *is_error = true;
        return;
    }
    if (off < 0 || count < 0) {
        buf_clear(out);
        buf_append_str(out, "lines_offset and lines_length must not be negative");
        free(path);
        *is_error = true;
        return;
    }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is not a regular file", path);
        free(path);
        *is_error = true;
        return;
    }
    buf_t b;
    buf_init(&b);
    int rc = bm_read_whole(path, &b, BM_FILE_CAP);
    if (rc == -2) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "file too large (over %llu bytes)", BM_FILE_CAP);
        free(path);
        *is_error = true;
        return;
    }
    if (rc == -1) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "cannot read '%s'", path);
        free(path);
        *is_error = true;
        return;
    }
    if (memchr(b.data, 0, b.len) || !utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "'%s' is not a textual file", path);
        free(path);
        *is_error = true;
        return;
    }
    /* split lines */
    size_t start = 0;
    unsigned long long lno = 0;
    bool first = true;
    for (size_t i = 0; i < b.len; i++) {
        if (b.data[i] == '\n') {
            size_t len = i - start;
            if (len && b.data[start + len - 1] == '\r') len--;
            if (lno >= (unsigned long long)off &&
                lno < (unsigned long long)off + (unsigned long long)count) {
                if (!first) buf_append_byte(out, '\n');
                buf_append(out, b.data + start, len);
                first = false;
            }
            lno++;
            start = i + 1;
        }
    }
    if (start < b.len) { /* final line without terminator */
        size_t len = b.len - start;
        if (len && b.data[start + len - 1] == '\r') len--;
        if (lno >= (unsigned long long)off &&
            lno < (unsigned long long)off + (unsigned long long)count) {
            if (!first) buf_append_byte(out, '\n');
            buf_append(out, b.data + start, len);
        }
        lno++;
    }
    buf_free(&b);
    if ((unsigned long long)off > lno) {
        buf_clear(out);
        buf_appendf(out, "lines_offset %lld beyond end of file (%llu lines)",
                    off, lno);
        free(path);
        *is_error = true;
        return;
    }
    free(path);
}

void tool_file_create(const cJSON *args, buf_t *out, bool *is_error) {
    char *path;
    const char *content = NULL;
    if (!(path = bm_need_path(args, "path", out)) ||
        !bm_need_str(args, "content", &content, out)) {
        *is_error = true;
        goto done;
    }
    bool overwrite = rec_bool(args, "overwrite", false);
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        goto done;
    }
    struct stat st;
    if (!stat(path, &st) && S_ISDIR(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is a directory", path);
        *is_error = true;
        goto done;
    }
    if (!bm_ensure_parent_dirs(path)) {
        buf_clear(out);
        buf_appendf(out, "cannot create parent directories of '%s'", path);
        *is_error = true;
        goto done;
    }
    /* write through an exclusive create when the caller said no
       overwrite: the open itself is the existence check, so two
       concurrent writers cannot both claim the path */
    if (!overwrite) {
        int fd = BM_OPEN_NEW(path);
        if (fd < 0) {
            buf_clear(out);
            if (errno == EEXIST)
                buf_appendf(out,
                            "'%s' already exists (pass overwrite=true)",
                            path);
            else
                buf_appendf(out, "cannot write '%s': %s", path,
                            strerror(errno));
            *is_error = true;
            goto done;
        }
        FILE *f = BM_FDOPEN(fd);
        if (!f) {
            int e = errno;
            BM_CLOSE(fd);
            buf_clear(out);
            buf_appendf(out, "cannot write '%s': %s", path, strerror(e));
            *is_error = true;
            goto done;
        }
        size_t n = strlen(content);
        bool ok = fwrite(content, 1, n, f) == n;
        ok = fclose(f) == 0 && ok;
        if (!ok) {
            remove(path); /* a failed create leaves nothing behind */
            buf_clear(out);
            buf_appendf(out, "write error on '%s'", path);
            *is_error = true;
            goto done;
        }
        buf_clear(out);
        buf_appendf(out, "wrote %lu bytes to %s", (unsigned long)n, path);
        goto done;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        buf_clear(out);
        buf_appendf(out, "cannot write '%s': %s", path, strerror(errno));
        *is_error = true;
        goto done;
    }
    size_t n = strlen(content);
    bool ok = fwrite(content, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(path);
        buf_clear(out);
        buf_appendf(out, "write error on '%s'", path);
        *is_error = true;
        goto done;
    }
    buf_clear(out);
    buf_appendf(out, "wrote %lu bytes to %s", (unsigned long)n, path);
done:
    free(path);
}

/* ---- file_delete ---- */

/* the tool is presented as a plain delete, but it moves the victim into
   the user's trash instead (freedesktop trash dirs on linux, ~/.Trash on
   macos, the recycle bin on windows) so a wrong call stays recoverable;
   when no trash exists (unreachable path, different filesystem) the file
   is irreversibly removed as before. directories move whole - no walk,
   no partial failures. */

/* "name" of "/a/b/name", "" of "/name", whole of "name"; never the
   leading slash, never past a trailing one; windows input is already
   slash-normalized, path_basename still takes both separators */
static const char *base_ptr(const char *path) {
    return path_basename(path);
}

#ifndef _WIN32 /* xdg trash is a freedesktop thing; windows recycles via shell */
/* is p a directory on the same filesystem as top */
static bool same_fs_mount(const char *top, const char *p) {
    struct stat a, b;
    if (BM_LSTAT(p, &b)) return false;         /* gone: let the move fail */
    if (BM_LSTAT(top, &a)) return false;
    return a.st_dev == b.st_dev;
}

/* does trash_dir/name already exist; unreadable trash counts as taken */
static bool trash_name_taken(const char *trash_dir, const char *name) {
    char p[BM_PATH_MAX];
    if (snprintf(p, sizeof p, "%s/%s", trash_dir, name) >= (int)sizeof p)
        return false;
    struct stat st;
    return BM_LSTAT(p, &st) == 0;
}

/* freedesktop trash spec: one trash per mount, at <mount>/.Trash/$UID
   (Sticky bit: only its owner may use it) or <mount>/.Trash-$UID;
   everything is a plain rename() away. returns the trash dir of the
   mount holding src, or NULL when none exists. the spec lets the
   implementation create a missing trash on the mount - the home one at
   $XDG_DATA_HOME/Trash (or ~/.local/share/Trash) and, on foreign
   mounts, .Trash-$UID - so a headless box still gets one. an existing
   .Trash admin dir without the sticky bit is never touched, and nothing
   is created when the mount is read-only (that caller falls back to a
   real delete). */
static const char *xdg_trash_layout(const char *src) {
    struct stat st;
    if (BM_LSTAT(src, &st)) return NULL;
    static char trash[BM_PATH_MAX];

    /* home mount: $XDG_DATA_HOME/Trash, else ~/.local/share/Trash */
    const char *home = getenv("HOME");
    if (home && *home) {
        const char *xdg = getenv("XDG_DATA_HOME");
        int w = xdg && xdg[0] == '/'
                    ? snprintf(trash, BM_PATH_MAX, "%s/Trash", xdg)
                    : snprintf(trash, BM_PATH_MAX, "%s/.local/share/Trash",
                               home);
        if (w >= (int)BM_PATH_MAX) return NULL;
        struct stat t;
        if (!BM_LSTAT(trash, &t) && S_ISDIR(t.st_mode) &&
            t.st_dev == st.st_dev)
            return trash;
        if (BM_LSTAT(trash, &t)) {
            /* absent: create it with the spec's subdirs, use it when it
               lands on the same filesystem */
            char sub[BM_PATH_MAX];
            if ((BM_MKDIR(trash) == 0 || errno == EEXIST) &&
                snprintf(sub, BM_PATH_MAX, "%s/files", trash) <
                    (int)BM_PATH_MAX &&
                (BM_MKDIR(sub) == 0 || errno == EEXIST)) {
                struct stat s2;
                if (!BM_LSTAT(trash, &s2) && S_ISDIR(s2.st_mode) &&
                    s2.st_dev == st.st_dev)
                    return trash;
            }
        }
    }

    /* other mounts: walk up from src to its mount root - the shallowest
       existing ancestor still on src's filesystem */
    char top[BM_PATH_MAX];
    if (snprintf(top, sizeof top, "%s", src) >= (int)sizeof top) return NULL;
    for (;;) {
        char *slash = strrchr(top, '/');
        if (!slash || slash == top) {
            strcpy(top, "/");
            break;
        }
        *slash = '\0';
        struct stat t;
        if (BM_LSTAT(top, &t) || t.st_dev != st.st_dev) {
            *slash = '/'; /* last good one */
            break;
        }
    }
    int w = snprintf(trash, sizeof trash, "%s/.Trash/%lu", top,
                     (unsigned long)geteuid());
    struct stat t;
    if (w < (int)sizeof trash && !BM_LSTAT(trash, &t) &&
        S_ISDIR(t.st_mode) && (t.st_mode & S_ISVTX))
        return trash;
    w = snprintf(trash, sizeof trash, "%s/.Trash-%lu", top,
                 (unsigned long)geteuid());
    if (w >= (int)sizeof trash) return NULL;
    if (!BM_LSTAT(trash, &t) && S_ISDIR(t.st_mode)) return trash;
    if (BM_LSTAT(trash, &t)) {
        /* absent: the spec lets us create ours on the mount */
        char sub[BM_PATH_MAX];
        if ((BM_MKDIR(trash) == 0 || errno == EEXIST) &&
            snprintf(sub, BM_PATH_MAX, "%s/files", trash) <
                (int)BM_PATH_MAX &&
            (BM_MKDIR(sub) == 0 || errno == EEXIST)) {
            struct stat s2;
            if (!BM_LSTAT(trash, &s2) && S_ISDIR(s2.st_mode)) return trash;
        }
    }
    return NULL;
}
#endif /* !_WIN32 */

#ifdef _WIN32
/* utf-8 in, utf-16 out */
static bool to_wide(const char *s, wchar_t *w, int cap) {
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) != 0;
}
#endif

/* move src into the user's trash. 0 moved, -1 no trash / move failed */
static int trash_move(const char *src, buf_t *into) {
#ifdef _WIN32
    /* recycle bin through the shell: FOF_ALLOWUNDO is what makes it a
       trash instead of a wipe; the double-null tail of pFrom required.
       the shell wants native separators: the tools normalized src to
       '/', they go back to '\' for this call alone */
    char native[BM_PATH_MAX];
    if (snprintf(native, sizeof native, "%s", src) >= (int)sizeof native)
        return -1;
    for (char *q = native; *q; q++)
        if (*q == '/') *q = '\\';
    wchar_t pathbuf[BM_PATH_MAX + 2];
    if (!to_wide(native, pathbuf, BM_PATH_MAX)) return -1;
    size_t n = wcslen(pathbuf);
    pathbuf[n + 1] = 0;
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = pathbuf;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI |
                FOF_SILENT;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        if (into) buf_append_str(into, "recycle bin");
        return 0;
    }
    return -1;
#else
#if defined(__APPLE__)
    const char *home = getenv("HOME");
    char trash_dir[BM_PATH_MAX], files_dir[BM_PATH_MAX];
    char *info_dir = NULL;
    bool have_trash = false;
    (void)src;
    if (home && *home &&
        snprintf(trash_dir, BM_PATH_MAX, "%s/.Trash", home) <
            (int)BM_PATH_MAX &&
        snprintf(files_dir, BM_PATH_MAX, "%s", trash_dir) <
            (int)BM_PATH_MAX) {
        struct stat st;
        if (stat(trash_dir, &st) == 0 && S_ISDIR(st.st_mode))
            have_trash = true;
    }
#else
    const char *t = xdg_trash_layout(src);
    char trash_dir[BM_PATH_MAX], files_dir[BM_PATH_MAX];
    char info_dir[BM_PATH_MAX];
    bool have_trash = false;
    (void)trash_dir; /* only the two subdirs below are used */
    if (t && snprintf(files_dir, BM_PATH_MAX, "%s/files", t) <
                (int)BM_PATH_MAX &&
        snprintf(info_dir, BM_PATH_MAX, "%s/info", t) < (int)BM_PATH_MAX) {
        /* the spec wants both subdirs present; created alongside the
           home trash by xdg_trash_layout, ensured here for a foreign
           admin trash that lost them */
        if (BM_MKDIR(files_dir) == 0 || errno == EEXIST) {
            BM_MKDIR(info_dir); /* failure only costs the .trashinfo */
            have_trash = true;
        }
    }
#endif
    if (!have_trash) return -1;
    if (!same_fs_mount(files_dir, src))
        return -1; /* cross-device rename would fail anyway */
    const char *base = base_ptr(src);
    char dst[BM_PATH_MAX];
    if (snprintf(dst, sizeof dst, "%s/%s", files_dir, base) >=
        (int)sizeof dst)
        return -1;
    if (trash_name_taken(files_dir, base)) {
        bool placed = false;
        for (int i = 2; i < 100 && !placed; i++) {
            char uniq[BM_PATH_MAX];
            if (snprintf(uniq, sizeof uniq, "%s/%s %d", files_dir, base, i) >=
                (int)sizeof uniq)
                break;
            struct stat st;
            if (BM_LSTAT(uniq, &st) == 0) continue;
            snprintf(dst, sizeof dst, "%s", uniq);
            placed = true;
        }
        if (!placed) return -1;
    }
    if (rename(src, dst) != 0) return -1;
#ifndef __APPLE__
    /* sidecar .trashinfo: original location + deletion date, so a
       trash-aware file manager can put it back */
    {
        const char *b2 = base_ptr(dst);
        char info_path[BM_PATH_MAX];
        if (snprintf(info_path, sizeof info_path, "%s/%s.trashinfo",
                     info_dir, b2) < (int)sizeof info_path) {
            FILE *f = fopen(info_path, "w");
            if (f) {
                time_t now = time(NULL);
                struct tm *tm = localtime(&now);
                char when[64] = "";
                if (tm)
                    snprintf(when, sizeof when, "%04d-%02d-%02dT%02d:%02d:%02d",
                             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                             tm->tm_hour, tm->tm_min, tm->tm_sec);
                fputs("[Trash Info]\nPath=", f);
                for (const char *p = src; *p; p++) {
                    /* RFC 2396 unreserved set, slash kept as separator */
                    if (isalnum((unsigned char)*p) || *p == '/' ||
                        *p == '-' || *p == '_' || *p == '.' || *p == '!' ||
                        *p == '~' || *p == '*' || *p == '\'' || *p == '(' ||
                        *p == ')')
                        fputc(*p, f);
                    else
                        fprintf(f, "%%%02X", (unsigned char)*p);
                }
                fprintf(f, "\nDeletionDate=%s\n", when);
                fclose(f);
            }
        }
    }
#endif
    if (into) buf_appendf(into, "%s", dst);
    return 0;
#endif
}

/* depth-first post-order removal: files unlink(3), directories recursed
   then rmdir(3); on failure returns the deepest path and errno context */
static void rm_tree(const char *path, buf_t *err, int depth) {
    if (depth > 64) {
        if (err) buf_appendf(err, "recursion limit at '%s'", path);
        return;
    }
    struct stat st;
    if (BM_LSTAT(path, &st)) { /* dangling symlink: still removable */
        if (remove(path) && err)
            buf_appendf(err, "'%s': %s", path, strerror(errno));
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (remove(path) && err)
            buf_appendf(err, "'%s': %s", path, strerror(errno));
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        if (err) buf_appendf(err, "'%s': %s", path, strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[BM_PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >=
            (int)sizeof child) {
            if (err) buf_appendf(err, "'%s/...' name too long", path);
            continue;
        }
        rm_tree(child, err, depth + 1);
    }
    closedir(d);
    if (BM_RMDIR(path) && err)
        buf_appendf(err, "'%s': %s", path, strerror(errno));
}

void tool_file_delete(const cJSON *args, buf_t *out, bool *is_error) {
    char *path;
    if (!(path = bm_need_path(args, "path", out))) {
        *is_error = true;
        return;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        free(path);
        return;
    }
    /* recoverable first: into the trash when one is reachable */
    buf_t where;
    buf_init(&where);
    if (trash_move(path, &where) == 0) {
        buf_clear(out);
        buf_appendf(out, "deleted %s", path);
        buf_free(&where);
        free(path);
        return;
    }
    buf_free(&where);
    buf_t err;
    buf_init(&err);
    rm_tree(path, &err, 0);
    if (err.data && err.data[0]) {
        buf_clear(out);
        buf_appendf(out, "delete failed: %s", err.data);
        *is_error = true;
    } else {
        buf_clear(out);
        buf_appendf(out, "deleted %s", path);
    }
    buf_free(&err);
    free(path);
}

/* ---- file_copy / file_move ---- */

/* one byte-true copy of a regular file: 0 on success, an errno value on
   failure (a failed stream call without errno becomes EIO).
   excl=true creates the destination exclusively: the open is the
   existence check, so two concurrent copies cannot both claim it */
static int copy_reg(const char *src, const char *dst, bool excl) {
    FILE *in = fopen(src, "rb");
    if (!in) return errno ? errno : EIO;
    FILE *out;
    if (excl) {
        int fd = BM_OPEN_NEW(dst);
        if (fd < 0) {
            int e = errno ? errno : EIO;
            fclose(in);
            return e;
        }
        out = BM_FDOPEN(fd);
        if (!out) {
            int e = errno ? errno : EIO;
            BM_CLOSE(fd);
            fclose(in);
            return e;
        }
    } else {
        out = fopen(dst, "wb");
        if (!out) {
            int e = errno ? errno : EIO;
            fclose(in);
            return e;
        }
    }
    char chunk[64 * 1024];
    size_t n;
    int e = 0;
    while (!e && (n = fread(chunk, 1, sizeof chunk, in)) > 0)
        if (fwrite(chunk, 1, n, out) != n) e = EIO;
    if (!e && ferror(in)) e = EIO;
    if (fclose(out) != 0 && !e) e = EIO;
    fclose(in);
    return e;
}

/* recursive copy: directories are recreated and descended, everything
   else is copied as a file. the first failure lands in err and stops
   the walk, leaving what was already written in place. 0 on success.
   excl: file destinations are created exclusively */
static int copy_tree(const char *src, const char *dst, buf_t *err,
                     int depth, bool excl) {
    if (depth > 64) {
        buf_appendf(err, "recursion limit at '%s'", src);
        return -1;
    }
    struct stat st;
    if (BM_LSTAT(src, &st)) {
        buf_appendf(err, "'%s': %s", src, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        buf_appendf(err, "'%s' is not a regular file or directory", src);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        int e = copy_reg(src, dst, excl);
        if (e) buf_appendf(err, "'%s': %s", dst, strerror(e));
        return e ? -1 : 0;
    }
    if (BM_MKDIR(dst) != 0 && errno != EEXIST) {
        buf_appendf(err, "'%s': %s", dst, strerror(errno));
        return -1;
    }
    DIR *d = opendir(src);
    if (!d) {
        buf_appendf(err, "'%s': %s", src, strerror(errno));
        return -1;
    }
    struct dirent *de;
    int rc = 0;
    while (!rc && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char csrc[BM_PATH_MAX], cdst[BM_PATH_MAX];
        if (snprintf(csrc, sizeof csrc, "%s/%s", src, de->d_name) >=
                (int)sizeof csrc ||
            snprintf(cdst, sizeof cdst, "%s/%s", dst, de->d_name) >=
                (int)sizeof cdst) {
            buf_appendf(err, "'%s/...' name too long", src);
            rc = -1;
            break;
        }
        rc = copy_tree(csrc, cdst, err, depth + 1, excl);
    }
    closedir(d);
    return rc;
}

/* shared body of file_copy and file_move: destpath names the target
   itself, or the directory the source lands inside (destpath/<source
   basename>) when destpath is an existing directory. an existing
   destination is an error unless overwrite=true, and a file never
   replaces a directory nor the other way round. a move is one rename(2)
   inside a filesystem; across two it is a full copy followed by an
   irreversible removal of the source. */
static void tool_file_copy_move(const cJSON *args, buf_t *out,
                                bool *is_error, bool move) {
    char *src = NULL, *dst = NULL;
    if (!(src = bm_need_path(args, "sourcepath", out)) ||
        !(dst = bm_need_path(args, "destpath", out))) {
        *is_error = true;
        goto done;
    }
    bool overwrite = rec_bool(args, "overwrite", false);
    if (!strlen(src) || !strlen(dst)) {
        buf_clear(out);
        buf_append_str(out, "sourcepath and destpath must not be empty");
        *is_error = true;
        goto done;
    }
    struct stat sst;
    if (stat(src, &sst)) {
        buf_clear(out);
        buf_appendf(out, "cannot access '%s': %s", src, strerror(errno));
        *is_error = true;
        goto done;
    }
    struct stat dst_st;
    bool into_dir = stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode);
    char target[BM_PATH_MAX];
    if (snprintf(target, sizeof target, "%s%s%s", dst, into_dir ? "/" : "",
                 into_dir ? base_ptr(src) : "") >= (int)sizeof target) {
        buf_clear(out);
        buf_appendf(out, "destpath too long");
        *is_error = true;
        goto done;
    }
    /* a directory copied into itself would recurse until the path cap */
    size_t sl = strlen(src);
    if (S_ISDIR(sst.st_mode) && strlen(target) > sl &&
        !strncmp(target, src, sl) && target[sl] == '/') {
        buf_clear(out);
        buf_appendf(out, "cannot %s a directory inside itself",
                    move ? "move" : "copy");
        *is_error = true;
        goto done;
    }
    struct stat tst;
    if (!BM_LSTAT(target, &tst)) {
        if (!overwrite) {
            buf_clear(out);
            buf_appendf(out, "'%s' already exists (pass overwrite=true)",
                        target);
            *is_error = true;
            goto done;
        }
        if (S_ISDIR(tst.st_mode) && !S_ISDIR(sst.st_mode)) {
            buf_clear(out);
            buf_appendf(out, "cannot replace directory '%s' with a file",
                        target);
            *is_error = true;
            goto done;
        }
        if (!S_ISDIR(tst.st_mode) && S_ISDIR(sst.st_mode)) {
            buf_clear(out);
            buf_appendf(out, "cannot replace file '%s' with a directory",
                        target);
            *is_error = true;
            goto done;
        }
    }
    if (move && BM_RENAME(src, target) == 0) {
        buf_clear(out);
        buf_appendf(out, "moved %s to %s", src, target);
        goto done;
    }
    if (move && errno != EXDEV) {
        buf_clear(out);
        buf_appendf(out, "move '%s' to '%s': %s", src, target,
                    strerror(errno));
        *is_error = true;
        goto done;
    }
    if (!bm_ensure_parent_dirs(target)) {
        buf_clear(out);
        buf_appendf(out, "cannot create the parent directories of '%s'",
                    target);
        *is_error = true;
        goto done;
    }
    buf_t err;
    buf_init(&err);
    if (copy_tree(src, target, &err, 0, !overwrite)) {
        buf_clear(out);
        buf_appendf(out, "copy '%s' to '%s' failed: %s", src, target,
                    err.data ? err.data : "unknown error");
        buf_free(&err);
        *is_error = true;
        goto done;
    }
    buf_free(&err);
    if (move) { /* cross-device: the copy is in, the source goes away */
        buf_t rmerr;
        buf_init(&rmerr);
        rm_tree(src, &rmerr, 0);
        if (rmerr.data && rmerr.data[0]) {
            buf_clear(out);
            buf_appendf(out, "copied to '%s' but removing '%s' failed: %s",
                        target, src, rmerr.data);
            buf_free(&rmerr);
            *is_error = true;
            goto done;
        }
        buf_free(&rmerr);
    }
    buf_clear(out);
    buf_appendf(out, "%s %s to %s", move ? "moved" : "copied", src, target);
done:
    free(src);
    free(dst);
}

void tool_file_copy(const cJSON *args, buf_t *out, bool *is_error) {
    tool_file_copy_move(args, out, is_error, false);
}

void tool_file_move(const cJSON *args, buf_t *out, bool *is_error) {
    tool_file_copy_move(args, out, is_error, true);
}


typedef struct fline {
    const char *s;
    size_t len; /* without terminator */
    const char *end;
    size_t endlen;
} fline_t;

/* split buffer into lines; a trailing '\n' terminates its line and does
   not open a phantom empty last line. the recorded terminator includes a
   preceding '\r' so crlf endings survive a rewrite */
static size_t split_lines(const char *buf, size_t len, fline_t **out) {
    size_t cap = 64, n = 0;
    fline_t *v = malloc(cap * sizeof *v);
    size_t start = 0;
    for (size_t i = 0; i < len;) {
        if (buf[i] == '\n') {
            if (n == cap) {
                cap *= 2;
                v = realloc(v, cap * sizeof *v);
            }
            v[n].s = buf + start;
            v[n].len = i - start;
            v[n].end = buf + i;
            v[n].endlen = 1;
            if (v[n].len && v[n].s[v[n].len - 1] == '\r') {
                v[n].len--;
                v[n].end--;
                v[n].endlen++;
            }
            n++;
            start = i + 1;
        }
        i++;
    }
    if (start < len) { /* final line without terminator */
        if (n == cap) {
            cap++;
            v = realloc(v, cap * sizeof *v);
        }
        v[n].s = buf + start;
        v[n].len = len - start;
        v[n].end = buf + len;
        v[n].endlen = 0;
        if (v[n].len && v[n].s[v[n].len - 1] == '\r') {
            v[n].len--;
            v[n].end--;
            v[n].endlen = 1;
        }
        n++;
    }
    *out = v;
    return n;
}

/* split a string argument into lines ('\n' separated) */
static size_t split_arg_lines(const char *s, fline_t **out) {
    return split_lines(s, strlen(s), out);
}

static bool line_eq_trim(const fline_t *a, const char *bs, size_t blen) {
    size_t al = bm_lead_ws(a->s, a->len);
    size_t at = bm_trail_ws(a->s + al, a->len - al);
    size_t a_len = a->len - al - at;
    size_t bl = bm_lead_ws(bs, blen);
    size_t bt = bm_trail_ws(bs + bl, blen - bl);
    size_t b_len = blen - bl - bt;
    return a_len == b_len && !memcmp(a->s + al, bs + bl, a_len);
}

/* one normalized byte of a line: a leading or trailing whitespace run
   vanishes, an internal run collapses to a single space */
typedef struct {
    const char *s;
    size_t n, i;
    bool emitted;
} norm_t;

static bool norm_next(norm_t *p, unsigned char *c) {
    while (p->i < p->n) {
        if (bm_is_ws(p->s[p->i])) {
            while (p->i < p->n && bm_is_ws(p->s[p->i])) p->i++;
            if (p->i >= p->n) return false; /* trailing run: dropped */
            if (!p->emitted) continue;      /* leading run: dropped */
            *c = ' ';
            return true;
        }
        *c = (unsigned char)p->s[p->i++];
        p->emitted = true;
        return true;
    }
    return false;
}

/* the relaxed comparison: how the whitespace inside a line is spelled out
   (a tab for spaces, two spaces for one) no longer matters */
static bool line_eq_ws(const char *as, size_t alen, const char *bs,
                       size_t blen) {
    norm_t a = { as, alen, 0, false }, b = { bs, blen, 0, false };
    for (;;) {
        unsigned char ca = 0, cb = 0;
        bool ha = norm_next(&a, &ca), hb = norm_next(&b, &cb);
        if (ha != hb) return false;
        if (!ha) return true;
        if (ca != cb) return false;
    }
}

/* a line holding nothing but whitespace */
static bool line_blank(const fline_t *l) {
    return bm_lead_ws(l->s, l->len) == l->len;
}

/* does the nol-line block starting at fl[idx] match the argument's lines? */
static bool block_match(const fline_t *fl, long long idx, size_t nfl,
                        const fline_t *ol, size_t nol, bool collapse) {
    if (idx < 0 || (size_t)idx + nol > nfl) return false;
    for (size_t k = 0; k < nol; k++) {
        const fline_t *a = &fl[(size_t)idx + k];
        if (collapse) {
            if (!line_eq_ws(a->s, a->len, ol[k].s, ol[k].len)) return false;
        } else if (!line_eq_trim(a, ol[k].s, ol[k].len)) {
            return false;
        }
    }
    return true;
}

/* start line of the first matching block: around line_number (1-based,
   +/-5 tolerance, nearest candidate first), or the whole file when
   line_number is 0 */
static long long block_find(const fline_t *fl, size_t nfl, long long line_number,
                            const fline_t *ol, size_t nol, bool collapse) {
    static const long long order[11] = { 0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5 };
    if (line_number > 0) {
        for (size_t ci = 0; ci < 11; ci++) {
            long long idx = line_number - 1 + order[ci];
            if (block_match(fl, idx, nfl, ol, nol, collapse)) return idx;
        }
        return -1;
    }
    for (size_t i = 0; i + nol <= nfl; i++)
        if (block_match(fl, (long long)i, nfl, ol, nol, collapse))
            return (long long)i;
    return -1;
}

void tool_file_edit(const cJSON *args, buf_t *out, bool *is_error) {
    char *path;
    const char *olds = NULL, *news = NULL;
    long long line_number = 0;
    if (!(path = bm_need_path(args, "path", out)) ||
        !bm_need_str(args, "oldString", &olds, out) ||
        !bm_need_str(args, "newString", &news, out) ||
        !bm_need_int(args, "line_number", &line_number, out)) {
        free(path);
        *is_error = true;
        return;
    }
    if (!strlen(path)) {
        buf_clear(out);
        buf_append_str(out, "path must not be empty");
        *is_error = true;
        goto done;
    }
    if (!strlen(olds)) {
        buf_clear(out);
        buf_append_str(out, "oldString must not be empty");
        *is_error = true;
        goto done;
    }
    if (line_number < 0) {
        buf_clear(out);
        buf_append_str(out, "line_number must not be negative");
        *is_error = true;
        goto done;
    }
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) {
        buf_clear(out);
        buf_appendf(out, "'%s' is not a regular file", path);
        *is_error = true;
        goto done;
    }
    buf_t b;
    buf_init(&b);
    int rc = bm_read_whole(path, &b, BM_FILE_CAP);
    if (rc != 0) {
        buf_free(&b);
        buf_clear(out);
        if (rc == -2)
            buf_appendf(out, "file too large (over %llu bytes)", BM_FILE_CAP);
        else
            buf_appendf(out, "cannot read '%s'", path);
        *is_error = true;
        goto done;
    }
    if (memchr(b.data, 0, b.len) || !utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        buf_clear(out);
        buf_appendf(out, "'%s' is not a textual file", path);
        *is_error = true;
        goto done;
    }
    fline_t *fl = NULL;
    size_t nfl = split_lines(b.data, b.len, &fl);
    fline_t *ol = NULL;
    size_t nol = split_arg_lines(olds, &ol);
    fline_t *nl = NULL;
    size_t nnl = split_arg_lines(news, &nl);

    /* matching passes, in order: the block as sent, the block as sent with
       internal whitespace runs collapsed, then each of those with the
       block's padded blank leading and trailing lines dropped. A literal
       match anywhere beats a relaxed one; inside a pass, the candidate
       closest to line_number wins. */
    size_t sk = 0, sn = nol;
    while (sn && line_blank(&ol[sk])) {
        sk++;
        sn--;
    }
    while (sn && line_blank(&ol[sk + sn - 1])) sn--;
    bool stripped = sn && (sk != 0 || sn != nol);
    size_t npass = stripped ? 4 : 2;

    long long found = -1;
    size_t mnl = nol;
    for (size_t pass = 0; pass < npass && found < 0; pass++) {
        bool use_stripped = pass >= 2;
        const fline_t *cl = use_stripped ? ol + sk : ol;
        size_t cn = use_stripped ? sn : nol;
        found = block_find(fl, nfl, line_number, cl, cn, pass % 2 == 1);
        if (found >= 0) mnl = cn;
    }
    if (found < 0) {
        free(fl);
        free(ol);
        free(nl);
        buf_free(&b);
        buf_clear(out);
        if (line_number > 0)
            buf_appendf(out,
                        "oldString not found at line %lld (tolerance 5) in %s",
                        line_number, path);
        else
            buf_appendf(out, "oldString not found in %s", path);
        *is_error = true;
        goto done;
    }

    /* indentation: base whitespace of the first replaced line, new block
       shifted so its first line lands on that column */
    buf_t basews;
    buf_init(&basews);
    size_t base_len = bm_lead_ws(fl[found].s, fl[found].len);
    buf_append(&basews, fl[found].s, base_len);
    size_t first_len = nnl ? bm_lead_ws(nl[0].s, nl[0].len) : 0;

    buf_t nb;
    buf_init(&nb);
    /* the replaced block runs to end of file when it consumed the last
       original line; then (and only then) may the last new line go
       without a terminator */
    bool block_at_eof = (size_t)found + mnl == nfl;
    for (size_t i = 0; i < nfl; i++) {
        if (i == (size_t)found) {
            for (size_t j = 0; j < nnl; j++) {
                bool blank = nl[j].len == 0 ||
                             bm_lead_ws(nl[j].s, nl[j].len) == nl[j].len;
                if (!blank) {
                    size_t lj = bm_lead_ws(nl[j].s, nl[j].len);
                    if (lj >= first_len) {
                        buf_append(&nb, basews.data, basews.len);
                        for (size_t k = 0; k < lj - first_len; k++)
                            buf_append_byte(&nb, ' ');
                    } else {
                        long long t = (long long)basews.len +
                                      (long long)lj - (long long)first_len;
                        if (t <= 0) {
                            /* dedented past the margin: no indent */
                        } else if ((size_t)t <= basews.len) {
                            buf_append(&nb, basews.data, (size_t)t);
                        } else {
                            buf_append(&nb, basews.data, basews.len);
                        }
                    }
                    buf_append(&nb, nl[j].s + lj, nl[j].len - lj);
                }
                /* terminator: inherit the corresponding original line's,
                   lines past the replaced block inherit its last line's;
                   every new line except a final one needs one */
                const fline_t *src = (j < mnl) ? &fl[found + j]
                                               : &fl[found + mnl - 1];
                if (block_at_eof && j + 1 == nnl)
                    buf_append(&nb, src->end, src->endlen);
                else if (src->endlen == 0)
                    buf_append_byte(&nb, '\n');
                else
                    buf_append(&nb, src->end, src->endlen);
            }
            i += mnl - 1; /* skip the replaced originals */
            continue;
        }
        buf_append(&nb, fl[i].s, fl[i].len);
        buf_append(&nb, fl[i].end, fl[i].endlen);
    }

    /* write to a sibling temp file, then rename over the target: a
       crash, a full disk or a second ctrl-c between truncate and write
       must never leave a torn or empty file behind */
    char tmp[BM_PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        free(fl);
        free(ol);
        free(nl);
        buf_free(&b);
        buf_free(&nb);
        buf_free(&basews);
        buf_clear(out);
        buf_appendf(out, "cannot write '%s': path too long", path);
        *is_error = true;
        goto done;
    }
    FILE *f = fopen(tmp, "wb");
    if (f) {
        size_t w = fwrite(nb.data, 1, nb.len, f);
        if (fclose(f) != 0 || w != nb.len) {
            remove(tmp);
            f = NULL;
        } else if (BM_CHMOD(tmp, st.st_mode & 07777) != 0) {
            /* the mode rides along: an edited script stays executable */
            remove(tmp);
            f = NULL;
        }
    }
    if (!f || BM_RENAME(tmp, path) != 0) {
        int e = errno; /* captured before the remove below clobbers it */
        if (f) remove(tmp);
        free(fl);
        free(ol);
        free(nl);
        buf_free(&b);
        buf_free(&nb);
        buf_free(&basews);
        buf_clear(out);
        buf_appendf(out, "cannot write '%s': %s", path, strerror(e));
        *is_error = true;
        goto done;
    }
    buf_clear(out);
    if (mnl == 1)
        buf_appendf(out, "replaced line %llu in %s",
                    (unsigned long long)(found + 1), path);
    else
        buf_appendf(out, "replaced lines %llu-%llu in %s",
                    (unsigned long long)(found + 1),
                    (unsigned long long)(found + mnl), path);
    free(fl);
    free(ol);
    free(nl);
    buf_free(&b);
    buf_free(&nb);
    buf_free(&basews);
done:
    free(path);
}

/* ---- file_analyze (structure analyzers, ported from flower) ---- */

/* one analyzer per file type, registered in the ANALYZERS table below:
   given a whole text file, its shape comes back as one line per element
   (line number, an indent per nesting level, the element - a full
   signature for a function) instead of the file's whole content. Adding
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

