/* mcp.c - mcp client (stdio, streamable http, legacy sse; v1 and v2 flows,
   design sec.6) and the shared json-rpc stdio server loop (design sec.9, sec.10). */
#include "llmkit.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define CONNECT_TIMEOUT 10.0
#define RPC_TIMEOUT 30.0

static const char *const v1_revisions[] = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", NULL,
};

/* shared with the agent/proxy initialize handlers */
bool mcp_protocol_supported(const char *rev) {
    for (int i = 0; v1_revisions[i]; i++)
        if (!strcmp(v1_revisions[i], rev)) return true;
    return !strcmp(rev, "2026-07-28");
}

/* ================= mgr ================= */

mcp_mgr_t *mcp_mgr_new(void) {
    mcp_mgr_t *m = calloc(1, sizeof *m);
    return m;
}

static void server_free(mcp_server_t *s) {
    if (s->type == MCP_SSE && s->have_sse_th) {
        /* the detached GET thread touches s->url and s->sse_ready for as
           long as the stream lives and cannot be stopped; freeing under it
           is a use-after-free. ponytail: leak the struct (one per
           disconnected legacy-sse server, none at process exit); make the
           thread joinable with a curl abort if it ever matters */
        return;
    }
    if (s->type == MCP_STDIO && s->have_reader) {
        /* kill the child (EOF for the reader), then wait until the reader
           closed the queue: after that it touches nothing we free here */
        spawn_kill(&s->sp);
        queue_wait_closed(s->q);
    }
    free(s->name);
    cJSON_Delete(s->cfg);
    if (s->q) queue_free(s->q);
    free(s->url);
    free(s->session);
    cJSON_Delete(s->tools);
    free(s);
}

void mcp_mgr_free(mcp_mgr_t *m) {
    for (size_t i = 0; i < m->n; i++) server_free(m->v[i]);
    free(m->v);
    for (size_t i = 0; i < m->listing.n; i++) {
        cJSON_Delete(m->listing.v[i].tool);
        free(m->listing.v[i].exposed_name);
    }
    free(m->listing.v);
    free(m);
}

void mcp_kill_all(mcp_mgr_t *m) {
    for (size_t i = 0; i < m->n; i++) {
        mcp_server_t *s = m->v[i];
        if (s->type == MCP_STDIO) spawn_kill(&s->sp);
    }
}

mcp_server_t *mcp_find(mcp_mgr_t *m, const char *name) {
    for (size_t i = 0; i < m->n; i++)
        if (!strcmp(m->v[i]->name, name)) return m->v[i];
    return NULL;
}

/* ================= json-rpc plumbing ================= */

static cJSON *rpc_make(mcp_server_t *s, const char *method, cJSON *params) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(r, "id", (double)++s->next_id);
    cJSON_AddStringToObject(r, "method", method);
    if (!params) params = cJSON_CreateObject();
    if (s->v2) {
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta,
                                 "io.modelcontextprotocol/protocolVersion",
                                 "2026-07-28");
        cJSON_AddItemToObject(meta, "clientCapabilities", cJSON_CreateObject());
        cJSON *ci = cJSON_CreateObject();
        cJSON_AddStringToObject(ci, "name", "llmkit");
        cJSON_AddStringToObject(ci, "version", LLMKIT_VERSION);
        cJSON_AddItemToObject(meta, "clientInfo", ci);
        cJSON_AddItemToObject(params, "_meta", meta);
    }
    cJSON_AddItemToObject(r, "params", params);
    return r;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- stdio transport ---- */

typedef struct stdio_reader_ctx {
    mcp_server_t *s;
} stdio_reader_ctx_t;

static void stdio_on_line(void *ctx, char *line) {
    stdio_reader_ctx_t *rc = ctx;
    cJSON *t = cJSON_Parse(line);
    free(line);
    if (!t) return;
    /* only responses (id + result/error) are enqueued; notifications and
       server-to-client requests are dropped */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(t, "result");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(t, "error");
    if ((res || err) && id && !cJSON_IsNull(id))
        queue_push(rc->s->q, t);
    else
        cJSON_Delete(t);
}

