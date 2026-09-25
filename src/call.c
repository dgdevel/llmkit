/* call.c - llmkit call: argv->record compiler + plain-text sink (design sec.11).
   One prompt in, one answer out; the conversation itself is the runner's
   engine, unchanged. The parser owns CLI shape only - value problems stay
   record validation, the requirements' two error tiers. */
#include "llmkit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= parsed command line ================= */

void call_cfg_free(call_cfg_t *c) {
    free(c->api_base);
    free(c->key);
    free(c->model);
    free(c->system);
    free(c->prompt);
    for (size_t i = 0; i < c->nhdrs; i++) {
        free(c->hdr_names[i]);
        free(c->hdr_values[i]);
    }
    free(c->hdr_names);
    free(c->hdr_values);
    for (size_t i = 0; i < c->nproxies; i++) free(c->proxies[i]);
    free(c->proxies);
    for (size_t i = 0; i < c->nterminals; i++) free(c->terminals[i]);
    free(c->terminals);
    memset(c, 0, sizeof *c);
}

/* argv[0] of the flag range, not of the process */
static int usage_err(char *err, size_t errsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static int usage_err(char *err, size_t errsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return 1;
}

static int cfg_hdr_add(call_cfg_t *c, const char *name, const char *value) {
    for (size_t i = 0; i < c->nhdrs; i++) {
        if (!strcmp(c->hdr_names[i], name)) { /* later replaces earlier */
            char *nv = strdup(value);
            if (!nv) return -1;
            free(c->hdr_values[i]);
            c->hdr_values[i] = nv;
            return 0;
        }
    }
    char **n = realloc(c->hdr_names, (c->nhdrs + 1) * sizeof *n);
    char **v = realloc(c->hdr_values, (c->nhdrs + 1) * sizeof *v);
    /* a failed realloc leaves the old block valid: keep whichever grew,
       both arrays stay owned and consistent, the caller frees them */
    if (n) c->hdr_names = n;
    if (v) c->hdr_values = v;
    char *nn = !n ? NULL : strdup(name);
    char *nv = !v ? NULL : strdup(value);
    if (!nn || !nv) {
        free(nn);
        free(nv);
        return -1;
    }
    c->hdr_names[c->nhdrs] = nn;
    c->hdr_values[c->nhdrs] = nv;
    c->nhdrs++;
    return 0;
}

static int cfg_proxy_add(call_cfg_t *c, const char *path) {
    char *d = strdup(path);
    if (!d) return -1;
    char **p = realloc(c->proxies, (c->nproxies + 1) * sizeof *p);
    if (!p) {
        free(d);
        return -1;
    }
    c->proxies = p;
    c->proxies[c->nproxies++] = d;
    return 0;
}

/* server name: basename minus last extension, any extension; NULL when the
   result would be empty (design sec.11) */
static char *proxy_name(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) n = (size_t)(dot - base);
    if (n == 0) return NULL;
    return strndup(base, n);
}

