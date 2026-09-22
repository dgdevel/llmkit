/* proxy.c — llmkit mcp-proxy (design §10): config pipeline, expose/hide
   resolution, schema rewrite, stdio server loop. */
#include "llmkit.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void proxy_fatal(proxy_state_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("llmkit mcp-proxy: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    p->ok = false;
}

/* free one exposed entry and everything it owns */
static void exposed_entry_free(exposed_tool_t *e) {
    cJSON_Delete(e->presentation);
    free(e->upstream_name);
    if (e->upstream_srv != (char *)-1) free(e->upstream_srv);
    for (size_t m = 0; m < e->nmap; m++) {
        free(e->from[m]);
        free(e->to[m]);
    }
    free(e->from);
    free(e->to);
    memset(e, 0, sizeof *e);
}

/* ---- config loading ---- */

bool proxy_config_line(proxy_state_t *p, char *line) {
    if (!p->ok) {
        free(line);
        return false;
    }
    cJSON *t = jsonl_parse_line(line);
    free(line);
    if (!t) {
        proxy_fatal(p, "malformed json line in config");
        return p->ok;
    }
    int k = rec_classify(t);
    bool first = p->first;
    p->first = false;
    switch (k) {
    case R_HEADER:
        if (first) {
            double v = rec_num(t, "version", -1);
            if (v != 1) proxy_fatal(p, "unsupported header version in config");
        }
        cJSON_Delete(t);
        return p->ok;
    case R_TOOLS: {
        char *m = validate_tools(t);
        if (m) {
            proxy_fatal(p, "invalid tools record: %s", m);
            free(m);
            cJSON_Delete(t);
            return p->ok;
        }
        if (p->tools_record) {
            proxy_fatal(p, "a second tools record is invalid in a config");
            cJSON_Delete(t);
            return p->ok;
        }
        p->tools_record = t;
        return p->ok;
    }
    case R_EXPOSE:
    case R_HIDE: {
        if (!rec_str(t, "tool")) {
            proxy_fatal(p, "%s record missing tool selector",
                        k == R_EXPOSE ? "expose" : "hide");
        }
        /* keep for the resolution phase */
        if (p->nex == p->capex) {
            p->capex = p->capex ? p->capex * 2 : 8;
            p->ex = realloc(p->ex, p->capex * sizeof *p->ex);
        }
        exposed_tool_t *e = &p->ex[p->nex++];
        memset(e, 0, sizeof *e);
        e->presentation = t; /* tree owned by the entry for now */
        e->upstream_srv = (char *)-1; /* marker: config record */
        (void)e;
        return p->ok;
    }
    default:
        proxy_fatal(p, "record type '%s' is invalid in a config",
                    rec_str(t, "type") ? rec_str(t, "type") : "(none)");
        cJSON_Delete(t);
        return p->ok;
    }
}

/* split "server.tool" */
static bool split_selector(const char *sel, char *srv, size_t srvsz, char *tool,
                           size_t toolsz) {
    const char *dot = strchr(sel, '.');
    if (!dot || dot == sel || !dot[1]) return false;
    size_t sl = (size_t)(dot - sel);
    if (sl >= srvsz || strlen(dot + 1) >= toolsz) return false;
    memcpy(srv, sel, sl);
    srv[sl] = '\0';
    snprintf(tool, toolsz, "%s", dot + 1);
    return true;
}

/* rename a property in place, keeping its position (cJSON linked list) */
static bool rename_property(cJSON *props, const char *from, const char *to) {
    for (cJSON *it = props->child; it; it = it->next) {
        if (it->string && !strcmp(it->string, from)) {
            char *ns = strdup(to);
            if (!ns) return false;
            cJSON_free(it->string);
            it->string = ns;
            return true;
        }
    }
    return false;
}