static void *stdio_reader_thread(void *arg) {
    stdio_reader_ctx_t *rc = arg;
    mcp_server_t *s = rc->s;
    jsonl_pusher_t p;
    /* reuse the jsonl byte rules: CR dropping, line split, utf-8 */
    jsonl_pusher_init(&p, stdio_on_line, rc);
    char bbuf[8192];
    for (;;) {
        ssize_t n = read(s->sp.from_fd, bbuf, sizeof bbuf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (jsonl_feed(&p, bbuf, (size_t)n) != 0) break;
    }
    jsonl_pusher_free(&p);
    spawn_wait(&s->sp);
    s->dead = true;
    /* queue_close is this thread's last touch of s and s->q: server_free
       waits for it before freeing (kill + queue_wait_closed) */
    queue_close(s->q);
    free(rc);
    return NULL;
}

static int stdio_rpc(mcp_server_t *s, cJSON *req, cJSON **reply,
                     double timeout, char *err, size_t errsz) {
    if (s->dead) {
        snprintf(err, errsz, "mcp server '%s' died", s->name);
        return 1;
    }
    char *line = cJSON_PrintUnformatted(req);
    if (!line) {
        snprintf(err, errsz, "out of memory");
        return 1;
    }
    size_t len = strlen(line);
    if (write_all(s->sp.to_fd, line, len) || write_all(s->sp.to_fd, "\n", 1)) {
        cJSON_free(line);
        s->dead = true;
        snprintf(err, errsz, "cannot write to mcp server '%s'", s->name);
        return 1;
    }
    cJSON_free(line);
    double reqid = rec_num(req, "id", 0);
    double deadline = mono_now() + (timeout > 0 ? timeout : RPC_TIMEOUT);
    for (;;) {
        double left = deadline - mono_now();
        if (left <= 0) {
            snprintf(err, errsz, "mcp server '%s' timed out", s->name);
            return 2;
        }
        cJSON *t = queue_pop_timeout(s->q, left);
        if (!t) {
            if (s->dead) {
                snprintf(err, errsz, "mcp server '%s' died", s->name);
                return 1;
            }
            continue; /* spurious wakeup: recompute the deadline */
        }
        if (rec_num(t, "id", -1) == reqid) {
            const cJSON *e = cJSON_GetObjectItemCaseSensitive(t, "error");
            if (e) {
                const char *m = rec_str(e, "message");
                snprintf(err, errsz, "%s",
                         m ? m : "json-rpc error from mcp server");
                cJSON_Delete(t);
                return 1;
            }
            const cJSON *res = cJSON_GetObjectItemCaseSensitive(t, "result");
            *reply = res ? cJSON_Duplicate(res, 1) : cJSON_CreateObject();
            cJSON_Delete(t);
            return 0;
        }
        cJSON_Delete(t); /* stale response */
    }
}

static void stdio_notify(mcp_server_t *s, const char *method) {
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", method);
    char *line = cJSON_PrintUnformatted(n);
    if (line) {
        write_all(s->sp.to_fd, line, strlen(line));
        write_all(s->sp.to_fd, "\n", 1);
        cJSON_free(line);
    }
    cJSON_Delete(n);
}

/* ---- http transports ---- */

typedef struct sse_get_ctx {
    mcp_server_t *s;
    sse_parser_t p;
} sse_get_ctx_t;

static void sse_get_event(void *ctx, const char *event, const char *data,
                          size_t n) {
    sse_get_ctx_t *c = ctx;
    if (!strcmp(event, "endpoint") && !c->s->sse_ready) {
        free(c->s->url);
        c->s->url = strndup(data, n);
        c->s->sse_ready = true;
    }
    /* server pushes arrive here and are ignored (design sec.6) */
}

static void sse_get_data(void *ctx, const char *bytes, size_t n) {
    sse_get_ctx_t *c = ctx;
    sse_feed(&c->p, bytes, n);
}

static void *sse_get_thread(void *arg) {
    mcp_server_t *s = arg;
    sse_get_ctx_t c;
    memset(&c, 0, sizeof c);
    c.s = s;
    sse_init(&c.p, sse_get_event, &c);
    http_req_t req;
    memset(&req, 0, sizeof req);
    buf_init(&req.resp);
    buf_init(&req.content_type);
    req.url = s->url; /* the GET url (from cfg) */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    req.hdrs = hdrs;
    req.body = "";
    req.body_len = 0;
    req.on_data = sse_get_data;
    req.cb_ctx = &c;
    req.read_to = 0; /* long-lived */
    http_perform(&req);
    buf_free(&req.resp);
    buf_free(&req.content_type);
    curl_slist_free_all(hdrs);
    sse_free(&c.p);
    return NULL;
}

/* picks the json-rpc response matching id out of an SSE body */
typedef struct sse_pick {
    double id;
    cJSON *found;
} sse_pick_t;

static void sse_pick_cb(void *ctx, const char *ev, const char *data,
                        size_t n) {
    sse_pick_t *p = ctx;
    if (p->found) return;
    if (strcmp(ev, "message")) return;
    cJSON *d = cJSON_ParseWithLength(data, n);
    if (!d) return;
    if (rec_num(d, "id", -2) == p->id) {
        p->found = d;
        return;
    }
    cJSON_Delete(d);
}

/* one POST round trip; reply is the result tree (owned) or error */
static int http_rpc(mcp_server_t *s, cJSON *req, cJSON **reply, double timeout,
                    char *err, size_t errsz) {
    char *body = cJSON_PrintUnformatted(req);
    if (!body) {
        snprintf(err, errsz, "out of memory");
        return 1;
    }
    struct curl_slist *hdrs = NULL;
    http_hdr_add_json(&hdrs);
    hdrs = curl_slist_append(hdrs, "Accept: application/json, text/event-stream");
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(s->cfg, "headers");
    /* header shape: validate_tools rejected bad ones up front */
    http_hdrs_from_json(&hdrs, custom, NULL, 0);
    if (s->session && s->session[0])
        http_hdr_add(&hdrs, "Mcp-Session-Id", s->session);

    http_req_t r;
    memset(&r, 0, sizeof r);
    buf_init(&r.resp);
    buf_init(&r.content_type);
    r.url = s->url;
    r.hdrs = hdrs;
    r.body = body;
    r.body_len = strlen(body);
    r.connect_to = CONNECT_TIMEOUT;
    r.total_to = timeout > 0 ? timeout : RPC_TIMEOUT;
    int rc = http_perform(&r);

    cJSON *msg = NULL;
    int ret = 0;
    if (rc == -1) {
        snprintf(err, errsz, "cannot reach mcp server '%s': %s", s->name,
                 curl_easy_strerror(r.curl_res));
        ret = 1;
    } else if (rc == 1) {
        snprintf(err, errsz, "mcp server '%s' answered HTTP %ld", s->name,
                 r.status);
        ret = 1;
    } else {
        if (r.session_id[0] && !s->session) s->session = strdup(r.session_id);
        bool is_sse =
            r.content_type.data &&
            strncasecmp(r.content_type.data, "text/event-stream",
                        strlen("text/event-stream")) == 0;
        if (is_sse) {
            /* parse events from the body; take the response for our id */
            sse_pick_t pctx = { rec_num(req, "id", 0), NULL };
            sse_parser_t p;
            sse_init(&p, sse_pick_cb, &pctx);
            sse_feed(&p, r.resp.data ? r.resp.data : "", r.resp.len);
            sse_eof(&p);
            sse_free(&p);
            msg = pctx.found;
        } else {
            msg = cJSON_ParseWithLength(r.resp.data ? r.resp.data : "",
                                        r.resp.len);
        }
        if (!msg) {
            snprintf(err, errsz, "mcp server '%s' returned a non-json reply",
                     s->name);
            ret = 1;
        }
    }
    cJSON_free(body);
    buf_free(&r.resp);
    buf_free(&r.content_type);
    curl_slist_free_all(hdrs);
    if (ret) return ret;

    const cJSON *e = cJSON_GetObjectItemCaseSensitive(msg, "error");
    if (e) {
        const char *m = rec_str(e, "message");
        snprintf(err, errsz, "%s", m ? m : "json-rpc error from mcp server");
        cJSON_Delete(msg);
        return 1;
    }
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(msg, "result");
    *reply = res ? cJSON_Duplicate(res, 1) : cJSON_CreateObject();
    cJSON_Delete(msg);
    return 0;
}

static int server_rpc(mcp_server_t *s, cJSON *req, cJSON **reply,
                      double timeout, char *err, size_t errsz) {
    if (s->type == MCP_STDIO)
        return stdio_rpc(s, req, reply, timeout, err, errsz);
    return http_rpc(s, req, reply, timeout, err, errsz);
}

/* ================= connect flows ================= */

static int server_connect(mcp_server_t *s, char *err, size_t errsz) {
    if (s->type == MCP_STDIO) {
        const char *cmd = rec_str(s->cfg, "command_line");
        if (spawn_shell(cmd, &s->sp)) {
            snprintf(err, errsz, "cannot spawn '%s'", s->name);
            return 1;
        }
        s->q = queue_new();
        stdio_reader_ctx_t *rc = calloc(1, sizeof *rc);
        rc->s = s;
        if (thread_start_detached(stdio_reader_thread, rc) != 0) {
            free(rc);
            snprintf(err, errsz, "cannot start reader for '%s'", s->name);
            return 1;
        }
        s->have_reader = true;
    } else if (s->type == MCP_HTTP) {
        s->url = strdup(rec_str(s->cfg, "url"));
    } else { /* legacy sse: open the GET stream, wait for the endpoint event */
        s->url = strdup(rec_str(s->cfg, "url"));
        s->have_sse_th = true;
        thread_start_detached(sse_get_thread, s);
        double deadline = mono_now() + CONNECT_TIMEOUT;
        while (!s->sse_ready && mono_now() < deadline) msleep(20);
        if (!s->sse_ready) {
            snprintf(err, errsz,
                     "mcp server '%s' sent no endpoint event", s->name);
            return 1;
        }
    }

    if (!s->v2) {
        /* v1: initialize carrying protocolVersion, then initialized */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "protocolVersion", s->protocol_req);
        cJSON_AddItemToObject(params, "capabilities", cJSON_CreateObject());
        cJSON *ci = cJSON_CreateObject();
        cJSON_AddStringToObject(ci, "name", "llmkit");
        cJSON_AddStringToObject(ci, "version", LLMKIT_VERSION);
        cJSON_AddItemToObject(params, "clientInfo", ci);
        cJSON *req = rpc_make(s, "initialize", params);
        cJSON *reply = NULL;
        int rc = server_rpc(s, req, &reply, CONNECT_TIMEOUT, err, errsz);
        cJSON_Delete(req);
        if (rc) return rc;
        const char *rev = rec_str(reply, "protocolVersion");
        if (!rev || !mcp_protocol_supported(rev) ||
            !strcmp(rev, "2026-07-28")) {
            snprintf(err, errsz,
                     "mcp server '%s' answered unsupported protocol '%s'",
                     s->name, rev ? rev : "(none)");
            cJSON_Delete(reply);
            return 1;
        }
        cJSON_Delete(reply);
        if (s->type == MCP_STDIO) {
            stdio_notify(s, "notifications/initialized");
        } else {
            /* POST the notification; the reply body is ignored */
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "jsonrpc", "2.0");
            cJSON_AddStringToObject(note, "method", "notifications/initialized");
            cJSON_AddItemToObject(note, "params", cJSON_CreateObject());
            cJSON *ignored = NULL;
            char nerr[256] = "";
            http_rpc(s, note, &ignored, CONNECT_TIMEOUT, nerr, sizeof nerr);
            cJSON_Delete(ignored);
            cJSON_Delete(note);
        }
    }

    /* tools/list */
    cJSON *req = rpc_make(s, "tools/list", cJSON_CreateObject());
    cJSON *reply = NULL;
    int rc = server_rpc(s, req, &reply, CONNECT_TIMEOUT, err, errsz);
    cJSON_Delete(req);
    if (rc) return rc;
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(reply, "tools");
    s->tools = cJSON_IsArray(tools) ? cJSON_Duplicate((cJSON *)tools, 1)
                                    : cJSON_CreateArray();
    cJSON_Delete(reply);
    s->connected = true;
    return 0;
}

