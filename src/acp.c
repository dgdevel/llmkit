/* acp.c - llmkit acp: the Agent Client Protocol server over stdio
   (docs/plan-acp.md, docs/acp.md).

   ndjson JSON-RPC 2.0 on stdin/stdout: one message per line, the reply
   flushed after every write. stdout carries protocol only - every log
   goes to stderr, the editor would choke on anything else.

   Sessions: session/new builds one engine the way repl_run_ex does
   (call_compile, then the options record, then the bundle extras), the
   conversation itself is a turn per session/prompt - tlist_ingest the
   user record, engine_start + engine_run, the sink translates every
   emitted record into a session/update notification. Cancel rides the
   process-global stop flag the engine already checks at its wire and
   tool boundaries (the repl SIGINT path); the reader thread holds the
   next request until the turn is done, then it resumes.

   Limitation: one process, one cwd. Every session/new chdir()s to its
   workspace, so two live sessions rooted at different directories
   would fight over the working directory (the builtin tools resolve
   relative paths against it). Zed runs one agent process per panel
   tab in practice; the server never refuses a second session, it just
   shares the last cwd. */
#include "llmkit.h"
#include "prompts.gen.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= parsed command line ================= */

/* which optional bundle rides along (plan: Flags) */
enum { ACP_BUNDLE_NONE = 0, ACP_BUNDLE_FULL, ACP_BUNDLE_READ_ONLY };

typedef struct acp_cfg {
    int protocol;                          /* PROTO_* */
    char *api_base;                        /* owned */
    char *key, *model, *system;            /* owned, NULL = absent */
    char *reasoning_effort;                /* owned, NULL = absent */
    long max_tokens;                       /* -1 absent, >0 sent */
    char **hdr_names, **hdr_values;        /* owned, parallel arrays */
    size_t nhdrs;
    char **proxies;                        /* owned config paths */
    size_t nproxies, capproxies;
    int bundle;                            /* ACP_BUNDLE_* */
} acp_cfg_t;

static void acp_cfg_free(acp_cfg_t *c) {
    free(c->api_base);
    free(c->key);
    free(c->model);
    free(c->reasoning_effort);
    free(c->system);
    for (size_t i = 0; i < c->nhdrs; i++) {
        free(c->hdr_names[i]);
        free(c->hdr_values[i]);
    }
    free(c->hdr_names);
    free(c->hdr_values);
    for (size_t i = 0; i < c->nproxies; i++) free(c->proxies[i]);
    free(c->proxies);
    memset(c, 0, sizeof *c);
}

/* strdup-append one string to a growable string list */
static int cfg_str_add(char ***v, size_t *n, size_t *cap, const char *s) {
    char *d = strdup(s);
    if (!d) return -1;
    ptr_push((void ***)v, n, cap, d);
    return 0;
}

/* server name: basename minus last extension, any extension; NULL when
   the result would be empty (same rule call's --mcp-proxy applies) */
static char *proxy_name(const char *path) {
    const char *base = path_basename(path);
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) n = (size_t)(dot - base);
    if (n == 0) return NULL;
    return strndup(base, n);
}

/* value flag: strdup once, "given twice" on repeat */
static int set_str_once(const char *flag, const char *v, char **dst,
                        bool *have, char *err, size_t errsz) {
    if (*have) return usage_err(err, errsz, "%s given twice", flag);
    *have = true;
    *dst = strdup(v);
    return 0;
}