int call_parse_ex(int argc, char **argv, call_cfg_t *c, char *err,
                  size_t errsz, bool with_prompt) {
    memset(c, 0, sizeof *c);
    c->protocol = -1;
    c->max_tokens = -1;
    bool have_base = false, have_key = false, have_model = false,
         have_system = false, have_prompt = false, have_mt = false;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--anthropic") || !strcmp(a, "--openai") ||
            !strcmp(a, "--openai-responses")) {
            if (c->protocol != -1) {
                usage_err(err, errsz, "protocol flag given twice");
                goto fail;
            }
            c->protocol = !strcmp(a, "--anthropic")     ? PROTO_ANTHROPIC
                          : !strcmp(a, "--openai")      ? PROTO_OPENAI
                                                         : PROTO_RESPONSES;
        } else if (!strcmp(a, "--key") || !strcmp(a, "--model") ||
                   !strcmp(a, "--max-tokens") ||
                   !strcmp(a, "--system-prompt") ||
                   (with_prompt && !strcmp(a, "--prompt")) ||
                   !strcmp(a, "--header") || !strcmp(a, "--mcp-proxy") ||
                   !strcmp(a, "--terminal-tool")) {
            if (i + 1 >= argc) {
                usage_err(err, errsz, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            if (!strcmp(a, "--key")) {
                if (have_key) {
                    usage_err(err, errsz, "--key given twice");
                    goto fail;
                }
                have_key = true;
                c->key = strdup(v);
            } else if (!strcmp(a, "--model")) {
                if (have_model) {
                    usage_err(err, errsz, "--model given twice");
                    goto fail;
                }
                have_model = true;
                c->model = strdup(v);
            } else if (!strcmp(a, "--max-tokens")) {
                if (have_mt) {
                    usage_err(err, errsz, "--max-tokens given twice");
                    goto fail;
                }
                char *end = NULL;
                errno = 0;
                long n = strtol(v, &end, 10);
                if (errno || !v[0] || !end || *end || n <= 0) {
                    usage_err(err, errsz,
                              "--max-tokens wants a positive integer");
                    goto fail;
                }
                have_mt = true;
                c->max_tokens = n;
            } else if (!strcmp(a, "--system-prompt")) {
                if (have_system) {
                    usage_err(err, errsz, "--system-prompt given twice");
                    goto fail;
                }
                have_system = true;
                c->system = strdup(v);
            } else if (with_prompt && !strcmp(a, "--prompt")) {
                if (have_prompt) {
                    usage_err(err, errsz, "--prompt given twice");
                    goto fail;
                }
                have_prompt = true;
                c->prompt = strdup(v);
            } else if (!strcmp(a, "--header")) {
                const char *eq = strchr(v, '=');
                if (!eq || eq == v || !eq[1]) {
                    usage_err(err, errsz, "--header wants <name>=<value>");
                    goto fail;
                }
                char *name = strndup(v, (size_t)(eq - v));
                if (!name) {
                    usage_err(err, errsz, "out of memory");
                    goto fail;
                }
                int rc = cfg_hdr_add(c, name, eq + 1);
                free(name);
                if (rc) {
                    usage_err(err, errsz, "out of memory");
                    goto fail;
                }
            } else if (!strcmp(a, "--terminal-tool")) {
                char *d = strdup(v);
                char **p =
                    d ? realloc(c->terminals,
                                (c->nterminals + 1) * sizeof *p) : NULL;
                if (!d || !p) {
                    free(d);
                    usage_err(err, errsz, "out of memory");
                    goto fail;
                }
                c->terminals = p;
                c->terminals[c->nterminals++] = d;
            } else { /* --mcp-proxy */
                char *nm = proxy_name(v);
                if (!nm) {
                    usage_err(err, errsz,
                              "--mcp-proxy config path '%s' yields an empty "
                              "server name", v);
                    goto fail;
                }
                for (size_t j = 0; j < c->nproxies; j++) {
                    char *other = proxy_name(c->proxies[j]);
                    bool dup = other && !strcmp(other, nm);
                    free(other);
                    if (dup) {
                        usage_err(err, errsz,
                                  "--mcp-proxy server name '%s' used twice",
                                  nm);
                        free(nm);
                        goto fail;
                    }
                }
                free(nm);
                if (cfg_proxy_add(c, v)) {
                    usage_err(err, errsz, "out of memory");
                    goto fail;
                }
            }
        } else if (a[0] == '-' && a[1] == '-') {
            usage_err(err, errsz, "unknown flag '%s'", a);
            goto fail;
        } else {
            if (have_base) {
                usage_err(err, errsz, "unexpected extra argument '%s'", a);
                goto fail;
            }
            have_base = true;
            c->api_base = strdup(a);
        }
    }
    if (c->protocol == -1) {
        usage_err(err, errsz, "missing protocol flag "
                              "(--anthropic, --openai or --openai-responses)");
        goto fail;
    }
    if (!have_base) {
        usage_err(err, errsz, "missing <api_base>");
        goto fail;
    }
    if (with_prompt && !have_prompt) {
        usage_err(err, errsz, "missing --prompt");
        goto fail;
    }
    /* --terminal-tool: the server prefix must name a --mcp-proxy server
       (design sec.11: which entry to mark is CLI shape); whether that
       server lists the tool stays record validation */
    for (size_t i = 0; i < c->nterminals; i++) {
        const char *v = c->terminals[i];
        const char *dot = strchr(v, '.');
        if (!dot || dot == v || !dot[1]) {
            usage_err(err, errsz,
                      "--terminal-tool wants <server.tool>, got '%s'", v);
            goto fail;
        }
        bool match = false;
        for (size_t j = 0; j < c->nproxies && !match; j++) {
            char *nm = proxy_name(c->proxies[j]);
            size_t nl = nm ? strlen(nm) : 0;
            match = nm && nl == (size_t)(dot - v) &&
                    !strncmp(v, nm, nl);
            free(nm);
        }
        if (!match) {
            usage_err(err, errsz,
                      "--terminal-tool server '%.*s' is no --mcp-proxy "
                      "server",
                      (int)(dot - v), v);
            goto fail;
        }
    }
    return 0;
fail:
    call_cfg_free(c);
    return 1;
}

