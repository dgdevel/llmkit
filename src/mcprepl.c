/* mcprepl.c - llmkit mcp-repl: an interactive tool console for one mcp
   server (design sec.16). No llm and no engine turns: one server connects
   through the mcp client (a one-entry tools record through mcp_reconcile),
   its tools/list becomes the console's vocabulary - one record per tool
   (the call signature with argument names, the tool's description, one
   detail line per described argument), tab completion over the names -
   and every submitted line is one tools/call: name(json, json, ...),
   positional arguments bound onto the tool's inputSchema. One timing
   line follows every call. */
#include "llmkit.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SRV_NAME "server" /* the one server's name in the tools record */

/* ================= output helpers ================= */

typedef struct mr_out {
    FILE *f;
    bool io_fail; /* a write failed: out-of-channel exit 1 */
    bool wrote;   /* any byte written */
    bool last_nl; /* last written byte was \n */
} mr_out_t;

static void mrwr(mr_out_t *o, const char *t, size_t n) {
    if (!n) return;
    if (fwrite(t, 1, n, o->f) != n || fflush(o->f) != 0) o->io_fail = true;
    o->wrote = true;
    o->last_nl = t[n - 1] == '\n';
}

static void mrwr_str(mr_out_t *o, const char *t) { mrwr(o, t, strlen(t)); }

static void mr_ensure_nl(mr_out_t *o) {
    if (o->wrote && !o->last_nl) mrwr_str(o, "\n");
}

static void mr_error(mr_out_t *o, const char *code, const char *msg) {
    mr_ensure_nl(o);
    mrwr_str(o, "! ");
    mrwr_str(o, code ? code : "error");
    mrwr_str(o, ": ");
    mrwr_str(o, msg ? msg : "");
    mrwr_str(o, "\n");
}

/* the engine's error records (connect failures above all) as lines */
static void mr_engine_sink(void *ctx, cJSON *rec) {
    mr_out_t *o = ctx;
    if (rec_classify(rec) == R_ERROR)
        mr_error(o, rec_str(rec, "code"), rec_str(rec, "message"));
    cJSON_Delete(rec);
}

/* ================= command line ================= */

void mcp_repl_cfg_free(mcp_repl_cfg_t *c) {
    free(c->target);
    free(c->protocol);
    for (size_t i = 0; i < c->nhdrs; i++) free(c->hdrs[i]);
    free(c->hdrs);
    memset(c, 0, sizeof *c);
}

static int usage_err(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int usage_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return 1;
}

int mcp_repl_parse(int argc, char **argv, mcp_repl_cfg_t *c, char *err,
                   size_t errsz) {
    memset(c, 0, sizeof *c);
    c->type = -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--stdio") || !strcmp(a, "--http") ||
            !strcmp(a, "--sse")) {
            if (c->type != -1) {
                usage_err(err, errsz, "transport flag given twice");
                goto fail;
            }
            if (i + 1 >= argc) {
                usage_err(err, errsz, "missing value for %s", a);
                goto fail;
            }
            c->type = !strcmp(a, "--stdio") ? MCP_STDIO
                      : !strcmp(a, "--http") ? MCP_HTTP
                                             : MCP_SSE;
            c->target = strdup(argv[++i]);
            if (!c->target) {
                usage_err(err, errsz, "out of memory");
                goto fail;
            }
        } else if (!strcmp(a, "--protocol")) {
            if (c->protocol) {
                usage_err(err, errsz, "--protocol given twice");
                goto fail;
            }
            if (i + 1 >= argc) {
                usage_err(err, errsz, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            if (!mcp_protocol_supported(v)) {
                usage_err(err, errsz,
                          "unsupported protocol revision '%s'", v);
                goto fail;
            }
            c->protocol = strdup(v);
            if (!c->protocol) {
                usage_err(err, errsz, "out of memory");
                goto fail;
            }
        } else if (!strcmp(a, "--header")) {
            if (i + 1 >= argc) {
                usage_err(err, errsz, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            const char *eq = strchr(v, '=');
            if (!eq || eq == v || !eq[1]) {
                usage_err(err, errsz, "--header wants <name>=<value>");
                goto fail;
            }
            char **h = realloc(c->hdrs, (c->nhdrs + 1) * sizeof *h);
            char *d = h ? strdup(v) : NULL;
            if (!h || !d) {
                free(d);
                usage_err(err, errsz, "out of memory");
                goto fail;
            }
            c->hdrs = h;
            c->hdrs[c->nhdrs++] = d;
        } else if (a[0] == '-' && a[1] == '-') {
            usage_err(err, errsz, "unknown flag '%s'", a);
            goto fail;
        } else {
            usage_err(err, errsz, "unexpected extra argument '%s'", a);
            goto fail;
        }
    }
    if (c->type == -1) {
        usage_err(err, errsz,
                  "missing transport flag (--stdio, --http or --sse)");
        goto fail;
    }
    return 0;
fail:
    mcp_repl_cfg_free(c);
    return 1;
}

/* ================= tools record ================= */

cJSON *mcp_repl_build_tools(const mcp_repl_cfg_t *c) {
    cJSON *srv = cJSON_CreateObject();
    cJSON_AddStringToObject(srv, "type",
                            c->type == MCP_STDIO    ? "stdio"
                            : c->type == MCP_HTTP   ? "http"
                                                    : "sse");
    cJSON_AddStringToObject(srv, "name", SRV_NAME);
    cJSON_AddStringToObject(srv, c->type == MCP_STDIO ? "command_line"
                                                      : "url",
                            c->target);
    if (c->protocol) cJSON_AddStringToObject(srv, "protocol", c->protocol);
    if (c->nhdrs) {
        cJSON *h = cJSON_AddObjectToObject(srv, "headers");
        for (size_t i = 0; i < c->nhdrs; i++) {
            const char *eq = strchr(c->hdrs[i], '=');
            char *nm = strndup(c->hdrs[i], (size_t)(eq - c->hdrs[i]));
            if (nm) {
                cJSON_AddStringToObject(h, nm, eq + 1);
                free(nm);
            }
        }
    }
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "tools");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(t, "tools"), srv);
    return t;
}