static void emit_connect_failed(engine_t *e, const char *name, const char *err,
                                bool fatal) {
    if (!e || !e->emit) return;
    char msg[512];
    snprintf(msg, sizeof msg, "mcp server '%s': %s", name, err);
    e->emit(e->emit_ctx, rec_error(EC_CONNECT_FAILED, msg, fatal));
}

static bool cfg_marks_terminal(const cJSON *srv_cfg, const char *tool_name) {
    const cJSON *tt =
        cJSON_GetObjectItemCaseSensitive(srv_cfg, "terminal_tools");
    if (!cJSON_IsArray(tt)) return false;
    for (const cJSON *e = tt->child; e; e = e->next)
        if (cJSON_IsString(e) && !strcmp(e->valuestring, tool_name))
            return true;
    return false;
}

static void listing_rebuild(mcp_mgr_t *m) {
    for (size_t i = 0; i < m->listing.n; i++) {
        cJSON_Delete(m->listing.v[i].tool);
        free(m->listing.v[i].exposed_name);
    }
    m->listing.n = 0;
    for (size_t i = 0; i < m->n; i++) {
        mcp_server_t *s = m->v[i];
        if (!s->connected || !s->tools) continue;
        for (const cJSON *t = s->tools->child; t; t = t->next) {
            const char *nm = rec_str(t, "name");
            if (!nm) continue;
            if (m->listing.n == m->listing.cap) {
                m->listing.cap = m->listing.cap ? m->listing.cap * 2 : 16;
                m->listing.v = realloc(
                    m->listing.v, m->listing.cap * sizeof *m->listing.v);
            }
            tool_entry_t *te = &m->listing.v[m->listing.n++];
            te->srv = s;
            te->tool = cJSON_Duplicate((cJSON *)t, 1);
            char *ex = malloc(strlen(s->name) + strlen(nm) + 2);
            sprintf(ex, "%s.%s", s->name, nm);
            te->exposed_name = ex;
            te->terminal = cfg_marks_terminal(s->cfg, nm);
        }
    }
}