static int cfg_hdr_add(acp_cfg_t *c, const char *name, const char *value) {
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

/* 0 ok, 1 usage error (err filled, cfg freed). The protocol flag takes
   no value here - the api_base is a positional argument, call-style */
static int acp_parse(int argc, char **argv, acp_cfg_t *c, char *err,
                     size_t errsz) {
    memset(c, 0, sizeof *c);
    c->protocol = -1;
    c->max_tokens = -1;
    c->bundle = ACP_BUNDLE_NONE;
    bool have_base = false, have_key = false, have_model = false,
         have_system = false, have_effort = false, have_mt = false,
         have_bundle = false;

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
                   !strcmp(a, "--reasoning-effort") ||
                   !strcmp(a, "--max-tokens") ||
                   !strcmp(a, "--system-prompt") || !strcmp(a, "--header") ||
                   !strcmp(a, "--mcp-proxy") || !strcmp(a, "--bundle")) {
            if (i + 1 >= argc) {
                usage_err(err, errsz, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            if (!strcmp(a, "--key")) {
                if (set_str_once(a, v, &c->key, &have_key, err, errsz))
                    goto fail;
            } else if (!strcmp(a, "--model")) {
                if (set_str_once(a, v, &c->model, &have_model, err, errsz))
                    goto fail;
            } else if (!strcmp(a, "--reasoning-effort")) {
                /* the value is not constrained: providers differ in what
                   they accept, violations surface as the endpoint's own
                   api_error (requirements sec.4) */
                if (set_str_once(a, v, &c->reasoning_effort, &have_effort,
                                 err, errsz))
                    goto fail;
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
                if (set_str_once(a, v, &c->system, &have_system, err, errsz))
                    goto fail;
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
            } else if (!strcmp(a, "--bundle")) {
                if (have_bundle) {
                    usage_err(err, errsz, "--bundle given twice");
                    goto fail;
                }
                have_bundle = true;
                if (!strcmp(v, "full")) c->bundle = ACP_BUNDLE_FULL;
                else if (!strcmp(v, "read-only"))
                    c->bundle = ACP_BUNDLE_READ_ONLY;
                else {
                    usage_err(err, errsz,
                              "--bundle wants full or read-only");
                    goto fail;
                }
            } else if (!strcmp(a, "--mcp-proxy")) {
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
                if (cfg_str_add(&c->proxies, &c->nproxies, &c->capproxies,
                                v)) {
                    usage_err(err, errsz, "out of memory");
                    goto fail;
                }
            }
            /* every branch above has copied v out of argv: hide the
               secret ones from the process list (ps/htop) */
            argv_mask_secret(a, argv[i]);
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
    return 0;
fail:
    acp_cfg_free(c);
    return 1;
}

/* the cfg -> call_cfg_t bridge: the acp flags map 1:1 onto call's own
   (protocol, api_base, key, model, reasoning_effort, max_tokens,
   system, hdrs, proxies), so call_compile and call_build_agent_tools
   serve verbatim. the copy owns nothing: the acp cfg outlives every
   session, the call cfgs are read-only views */
static void acp_call_cfg(const acp_cfg_t *a, call_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->protocol = a->protocol;
    c->api_base = a->api_base;
    c->key = a->key;
    c->model = a->model;
    c->reasoning_effort = a->reasoning_effort;
    c->max_tokens = a->max_tokens;
    c->hdr_names = a->hdr_names;
    c->hdr_values = a->hdr_values;
    c->nhdrs = a->nhdrs;
    c->proxies = a->proxies;
    c->nproxies = a->nproxies;
}

/* ================= sessions ================= */

/* one acp session: the engine, its deepwiki temp config (one per
   session, the repl keeps one per process for the same reason) and the
   turn bookkeeping cancel reads */
typedef struct acp_session {
    char *id;        /* owned, opaque hex token */
    engine_t *e;     /* owned; NULL until session/new succeeded */
    char *cwd;       /* owned, the session's workspace; NULL = inherit */
    char deepwiki_cfg[512]; /* the deepwiki proxy's temp config path */
    bool running;    /* a session/prompt turn is executing */
} acp_session_t;

typedef struct acp_state {
    acp_cfg_t cfg;
    char exe_path[4096]; /* self_exe: the builtin server's command line */
    acp_session_t *sessions;
    size_t nsessions, capsessions;
    unsigned long id_counter;
    queue_t *requests;  /* the reader thread's raw-line queue */
} acp_state_t;

/* an opaque session id: clock, pid and a process-local counter hashed
   into one hex word - unique across sessions of this process without
   pulling a random source in */
static char *acp_session_id(unsigned long n) {
    unsigned long long mix =
        (unsigned long long)n * 0x9e3779b97f4a7c15ULL +
        (unsigned long long)(mono_now() * 1e6) + (unsigned long)getpid();
    mix ^= mix >> 29;
    mix *= 0xbf58476d1ce4e5b9ULL;
    mix ^= mix >> 32;
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)mix);
    return strdup(buf);
}

static acp_session_t *acp_session_new(acp_state_t *st) {
    char *id = acp_session_id(++st->id_counter);
    if (!id) return NULL;
    if (st->nsessions == st->capsessions) {
        size_t cap = st->capsessions ? st->capsessions * 2 : 4;
        acp_session_t *v =
            realloc(st->sessions, cap * sizeof *st->sessions);
        if (!v) {
            free(id);
            return NULL;
        }
        st->sessions = v;
        st->capsessions = cap;
    }
    acp_session_t *s = &st->sessions[st->nsessions++];
    memset(s, 0, sizeof *s);
    s->id = id;
    return s;
}

static acp_session_t *acp_session_find(acp_state_t *st, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < st->nsessions; i++)
        if (!strcmp(st->sessions[i].id, id)) return &st->sessions[i];
    return NULL;
}