static bool prop_names_unique(cJSON *props) {
    for (cJSON *a = props->child; a; a = a->next)
        for (cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    return true;
}

/* apply the arguments renames/description overrides to a schema copy */
static bool rewrite_schema(cJSON *schema, const cJSON *argcfg,
                           char *err, size_t errsz) {
    if (!argcfg) return true;
    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (!cJSON_IsObject(props)) return true;
    for (const cJSON *a = argcfg->child; a; a = a->next) {
        const char *upstream_name = a->string;
        cJSON *item = NULL;
        for (cJSON *it = props->child; it; it = it->next)
            if (it->string && !strcmp(it->string, upstream_name)) item = it;
        if (!item) continue; /* not a top level property: identity default */
        const char *newname = rec_str(a, "name");
        if (newname && strcmp(newname, upstream_name)) {
            if (!rename_property(props, upstream_name, newname)) continue;
            /* keep required state */
            cJSON *req = cJSON_GetObjectItemCaseSensitive(schema, "required");
            if (cJSON_IsArray(req))
                for (cJSON *r = req->child; r; r = r->next)
                    if (cJSON_IsString(r) && r->valuestring &&
                        !strcmp(r->valuestring, upstream_name)) {
                        cJSON_free(r->valuestring);
                        r->valuestring = strdup(newname);
                    }
        }
        const char *newdesc = rec_str(a, "description");
        if (newdesc && cJSON_IsObject(item)) {
            cJSON *d = cJSON_GetObjectItemCaseSensitive(item, "description");
            if (d) cJSON_SetValuestring(d, newdesc);
            else cJSON_AddStringToObject(item, "description", newdesc);
        }
    }
    if (!prop_names_unique(props)) {
        snprintf(err, errsz, "renamed arguments are not unique");
        return false;
    }
    return true;
}

int proxy_resolve_and_build(proxy_state_t *p) {
    if (!p->tools_record) {
        proxy_fatal(p, "config requires exactly one tools record");
        return -1;
    }
    /* connect upstreams; non-required failures are skipped silently */
    engine_t *nuleng = NULL;
    (void)nuleng;
    engine_t tmpeng;
    memset(&tmpeng, 0, sizeof tmpeng);
    tmpeng.emit = NULL; /* no record channel */
    if (mcp_reconcile(p->mgr, &tmpeng, p->tools_record)) {
        proxy_fatal(p, "required upstream server failed to connect");
        return -1;
    }

    /* mode check: mixing expose and hide is fatal */
    bool have_expose = false, have_hide = false;
    for (size_t i = 0; i < p->nex; i++) {
        cJSON *t = p->ex[i].presentation;
        int k = rec_classify(t);
        if (k == R_EXPOSE) have_expose = true;
        if (k == R_HIDE) have_hide = true;
    }
    if (have_expose && have_hide) {
        proxy_fatal(p, "mixing expose and hide records is invalid");
        return -1;
    }

    /* resolve selectors against connected listings */
    char err[512];
    for (size_t i = 0; i < p->nex; i++) {
        cJSON *t = p->ex[i].presentation;
        const char *sel = rec_str(t, "tool");
        char srv[256], tool[256];
        if (!split_selector(sel, srv, sizeof srv, tool, sizeof tool)) {
            proxy_fatal(p, "invalid tool selector '%s'", sel);
            return -1;
        }
        const cJSON *list =
            cJSON_GetObjectItemCaseSensitive(p->tools_record, "tools");
        bool in_config = false;
        for (const cJSON *s = list ? list->child : NULL; s; s = s->next)
            if (!strcmp(rec_str(s, "name"), srv)) in_config = true;
        if (!in_config) {
            proxy_fatal(p, "expose/hide names server '%s' absent from the "
                           "tools record",
                        srv);
            return -1;
        }
        mcp_server_t *server = mcp_find(p->mgr, srv);
        if (!server || !server->connected) {
            /* dropped with the failed server */
            continue;
        }
        cJSON *upstream = NULL;
        for (const cJSON *tt = server->tools->child; tt; tt = tt->next)
            if (!strcmp(rec_str(tt, "name"), tool)) {
                upstream = (cJSON *)tt;
                break;
            }
        if (!upstream) {
            proxy_fatal(p, "server '%s' does not list tool '%s'", srv, tool);
            return -1;
        }
        p->ex[i].srv = server;
        p->ex[i].upstream_name = strdup(tool);
        p->ex[i].upstream_srv = strdup(srv);
        (void)err;
    }

    /* uniqueness: each upstream tool by at most one record; exposed names
       unique */
    for (size_t i = 0; i < p->nex; i++) {
        if (!p->ex[i].srv) continue;
        for (size_t j = i + 1; j < p->nex; j++) {
            if (!p->ex[j].srv) continue;
            if (p->ex[i].srv == p->ex[j].srv &&
                !strcmp(p->ex[i].upstream_name, p->ex[j].upstream_name)) {
                proxy_fatal(p, "upstream tool '%s.%s' is named by more than "
                               "one expose/hide record",
                            p->ex[i].upstream_srv, p->ex[i].upstream_name);
                return -1;
            }
        }
    }

    /* build the exposed list (whitelist order or server/listing order) */
    exposed_tool_t *out = NULL;
    size_t nout = 0, capout = 0;

    if (have_expose) {
        for (size_t i = 0; i < p->nex; i++) {
            if (!p->ex[i].srv) continue;
            if (nout == capout) {
                capout = capout ? capout * 2 : 8;
                out = realloc(out, capout * sizeof *out);
            }
            out[nout++] = p->ex[i];
            /* ownership moved to out: detach so the replace loop below
               does not free what out now owns */
            memset(&p->ex[i], 0, sizeof p->ex[i]);
        }
    } else {
        const tool_listing_t *tl = mcp_listing(p->mgr);
        for (size_t i = 0; i < tl->n; i++) {
            bool hidden = false;
            for (size_t j = 0; j < p->nex; j++)
                if (p->ex[j].srv == tl->v[i].srv &&
                    !strcmp(p->ex[j].upstream_name,
                            rec_str(tl->v[i].tool, "name")))
                    hidden = true;
            if (hidden) continue;
            if (nout == capout) {
                capout = capout ? capout * 2 : 8;
                out = realloc(out, capout * sizeof *out);
            }
            exposed_tool_t *e = &out[nout++];
            memset(e, 0, sizeof *e);
            e->srv = tl->v[i].srv;
            e->upstream_name = strdup(rec_str(tl->v[i].tool, "name"));
            e->upstream_srv = strdup(tl->v[i].srv->name);
        }
    }

    /* rewrite presentations */
    for (size_t i = 0; i < nout; i++) {
        exposed_tool_t *e = &out[i];
        cJSON *upstream = NULL;
        for (const cJSON *tt = e->srv->tools->child; tt; tt = tt->next)
            if (!strcmp(rec_str(tt, "name"), e->upstream_name)) {
                upstream = (cJSON *)tt;
                break;
            }
        /* name: expose `name` when present, else the selector; expose
           entries carry their config record as the presentation (hide and
           default entries carry NULL) */
        const char *expose_name = NULL;
        cJSON *cfgrec = e->presentation;
        char selname[600];
        snprintf(selname, sizeof selname, "%s.%s", e->upstream_srv,
                 e->upstream_name);
        if (cfgrec) {
            const char *n = rec_str(cfgrec, "name");
            expose_name = n ? n : selname;
        } else {
            expose_name = selname;
        }

        cJSON *pres = cJSON_Duplicate(upstream, 1);
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(pres, "name"),
                             expose_name);
        if (cfgrec) {
            const char *d = rec_str(cfgrec, "description");
            if (d) {
                cJSON *od = cJSON_GetObjectItemCaseSensitive(pres, "description");
                if (od) cJSON_SetValuestring(od, d);
                else cJSON_AddStringToObject(pres, "description", d);
            }
            /* argument mapping */
            const cJSON *args =
                cJSON_GetObjectItemCaseSensitive(cfgrec, "arguments");
            if (args) {
                for (const cJSON *a = args->child; a; a = a->next) {
                    const char *up = a->string;
                    const char *nm = rec_str(a, "name");
                    if (nm) {
                        e->from = realloc(e->from,
                                          (e->nmap + 1) * sizeof *e->from);
                        e->to = realloc(e->to,
                                        (e->nmap + 1) * sizeof *e->to);
                        e->from[e->nmap] = strdup(nm);
                        e->to[e->nmap] = strdup(up);
                        e->nmap++;
                    }
                }
                char err2[256] = "";
                if (!rewrite_schema(
                        cJSON_GetObjectItemCaseSensitive(pres, "inputSchema"),
                        args, err2, sizeof err2)) {
                    proxy_fatal(p, "tool '%s': %s", expose_name, err2);
                    cJSON_Delete(pres);
                    for (size_t k = 0; k < nout; k++) exposed_entry_free(&out[k]);
                    free(out);
                    return -1;
                }
            }
        }
        /* exposed names must be unique */
        for (size_t j = 0; j < i; j++)
            if (!strcmp(rec_str(out[j].presentation, "name"), expose_name)) {
                proxy_fatal(p, "duplicate exposed name '%s'", expose_name);
                cJSON_Delete(pres);
                for (size_t k = 0; k < nout; k++) exposed_entry_free(&out[k]);
                free(out);
                return -1;
            }
        cJSON_Delete(e->presentation); /* the config record, if any */
        e->presentation = pres;
    }

    /* replace p->ex with the final list */
    for (size_t i = 0; i < p->nex; i++) exposed_entry_free(&p->ex[i]);
    free(p->ex);
    p->ex = out;
    p->nex = nout;
    p->capex = capout;
    return 0;
}

/* ---- rpc handlers ---- */

int proxy_handle(void *ctx, const char *method, cJSON *params,
                 cJSON *id, cJSON **result_out, char **errmsg_out) {
    proxy_state_t *p = ctx;
    (void)id;
    if (!strcmp(method, "initialize")) {
        cJSON *res = cJSON_CreateObject();
        const char *pr = rec_str(params, "protocolVersion");
        const char *use =
            (pr && (!strcmp(pr, "2026-07-28") || !strcmp(pr, "2025-11-25") ||
                    !strcmp(pr, "2025-06-18") || !strcmp(pr, "2025-03-26") ||
                    !strcmp(pr, "2024-11-05")))
                ? pr
                : "2025-11-25";
        cJSON_AddStringToObject(res, "protocolVersion", use);
        cJSON *caps = cJSON_CreateObject();
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(res, "capabilities", caps);
        cJSON *si = cJSON_CreateObject();
        cJSON_AddStringToObject(si, "name", "llmkit-mcp-proxy");
        cJSON_AddStringToObject(si, "version", LLMKIT_VERSION);
        cJSON_AddItemToObject(res, "serverInfo", si);
        *result_out = res;
        return 0;
    }
    if (!strcmp(method, "notifications/initialized") || !strcmp(method, "ping"))
        return 2;
    if (!strcmp(method, "tools/list")) {
        cJSON *res = cJSON_CreateObject();
        cJSON *tools = cJSON_CreateArray();
        for (size_t i = 0; i < p->nex; i++)
            cJSON_AddItemToArray(tools,
                                 cJSON_Duplicate(p->ex[i].presentation, 1));
        cJSON_AddItemToObject(res, "tools", tools);
        *result_out = res;
        return 0;
    }
    if (!strcmp(method, "tools/call")) {
        const char *name = rec_str(params, "name");
        exposed_tool_t *e = NULL;
        if (name)
            for (size_t i = 0; i < p->nex; i++)
                if (!strcmp(rec_str(p->ex[i].presentation, "name"), name))
                    e = &p->ex[i];
        if (!e) {
            *errmsg_out = strdup(name ? "unknown tool" : "missing tool name");
            return 1;
        }
        /* inverse argument mapping */
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        cJSON *upargs = cJSON_CreateObject();
        if (cJSON_IsObject(args))
            for (const cJSON *a = args->child; a; a = a->next) {
                const char *target = a->string;
                for (size_t m = 0; m < e->nmap; m++)
                    if (!strcmp(e->from[m], a->string)) target = e->to[m];
                cJSON_AddItemToObject(upargs, target,
                                      cJSON_Duplicate((cJSON *)a, 1));
            }
        cJSON *result = NULL;
        char err[512] = "";
        int rc = mcp_call_raw(p->mgr, e->srv, e->upstream_name, upargs, &result,
                              err, sizeof err, -1);
        cJSON_Delete(upargs);
        if (rc != 0) {
            *errmsg_out = strdup(err[0] ? err : "upstream call failed");
            return 1;
        }
        *result_out = result; /* relayed verbatim */
        return 0;
    }
    *errmsg_out = strdup("method not found");
    return 1;
}

static void proxy_file_on_line(void *ctx, char *line) {
    proxy_state_t *p = ctx;
    if (!proxy_config_line(p, line)) {
        /* fatal already printed; keep draining */
    }
}

void proxy_state_init(proxy_state_t *p) {
    memset(p, 0, sizeof *p);
    p->ok = true;
    p->first = true;
    p->mgr = mcp_mgr_new();
}

void proxy_state_free(proxy_state_t *p) {
    mcp_kill_all(p->mgr);
    mcp_mgr_free(p->mgr);
    for (size_t i = 0; i < p->nex; i++) exposed_entry_free(&p->ex[i]);
    free(p->ex);
    cJSON_Delete(p->tools_record);
    memset(p, 0, sizeof *p);
}

static bool proxy_load_file(const char *path, proxy_state_t *p) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    jsonl_pusher_t push;
    jsonl_pusher_init(&push, proxy_file_on_line, p);
    char bbuf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(bbuf, 1, sizeof bbuf, f)) > 0)
        if (jsonl_feed(&push, bbuf, n) != 0) {
            ok = false;
            break;
        }
    if (ok && jsonl_eof(&push) != 0) ok = false;
    jsonl_pusher_free(&push);
    fclose(f);
    return ok;
}

int cmd_proxy(const char *config_path) {
    signals_init();
    proxy_state_t p;
    proxy_state_init(&p);

    if (!proxy_load_file(config_path, &p) || !p.ok ||
        proxy_resolve_and_build(&p) != 0) {
        fprintf(stderr, "llmkit mcp-proxy: fatal config error in %s\n",
                config_path);
        proxy_state_free(&p);
        return 1;
    }

    rpc_handler_t h = { proxy_handle, &p };
    rpc_serve_stdio(&h);
    proxy_state_free(&p);
    return 0;
}