int call_parse(int argc, char **argv, call_cfg_t *c, char *err, size_t errsz) {
    return call_parse_ex(argc, argv, c, err, errsz, true);
}

/* ================= record builders ================= */

void call_shell_quote(buf_t *b, const char *s) {
    buf_append_byte(b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_append_str(b, "'\\''");
        else buf_append_byte(b, *p);
    }
    buf_append_byte(b, '\'');
}

static cJSON *text_record(const char *type, const char *text) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", type);
    cJSON *content = cJSON_AddArrayToObject(t, "content");
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", text);
    cJSON_AddItemToArray(content, blk);
    return t;
}

cJSON *call_build_llm(const call_cfg_t *c) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "llm");
    cJSON_AddStringToObject(
        t, "endpoint_protocol",
        c->protocol == PROTO_ANTHROPIC ? "anthropic"
        : c->protocol == PROTO_RESPONSES ? "openai_responses"
                                         : "openai");
    cJSON_AddStringToObject(t, "api_base", c->api_base);
    if (c->key) cJSON_AddStringToObject(t, "api_key", c->key);
    if (c->model) cJSON_AddStringToObject(t, "model", c->model);
    if (c->max_tokens > 0) {
        cJSON *io = cJSON_AddObjectToObject(t, "inference_options");
        cJSON_AddNumberToObject(io, "max_tokens", (double)c->max_tokens);
    }
    if (c->nhdrs) {
        cJSON *h = cJSON_AddObjectToObject(t, "headers");
        for (size_t i = 0; i < c->nhdrs; i++)
            cJSON_AddStringToObject(h, c->hdr_names[i], c->hdr_values[i]);
    }
    return t;
}

cJSON *call_build_system(const call_cfg_t *c) {
    if (!c->system) return NULL;
    return text_record("system", c->system);
}

/* --terminal-tool fold helper: does <server.tool> belong to this server?
   The exact-dot boundary keeps server 'fs' from matching 'fs2.tool'. */
static bool terminal_of_server(const char *tv, const char *srv_name) {
    size_t nl = strlen(srv_name);
    return strncmp(tv, srv_name, nl) == 0 && tv[nl] == '.' && tv[nl + 1];
}

cJSON *call_build_tools(const call_cfg_t *c, const char *exe_path) {
    if (!c->nproxies) return NULL;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "tools");
    cJSON *arr = cJSON_AddArrayToObject(t, "tools");
    for (size_t i = 0; i < c->nproxies; i++) {
        buf_t cl;
        buf_init(&cl);
        call_shell_quote(&cl, exe_path);
        buf_append_str(&cl, " mcp-proxy ");
        call_shell_quote(&cl, c->proxies[i]);
        cJSON *srv = cJSON_CreateObject();
        cJSON_AddStringToObject(srv, "type", "stdio");
        char *nm = proxy_name(c->proxies[i]);
        cJSON_AddStringToObject(srv, "name", nm);
        cJSON_AddStringToObject(srv, "command_line", cl.data);
        buf_free(&cl);
        /* fold this server's --terminal-tool entries, argv order */
        cJSON *tt = NULL;
        for (size_t k = 0; nm && k < c->nterminals; k++) {
            const char *tv = c->terminals[k];
            if (!terminal_of_server(tv, nm)) continue;
            if (!tt) tt = cJSON_AddArrayToObject(srv, "terminal_tools");
            cJSON_AddItemToArray(tt,
                                 cJSON_CreateString(tv + strlen(nm) + 1));
        }
        free(nm);
        cJSON_AddItemToArray(arr, srv);
    }
    return t;
}

cJSON *call_build_user(const char *prompt) {
    return text_record("user", prompt);
}

/* ================= sink and run ================= */

typedef struct call_sink {
    FILE *out, *err;
    bool wrote;    /* any text byte written */
    bool last_nl;  /* last written byte was \n */
    bool io_fail;  /* stdout write failed (design sec.12: exit 1) */
    /* tool_response snapshot: the terminal ending's answer is the record
       whose id matches engine terminal_id (design sec.11) */
    char **tresp_ids, **tresp_texts;
    size_t ntresp, captresp;
} call_sink_t;

static void call_sink_write(call_sink_t *s, const char *t) {
    if (t && *t) {
        size_t n = strlen(t);
        if (fwrite(t, 1, n, s->out) != n || fflush(s->out) != 0)
            s->io_fail = true;
        s->wrote = true;
        s->last_nl = t[n - 1] == '\n';
    }
}