/* ================= listing: tool records ================= */

/* the schema's json type as the console writes it: json has one number
   type, a float parameter takes an integer literal unchanged */
static const char *sig_type(const cJSON *prop) {
    const char *t = rec_str(prop, "type");
    if (!t) return "any";
    if (!strcmp(t, "number")) return "float";
    if (!strcmp(t, "integer")) return "int";
    if (!strcmp(t, "boolean")) return "bool";
    if (!strcmp(t, "string")) return "string";
    if (!strcmp(t, "array")) return "array";
    if (!strcmp(t, "object")) return "object";
    return t; /* unknown type words pass through verbatim */
}

/* "type name": one schema property as the record's argument word */
static void sig_arg(buf_t *out, const cJSON *p) {
    buf_append_str(out, sig_type(p));
    if (p->string) {
        buf_append_byte(out, ' ');
        buf_append_str(out, p->string);
    }
}

/* one tool as a record: the header line is the call signature with the
   argument names plus the tool's description, then one detail line per
   described argument (the header already named the rest) */
void mcp_repl_record(const cJSON *tool, buf_t *out) {
    const cJSON *sch =
        cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    const cJSON *props = sch
        ? cJSON_GetObjectItemCaseSensitive(sch, "properties") : NULL;
    const char *nm = rec_str(tool, "name");
    buf_append_str(out, nm ? nm : "?");
    buf_append_byte(out, '(');
    bool first = true;
    if (cJSON_IsObject(props))
        for (const cJSON *p = props->child; p; p = p->next) {
            if (!first) buf_append_str(out, ", ");
            first = false;
            sig_arg(out, p);
        }
    buf_append_byte(out, ')');
    const char *desc = rec_str(tool, "description");
    if (desc && *desc) {
        buf_append_str(out, ": ");
        buf_append_str(out, desc);
    }
    buf_append_byte(out, '\n');
    if (cJSON_IsObject(props))
        for (const cJSON *p = props->child; p; p = p->next) {
            const char *d = rec_str(p, "description");
            if (!d || !*d) continue;
            buf_append_str(out, "- ");
            sig_arg(out, p);
            buf_append_str(out, ": ");
            buf_append_str(out, d);
            buf_append_byte(out, '\n');
        }
}

static const cJSON *tool_find(const cJSON *tools, const char *name) {
    for (const cJSON *t = tools ? tools->child : NULL; t; t = t->next) {
        const char *nm = rec_str(t, "name");
        if (nm && !strcmp(nm, name)) return t;
    }
    return NULL;
}

static void list_tools(mr_out_t *o, const cJSON *tools) {
    mrwr_str(o, "Tools available:\n");
    buf_t rec;
    buf_init(&rec);
    for (const cJSON *t = tools ? tools->child : NULL; t; t = t->next) {
        buf_clear(&rec);
        mcp_repl_record(t, &rec);
        mrwr(o, rec.data ? rec.data : "", rec.len);
    }
    buf_free(&rec);
}