static void acp_session_free(acp_session_t *s) {
    free(s->id);
    free(s->cwd);
    if (s->e) {
        /* no reader queue ever attached to the engine itself: the acp
           reader serves the protocol stream, not the engine's inq */
        engine_free(s->e);
    }
    repl_deepwiki_cleanup(s->deepwiki_cfg);
    memset(s, 0, sizeof *s);
}

static void acp_state_free(acp_state_t *st) {
    acp_cfg_free(&st->cfg);
    for (size_t i = 0; i < st->nsessions; i++)
        acp_session_free(&st->sessions[i]);
    free(st->sessions);
    memset(st, 0, sizeof *st);
}

/* ================= framing ================= */

/* one protocol message out: unformatted json + '\n', flushed per line.
   stderr would be a caller bug - the editor reads exactly this stream.
   g_acp_out redirects the channel for the selfcheck only */
static void acp_send(cJSON *msg) {
    FILE *out = g_acp_out ? g_acp_out : stdout;
    char *s = cJSON_PrintUnformatted(msg);
    if (s) {
        fputs(s, out);
        fputc('\n', out);
        fflush(out);
        cJSON_free(s);
    }
    cJSON_Delete(msg);
}

/* a response envelope; owns id (may be NULL for notifications) */
static void acp_send_result(cJSON *id, cJSON *result) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? id : cJSON_CreateNull());
    cJSON_AddItemToObject(r, "result", result);
    acp_send(r);
}

/* the JSON-RPC error envelope; id null when the request never parsed */
static void acp_send_error(cJSON *id, int code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? id : cJSON_CreateNull());
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "code", (double)code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddItemToObject(r, "error", e);
    acp_send(r);
}

/* ================= record -> session/update translation ================= */

/* the acp kind of a tool: the builtin families and deepwiki map onto
   the protocol's read/edit/execute/fetch/think, everything else (a
   user's own mcp server, now, the helper subagent) is "other" */
static const char *acp_tool_kind(const char *tool) {
    if (!tool) return "other";
    /* the flattened bundled names first (the agent's own surface) */
    if (!strncmp(tool, "file_", 5)) {
        /* the write family: create, edit, copy, move, delete */
        if (!strcmp(tool + 5, "create") || !strcmp(tool + 5, "edit") ||
            !strcmp(tool + 5, "copy") || !strcmp(tool + 5, "move") ||
            !strcmp(tool + 5, "delete"))
            return "edit";
        return "read"; /* file_read, file_analyze */
    }
    if (!strcmp(tool, "files_list") || !strcmp(tool, "files_search"))
        return "read";
    if (!strcmp(tool, "process_exec")) return "execute";
    if (!strncmp(tool, "process_", 8)) return "execute";
    if (!strcmp(tool, "web_search") || !strcmp(tool, "web_fetch"))
        return "fetch";
    if (!strncmp(tool, "repo_", 5)) return "read"; /* deepwiki trio */
    if (!strcmp(tool, "helper")) return "think";
    if (!strcmp(tool, "skills_search") || !strcmp(tool, "skills_read"))
        return "read";
    return "other";
}

/* the tool_call title: the tool name, then the first string argument
   elided to ~60 chars - the editor's tooltip one-liner */
static void acp_tool_title(buf_t *b, const char *tool, const cJSON *args) {
    buf_append_str(b, tool ? tool : "");
    if (!cJSON_IsObject(args)) return;
    for (const cJSON *f = args->child; f; f = f->next) {
        if (!cJSON_IsString(f) || !f->valuestring || !*f->valuestring)
            continue;
        buf_append_str(b, " ");
        /* first 40 + last 15 of a long value, "..." between (the trim
           list's elision shape, tighter for a tooltip) */
        size_t n = strlen(f->valuestring);
        if (n <= 60) {
            buf_append(b, f->valuestring, n);
        } else {
            buf_append(b, f->valuestring, 40);
            buf_append_str(b, "...");
            buf_append(b, f->valuestring + n - 15, 15);
        }
        return; /* the first string argument only */
    }
}

/* {type:"text", text:...} content block */
static cJSON *acp_text_block(const char *text) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text ? text : "");
    return c;
}