int mcp_reconcile(mcp_mgr_t *m, engine_t *e, const cJSON *tools_record) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(tools_record, "tools");
    size_t want_n =
        (size_t)(cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0);

    /* disconnect servers no longer listed */
    for (size_t i = 0; i < m->n;) {
        const char *nm = m->v[i]->name;
        bool keep = false;
        for (const cJSON *s = list ? list->child : NULL; s; s = s->next)
            if (!strcmp(nm, rec_str(s, "name"))) keep = true;
        if (!keep) {
            mcp_server_t *dead = m->v[i];
            server_free(dead); /* kills the child, waits for the reader */
            memmove(&m->v[i], &m->v[i + 1], (m->n - i - 1) * sizeof *m->v);
            m->n--;
        } else {
            i++;
        }
    }

    /* connect new servers, in tools-record order */
    for (const cJSON *sc = list ? list->child : NULL; sc; sc = sc->next) {
        const char *nm = rec_str(sc, "name");
        mcp_server_t *s = mcp_find(m, nm);
        if (s) {
            cJSON_Delete(s->cfg);
            s->cfg = cJSON_Duplicate((cJSON *)sc, 1);
            s->required = rec_bool(sc, "required", false);
            continue; /* already connected: keep the connection */
        }
        s = calloc(1, sizeof *s);
        s->name = strdup(nm);
        s->cfg = cJSON_Duplicate((cJSON *)sc, 1);
        const char *ty = rec_str(sc, "type");
        s->type = !strcmp(ty, "stdio")    ? MCP_STDIO
                  : !strcmp(ty, "http")   ? MCP_HTTP
                                          : MCP_SSE;
        s->required = rec_bool(sc, "required", false);
        const char *pr = rec_str(sc, "protocol");
        if (!pr) pr = "2025-11-25";
        snprintf(s->protocol_req, sizeof s->protocol_req, "%s", pr);
        s->v2 = !strcmp(pr, "2026-07-28");
        s->sp.to_fd = s->sp.from_fd = -1;
        char err[512] = "";
        if (server_connect(s, err, sizeof err)) {
            bool fatal = s->required;
            emit_connect_failed(e, nm, err, fatal);
            server_free(s); /* kills the child, waits for the reader */
            if (fatal) {
                mcp_kill_all(m);
                return EXIT_CONNECT_FAILED;
            }
            continue;
        }
        if (m->n == m->cap) {
            m->cap = m->cap ? m->cap * 2 : 8;
            m->v = realloc(m->v, m->cap * sizeof *m->v);
        }
        m->v[m->n++] = s;
    }

    /* stable reorder to tools-record order */
    {
        mcp_server_t **ord = NULL;
        size_t no = 0;
        if (cJSON_IsArray(list)) {
            ord = calloc(want_n ? want_n : 1, sizeof *ord);
            for (const cJSON *sc = list->child; sc; sc = sc->next) {
                mcp_server_t *s = mcp_find(m, rec_str(sc, "name"));
                if (s) ord[no++] = s;
            }
        }
        if (no == m->n && ord) {
            memcpy(m->v, ord, no * sizeof *m->v);
        }
        free(ord);
    }
    listing_rebuild(m);

    /* terminal_tools name resolution: every name must appear in its own
       server's listing (typo catching, the proxy's expose/hide rule). A
       server that failed to connect never reached m->v: not validated,
       its tools are not offered anyway. */
    for (size_t i = 0; i < m->n; i++) {
        mcp_server_t *s = m->v[i];
        const cJSON *tt =
            cJSON_GetObjectItemCaseSensitive(s->cfg, "terminal_tools");
        if (!cJSON_IsArray(tt)) continue;
        for (const cJSON *name = tt->child; name; name = name->next) {
            if (!cJSON_IsString(name)) continue; /* shape: validation */
            const char *nm = name->valuestring;
            bool found = false;
            for (const cJSON *t = s->tools ? s->tools->child : NULL; t;
                 t = t->next) {
                const cJSON *tn = cJSON_GetObjectItemCaseSensitive(t, "name");
                if (cJSON_IsString(tn) && !strcmp(tn->valuestring, nm)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "terminal_tools names tool '%s', not listed by "
                         "server '%s'",
                         nm, s->name);
                if (e && e->emit)
                    e->emit(e->emit_ctx,
                            rec_error(EC_INVALID_RECORD, msg, true));
                return EXIT_INVALID_RECORD;
            }
        }
    }
    return 0;
}