/* ================= call syntax ================= */

static char *syn_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *syn_err(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return strdup(msg);
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* one comma-separated literal, accumulated in lit: trim the spaces, parse
   it, append to vals. argpos is the 1-based position for messages. */
static char *push_literal(buf_t *lit, cJSON *vals, size_t argpos) {
    size_t b = 0, e = lit->len;
    while (b < e && lit->data[b] == ' ') b++;
    while (e > b && lit->data[e - 1] == ' ') e--;
    if (b == e)
        return syn_err("argument %lu is empty",
                       (unsigned long)argpos);
    char save = lit->data[e];
    lit->data[e] = '\0';
    cJSON *v = cJSON_Parse(lit->data + b);
    lit->data[e] = save;
    if (!v)
        return syn_err("argument %lu is not a json literal",
                       (unsigned long)argpos);
    cJSON_AddItemToArray(vals, v);
    return NULL;
}

/* copy a "..." literal starting at *pp (at the opening quote), escapes
   included; *pp lands past the closing quote */
static char *scan_string(const char **pp, buf_t *lit, size_t argpos) {
    const char *p = *pp;
    buf_append_byte(lit, *p++);
    for (;;) {
        char sc = *p;
        if (sc == '\0' || sc == '\n')
            return syn_err("unterminated string in argument %lu",
                           (unsigned long)argpos);
        buf_append_byte(lit, *p++);
        if (sc == '\\') {
            if (*p == '\0' || *p == '\n')
                return syn_err("unterminated string in argument %lu",
                               (unsigned long)argpos);
            buf_append_byte(lit, *p++);
        } else if (sc == '"') {
            *pp = p;
            return NULL;
        }
    }
}

/* copy a {...} / [...] literal starting at *pp (at the opening bracket),
   nested containers and strings included */
static char *scan_container(const char **pp, buf_t *lit, size_t argpos) {
    const char *p = *pp;
    int depth = 0;
    do {
        char ic = *p;
        if (ic == '\0' || ic == '\n')
            return syn_err("unbalanced brackets in argument %lu",
                           (unsigned long)argpos);
        if (ic == '"') {
            char *e = scan_string(&p, lit, argpos);
            if (e) return e;
            continue;
        }
        buf_append_byte(lit, *p++);
        if (ic == '{' || ic == '[') depth++;
        else if (ic == '}' || ic == ']') depth--;
    } while (depth > 0);
    *pp = p;
    return NULL;
}

/* the inside of the parens: one json literal per iteration, copied up to
   the top-level ',' or ')' and parsed by push_literal. *pp advances past
   the closing ')'. */
static char *split_args(const char **pp, cJSON *vals) {
    const char *p = *pp;
    buf_t lit;
    buf_init(&lit);
    char *err = NULL;
    for (;;) {
        for (;;) { /* one literal: bytes up to the top-level , or ) */
            char ch = *p;
            if (ch == '\0' || ch == '\n') {
                err = syn_err("missing ')'");
                goto out;
            }
            if (ch == ',' || ch == ')') break;
            if (ch == '"')
                err = scan_string(&p, &lit,
                                  (size_t)cJSON_GetArraySize(vals) + 1);
            else if (ch == '{' || ch == '[')
                err = scan_container(&p, &lit,
                                     (size_t)cJSON_GetArraySize(vals) + 1);
            else
                buf_append_byte(&lit, *p++);
            if (err) goto out;
        }
        err = push_literal(&lit, vals,
                           (size_t)cJSON_GetArraySize(vals) + 1);
        if (err) goto out;
        buf_clear(&lit);
        if (*p == ')') {
            p++;
            break;
        }
        p++; /* past ',' */
    }
out:
    buf_free(&lit);
    if (err) return err;
    *pp = p;
    return NULL;
}

/* "name(json, json, ...)" -> name (malloc'd) + the positional literals
   (an array, declaration order). A bare name is a zero-argument call.
   Returns NULL or a malloc'd message. */
char *mcp_repl_split(const char *line, char **name_out, cJSON **vals_out) {
    *name_out = NULL;
    *vals_out = NULL;
    const char *p = line;
    while (*p == ' ') p++;
    const char *n0 = p;
    while (name_char(*p)) p++;
    size_t nl = (size_t)(p - n0);
    if (!nl) return syn_err("expected a tool call: name(json, ...)");
    while (*p == ' ') p++;
    cJSON *vals = cJSON_CreateArray();
    if (!*p) { /* bare name: zero arguments */
        *name_out = strndup(n0, nl);
        *vals_out = vals;
        return NULL;
    }
    if (*p != '(') {
        cJSON_Delete(vals);
        return syn_err("expected '(' after the tool name");
    }
    p++;
    {
        /* the empty argument list: name() */
        const char *q = p;
        while (*q == ' ') q++;
        if (*q == ')') p = q + 1;
        else {
            char *err = split_args(&p, vals);
            if (err) {
                cJSON_Delete(vals);
                return err;
            }
        }
    }
    while (*p == ' ') p++;
    if (*p && *p != '\n') {
        cJSON_Delete(vals);
        return syn_err("unexpected text after ')'");
    }
    *name_out = strndup(n0, nl);
    *vals_out = vals;
    return NULL;
}

/* the positional literals onto the tool's schema properties, declaration
   order; required-but-absent and too-many are errors. Integer literals
   bind to float parameters unchanged: json has one number type. */
char *mcp_repl_bind(const cJSON *tool, const cJSON *vals, cJSON **args_out) {
    *args_out = NULL;
    const cJSON *sch =
        cJSON_GetObjectItemCaseSensitive(tool, "inputSchema");
    const cJSON *props = sch
        ? cJSON_GetObjectItemCaseSensitive(sch, "properties") : NULL;
    size_t nprops = cJSON_IsObject(props) ? cJSON_GetArraySize(props) : 0;
    size_t nvals = cJSON_IsArray(vals) ? cJSON_GetArraySize(vals) : 0;
    if (nvals > nprops) {
        return syn_err("%lu arguments, the tool takes %lu",
                       (unsigned long)nvals, (unsigned long)nprops);
    }
    cJSON *args = cJSON_CreateObject();
    const cJSON *v = vals ? vals->child : NULL;
    if (cJSON_IsObject(props))
        for (const cJSON *p = props->child; p && v; p = p->next, v = v->next)
            cJSON_AddItemToObject(args, p->string,
                                  cJSON_Duplicate((cJSON *)v, 1));
    /* required check: a required property past the given count is a miss */
    const cJSON *req = sch
        ? cJSON_GetObjectItemCaseSensitive(sch, "required") : NULL;
    if (cJSON_IsArray(req))
        for (const cJSON *r = req->child; r; r = r->next) {
            if (!cJSON_IsString(r)) continue;
            if (!cJSON_GetObjectItemCaseSensitive(args, r->valuestring)) {
                char *m = syn_err("missing required argument '%s'",
                                  r->valuestring);
                cJSON_Delete(args);
                return m;
            }
        }
    *args_out = args;
    return NULL;
}

/* ================= results and timing ================= */

static void render_result(mr_out_t *o, const cJSON *result) {
    bool is_err = rec_bool(result, "isError", false);
    if (is_err) mrwr_str(o, "! tool error: ");
    const cJSON *content =
        cJSON_GetObjectItemCaseSensitive(result, "content");
    bool any_text = false;
    long nontext = 0;
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const char *ty = rec_str(b, "type");
            if (ty && !strcmp(ty, "text")) {
                const char *tx = rec_str(b, "text");
                if (any_text) mrwr_str(o, "\n");
                mrwr_str(o, tx ? tx : "");
                any_text = true;
            } else {
                nontext++;
            }
        }
    if (!any_text && !is_err) {
        /* no text to show: the structured payload, when there is one */
        const cJSON *sc =
            cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
        if (sc) {
            buf_t b;
            buf_init(&b);
            buf_append_tree(&b, sc);
            mrwr(o, b.data ? b.data : "", b.len);
            mrwr_str(o, "\n");
            buf_free(&b);
        }
    }
    mr_ensure_nl(o);
    if (nontext) {
        char note[64];
        snprintf(note, sizeof note, "[%ld non-text content block%s]\n",
                 nontext, nontext == 1 ? "" : "s");
        mrwr_str(o, note);
    }
}