static void call_sink_tresp_push(call_sink_t *s, cJSON *rec) {
    if (s->ntresp == s->captresp) {
        s->captresp = s->captresp ? s->captresp * 2 : 8;
        s->tresp_ids = realloc(s->tresp_ids, s->captresp * sizeof(char *));
        s->tresp_texts = realloc(s->tresp_texts, s->captresp * sizeof(char *));
    }
    const char *id = rec_str(rec, "id");
    const char *tx = rec_str(rec, "text");
    s->tresp_ids[s->ntresp] = strdup(id ? id : "");
    s->tresp_texts[s->ntresp] = strdup(tx ? tx : "");
    s->ntresp++;
}

static void call_sink_fn(void *ctx, cJSON *rec) {
    call_sink_t *s = ctx;
    int k = rec_classify(rec);
    if (k == R_RESPONSE) {
        call_sink_write(s, rec_str(rec, "text"));
    } else if (k == R_TOOL_RESPONSE) {
        call_sink_tresp_push(s, rec);
    } else if (k == R_ERROR) {
        const char *code = rec_str(rec, "code");
        const char *msg = rec_str(rec, "message");
        fprintf(s->err, "llmkit call: %s: %s\n", code ? code : "error",
                msg ? msg : "");
        fflush(s->err);
    }
    cJSON_Delete(rec);
}

static void sink_error(call_sink_t *s, const char *code, const char *msg) {
    call_sink_fn(s, rec_error(code, msg, true));
}

/* every argv string is UTF-8 checked at compile: the runner gets the same
   guarantee from the jsonl byte pipeline, argv is this command's channel */
static int utf8_check_str(const char *s) {
    return !s || utf8_valid((const uint8_t *)s, strlen(s));
}

static int compile_fail(engine_t *e, char *msg /* malloc'd or NULL */,
                        const char *stat) {
    engine_emit_record(e, rec_error(EC_INVALID_RECORD, msg ? msg : stat, true));
    free(msg);
    return EXIT_INVALID_RECORD;
}

int call_compile(const call_cfg_t *c, engine_t *e, const char *exe_path) {
    if (!utf8_check_str(c->api_base) || !utf8_check_str(c->key) ||
        !utf8_check_str(c->model) || !utf8_check_str(c->system)) {
        engine_emit_record(e, rec_error(EC_INVALID_RECORD,
                                        "invalid UTF-8 in a flag value",
                                        true));
        return EXIT_INVALID_RECORD;
    }
    for (size_t i = 0; i < c->nhdrs; i++)
        if (!utf8_check_str(c->hdr_names[i]) ||
            !utf8_check_str(c->hdr_values[i])) {
            engine_emit_record(e, rec_error(EC_INVALID_RECORD,
                                            "invalid UTF-8 in a --header "
                                            "value",
                                            true));
            return EXIT_INVALID_RECORD;
        }

    cJSON *llm = call_build_llm(c);
    int rc = 0;
    char *m = validate_llm(llm);
    if (m) {
        rc = compile_fail(e, m, "invalid llm record");
        cJSON_Delete(llm);
        return rc;
    }
    engine_apply_config_record(e, llm);
    cJSON_Delete(llm);

    if (c->system) {
        cJSON *sys = call_build_system(c);
        m = validate_content(cJSON_GetObjectItemCaseSensitive(sys, "content"));
        if (m) {
            rc = compile_fail(e, m, "invalid system record");
            cJSON_Delete(sys);
            return rc;
        }
        engine_apply_config_record(e, sys);
        cJSON_Delete(sys);
    }

    if (c->nproxies) {
        cJSON *tools = call_build_tools(c, exe_path);
        m = validate_tools(tools);
        if (m) {
            rc = compile_fail(e, m, "invalid tools record");
            cJSON_Delete(tools);
            return rc;
        }
        engine_apply_config_record(e, tools);
        cJSON_Delete(tools);
    }
    return 0;
}