const tool_listing_t *mcp_listing(mcp_mgr_t *m) { return &m->listing; }

bool mcp_tool_is_terminal(mcp_mgr_t *m, const char *tool) {
    for (size_t i = 0; i < m->listing.n; i++)
        if (!strcmp(m->listing.v[i].exposed_name, tool))
            return m->listing.v[i].terminal;
    return false;
}

/* ================= tools/call ================= */

int mcp_call_raw(mcp_server_t *srv, const char *upstream_tool,
                 const cJSON *args, cJSON **result_out, char *err,
                 size_t errsz, double timeout) {
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", upstream_tool);
    cJSON_AddItemToObject(params, "arguments",
                          args ? cJSON_Duplicate((cJSON *)args, 1)
                               : cJSON_CreateObject());
    cJSON *req = rpc_make(srv, "tools/call", params);
    cJSON *reply = NULL;
    int rc = server_rpc(srv, req, &reply, timeout, err, errsz);
    cJSON_Delete(req);
    if (rc) return rc;
    *result_out = reply;
    return 0;
}

int mcp_call(mcp_mgr_t *m, const char *tool, cJSON *args, buf_t *text_out,
             bool *is_error, char *err, size_t errsz, double timeout) {
    const char *dot = strchr(tool, '.');
    if (!dot) {
        snprintf(err, errsz, "tool '%s' has no server prefix", tool);
        return 1;
    }
    char srvname[256];
    size_t sl = (size_t)(dot - tool);
    if (sl >= sizeof srvname) {
        snprintf(err, errsz, "tool server name too long in '%.64s...'", tool);
        return 1;
    }
    memcpy(srvname, tool, sl);
    srvname[sl] = '\0';
    mcp_server_t *s = mcp_find(m, srvname);
    if (!s || !s->connected) {
        snprintf(err, errsz, "unknown tool server '%s'", srvname);
        return 1;
    }
    cJSON *result = NULL;
    int rc = mcp_call_raw(s, dot + 1, args, &result, err, errsz, timeout);
    if (rc) return rc;

    /* map the result to text (requirements sec.6) */
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    bool any_text = false, any_non_text = false;
    if (cJSON_IsArray(content))
        for (const cJSON *b = content->child; b; b = b->next) {
            const char *ty = rec_str(b, "type");
            if (ty && !strcmp(ty, "text")) {
                const char *t = rec_str(b, "text");
                if (any_text) buf_append_byte(text_out, '\n');
                buf_append_str(text_out, t ? t : "");
                any_text = true;
            } else {
                any_non_text = true;
            }
        }
    bool is_err = rec_bool(result, "isError", false);
    if (any_non_text) {
        cJSON_Delete(result);
        snprintf(err, errsz, "tool '%s' returned non-text content", tool);
        return 1;
    }
    if (!any_text &&
        cJSON_GetObjectItemCaseSensitive(result, "structuredContent")) {
        cJSON_Delete(result);
        snprintf(err, errsz, "tool '%s' returned no text content", tool);
        return 1;
    }
    cJSON_Delete(result);
    if (is_err) {
        *is_error = true;
        return 0;
    }
    return 0;
}