cJSON *acp_record_to_update(const char *session_id, const cJSON *record) {
    if (!record) return NULL;
    int k = rec_classify(record);
    if (k != R_RESPONSE && k != R_THINKING && k != R_TOOL_REQUEST &&
        k != R_TOOL_RESPONSE)
        return NULL; /* config, control, errors: not session updates */
    cJSON *u = cJSON_CreateObject();
    if (k == R_RESPONSE || k == R_THINKING) {
        /* partial and final alike: acp streams the chunks as they
           arrive, the client concatenates; the final record's empty
           remainder still goes out (the stream's own close) */
        cJSON_AddStringToObject(
            u, "sessionUpdate",
            k == R_RESPONSE ? "agent_message_chunk" : "agent_thought_chunk");
        cJSON_AddItemToObject(u, "content", acp_text_block(
                                              rec_str(record, "text")));
    } else if (k == R_TOOL_REQUEST) {
        buf_t title;
        buf_init(&title);
        acp_tool_title(&title, rec_str(record, "tool"),
                       cJSON_GetObjectItemCaseSensitive(record, "arguments"));
        cJSON_AddStringToObject(u, "sessionUpdate", "tool_call");
        cJSON_AddStringToObject(
            u, "toolCallId",
            rec_str(record, "id") ? rec_str(record, "id") : "");
        cJSON_AddStringToObject(u, "title", title.data ? title.data : "");
        cJSON_AddStringToObject(u, "kind",
                                acp_tool_kind(rec_str(record, "tool")));
        cJSON_AddStringToObject(u, "status", "pending");
        cJSON *loc = cJSON_CreateObject();
        cJSON_AddItemToObject(loc, "start", cJSON_CreateObject());
        cJSON_AddItemToObject(loc, "end", cJSON_CreateObject());
        cJSON_AddItemToObject(u, "locations", loc);
        buf_free(&title);
    } else { /* R_TOOL_RESPONSE */
        cJSON_AddStringToObject(u, "sessionUpdate", "tool_call_update");
        cJSON_AddStringToObject(
            u, "toolCallId",
            rec_str(record, "id") ? rec_str(record, "id") : "");
        /* acp renders a completed call's content; a failed one reports
           the message through the error field */
        if (rec_bool(record, "is_error", false)) {
            cJSON_AddStringToObject(u, "status", "failed");
            cJSON *er = cJSON_CreateObject();
            cJSON_AddStringToObject(er, "message",
                                    rec_str(record, "text")
                                        ? rec_str(record, "text")
                                        : "");
            cJSON_AddItemToObject(u, "error", er);
        } else {
            cJSON_AddStringToObject(u, "status", "completed");
            cJSON_AddItemToObject(u, "content",
                                  acp_text_block(rec_str(record, "text")));
        }
    }
    /* wrap: the notification envelope the sink sends as-is */
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", "session/update");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "sessionId", session_id ? session_id : "");
    cJSON_AddItemToObject(p, "update", u);
    cJSON_AddItemToObject(n, "params", p);
    return n;
}

/* the engine sink: translate one emitted record and ship it. the sink
   OWNS the record (every llmkit sink consumes it) */
static void acp_sink_fn(void *ctx, cJSON *record) {
    acp_session_t *s = ctx;
    cJSON *n = acp_record_to_update(s ? s->id : NULL, record);
    if (n) acp_send(n);
    cJSON_Delete(record);
}

/* ================= method handlers ================= */

/* {"protocolVersion":1, agentCapabilities:{loadSession:false,
   promptCapabilities:all false}, authMethods:[]} - the fixed banner
   (plan: message loop) */
static int acp_handle_initialize(const cJSON *params, cJSON **result_out) {
    (void)params; /* the client's version is echoed below: only 1 exists */
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "protocolVersion", 1);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "loadSession", false);
    cJSON *pc = cJSON_CreateObject();
    cJSON_AddBoolToObject(pc, "image", false);
    cJSON_AddBoolToObject(pc, "audio", false);
    cJSON_AddBoolToObject(pc, "embeddedContext", false);
    cJSON_AddItemToObject(caps, "promptCapabilities", pc);
    cJSON_AddItemToObject(res, "agentCapabilities", caps);
    cJSON_AddItemToObject(res, "authMethods", cJSON_CreateArray());
    *result_out = res;
    return 0;
}

/* apply one tools record the repl way: validate first, then snapshot */
static int acp_apply_tools(engine_t *e, cJSON *tools) {
    char *m = validate_tools(tools);
    if (m) {
        engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
        free(m);
        return EXIT_INVALID_RECORD;
    }
    engine_apply_config_record(e, tools);
    return 0;
}

/* the bundle: the agent surface minus its interactive-only extras. full
   carries the builtin server, the deepwiki server and AGENTS.md;
   read-only the same with the builtin's file-writing tools hidden
   (deepwiki is read-only by nature, it stays); none attaches nothing */