int call_run(const call_cfg_t *c, FILE *out, FILE *errf, const char *exe_path,
             wire_t *(*factory)(engine_t *)) {
    call_sink_t s;
    memset(&s, 0, sizeof s);
    s.out = out;
    s.err = errf;
    engine_t *e = engine_new(call_sink_fn, &s);
    if (factory) e->wire_factory = factory;
    int rc = 0;

    rc = call_compile(c, e, exe_path);
    if (rc) goto done;

    if (!utf8_check_str(c->prompt)) {
        sink_error(&s, EC_INVALID_RECORD, "invalid UTF-8 in a flag value");
        rc = EXIT_INVALID_RECORD;
        goto done;
    }

    {
        cJSON *user = call_build_user(c->prompt);
        char *m = validate_content(
            cJSON_GetObjectItemCaseSensitive(user, "content"));
        if (m) {
            sink_error(&s, EC_INVALID_RECORD, m);
            free(m);
            cJSON_Delete(user);
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        tlist_ingest(&e->tr, user);
        cJSON_Delete(user);
    }

    rc = engine_start(e); /* validates llm + user presence, connects servers */
    if (rc == 0) rc = engine_run(e);

    /* terminal ending: the answer is the terminal tool's tool_response
       text, not a final response (there is none) */
    if (rc == EXIT_TERMINAL_TOOL && e->terminal_id) {
        for (size_t i = 0; i < s.ntresp; i++) {
            if (!strcmp(s.tresp_ids[i], e->terminal_id)) {
                call_sink_write(&s, s.tresp_texts[i]);
                break;
            }
        }
    }

done:
    /* the answer ends with a newline; an empty answer writes nothing */
    if ((rc == 0 || rc == EXIT_TERMINAL_TOOL) && s.wrote && !s.last_nl &&
        !s.io_fail) {
        if (fputc('\n', out) == EOF || fflush(out) != 0) s.io_fail = true;
    }
    if (s.io_fail) rc = EXIT_OUT_OF_CHANNEL;
    for (size_t i = 0; i < s.ntresp; i++) {
        free(s.tresp_ids[i]);
        free(s.tresp_texts[i]);
    }
    free(s.tresp_ids);
    free(s.tresp_texts);
    engine_free(e);
    return rc;
}

/* ================= command ================= */

/* 0 ok; 1 out-of-channel (exit 1); 2 record tier (exit 2) */
static int read_stdin_prompt(char **out, char *err, size_t errsz) {
    buf_t b;
    buf_init(&b);
    char tmp[8192];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, tmp, sizeof tmp);
        if (n < 0) {
            if (errno == EINTR) continue;
            buf_free(&b);
            snprintf(err, errsz, "stdin read failed");
            return 1;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if ((unsigned char)tmp[i] == 0) {
                buf_free(&b);
                snprintf(err, errsz, "NUL byte in prompt");
                return 2;
            }
            if (tmp[i] == '\r') continue; /* sec.3 byte hygiene, minus lines */
            buf_append_byte(&b, tmp[i]);
        }
    }
    if (b.len >= 3 && !memcmp(b.data, "\xef\xbb\xbf", 3)) {
        buf_free(&b);
        snprintf(err, errsz, "leading BOM in prompt");
        return 2;
    }
    if (!utf8_valid((const uint8_t *)b.data, b.len)) {
        buf_free(&b);
        snprintf(err, errsz, "invalid UTF-8 in prompt");
        return 2;
    }
    *out = buf_steal(&b, NULL);
    return 0;
}

static void self_exe(char *out, size_t sz, const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n > 0 && (size_t)n < sz - 1) {
        out[n] = '\0';
        return;
    }
    snprintf(out, sz, "%s", argv0 ? argv0 : "llmkit");
}

static void call_usage(FILE *out) {
    fputs("usage: llmkit call (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "                [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "                [--system-prompt <text>] "
          "[--header <name=value>]...\n"
          "                [--mcp-proxy <config>]... "
          "[--terminal-tool <name.tool>]...\n"
          "                --prompt <text|->\n",
          out);
}

int cmd_call(int argc, char **argv) {
    call_cfg_t c;
    char err[256] = "";
    if (call_parse(argc - 2, argv + 2, &c, err, sizeof err) != 0) {
        fprintf(stderr, "llmkit call: %s\n", err);
        call_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    if (!strcmp(c.prompt, "-")) {
        char *p = NULL;
        int rc = read_stdin_prompt(&p, err, sizeof err);
        if (rc == 2) {
            fprintf(stderr, "llmkit call: invalid_record: %s\n", err);
            call_cfg_free(&c);
            return EXIT_INVALID_RECORD;
        }
        if (rc == 1) {
            fprintf(stderr, "llmkit call: %s\n", err);
            call_cfg_free(&c);
            return EXIT_OUT_OF_CHANNEL;
        }
        free(c.prompt);
        c.prompt = p;
    }
    char exe[4096];
    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
    int rc = call_run(&c, stdout, stderr, exe, NULL);
    call_cfg_free(&c);
    return rc;
}