/* ================= json-rpc stdio server loop ================= */

typedef struct serve_ctx {
    rpc_handler_t *h;
    rpc_write_fn wr;
    void *io;
    bool stop;
} serve_ctx_t;

static void serve_line(serve_ctx_t *c, const char *line) {
    cJSON *msg = cJSON_Parse(line);
    if (!msg || !cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        return;
    }
    const char *method = rec_str(msg, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (!method) {
        cJSON_Delete(msg);
        return;
    }
    cJSON *idc = cJSON_IsNull(id) ? NULL
                : id              ? cJSON_Duplicate(id, 1)
                                  : NULL;
    cJSON *pcopy = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
    cJSON *result = NULL;
    char *errmsg = NULL;
    int rc = c->h->handle(c->h->ctx, method, pcopy, idc, &result, &errmsg);
    if (idc && rc == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(resp, "id", idc);
        cJSON_AddItemToObject(resp, "result", result ? result : cJSON_CreateObject());
        char *out = cJSON_PrintUnformatted(resp);
        if (out) {
            c->wr(c->io, out, strlen(out));
            c->wr(c->io, "\n", 1);
            cJSON_free(out);
        }
        cJSON_Delete(resp);
    } else if (idc && rc == 1) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(resp, "id", idc);
        cJSON *errobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(errobj, "code", -32000);
        cJSON_AddStringToObject(errobj, "message",
                                errmsg ? errmsg : "internal error");
        cJSON_AddItemToObject(resp, "error", errobj);
        char *out = cJSON_PrintUnformatted(resp);
        if (out) {
            c->wr(c->io, out, strlen(out));
            c->wr(c->io, "\n", 1);
            cJSON_free(out);
        }
        cJSON_Delete(resp);
    } else {
        cJSON_Delete(idc);
    }
    free(errmsg);
    /* the result was consumed only when it went into a success reply */
    if (result && !(rc == 0 && idc)) cJSON_Delete(result);
    cJSON_Delete(pcopy);
    cJSON_Delete(msg);
    if (rc < 0) c->stop = true;
}