/* [HH:MM:SS] <seconds>: one stamped timing line per call, repl's stamp */
static void render_timing(mr_out_t *o, double seconds) {
    char ts[32] = "", line[64];
    stamp_now(ts, sizeof ts);
    int n = snprintf(line, sizeof line, "%s%.3fs\n", ts, seconds);
    if (n <= 0) return;
    mrwr(o, line, (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

/* ================= session ================= */

static void help_text(mr_out_t *o) {
    mrwr_str(o,
             "lines:\n"
             "  name(json, ...)   call a tool, arguments are json literals\n"
             "  tools             list the tools again\n"
             "  help              this text\n"
             "  quit              end the session (Ctrl-D as well)\n");
}

/* tab completion over the tool names: the trailing name token extends to
   the unique match (plus '('), to the longest common prefix of several,
   or lists the candidates below the line */
static void complete_tool(editor_t *ed, void *ctx) {
    const cJSON *tools = ((mcp_server_t *)ctx)->tools;
    const char *line = ed->line.data ? ed->line.data : "";
    if (strchr(line, '(')) return; /* argument editing: no candidates */
    size_t end = ed->line.len, start = end;
    while (start > 0 && name_char(line[start - 1])) start--;
    size_t tlen = end - start;
    const char *tok = line + start;

    size_t nmatch = 0;
    const char **names = NULL;
    if (tools)
        for (const cJSON *t = tools->child; t; t = t->next) {
            const char *nm = rec_str(t, "name");
            if (!nm || strncmp(nm, tok, tlen)) continue;
            const char **g = realloc(names, (nmatch + 1) * sizeof *g);
            if (!g) {
                free(names);
                return;
            }
            names = g;
            names[nmatch++] = nm;
        }
    if (!nmatch) {
        free(names);
        return;
    }
    if (nmatch == 1) {
        buf_t nl;
        buf_init(&nl);
        buf_append(&nl, line, start);
        buf_append_str(&nl, names[0]);
        buf_append_byte(&nl, '(');
        editor_set_line(ed, nl.data ? nl.data : "", nl.len);
        buf_free(&nl);
        free(names);
        return;
    }
    /* longest common prefix of the matches beyond the typed token */
    size_t lcp = tlen;
    for (;;) {
        char c = names[0][lcp];
        if (!c) break;
        bool all = true;
        for (size_t i = 1; i < nmatch && all; i++)
            if (names[i][lcp] != c) all = false;
        if (!all) break;
        lcp++;
    }
    if (lcp > tlen) {
        buf_t nl;
        buf_init(&nl);
        buf_append(&nl, line, start);
        buf_append(&nl, names[0], lcp);
        editor_set_line(ed, nl.data ? nl.data : "", nl.len);
        buf_free(&nl);
    } else {
        buf_t cand;
        buf_init(&cand);
        for (size_t i = 0; i < nmatch; i++) {
            if (i) buf_append_str(&cand, "  ");
            buf_append_str(&cand, names[i]);
        }
        editor_note(ed, cand.data ? cand.data : "");
        buf_free(&cand);
    }
    free(names);
}

static int run_call(mcp_server_t *srv, mr_out_t *o, const char *line) {
    char *name = NULL;
    cJSON *vals = NULL, *args = NULL, *result = NULL;
    int rc = 0;
    char *err = mcp_repl_split(line, &name, &vals);
    if (err) {
        mr_error(o, "syntax", err);
        goto out;
    }
    const cJSON *tool = tool_find(srv->tools, name);
    if (!tool) {
        char msg[300];
        snprintf(msg, sizeof msg, "unknown tool '%s' - 'tools' lists them",
                 name);
        mr_error(o, "syntax", msg);
        goto out;
    }
    err = mcp_repl_bind(tool, vals, &args);
    if (err) {
        mr_error(o, "syntax", err);
        goto out;
    }
    {
        char cerr[512] = "";
        double t0 = mono_now();
        rc = mcp_call_raw(srv, name, args, &result, cerr, sizeof cerr, -1);
        double dt = mono_now() - t0;
        if (rc)
            mr_error(o, EC_TOOL_FAILED, cerr);
        else
            render_result(o, result);
        render_timing(o, dt);
    }
out:
    free(err);
    free(name);
    cJSON_Delete(vals);
    cJSON_Delete(args);
    cJSON_Delete(result);
    return rc;
}

/* hygiene of a submitted line: strict UTF-8 (CR and NUL never arrive) */
static bool line_valid(const buf_t *b) {
    return utf8_valid((const uint8_t *)(b->data ? b->data : ""), b->len);
}

int mcp_repl_run(const mcp_repl_cfg_t *c, int in_fd, FILE *out) {
    mr_out_t mo;
    memset(&mo, 0, sizeof mo);
    mo.f = out;
    int rc = EXIT_OK;
    /* session state, initialized before the first goto done: the early
       connect failures unwind through the same cleanup */
    bool tty = isatty(in_fd);
    buf_t line;
    buf_init(&line);
    editor_t ed;
    memset(&ed, 0, sizeof ed);
    plain_reader_t pr;
    memset(&pr, 0, sizeof pr);

    engine_t *e = engine_new(mr_engine_sink, &mo);
    cJSON *trec = mcp_repl_build_tools(c);
    {
        /* the builder is in-tree; validation is still run: it owns the
           record's rules, not the caller */
        char *verr = validate_tools(trec);
        if (verr) {
            mr_error(&mo, EC_INVALID_RECORD, verr);
            free(verr);
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
    }
    rc = mcp_reconcile((mcp_mgr_t *)e->mcp, e, trec);
    cJSON_Delete(trec);
    if (rc) goto done; /* the sink rendered the error record */
    mcp_server_t *srv = mcp_find((mcp_mgr_t *)e->mcp, SRV_NAME);
    if (!srv || !srv->connected) {
        rc = EXIT_CONNECT_FAILED;
        goto done;
    }
    list_tools(&mo, srv->tools);

    if (tty) {
        editor_init(&ed, out, in_fd, "> ");
        ed.on_tab = complete_tool; /* names from the server's listing */
        ed.tab_ctx = srv;
    } else {
        plain_init(&pr, in_fd);
    }

    for (;;) { /* calls never end the session: EOF and quit exit 0 */
        int r;
        if (g_stop_flag) { /* SIGINT racing the read start: empty line */
            g_stop_flag = 0;
            r = ED_QUIT;
            goto handled;
        }
        if (tty) {
            mrwr_str(&mo, "> ");
            r = editor_line(&ed);
            mrwr_str(&mo, "\n");
        } else {
            r = plain_line(&pr, &line);
        }
    handled:
        if (r == ED_QUIT) {
            rc = EXIT_INTERRUPTED;
            goto done;
        }
        if (r == ED_EOF) {
            goto done; /* exit 0 */
        }
        if (r == ED_BAD) {
            mr_error(&mo, EC_INVALID_RECORD, "NUL byte in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        if (r == ED_CLEAR) continue; /* discarded input, fresh prompt */
        /* ED_SUBMIT: the editor owns its buffer, the loop works on line */
        if (tty) {
            buf_clear(&line);
            if (ed.line.len)
                buf_append(&line, ed.line.data, ed.line.len);
        }
        if (!line.len) continue; /* empty input: ignored */
        if (!line_valid(&line)) {
            mr_error(&mo, EC_INVALID_RECORD, "invalid UTF-8 in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        if (tty)
            editor_hist_push(&ed, line.data);
        else {
            /* no editor echoed it: the line renders as its own block */
            mr_ensure_nl(&mo);
            mrwr_str(&mo, line.data);
            mrwr_str(&mo, "\n");
        }
        if (!tool_find(srv->tools, line.data)) {
            /* builtin words, unless a tool shadows the name */
            if (!strcmp(line.data, "quit") || !strcmp(line.data, "exit"))
                goto done; /* session end, the last ending's code */
            if (!strcmp(line.data, "tools")) {
                list_tools(&mo, srv->tools);
                continue;
            }
            if (!strcmp(line.data, "help") || !strcmp(line.data, "?")) {
                help_text(&mo);
                continue;
            }
        }
        run_call(srv, &mo, line.data); /* errors render, the session lives */
    }
done:
    buf_free(&line);
    if (tty) editor_free(&ed);
    else plain_free(&pr);
    engine_free(e); /* the stdio child dies here */
    if (mo.io_fail) rc = EXIT_OUT_OF_CHANNEL;
    return rc;
}

/* ================= command ================= */

static void mcp_repl_usage(FILE *out) {
    fputs("usage: llmkit mcp-repl (--stdio <command> | --http <url> | "
          "--sse <url>)\n"
          "                     [--protocol <revision>] "
          "[--header <name=value>]...\n",
          out);
}

int cmd_mcp_repl(int argc, char **argv) {
    signals_init(); /* SIGINT is an input control: the stage rule at the
                       prompt, honored at the next prompt in a call */
    mcp_repl_cfg_t c;
    char err[256] = "";
    if (mcp_repl_parse(argc - 2, argv + 2, &c, err, sizeof err) != 0) {
        fprintf(stderr, "llmkit mcp-repl: %s\n", err);
        mcp_repl_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    int rc = mcp_repl_run(&c, STDIN_FILENO, stdout);
    mcp_repl_cfg_free(&c);
    return rc;
}