static int acp_session_bundle(acp_state_t *st, acp_session_t *s,
                              call_cfg_t *c) {
    engine_t *e = s->e;
    /* the bundled default prompt: an explicit --system-prompt replaces
       it (call_compile already applied the flag's own) */
    if (!c->system) {
        c->system = (char *)prompt_system_prompts_agent;
        int rc = call_compile(c, e, st->exe_path);
        c->system = NULL;
        if (rc) return rc;
    }
    cJSON *tools = call_build_agent_tools(
        c, st->exe_path, st->cfg.bundle == ACP_BUNDLE_READ_ONLY, s->cwd);
    int rc = acp_apply_tools(e, tools);
    cJSON_Delete(tools);
    if (rc) return rc;
    /* deepwiki: skipped whole when a server of that name exists (a
       --mcp-proxy config may carry its own), built into the session's
       own temp config otherwise */
    const cJSON *cur =
        e->tools_cfg
            ? cJSON_GetObjectItemCaseSensitive(e->tools_cfg, "tools") : NULL;
    bool taken = false;
    if (cJSON_IsArray(cur))
        for (const cJSON *t = cur->child; t && !taken; t = t->next)
            if (!strcmp(rec_str(t, "name"), "deepwiki")) taken = true;
    if (taken) {
        engine_emit_record(
            e, rec_error(EC_INVALID_RECORD,
                         "a server named 'deepwiki' already exists: the "
                         "bundled deepwiki server is skipped",
                         false));
        return 0;
    }
    cJSON *dw = NULL;
    rc = repl_deepwiki_build(st->exe_path, e, s->deepwiki_cfg,
                             sizeof s->deepwiki_cfg, &dw);
    if (rc) return rc;
    if (!dw) return 0; /* degraded: no temp file, the session lives */
    cJSON *merged =
        e->tools_cfg ? cJSON_Duplicate(e->tools_cfg, 1)
                     : cJSON_Parse("{\"type\":\"tools\",\"tools\":[]}");
    cJSON_AddItemToArray(
        cJSON_GetObjectItemCaseSensitive(merged, "tools"), dw);
    rc = acp_apply_tools(e, merged);
    cJSON_Delete(merged);
    return rc;
}

/* {cwd, mcpServers[]} -> {sessionId, modes:[]}: the engine bridge.
   everything a session needs is built here, once - the flags' llm and
   options first, then the bundle's prompt and servers */
static int acp_handle_session_new(acp_state_t *st, const cJSON *params,
                                  cJSON **result_out) {
    /* the workspace is carried on the session, never a parent chdir:
     * zed reuses this one process across conversations and their cwds
     * may differ, so the only process-global state left is none - the
     * builtin child spawns with its own --cwd (acp_session_bundle) and
     * the AGENTS.md read takes the explicit dir */
    const char *cwd = rec_str(params, "cwd");
    if (cwd && !*cwd) cwd = NULL; /* empty = inherit, same as absent */
    const cJSON *mcp = cJSON_GetObjectItemCaseSensitive(params, "mcpServers");
    if (cJSON_IsArray(mcp) && mcp->child)
        fprintf(stderr,
                "llmkit acp: session/new mcpServers ignored (attach "
                "servers with --mcp-proxy instead)\n");
    acp_session_t *s = acp_session_new(st);
    if (!s) {
        *result_out = NULL;
        return -1;
    }
    if (cwd) {
        s->cwd = strdup(cwd);
        if (!s->cwd) goto fail;
    }
    s->e = engine_new(acp_sink_fn, s);
    if (!s->e) goto fail;
    if (g_acp_wire_factory) /* the selfcheck's fake wire (seam) */
        s->e->wire_factory = g_acp_wire_factory;
    s->e->keep_mcp = true;  /* the session continues past turn endings */
    s->e->llm_retry = true; /* agent semantics: fibonacci llm retries */
    call_cfg_t c;
    acp_call_cfg(&st->cfg, &c);
    int rc = call_compile(&c, s->e, st->exe_path);
    if (rc == 0 && st->cfg.bundle != ACP_BUNDLE_NONE)
        rc = acp_session_bundle(st, s, &c);
    if (rc == 0) {
        cJSON *opts = repl_build_options();
        engine_apply_config_record(s->e, opts);
        cJSON_Delete(opts);
    }
    if (rc == 0 && st->cfg.bundle != ACP_BUNDLE_NONE) {
        /* AGENTS.md rides after the prompt - read from the session's
           own workspace dir, never a process-global cwd */
        rc = inject_agents_md(s->e, s->cwd, c.system);
    }
    if (rc) {
    fail:
        acp_session_free(s);
        st->nsessions--; /* the failed slot never existed for the client */
        *result_out = NULL;
        return -1;
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "sessionId", s->id);
    cJSON_AddItemToObject(res, "modes", cJSON_CreateArray());
    *result_out = res;
    return 0;
}

/* join the prompt's text blocks with \n; false when a block of another
   type appears (we advertised promptCapabilities all false) or no text
   arrived at all */
static bool acp_prompt_join(const cJSON *blocks, buf_t *out) {
    if (!cJSON_IsArray(blocks)) return false;
    bool any = false;
    for (const cJSON *b = blocks->child; b; b = b->next) {
        const char *ty = rec_str(b, "type");
        if (!ty || strcmp(ty, "text")) return false;
        const char *tx = rec_str(b, "text");
        if (!tx) return false;
        if (any) buf_append_byte(out, '\n');
        buf_append_str(out, tx);
        any = true;
    }
    return any;
}

/* the engine's ending -> the acp stop reason. the transcript's last
   finish_reason is the endpoint's own word for it (the wires normalize
   length and content_filter), the exit code carries the rest */
static const char *acp_stop_reason(int rc, const cJSON *last) {
    if (rc == EXIT_INTERRUPTED) return "cancelled";
    const char *fr = rec_str(last, "finish_reason");
    if (fr) {
        if (!strcmp(fr, "length")) return "max_tokens";
        if (!strcmp(fr, "content_filter")) return "refusal";
    }
    if (rc == EXIT_OK) return "end_turn";
    /* fatal endings have no acp stop reason of their own: the error
       records already reached the log, the client sees a plain turn
       end (the protocol has no channel for an agent-side failure) */
    fprintf(stderr, "llmkit acp: turn ended with exit code %d\n", rc);
    return "end_turn";
}

/* the transcript's last final response/tool_request record: the one
   whose finish_reason the stop reason reads */
static const cJSON *acp_last_finish(const engine_t *e) {
    for (size_t i = e->tr.n; i > 0; i--) {
        const trec_t *r = e->tr.v[i - 1];
        if (r->kind != T_TEXT && r->kind != T_TREQ) continue;
        if (r->tree) return r->tree;
    }
    return NULL;
}

/* {sessionId, prompt:[blocks]} -> the turn: the user record, one
   engine_run, the streamed updates went out through the sink already.
   returns 0 with *result_out set, or a negative error code */
static int acp_handle_session_prompt(acp_state_t *st, const cJSON *params,
                                     cJSON **result_out) {
    acp_session_t *s = acp_session_find(st, rec_str(params, "sessionId"));
    if (!s || !s->e) {
        *result_out = NULL;
        return -2;
    }
    if (s->running) { /* one turn at a time: the protocol is serial */
        *result_out = NULL;
        return -4;
    }
    buf_t text;
    buf_init(&text);
    bool ok = acp_prompt_join(
        cJSON_GetObjectItemCaseSensitive(params, "prompt"), &text);
    if (!ok) {
        buf_free(&text);
        *result_out = NULL;
        return -5; /* -32602: a block type we never advertised */
    }
    cJSON *user = call_build_user(text.data ? text.data : "");
    buf_free(&text);
    char *m = validate_content(
        cJSON_GetObjectItemCaseSensitive(user, "content"));
    if (m) {
        engine_emit_record(s->e, rec_error(EC_INVALID_RECORD, m, true));
        free(m);
        cJSON_Delete(user);
        *result_out = NULL;
        return -1;
    }
    tlist_ingest(&s->e->tr, user); /* the turn's user record, repl rule */
    cJSON_Delete(user);
    s->running = true;
    int rc = engine_start(s->e);
    if (rc == 0) rc = engine_run(s->e);
    s->running = false;
    /* a session/cancel or SIGINT the turn consumed: the flag is not
       ours to keep (the repl clears it the same way) */
    if (g_stop_flag && rc == EXIT_INTERRUPTED) g_stop_flag = 0;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "stopReason",
                            acp_stop_reason(rc, acp_last_finish(s->e)));
    *result_out = res;
    return 0;
}

/* the engine-visible stop shared by the cancel paths. the flag the
   repl's SIGINT handler raises is the same one engine_run checks at
   every wire and tool boundary (and the wire's curl write callback
   aborts on), so setting it ends the in-flight turn at its next
   boundary; without a running turn it is a no-op */
static void acp_raise_stop(acp_state_t *st, const char *session_id) {
    acp_session_t *s = acp_session_find(st, session_id);
    if (!s || !s->running) return; /* nothing in flight: the no-op */
    fprintf(stderr, "llmkit acp: session/cancel: stopping the turn\n");
    g_stop_flag = 1; /* engine_run's own orderly-stop trigger */
}

/* session/cancel {sessionId} */
static void acp_handle_cancel(acp_state_t *st, const cJSON *params) {
    acp_raise_stop(st, rec_str(params, "sessionId"));
}

/* ================= dispatch ================= */

/* one parsed message: route by method, answer requests, log
   notifications */
static void acp_serve_message(acp_state_t *st, cJSON *msg) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *idc =
        (id && !cJSON_IsNull(id)) ? cJSON_Duplicate(id, true) : NULL;
    if (!cJSON_IsString(method) || !method->valuestring) {
        /* no method: only a stray response to a request we never made */
        if (idc) {
            acp_send_error(idc, -32600, "invalid request: no method");
        } else {
            fprintf(stderr, "llmkit acp: dropped a message with no method\n");
            cJSON_Delete(idc);
        }
        return;
    }
    const char *m = method->valuestring;
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    cJSON *params_own = params ? cJSON_Duplicate(params, true) : NULL;
    int rc = 0;
    cJSON *result = NULL;
    if (!strcmp(m, "initialize")) {
        rc = acp_handle_initialize(params_own, &result);
    } else if (!strcmp(m, "session/new")) {
        rc = acp_handle_session_new(st, params_own, &result);
    } else if (!strcmp(m, "session/prompt")) {
        rc = acp_handle_session_prompt(st, params_own, &result);
    } else if (!strcmp(m, "session/cancel")) {
        acp_handle_cancel(st, params_own);
        rc = 2;
    } else {
        rc = -1;
    }
    cJSON_Delete(params_own);
    if (rc == 2) { /* handled; answered only when the client asked */
        if (idc) acp_send_result(idc, cJSON_CreateObject());
        else fprintf(stderr, "llmkit acp: %s handled without a reply\n", m);
        return;
    }
    if (!idc) { /* a notification: log, never answer */
        fprintf(stderr, "llmkit acp: notification '%s' ignored\n", m);
        return;
    }
    if (rc == -2) {
        acp_send_error(idc, -32602, "invalid params: unknown session id");
        return;
    }
    if (rc == -3) {
        acp_send_error(idc, -32602, "invalid params: cwd not accessible");
        return;
    }
    if (rc == -4) {
        acp_send_error(idc, -32602,
                       "invalid params: a turn is already running");
        return;
    }
    if (rc == -5) {
        acp_send_error(idc, -32602,
                       "invalid params: prompt blocks must be text");
        return;
    }
    if (rc == -1) { /* unknown method (JSON-RPC 2.0 sec.5.1) */
        acp_send_error(idc, -32601, "method not found");
        return;
    }
    if (rc != 0 || !result) {
        acp_send_error(idc, -32603, "internal error");
        return;
    }
    acp_send_result(idc, result);
}

/* one line in: parse, serve, answer a parse error with id null
   (JSON-RPC 2.0 sec.5.1); serving never stops the loop */
static void acp_serve_line(void *ctx, char *line) {
    acp_state_t *st = ctx;
    cJSON *msg = jsonl_parse_line(line);
    if (!msg) {
        /* jsonl.c keeps lines single-object; an array would be a
           JSON-RPC batch, and ACP never batches (plan: message loop) */
        cJSON *t = cJSON_Parse(line);
        if (t && cJSON_IsArray(t)) {
            cJSON_Delete(t);
            acp_send_error(NULL, -32600,
                           "invalid request: batches not supported");
            return;
        }
        cJSON_Delete(t);
        acp_send_error(NULL, -32700, "parse error");
        return;
    }
    acp_serve_message(st, msg);
    cJSON_Delete(msg);
}

/* ================= the reader thread ================= */

/* raw lines from stdin into the queue; the main thread owns parsing,
   the engine and every write (plan: threading contract). one exception
   rides inline: a session/cancel whose turn is running moves the stop
   flag right here, because the main thread sits inside engine_run
   until the next wire or tool boundary and only this thread is awake
   to answer. the request still queues: the main thread sends the {}
   reply once the turn answered */
static void acp_reader_on_line(void *ctx, char *line) {
    acp_state_t *st = ctx;
    cJSON *msg = jsonl_parse_line(line);
    if (msg) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(msg, "method");
        if (cJSON_IsString(m) && m->valuestring &&
            !strcmp(m->valuestring, "session/cancel"))
            acp_raise_stop(
                st, rec_str(cJSON_GetObjectItemCaseSensitive(msg, "params"),
                            "sessionId"));
        cJSON_Delete(msg);
    }
    queue_push(st->requests, line);
}

static void *acp_reader_thread(void *arg) {
    acp_state_t *st = arg;
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, acp_reader_on_line, st);
    char buf[8192];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop_flag) break; /* nothing after the stop */
                continue;
            }
            break;
        }
        if (n == 0) break;
        if (jsonl_feed(&p, buf, (size_t)n) != 0) break; /* bad bytes */
    }
    jsonl_pusher_free(&p); /* drops a trailing fragment without \n */
    queue_close(st->requests);
    return NULL;
}

/* ================= framing loop ================= */

/* pop requests, serve them, stop at EOF. a turn blocks the loop by
   design: the protocol is one request at a time, and the reader thread
   holds whatever arrives mid-turn (a cancel included - it takes effect
   when the engine's next boundary check reads the flag) */
static int acp_framing_loop(acp_state_t *st) {
    for (;;) {
        char *line = queue_pop_timeout(st->requests, 0.2);
        if (line) {
            acp_serve_line(st, line);
            free(line);
            continue;
        }
        /* timeout: the stop flag (SIGINT) and EOF both end the loop */
        if (g_stop_flag && !queue_closed(st->requests)) continue;
        if (queue_closed(st->requests)) {
            /* the reader finished: drain what it queued, then stop */
            while ((line = queue_try_pop(st->requests))) {
                acp_serve_line(st, line);
                free(line);
            }
            return g_stop_flag ? EXIT_INTERRUPTED : EXIT_OK;
        }
    }
}

/* ================= test hooks (selfcheck) ================= */

/* the protocol output stream: stdout in production, a memstream under
   the selfcheck so the emitted ndjson is assertable. every write goes
   through it, the framing stays byte-identical */
FILE *g_acp_out = NULL;
/* the wire factory seam (selfcheck): every session engine picks it up,
   a fake wire turns a turn into a scripted response without network */
wire_t *(*g_acp_wire_factory)(struct engine *);

void *acp_test_state_new(const char *flag_line) {
    acp_state_t *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    /* split the flag line the way a shell would: spaces, no quoting
       needed for the flags the tests use */
    char *dup = strdup(flag_line);
    char *argv[32];
    int argc = 0;
    if (dup) {
        char *tok = strtok(dup, " ");
        while (tok && argc < 32) {
            argv[argc++] = tok;
            tok = strtok(NULL, " ");
        }
    }
    char err[256] = "";
    if (!dup || acp_parse(argc, argv, &st->cfg, err, sizeof err) != 0) {
        fprintf(stderr, "llmkit acp: test state: %s\n", err);
        free(dup);
        acp_state_free(st);
        return NULL;
    }
    free(dup);
    self_exe(st->exe_path, sizeof st->exe_path, "llmkit");
    st->requests = queue_new();
    return st;
}

void acp_test_serve(void *vst, const char *line) {
    acp_state_t *st = vst;
    char *dup = strdup(line);
    if (!dup) return;
    acp_serve_line(st, dup);
    free(dup);
}

void acp_test_state_free(void *vst) {
    acp_state_t *st = vst;
    if (!st) return;
    if (st->requests) queue_free(st->requests);
    acp_state_free(st);
}

/* ================= command ================= */

static void acp_usage(FILE *out) {
    fputs("usage: llmkit acp (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "              [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "              [--reasoning-effort <value>] "
          "[--system-prompt <text>]\n"
          "              [--header <name=value>]...\n"
          "              [--mcp-proxy <config>]... "
          "[--bundle full|read-only]\n"
          "\n"
          "       an agent client protocol server on stdio: ndjson\n"
          "       json-rpc in, ndjson json-rpc out, one session per\n"
          "       session/new; --bundle attaches the agent toolset\n"
          "       (builtin mcp + deepwiki), read-only without the\n"
          "       file-writing tools\n",
          out);
}

int cmd_acp(int argc, char **argv) {
    signals_init();
    acp_state_t st;
    memset(&st, 0, sizeof st);
    char err[256] = "";
    if (acp_parse(argc - 2, argv + 2, &st.cfg, err, sizeof err) != 0) {
        fprintf(stderr, "llmkit acp: %s\n", err);
        acp_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    if (!utf8_valid((const uint8_t *)st.cfg.api_base,
                    strlen(st.cfg.api_base)) ||
        (st.cfg.key &&
         !utf8_valid((const uint8_t *)st.cfg.key, strlen(st.cfg.key)))) {
        fprintf(stderr, "llmkit acp: invalid UTF-8 in a flag value\n");
        acp_state_free(&st);
        return EXIT_OUT_OF_CHANNEL;
    }
    if (strncasecmp(st.cfg.api_base, "http://", 7) != 0 &&
        strncasecmp(st.cfg.api_base, "https://", 8) != 0) {
        fprintf(stderr,
                "llmkit acp: <api_base> must start with http:// or "
                "https://\n");
        acp_state_free(&st);
        acp_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    self_exe(st.exe_path, sizeof st.exe_path, argv ? argv[0] : NULL);
    /* protocol only on stdout: the ndjson loop below owns fd 1, every
       log lands on stderr (plan: framing) */
    st.requests = queue_new();
    thread_start_detached(acp_reader_thread, &st);
    int rc = acp_framing_loop(&st);
    queue_close(st.requests);
    acp_state_free(&st);
    return rc;
}