static void serve_on_line(void *ctx, char *line) {
    serve_ctx_t *c = ctx;
    serve_line(c, line);
    free(line);
}

/* the default stdio server entry needs no io context: fd 0/1 */
static int rpc_read_fd0(void *ctx, char *buf, size_t bufsz) {
    (void)ctx;
    ssize_t n = read(0, buf, bufsz);
    return (int)n;
}

static void rpc_write_fd1(void *ctx, const char *line, size_t n) {
    (void)ctx;
    write_all(1, line, n);
}

void rpc_serve(rpc_handler_t *h, rpc_read_fn rd, rpc_write_fn wr, void *io) {
    serve_ctx_t c = { h, wr, io, false };
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, serve_on_line, &c);
    char tmp[4096];
    while (!c.stop && !g_stop_flag) {
        int n = rd(io, tmp, sizeof tmp);
        if (n <= 0) break;
        if (jsonl_feed(&p, tmp, (size_t)n) != 0) break;
    }
    jsonl_eof(&p);
    jsonl_pusher_free(&p);
}

/* default stdio server entry: read fd 0, write fd 1 */
int rpc_serve_stdio(rpc_handler_t *h) {
    rpc_serve(h, rpc_read_fd0, rpc_write_fd1, NULL);
    return 0;
}

void tlist_truncate(tlist_t *l, size_t n) {
    if (l->n <= n) return;
    for (size_t i = n; i < l->n; i++) trec_free(l->v[i]);
    l->n = n;
}

void http_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }
