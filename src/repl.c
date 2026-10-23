/* repl.c - llmkit repl and llmkit agent: the interactive chat front-end
   (design sec.12).
   call's compiler minus --prompt, one session loop over one engine, and
   the display sink: ascii separators, tty-gated bold and italic,
   thinking and tool traffic rendered. Input is the shared raw-mode line
   editor of src/editor.c on a tty - the two Ctrl-C stages and the typed
   echo lean on it - or its plain line loop on anything else. Without
   --system-prompt the session starts from the bundled default prompt
   (src/prompts/system_prompts/repl.txt, tools/gen-prompts.sh).
   `agent` is the same session with the extras of repl_opts_t: the
   built-in mcp server rides the tools record, ./AGENTS.md is injected
   as a second system content block, and --conversation-store persists
   the conversation as jsonl - the runner's record catalogue - replaying
   an existing file's records through the prettyprint renderer before
   the first prompt. */
#include "llmkit.h"
#include "prompts.gen.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

/* the line that prefixes the AGENTS.md content block */
#define AGENTS_MD_PREFIX "##### Content of AGENTS.md #####\n"

/* ================= display sink ================= */
/* the shared framing state machine (pretty.c) plus the session's own
   extras: the conversation store and the turn's timing spans and token
   totals */

typedef struct repl_sink {
    dspy_t d; /* the framing state (out, styles, rules) */
    /* the conversation store: appended per completed record when set */
    FILE *store;
    bool store_fail; /* a store write failed: io_error exit 7 */
    /* per-turn timing, the mono clock taken at sink depth: reset by the
       session loop before each engine_run */
    double t_turn;   /* the request clock: prompt processing counts here */
    double t_first;  /* first streamed token of the turn, 0 until it lands */
    double t_think;  /* open thinking generation segment, 0 when closed */
    double t_resp;   /* open response generation segment, 0 when closed */
    double think_s;  /* thinking generation, accumulated over all blocks */
    double resp_s;   /* response generation, accumulated over all blocks */
    bool resp_done;  /* a response block completed this turn */
    double turn_s;   /* the turn's wall time, from the final record */
    /* the turn's tokens as the endpoint reported them: each model round
       re-sends the whole conversation, so the reading that matches the
       endpoint's own log is the LAST round's - input is the conversation's
       size at the turn's end, output that round's generation */
    double tok_in;   /* input tokens */
    double tok_out;  /* output tokens */
    bool have_usage; /* a record of this turn reported usage */
    /* the agent's quiet display: thinking text suppressed to one
       `thinking...` line per turn (--verbose renders it whole) */
    bool quiet_think; /* armed at startup, never reset */
    bool think_shown; /* this turn's line drew already */
    bool think_done;  /* this turn's thinking blocks all closed */
} repl_sink_t;

/* the store's write side: called from the sink for every emitted record
   and from the loop for every submitted user record */
static void store_persist(repl_sink_t *s, const cJSON *rec) {
    if (!s->store || s->store_fail) return;
    int k = rec_classify(rec);
    /* every transcript record, partials included: a streamed block
       arrives as partial chunks plus an empty-text final, the text only
       exists whole in that sequence (the runner's stdout stream, refolded
       by tlist_ingest on load). Config and control records are not
       conversation. */
    bool keep = k == R_USER || k == R_ERROR || k == R_TOOL_REQUEST ||
                k == R_TOOL_RESPONSE || k == R_THINKING || k == R_RESPONSE;
    if (!keep) return;
    char *p = cJSON_PrintUnformatted((cJSON *)rec);
    if (!p) return;
    size_t n = strlen(p);
    if (fwrite(p, 1, n, s->store) != n || fputc('\n', s->store) == EOF ||
        fflush(s->store) != 0)
        s->store_fail = true;
    cJSON_free(p);
}

/* the turn's timing spans and token totals, reset by the session loop at
   turn start */
static void timing_reset(repl_sink_t *s) {
    s->t_turn = mono_now();
    s->t_first = 0;
    s->t_think = 0;
    s->t_resp = 0;
    s->think_s = 0;
    s->resp_s = 0;
    s->turn_s = 0;
    s->resp_done = false;
    s->tok_in = 0;
    s->tok_out = 0;
    s->have_usage = false;
    s->think_shown = false; /* the turn's quiet `thinking...` line */
    s->think_done = false;
}

/* the turn's token reading, round by round: usage rides the last record
   of each model round (the wires' rule), so a turn whose tools took
   several rounds reports several. Every round's input already counts the
   whole conversation again - summing the rounds would inflate the turn
   far past the endpoint's own numbers - so the LAST round's usage is
   kept instead: input = the conversation's size at the turn's end,
   output = the final round's generation, exactly what the endpoint
   logged for the turn's last request. Absent usage is simply not
   counted - the rendering shows only what the endpoint reported */
static void usage_accum(repl_sink_t *s, const cJSON *rec) {
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(rec, "usage");
    if (!cJSON_IsObject(u)) return;
    s->tok_in = rec_num(u, "input_tokens", 0.0);
    s->tok_out = rec_num(u, "output_tokens", 0.0);
    s->have_usage = true;
}

/* the turn's stats line, drawn at every request's end whatever the
   termination: response completed, interrupted, fatal, max rounds. The
   spans shown are whatever accumulated - partial or zero on an end that
   never completed a response block. The turn's wall time falls back to
   the sink's own clock when the engine never stamped a final record.
   The exit code shows when the end was not EXIT_OK */
static void render_stats_exit(repl_sink_t *s, int rc) {
    char ts[32] = "", tok[96] = "", line[320];
    stamp_now(ts, sizeof ts);
    if (s->have_usage)
        snprintf(tok, sizeof tok,
                 " | input %.0f tok | output %.0f tok | total %.0f tok",
                 s->tok_in, s->tok_out, s->tok_in + s->tok_out);
    double turn = s->turn_s > 0 ? s->turn_s : mono_now() - s->t_turn;
    int n;
    if (rc == EXIT_OK)
        n = snprintf(line, sizeof line,
                     "%sfirst token %.2fs | thinking %.2fs | response %.2fs"
                     "%s | turn %.2fs\n",
                     ts, s->t_first ? s->t_first - s->t_turn : 0.0,
                     s->think_s, s->resp_s, tok, turn);
    else
        n = snprintf(line, sizeof line,
                     "%sfirst token %.2fs | thinking %.2fs | response %.2fs"
                     "%s | turn %.2fs | ended %d\n",
                     ts, s->t_first ? s->t_first - s->t_turn : 0.0,
                     s->think_s, s->resp_s, tok, turn, rc);
    if (n <= 0) return;
    dspy_ensure_nl(&s->d);
    dspy_write(&s->d, line,
               (size_t)n >= sizeof line ? sizeof line - 1 : (size_t)n);
}

static void repl_sink_fn(void *ctx, cJSON *rec) {
    repl_sink_t *s = ctx;
    store_persist(s, rec);
    usage_accum(s, rec); /* the turn's totals, round by round */
    int k = rec_classify(rec);
    const char *tx = rec_str(rec, "text");
    if (k == R_THINKING && s->quiet_think) {
        /* agent's quiet display: one line stands for the block, the
           streamed text never renders (timing still counts it). the
           line draws at the block's start - the final-only block's own
           record or the first partial - so a thinking-only turn shows
           its line even when no text chunk survives the wire */
        bool partial = rec_bool(rec, "partial", false);
        if (!partial) s->think_done = true;
        if (!s->think_shown && (!partial || (tx && *tx))) {
            s->think_shown = true;
            s->d.sep_pending = true;
            dspy_styled(&s->d, s->d.st->italic, "thinking...");
            dspy_ensure_nl(&s->d);
            s->d.cur = -1;
        }
        if (tx && *tx) { /* the token clock still counts the thinking */
            double now = mono_now();
            if (!s->t_first) s->t_first = now;
            if (!s->t_think) s->t_think = now;
        }
        if (s->think_done && s->t_think) {
            double now = mono_now();
            s->think_s += now - s->t_think;
            s->t_think = 0;
        }
    } else if (k == R_THINKING || k == R_RESPONSE) {
        /* arm the separator only when something will render: the wire's
           block-close records carry empty text (signature, usage) and
           must not open a block that never draws */
        if (tx && *tx && s->d.cur != k) {
            s->d.sep_pending = true;
            s->d.cur = k;
        }
        if (tx && *tx) { /* a token landed: open its generation segment */
            double now = mono_now();
            if (!s->t_first) s->t_first = now;
            if (k == R_THINKING) {
                if (!s->t_think) s->t_think = now;
            } else if (!s->t_resp) {
                s->t_resp = now;
            }
        }
        dspy_styled(&s->d, k == R_THINKING ? s->d.st->italic : "", tx);
        if (!rec_bool(rec, "partial", false)) {
            double now = mono_now(); /* the block closed its segment */
            if (k == R_THINKING) {
                if (s->t_think) {
                    s->think_s += now - s->t_think;
                    s->t_think = 0;
                }
            } else {
                if (s->t_resp) {
                    s->resp_s += now - s->t_resp;
                    s->t_resp = 0;
                }
                s->resp_done = true;
                s->turn_s = rec_num(rec, "duration_s", 0.0);
            }
            dspy_ensure_nl(&s->d); /* per-block trailing newline, call's rule */
            s->d.cur = -1;
        }
    } else if (k == R_TOOL_REQUEST) {
        dspy_tool_request(&s->d, rec);
    } else if (k == R_TOOL_RESPONSE) {
        dspy_brief_count(&s->d, rec);
        dspy_tool_response(&s->d, rec);
    } else if (k == R_ERROR) {
        dspy_error_line(&s->d, rec_str(rec, "code"), rec_str(rec, "message"));
    }
    /* everything else - start markers above all - renders nothing */
    cJSON_Delete(rec);
}

/* the user block in non-tty mode: no editor echoed it, the loop renders it */
static void render_user_block(repl_sink_t *s, const char *text) {
    dspy_rule(&s->d, '='); /* heavy rule opens each user block */
    dspy_styled(&s->d, s->d.st->bold, text);
    dspy_ensure_nl(&s->d);
}

/* ================= input: line results ================= */
/* (ED_SUBMIT/ED_EOF/ED_CLEAR/ED_QUIT/ED_BAD live in llmkit.h: the editor
   is shared with mcp-repl, src/editor.c) */

/* ================= conversation store ================= */
/* the store is the conversation as jsonl - the runner's record
   catalogue, exactly what prettyprint renders and what a resumed
   session replays into its transcript. One line per transcript record,
   appended as the session runs. */

/* ---- store load: replay through the prettyprint renderer, records
   into the transcript ---- */

typedef struct store_load {
    engine_t *e;
    pretty_live_t *live;
    int rc; /* EXIT_OK until a line fails */
} store_load_t;

static void store_load_fail(store_load_t *sc, const char *msg) {
    if (sc->rc != EXIT_OK) return;
    sc->rc = EXIT_INVALID_RECORD;
    cJSON *err = rec_error(EC_INVALID_RECORD, msg, true);
    pretty_live_record(sc->live, err);
    cJSON_Delete(err);
}

static void store_load_line(void *ctx, char *line /*malloc'd*/) {
    store_load_t *sc = ctx;
    if (sc->rc != EXIT_OK) {
        free(line);
        return;
    }
    cJSON *rec = jsonl_parse_line(line);
    free(line);
    if (!rec) {
        store_load_fail(sc, "malformed json line in the conversation store");
        return;
    }
    int k = rec_classify(rec);
    switch (k) {
    case R_USER:
    case R_THINKING:
    case R_RESPONSE:
    case R_TOOL_REQUEST:
    case R_TOOL_RESPONSE:
    case R_ERROR: {
        if (k == R_USER) {
            char *m = validate_content(
                cJSON_GetObjectItemCaseSensitive(rec, "content"));
            if (m) {
                store_load_fail(sc, m);
                free(m);
                cJSON_Delete(rec);
                return;
            }
        }
        tlist_ingest(&sc->e->tr, rec); /* duplicates what it needs */
        pretty_live_record(sc->live, rec);
        cJSON_Delete(rec);
        return;
    }
    default: /* config and control records: not conversation */
        cJSON_Delete(rec);
        return;
    }
}

/* replay a store onto out and into the engine transcript; 0 ok - a file
   that does not exist is a fresh conversation - else the exit code */
static int store_load(engine_t *e, FILE *out, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    pretty_live_t *live = pretty_live_new(out);
    if (!live) {
        fclose(f);
        return EXIT_OUT_OF_CHANNEL;
    }
    store_load_t sc = { e, live, EXIT_OK };
    bool ok = jsonl_read_stream(f, store_load_line, &sc);
    fclose(f);
    int rc = sc.rc;
    if (!ok) {
        if (rc == EXIT_OK)
            store_load_fail(&sc,
                            "invalid utf-8 or NUL byte in the conversation "
                            "store");
        rc = EXIT_INVALID_RECORD;
    }
    if (rc == EXIT_OK) {
        /* a store that ends mid-block - an interrupted turn's trailing
           partial: the block closes, the next turn starts a fresh one */
        if (e->tr.n) {
            trec_t *last = e->tr.v[e->tr.n - 1];
            if ((last->kind == T_THINK || last->kind == T_TEXT) &&
                !last->complete)
                last->complete = true;
        }
    }
    if (pretty_live_io_failed(live)) rc = EXIT_OUT_OF_CHANNEL;
    pretty_live_flush(live); /* held brief request lines, not lost */
    pretty_live_free(live);
    return rc;
}

/* ---- the second system entry: ./AGENTS.md ---- */

/* one more text block appended to the session's system record: the
   record's current blocks are kept (empty prompt text and no record
   yet leave the block standing alone), the new one rides last.
   validate_content vets the assembled record */
static int system_append_block(engine_t *e, const char *prompt_text,
                               const char *block) {
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "type", "system");
    cJSON *content = cJSON_AddArrayToObject(sys, "content");
    const cJSON *cur =
        e->system
            ? cJSON_GetObjectItemCaseSensitive(e->system, "content") : NULL;
    if (cJSON_IsArray(cur))
        for (const cJSON *b = cur->child; b; b = b->next)
            cJSON_AddItemToArray(content, cJSON_Duplicate((cJSON *)b, 1));
    if (prompt_text && *prompt_text &&
        !(cJSON_IsArray(cur) && cur->child)) { /* nothing carried yet */
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        cJSON_AddStringToObject(blk, "text", prompt_text);
        cJSON_AddItemToArray(content, blk);
    }
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "text");
    cJSON_AddStringToObject(blk, "text", block);
    cJSON_AddItemToArray(content, blk);
    int rc = 0;
    char *m = validate_content(content);
    if (m) {
        engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
        free(m);
        rc = EXIT_INVALID_RECORD;
    } else {
        engine_apply_config_record(e, sys);
    }
    cJSON_Delete(sys);
    return rc;
}

/* the prompt's own content block stays first; the file's content rides
   in its own block after it, prefixed with the content line */
static int inject_agents_md(engine_t *e, const char *prompt_text) {
    FILE *f = fopen("AGENTS.md", "r");
    if (!f) return 0; /* absent: nothing to inject */
    buf_t b;
    buf_init(&b);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&b, tmp, n);
    bool ioerr = ferror(f) != 0;
    fclose(f);

    int rc = 0;
    if (ioerr) {
        engine_emit_record(e,
                           rec_error(EC_IO_ERROR, "cannot read AGENTS.md", true));
        rc = EXIT_IO_ERROR;
    } else {
        if (b.len >= 3 && !memcmp(b.data, "\xef\xbb\xbf", 3)) {
            memmove(b.data, b.data + 3, b.len - 3); /* editor artifact */
            b.len -= 3;
        }
        if (!utf8_valid((const uint8_t *)(b.data ? b.data : ""), b.len)) {
            engine_emit_record(e,
                               rec_error(EC_INVALID_RECORD,
                                         "invalid UTF-8 in AGENTS.md", true));
            rc = EXIT_INVALID_RECORD;
        } else {
            buf_t tx;
            buf_init(&tx);
            buf_append_str(&tx, AGENTS_MD_PREFIX);
            buf_append(&tx, b.data ? b.data : "", b.len);
            rc = system_append_block(e, prompt_text,
                                     tx.data ? tx.data : "");
            buf_free(&tx);
        }
    }
    buf_free(&b);
    return rc;
}

/* ================= the helper subagent ================= */

/* one temp jsonl per session, both writable only by us: the seed the
   subagent boots from and the proxy config that renames its tool. Same
   recipe as builtin.c's proc_mktemp, minus the reuse-the-pid trick. */
static FILE *repl_mktemp(char *path, size_t sz) {
#ifdef _WIN32
    static unsigned seq;
    char dir[264];
    DWORD dn = GetTempPathA(sizeof dir, dir);
    if (!dn || dn >= sizeof dir) return NULL;
    for (int i = 0; i < 4096; i++) {
        snprintf(path, sz, "%sllmkit-helper-%lu-%u.tmp", dir,
                 (unsigned long)GetCurrentProcessId(), ++seq);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_NOINHERIT);
            if (fd < 0) {
                CloseHandle(h);
                return NULL;
            }
            return fdopen(fd, "wb");
        }
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ACCESS_DENIED)
            return NULL;
    }
    return NULL;
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    if (snprintf(path, sz, "%s/llmkit-helper-XXXXXX", dir) >= (int)sz &&
        snprintf(path, sz, "/tmp/llmkit-helper-XXXXXX") >= (int)sz)
        return NULL; /* an overlong TMPDIR falls back, then gives up */
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    return fdopen(fd, "w");
#endif
}

static void write_jsonl_line(FILE *f, const cJSON *rec) {
    char *p = cJSON_PrintUnformatted((cJSON *)rec);
    if (p) {
        fputs(p, f);
        fputc('\n', f);
        cJSON_free(p);
    }
}

static void helper_fail(engine_t *e, const char *what) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "cannot prepare the helper subagent: %s failed", what);
    engine_emit_record(e, rec_error(EC_IO_ERROR, msg, true));
}

int repl_helper_build(const call_cfg_t *c, const char *system_prompt,
                      const cJSON *base_tools, const char *exe_path,
                      bool verbose, engine_t *e, char *seed_path,
                      size_t seed_sz, char *cfg_path, size_t cfg_sz,
                      cJSON **srv_out) {
    *srv_out = NULL;
    *seed_path = *cfg_path = '\0';
    FILE *seed = repl_mktemp(seed_path, seed_sz);
    if (!seed) {
        helper_fail(e, "the seed temp file");
        return EXIT_IO_ERROR;
    }
    FILE *cfg = repl_mktemp(cfg_path, cfg_sz);
    if (!cfg) {
        fclose(seed);
        remove(seed_path);
        *seed_path = '\0';
        helper_fail(e, "the config temp file");
        return EXIT_IO_ERROR;
    }
    /* the seed: header, the session's llm and system records, the
       subagent's tools (the session's servers minus the helper entry
       itself - the recursion cut), and the agent-as-tool record */
    cJSON *hdr = cJSON_Parse("{\"type\":\"header\",\"version\":1}");
    write_jsonl_line(seed, hdr);
    cJSON_Delete(hdr);
    cJSON *llm = call_build_llm(c);
    write_jsonl_line(seed, llm);
    cJSON_Delete(llm);
    /* the session's --verbose: the helper's live mirror renders the
       whole thinking and tool traffic; absent (the quiet default) it
       draws one thinking line, trimmed arguments, reply summaries */
    if (verbose) {
        cJSON *opts = cJSON_Parse(
            "{\"type\":\"options\",\"verbose\":true}");
        write_jsonl_line(seed, opts);
        cJSON_Delete(opts);
    }
    if (system_prompt && *system_prompt) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "type", "system");
        cJSON *content = cJSON_AddArrayToObject(sys, "content");
        cJSON *blk = cJSON_CreateObject();
        cJSON_AddStringToObject(blk, "type", "text");
        cJSON_AddStringToObject(blk, "text", system_prompt);
        cJSON_AddItemToArray(content, blk);
        write_jsonl_line(seed, sys);
        cJSON_Delete(sys);
    }
    if (base_tools) {
        const cJSON *list =
            cJSON_GetObjectItemCaseSensitive(base_tools, "tools");
        if (cJSON_IsArray(list) && list->child) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "tools");
            cJSON *arr = cJSON_AddArrayToObject(t, "tools");
            for (const cJSON *s = list->child; s; s = s->next)
                if (strcmp(rec_str(s, "name"), "helper"))
                    cJSON_AddItemToArray(arr, cJSON_Duplicate((cJSON *)s, 1));
            write_jsonl_line(seed, t);
            cJSON_Delete(t);
        }
    }
    /* the agent-as-tool record, its two descriptions bundled with the
       rest of the prompt texts (src/prompts/mcp/helper/) */
    cJSON *aat = cJSON_CreateObject();
    cJSON_AddStringToObject(aat, "type", "agent-as-tool");
    cJSON_AddStringToObject(aat, "tool_description",
                            prompt_mcp_helper_description);
    cJSON_AddStringToObject(aat, "input_description",
                            prompt_mcp_helper_arguments_prompt);
    write_jsonl_line(seed, aat);
    cJSON_Delete(aat);
    fclose(seed);

    /* the proxy config: one server exposing the subagent, one expose
       renaming its tool `helper(prompt)` */
    buf_t cl;
    buf_init(&cl);
    call_shell_quote(&cl, exe_path);
    buf_append_str(&cl, " agent-as-tool ");
    call_shell_quote(&cl, seed_path);
    cJSON *tools = cJSON_CreateObject();
    cJSON_AddStringToObject(tools, "type", "tools");
    cJSON *arr = cJSON_AddArrayToObject(tools, "tools");
    cJSON *srv1 = cJSON_CreateObject();
    cJSON_AddStringToObject(srv1, "type", "stdio");
    cJSON_AddStringToObject(srv1, "name", "as-tool");
    cJSON_AddStringToObject(srv1, "command_line", cl.data);
    cJSON_AddItemToArray(arr, srv1);
    write_jsonl_line(cfg, tools);
    cJSON_Delete(tools);
    buf_free(&cl);
    cJSON *expose = cJSON_CreateObject();
    cJSON_AddStringToObject(expose, "type", "expose");
    cJSON_AddStringToObject(expose, "tool", "as-tool.invoke");
    cJSON_AddStringToObject(expose, "name", "helper");
    cJSON *args = cJSON_AddObjectToObject(expose, "arguments");
    cJSON *arg = cJSON_CreateObject();
    cJSON_AddStringToObject(arg, "name", "prompt");
    cJSON_AddItemToObject(args, "input", arg);
    write_jsonl_line(cfg, expose);
    cJSON_Delete(expose);
    fclose(cfg);

    /* the session's tools entry: the proxy as the `helper` server */
    cJSON *srv = cJSON_CreateObject();
    cJSON_AddStringToObject(srv, "type", "stdio");
    cJSON_AddStringToObject(srv, "name", "helper");
    buf_init(&cl);
    call_shell_quote(&cl, exe_path);
    buf_append_str(&cl, " mcp-proxy ");
    call_shell_quote(&cl, cfg_path);
    cJSON_AddStringToObject(srv, "command_line", cl.data);
    buf_free(&cl);
    /* not required: a helper that fails to spawn (full temp dir, moved
       exe) degrades to a missing tool instead of killing the session */
    cJSON_AddBoolToObject(srv, "required", false);
    *srv_out = srv;
    return EXIT_OK;
}

void repl_helper_cleanup(const char *seed_path, const char *cfg_path) {
    if (seed_path && *seed_path) remove(seed_path);
    if (cfg_path && *cfg_path) remove(cfg_path);
}

/* the free deepwiki server: one jsonl mcp-proxy config (an http
   upstream plus three expose records) in a temp file, plus the stdio
   server entry that runs it. exposes rename its tools per the bundled
   descriptor: repo_topics/repo_info/repo_ask with argument
   descriptions. failure is never fatal here: the entry carries
   required:false, and the build itself only fails when no temp file
   can be made - then the agent simply runs without the server */
int repl_deepwiki_build(const char *exe_path, engine_t *e,
                        char *cfg_path, size_t cfg_sz, cJSON **srv_out) {
    *srv_out = NULL;
    *cfg_path = '\0';
    FILE *cfg = repl_mktemp(cfg_path, cfg_sz);
    if (!cfg) {
        engine_emit_record(
            e, rec_error(EC_IO_ERROR,
                         "cannot prepare the deepwiki server: the config "
                         "temp file failed",
                         false));
        return EXIT_OK; /* degraded: no server, the session lives */
    }
    /* the upstream: streamable http, mcp.deepwiki.com */
    cJSON *tools = cJSON_CreateObject();
    cJSON_AddStringToObject(tools, "type", "tools");
    cJSON *arr = cJSON_AddArrayToObject(tools, "tools");
    cJSON *up = cJSON_CreateObject();
    cJSON_AddStringToObject(up, "type", "http");
    cJSON_AddStringToObject(up, "name", "deepwiki-upstream");
    cJSON_AddStringToObject(up, "url", "https://mcp.deepwiki.com/mcp");
    cJSON_AddItemToArray(arr, up);
    write_jsonl_line(cfg, tools);
    cJSON_Delete(tools);
    /* the exposes: upstream tool -> renamed with a fresh description */
    static const struct {
        const char *tool, *name, *desc, *arg, *argdesc;
    } ex[] = {
        { "read_wiki_structure", "repo_topics",
          "List of topics for a GitHub repo",
          "repoName", "GitHub repo in owner/repo format" },
        { "read_wiki_contents", "repo_info",
          "Extensive info about GitHub repo",
          "repoName", "GitHub repo in owner/repo format" },
        { "ask_wiki_question", "repo_ask",
          "Ask question about repo",
          "repoName", "GitHub repo in owner/repo format" },
    };
    for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        cJSON *er = cJSON_CreateObject();
        cJSON_AddStringToObject(er, "type", "expose");
        char sel[128];
        snprintf(sel, sizeof sel, "deepwiki-upstream.%s", ex[i].tool);
        cJSON_AddStringToObject(er, "tool", sel);
        cJSON_AddStringToObject(er, "name", ex[i].name);
        cJSON_AddStringToObject(er, "description", ex[i].desc);
        cJSON *args = cJSON_AddObjectToObject(er, "arguments");
        cJSON *a1 = cJSON_CreateObject();
        cJSON_AddStringToObject(a1, "description", ex[i].argdesc);
        cJSON_AddItemToObject(args, ex[i].arg, a1);
        if (!strcmp(ex[i].tool, "ask_wiki_question")) { /* the second arg */
            cJSON *a2 = cJSON_CreateObject();
            cJSON_AddStringToObject(a2, "description",
                                    "question to ask, in natural language");
            cJSON_AddItemToObject(args, "question", a2);
        }
        write_jsonl_line(cfg, er);
        cJSON_Delete(er);
    }
    fclose(cfg);
    /* the session's tools entry: the proxy as the `deepwiki` server,
       not required - offline (or a blocked endpoint) degrades to a
       warning line and a session without the server */
    cJSON *srv = cJSON_CreateObject();
    cJSON_AddStringToObject(srv, "type", "stdio");
    cJSON_AddStringToObject(srv, "name", "deepwiki");
    buf_t cl;
    buf_init(&cl);
    call_shell_quote(&cl, exe_path);
    buf_append_str(&cl, " mcp-proxy ");
    call_shell_quote(&cl, cfg_path);
    cJSON_AddStringToObject(srv, "command_line", cl.data);
    buf_free(&cl);
    cJSON_AddBoolToObject(srv, "required", false);
    *srv_out = srv;
    return EXIT_OK;
}

/* wipe the deepwiki temp config (safe when never built) */
void repl_deepwiki_cleanup(const char *cfg_path) {
    if (cfg_path && *cfg_path) remove(cfg_path);
}

/* ================= session ================= */

/* ================= slash commands ================= */

/* the session's slash state: shared by the dispatch and the model
   picker's line reads. No key-macro generalization - every command
   owns its whole flow. */
typedef struct slash_state {
    editor_t *ed;             /* the session's tty editor, NULL = plain */
    plain_reader_t *pr;       /* the session's plain reader, NULL = tty */
    dspy_t *d;                /* the session's sink display */
    const repl_opts_t *o;     /* the session's extras (helper tool) */
    const char *exe_path;     /* the helper seed's command lines */
    const char *main_prompt;  /* the session's own prompt, restored */
    buf_t pick;               /* the picker's last submitted line */
    const char *seed_path;    /* the helper's seed file (agent) */
    const char *cfg_path;     /* the helper's proxy config (agent) */
    call_cfg_t cfg;           /* the helper seed's source (its own copy) */
} slash_state_t;

/* `/help`: the session's command catalogue */
static void slash_help(dspy_t *d) {
    const char *lines[] = {
        "commands:",
        "  /help          this list",
        "  /models        pick the model of the session",
        "  /cd <dir>      change the working directory",
        "  /new           start a fresh conversation",
    };
    dspy_ensure_nl(d);
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        dspy_write_str(d, lines[i]), dspy_write_str(d, "\n");
}

/* errno text when the platform's codes are errno (posix); windows
   GetLastError codes fall to the lead text */
static const char *cd_err_of(int code, const char *lead) {
    if (code >= 0 && code < 200 && strerror(code) && *strerror(code))
        return strerror(code);
    return lead;
}

/* `/cd <dir>`: move the session's working directory. the tool children
   (builtin mcp, helper, stdio proxies) inherit the cwd of their spawn:
   they drop and reconcile respawns them in the new directory. the model
   hears one system block - "current working directory changed to <dir>"
   - the dir relative to the old one when they share a directory beyond
   the root ("../prj2"), the absolute path otherwise ("/dir2"). 0 the
   directory changed (line drawn, message queued), 1 not */
static int slash_cd(slash_state_t *sl, const char *arg, engine_t *e) {
    dspy_t *d = sl->d;
    char msg[512];
    int mn;
    if (!*arg) {
        dspy_ensure_nl(d);
        mn = snprintf(msg, sizeof msg, "usage: /cd <directory>");
        dspy_write(d, msg, (size_t)mn >= sizeof msg ? sizeof msg - 1
                                                    : (size_t)mn);
        dspy_write_str(d, "\n");
        return 1;
    }
    char cwd[4096] = "";
    char *cwdh = NULL;
    int rc = path_getcwd(&cwdh);
    if (rc) {
        dspy_error_line(d, NULL, cd_err_of(rc, "cannot get the cwd"));
        return 1;
    }
    snprintf(cwd, sizeof cwd, "%s", cwdh);
    free(cwdh);
    /* the direct syscall takes the raw path: windows '\' folds to the
       canonical '/' first, no shell ever sees it */
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", arg);
    path_slashes(dir);
    int cdrc = path_chdir(dir);
    if (cdrc) {
        char why[280];
        snprintf(why, sizeof why, "cannot chdir to '%s': %s", arg,
                 cd_err_of(cdrc, "failed"));
        dspy_error_line(d, NULL, why);
        return 1;
    }
    char *now = NULL;
    rc = path_getcwd(&now);
    if (rc) {
        dspy_error_line(d, NULL, cd_err_of(rc, "cannot get the cwd"));
        return 1;
    }
    if (!strcmp(cwd, now)) {
        free(now);
        return 1; /* same place: nothing to tell the model */
    }
    char *rel = path_relative(cwd, now);
    if (!rel) {
        free(now);
        dspy_error_line(d, NULL, "out of memory");
        return 1;
    }
    /* the tool children that carry a cwd: gone, reconcile respawns */
    mcp_drop_stdio((mcp_mgr_t *)e->mcp);
    /* the system block the model reads before its next turn */
    buf_t blk;
    buf_init(&blk);
    buf_appendf(&blk,
                "current working directory changed to %s\n"
                "the current working directory is now the root of the "
                "conversation: file tools and commands below it resolve "
                "without a path, the ones outside need an absolute path",
                rel);
    rc = system_append_block(e, NULL, blk.data ? blk.data : "");
    buf_free(&blk);
    free(rel);
    dspy_ensure_nl(d);
    mn = snprintf(msg, sizeof msg, "cd: %s", now);
    dspy_write(d, msg, (size_t)mn >= sizeof msg ? sizeof msg - 1
                                                : (size_t)mn);
    dspy_write_str(d, "\n");
    free(now);
    return rc ? 1 : 0;
}

/* tab completion over the model ids (mcprepl's shape, whole lines) */
static void complete_model(void *ctx, const char *line,
                           editor_candidates_t *c) {
    const cJSON *models = ctx;
    const cJSON *it;
    cJSON_ArrayForEach(it, models) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(id) && id->valuestring &&
            strstr(id->valuestring, line) == id->valuestring)
            editor_add_candidate(c, id->valuestring);
    }
}

/* one plain (non-tty) line, the editor's results: ED_QUIT/ED_CLEAR
   counts as a cancel, EOF as a failure */
static int pick_read_line(plain_reader_t *pr, buf_t *out) {
    int r = plain_line(pr, out);
    if (r == ED_SUBMIT) return 0;
    if (r == ED_QUIT || r == ED_CLEAR) return 1;
    return 2; /* EOF (and the hygiene codes, never here) */
}

/* one picker line with the model ids as tab completions; 0 submit, 1
   cancel (ctrl-c / empty-line clear), 2 failure */
static int pick_read_model(slash_state_t *sl, const cJSON *models) {
    if (sl->ed) {
        /* the picker owns the tab hook for its read: the session's own
           (path completion) swaps out for the model ids, then back */
        editor_tab_fn save_tab = sl->ed->on_tab;
        void *save_ctx = sl->ed->tab_ctx;
        sl->ed->on_tab = complete_model;
        sl->ed->tab_ctx = (void *)models;
        int r = editor_line(sl->ed);
        sl->ed->on_tab = save_tab;
        sl->ed->tab_ctx = save_ctx;
        if (r == ED_SUBMIT) {
            buf_clear(&sl->pick);
            buf_append(&sl->pick, sl->ed->line.data, sl->ed->line.len);
            return 0;
        }
        if (r == ED_QUIT || r == ED_CLEAR) return 1;
        return 2;
    }
    buf_clear(&sl->pick);
    return pick_read_line(sl->pr, &sl->pick);
}

/* the /models picker: fetch the endpoint's list, let the user choose
   one (tab completion on a tty), apply it to the session - engine,
   seed, cfg - and inherit it to the helper. 0 picked and applied, 1
   canceled, 2 failure (message already drawn) */
static int slash_models(slash_state_t *sl, engine_t *e) {
    dspy_t *d = sl->d;
    const repl_opts_t *o = sl->o;
    /* the list is the session's own flags' - one source of truth */
    const cJSON *headers =
        cJSON_GetObjectItemCaseSensitive(e->llm, "headers");
    char err[256] = "";
    cJSON *models = models_fetch(e->protocol, rec_str(e->llm, "api_base"),
                                 rec_str(e->llm, "api_key"), headers, err,
                                 sizeof err);
    if (!models) {
        dspy_error_line(d, NULL, err);
        return 2;
    }
    if (d->io_fail) { /* the fetch drew, the write failed */
        cJSON_Delete(models);
        return 2;
    }
    size_t n = cJSON_GetArraySize(models);
    if (!n) {
        dspy_error_line(d, NULL, "no models reported");
        cJSON_Delete(models);
        return 2;
    }
    /* the list: ids aligned to the widest, owner and created inline */
    size_t idw = 2, obw = 5;
    const cJSON *it;
    cJSON_ArrayForEach(it, models) {
        const cJSON *v;
        size_t w;
        if ((v = cJSON_GetObjectItemCaseSensitive(it, "id")) &&
            cJSON_IsString(v) && (w = strlen(v->valuestring)) > idw)
            idw = w;
        if ((v = cJSON_GetObjectItemCaseSensitive(it, "owned_by")) &&
            cJSON_IsString(v) && (w = strlen(v->valuestring)) > obw)
            obw = w;
    }
    char hdr[300];
    int hn = snprintf(hdr, sizeof hdr, "%-*s  %-*s  %s", (int)idw, "id",
                      (int)obw, "owner", "created");
    dspy_ensure_nl(d);
    dspy_write(d, hdr, (size_t)hn >= sizeof hdr ? sizeof hdr - 1
                                                : (size_t)hn);
    dspy_write_str(d, "\n");
    cJSON_ArrayForEach(it, models) {
        char created[32] = "-";
        const cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "created");
        if (cJSON_IsNumber(cr)) {
            time_t t = (time_t)cr->valuedouble;
            struct tm tm;
            gmtime_r(&t, &tm);
            strftime(created, sizeof created, "%Y-%m-%d", &tm);
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *ob = cJSON_GetObjectItemCaseSensitive(it, "owned_by");
        char ln[512];
        int wn = snprintf(ln, sizeof ln, "%-*s  %-*s  %s", (int)idw,
                          cJSON_IsString(id) ? id->valuestring : "",
                          (int)obw,
                          cJSON_IsString(ob) ? ob->valuestring : "",
                          created);
        dspy_write(d, ln, (size_t)wn >= sizeof ln ? sizeof ln - 1
                                                  : (size_t)wn);
        dspy_write_str(d, "\n");
    }
    char cur[128] = "";
    const cJSON *cm = cJSON_GetObjectItemCaseSensitive(e->llm, "model");
    if (cJSON_IsString(cm)) {
        snprintf(cur, sizeof cur, "%s", cm->valuestring);
        char msg[300];
        int mn = snprintf(msg, sizeof msg,
                          "(current %s - empty keeps it, ctrl-c cancels)",
                          cur);
        dspy_write(d, msg, (size_t)mn >= sizeof msg ? sizeof msg - 1
                                                    : (size_t)mn);
        dspy_write_str(d, "\n");
    }
    for (;;) {
        int r = pick_read_model(sl, models);
        if (r) { /* canceled or failed: the session's own prompt again */
            if (r == 2) d->io_fail = true;
            cJSON_Delete(models);
            return r;
        }
        if (!sl->pick.len) {
            if (*cur) break; /* empty line: keep the current model */
            dspy_write_str(d, "(no model yet - pick one)\n");
            continue;
        }
        bool found = false;
        cJSON_ArrayForEach(it, models) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (cJSON_IsString(id) && id->valuestring &&
                !strcmp(id->valuestring, sl->pick.data))
                found = true;
        }
        if (found) break;
        char msg[400];
        int mn = snprintf(msg, sizeof msg,
                          "no '%s' in the list - pick again", sl->pick.data);
        dspy_write(d, msg,
                   (size_t)mn >= sizeof msg ? sizeof msg - 1 : (size_t)mn);
        dspy_write_str(d, "\n");
    }
    char *model = NULL;
    if (sl->pick.len) {
        model = sl->pick.data;
    } else if (*cur) {
        model = cur;
    }
    if (!model) { /* no current model and none picked: stay unset */
        cJSON_Delete(models);
        return 1;
    }
    if (*cur && !strcmp(model, cur)) {
        /* the current model unchanged: nothing to swap */
        cJSON_Delete(models);
        return 0;
    }
    /* the swap: engine first (the llm record is the source the wire
       reads per turn), then the helper's seed rewrite - spawn and
       cfg files keep their paths, the files carry the new model */
    cJSON *llm = cJSON_Duplicate(e->llm, 1);
    cJSON_DeleteItemFromObject(llm, "model");
    cJSON_AddStringToObject(llm, "model", model);
    char *verr = validate_llm(llm);
    if (verr) {
        dspy_error_line(d, NULL, verr);
        free(verr);
        cJSON_Delete(llm);
        cJSON_Delete(models);
        return 2;
    }
    engine_apply_config_record(e, llm);
    cJSON_Delete(llm);
    if (o->helper_tool) { /* the subagent inherits the session's model */
        /* the seed rebuild reads the cfg copy: its model field moves to
           the picked one (the session's llm record already did) */
        free(sl->cfg.model);
        sl->cfg.model = strdup(model);
        if (!sl->cfg.model) {
            cJSON_Delete(models);
            return 2; /* engine already applied - nothing more */
        }
        /* the rebuild drops the old helper entry from the live tools
           record: the spawn keeps its config path, the rewritten seed
           file carries the new model, reconcile swaps the child */
        if (e->tools_cfg) {
            cJSON *list =
                cJSON_GetObjectItemCaseSensitive(e->tools_cfg, "tools");
            if (cJSON_IsArray(list)) {
                cJSON *kept = cJSON_CreateArray();
                if (!kept) {
                    cJSON_Delete(models);
                    return 2; /* engine already applied - nothing more */
                }
                const cJSON *s;
                cJSON_ArrayForEach(s, list) {
                    if (strcmp(rec_str(s, "name"), "helper"))
                        cJSON_AddItemToArray(kept,
                                             cJSON_Duplicate((cJSON *)s, 1));
                }
                cJSON_DeleteItemFromObject(e->tools_cfg, "tools");
                cJSON_AddItemToObject(e->tools_cfg, "tools", kept);
            }
        }
        /* rewrite: the old seed and cfg files (their children hold no
           descriptors - each read its file fully at startup) go first,
           the rebuild fills the slots with its own fresh temps */
        remove(sl->seed_path);
        remove(sl->cfg_path);
        cJSON *helper_srv = NULL;
        int rc = repl_helper_build(&sl->cfg, prompt_system_prompts_helper,
                                   e->tools_cfg, sl->exe_path,
                                   sl->o->full_tools, e,
                                   (char *)sl->seed_path,
                                   strlen(sl->seed_path) + 1,
                                   (char *)sl->cfg_path,
                                   strlen(sl->cfg_path) + 1, &helper_srv);
        if (rc == EXIT_OK) {
            cJSON *tools = e->tools_cfg
                               ? cJSON_Duplicate(e->tools_cfg, 1)
                               : cJSON_Parse("{\"type\":\"tools\",\"tools\":[]}");
            cJSON_AddItemToArray(
                cJSON_GetObjectItemCaseSensitive(tools, "tools"), helper_srv);
            char *m2 = validate_tools(tools);
            if (m2) {
                dspy_error_line(d, NULL, m2);
                free(m2);
                cJSON_Delete(tools);
                rc = EXIT_IO_ERROR;
            } else {
                engine_apply_config_record(e, tools);
                cJSON_Delete(tools);
            }
        } else if (helper_srv) {
            cJSON_Delete(helper_srv);
        }
    }
    {
        char msg[300];
        int mn = snprintf(msg, sizeof msg, "model: %s", model);
        dspy_write(d, msg,
                   (size_t)mn >= sizeof msg ? sizeof msg - 1 : (size_t)mn);
        dspy_write_str(d, "\n");
    }
    cJSON_Delete(models);
    return 0;
}

/* the headstart's display twin: the listing is a system block, not a
   transcript tool call, but the shell shows it as the files_list call
   it stands for - request line then reply text, brief style. display
   only: no record reaches the engine, the store, or the transcript */
static void headstart_show(dspy_t *d, const char *ls) {
    cJSON *args = cJSON_Parse(
        "{\"path\":\".\",\"regex\":\".*\",\"show_hidden_files\":true}");
    cJSON *req = rec_tool_request("files_list", args, "headstart");
    dspy_tool_request(d, req);
    cJSON *resp = rec_tool_response("headstart", ls, false);
    dspy_brief_count(d, resp);
    dspy_tool_response(d, resp);
    cJSON_Delete(args);
    cJSON_Delete(req);
    cJSON_Delete(resp);
}

/* `/new`: drop the conversation, start fresh like a boot. the model
   keeps the session's llm (and model) and tool children - what clears
   is the transcript and the system blocks the boot wrote (the
   session's own system prompt, the headstart listing, AGENTS.md),
   rebuilt from the same sources. the store keeps appending: the new
   conversation lands in the same file, one history. */
static int slash_new(slash_state_t *sl, engine_t *e) {
    dspy_t *d = sl->d;
    tlist_clear(&e->tr);
    e->llm_mark = 0; /* thinking of the dropped turns is gone with them */
    /* the boot's system blocks, rebuilt: the prompt text (an explicit
       --system-prompt overrode the default, sl->cfg carries it), then
       headstart and AGENTS.md as their own blocks */
    const char *prompt_text = sl->cfg.system;
    if (prompt_text) {
        cJSON *sys = call_build_system(&sl->cfg);
        char *m = validate_content(
            cJSON_GetObjectItemCaseSensitive(sys, "content"));
        if (m) { /* the prompt text itself: immutable since the boot */
            dspy_error_line(d, NULL, m);
            free(m);
            cJSON_Delete(sys);
            return 1;
        }
        engine_apply_config_record(e, sys);
        cJSON_Delete(sys);
    }
    if (sl->o->headstart) {
        char *ls = builtin_headstart_listing();
        if (ls) {
            headstart_show(d, ls);
            int rc = system_append_block(e, NULL, ls);
            free(ls);
            if (rc) return 1;
        }
    }
    if (sl->o->agents_md) {
        int rc = inject_agents_md(e, NULL);
        if (rc) return 1;
    }
    dspy_ensure_nl(d);
    dspy_write_str(d, "new conversation\n");
    return 0;
}

/* one input line: a slash command consumed (its own result), any other
   input falls through to the prompt (1). The model picker gets the
   `model> ` prompt; the session's own is restored around it. */
static int slash_dispatch(slash_state_t *sl, const char *s, engine_t *e) {
    if (!strcmp(s, "/help")) {
        slash_help(sl->d);
        return 0;
    }
    if (!strcmp(s, "/models")) {
        if (sl->ed) {
            /* the bold `model> ` prompt, styled like the session's own */
            static _Thread_local char pp[80];
            snprintf(pp, sizeof pp, "%smodel> ", sl->d->st->bold);
            sl->ed->prompt = pp;
        }
        int r = slash_models(sl, e);
        if (sl->ed) sl->ed->prompt = sl->main_prompt;
        (void)r; /* both cancel and failure leave the session intact */
        return 0;
    }
    if (!strncmp(s, "/cd", 3) && (!s[3] || s[3] == ' ')) {
        /* the directory: everything after the command, leading spaces
           skipped - the typed line is the path, quotes never stripped */
        const char *arg = s + 3;
        while (*arg == ' ') arg++;
        slash_cd(sl, arg, e);
        return 0;
    }
    if (!strcmp(s, "/new")) {
        slash_new(sl, e);
        return 0;
    }
    {
        char msg[300];
        int mn = snprintf(msg, sizeof msg,
                          "unknown command '%s' - /help lists them", s);
        dspy_write(sl->d, msg,
                   (size_t)mn >= sizeof msg ? sizeof msg - 1 : (size_t)mn);
        dspy_write_str(sl->d, "\n");
    }
    return 0;
}

cJSON *repl_build_options(void) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "options");
    cJSON_AddNumberToObject(t, "stream_interval", 0);
    return t;
}

/* hygiene of a submitted line: strict UTF-8 (CR and NUL never arrive) */
static bool line_valid(const buf_t *b) {
    return utf8_valid((const uint8_t *)(b->data ? b->data : ""), b->len);
}

int repl_run(const call_cfg_t *c, int in_fd, FILE *out, const char *exe_path,
             wire_t *(*factory)(engine_t *)) {
    repl_opts_t o = { .default_prompt = prompt_system_prompts_repl };
    /* zero opts: no quiet trims - those are the agent's defaults */
    return repl_run_ex(c, &o, in_fd, out, exe_path, factory);
}

int repl_run_ex(const call_cfg_t *c, const repl_opts_t *o, int in_fd,
                FILE *out, const char *exe_path,
                wire_t *(*factory)(engine_t *)) {
    style_t st;
    style_probe(&st, out);
    char helper_seed[512] = "", helper_cfg[512] = "";
    char deepwiki_cfg[512] = "";
    repl_sink_t sink;
    memset(&sink, 0, sizeof sink);
    sink.d.out = out;
    sink.d.st = &st;
    sink.d.cur = -1;
    /* agent's brief tool traffic: the listed tools collapse to one
       summary line each unless --verbose kept the full replies; the
       bundled server's prefix never shows - its tools flatten to bare
       names, old stored transcripts with dotted names still strip */
    dspy_brief_tools(&sink.d, o->full_tools
                                  ? NULL
                                  : (o->helper_tool
                                         ? DSPY_BRIEF_TOOLS " helper"
                                         : DSPY_BRIEF_TOOLS));
    /* the prefixes whose names add no signal: the flattened bundled
       server and helper subagent (bare names on the wire now; the
       strip covers the dotted form of replayed records) */
    {
        char strip[64] = "";
        if (o->builtin_mcp) snprintf(strip, sizeof strip, "builtin");
        if (o->helper_tool)
            snprintf(strip + strlen(strip), sizeof strip - strlen(strip),
                     "%shelper", *strip ? " " : "");
        if (*strip) dspy_brief_strip(&sink.d, strip);
    }
    /* agent's quiet display, --verbose restores the full traffic: the
       trim list's tools render long string arguments elided, thinking
       collapses to one line */
    sink.d.trim_args = o->trim_args;
    sink.quiet_think = o->quiet_think;

    engine_t *e = engine_new(repl_sink_fn, &sink);
    if (factory) e->wire_factory = factory;
    e->keep_mcp = true; /* the session continues; engine_free tears down */
    e->llm_retry = o->llm_retry; /* agent: fibonacci retry of llm calls */

    /* input first: every goto done below releases what it meets */
    bool tty = isatty(in_fd);
    editor_t ed;
    plain_reader_t pr;
    char prompt[48]; /* the editor draws it; escapes cost no columns */
    snprintf(prompt, sizeof prompt, "%s> ", st.bold);
    if (tty) {
        /* the typed line is the user block: the prompt carries the bold
           sequence, the editor counts its width past the escape */
        editor_init(&ed, out, in_fd, prompt);
        /* tab completes the trailing @-sigiled path word */
        ed.on_tab = editor_complete_path;
    } else
        plain_init(&pr, in_fd);
    int last_ending = EXIT_OK; /* EOF before any input exits 0 */
    bool need_rule = true;     /* rule vs no-rule prompt redraws */
    buf_t line;
    buf_init(&line);
    /* the session's slash command state: bound fully after the cfg's
       own copy exists (the /models picker owns its inputs) */
    slash_state_t sl;
    memset(&sl, 0, sizeof sl);
    buf_init(&sl.pick);

    /* the bundled default system prompt: an explicit --system-prompt
       replaces it, and (per the call contract) a different empty text vs
       no record stays distinguishable on the wire. the cast is safe: the
       config is read-only past this point (call_compile never writes
       through the pointer, cmd_repl frees the original). */
    call_cfg_t cfg = *c;
    if (!cfg.system && o->default_prompt)
        cfg.system = (char *)o->default_prompt;

    /* the slash state binds the session's own inputs: the editor or
       plain reader, the display, the helper's file slots and the cfg
       copy the seed rebuild reads */
    sl.ed = tty ? &ed : NULL;
    sl.pr = tty ? NULL : &pr;
    sl.d = &sink.d;
    sl.o = o;
    sl.exe_path = exe_path;
    sl.main_prompt = prompt;
    sl.seed_path = helper_seed;
    sl.cfg_path = helper_cfg;
    sl.cfg = cfg;

    int rc = call_compile(&cfg, e, exe_path);
    if (rc) goto done;
    {
        /* the one record call does not compile: display latency is the
           point, every streamed chunk renders as it arrives */
        cJSON *opts = repl_build_options();
        engine_apply_config_record(e, opts);
        cJSON_Delete(opts);
    }
    if (o->headstart) {
        /* the agent's head start: the working directory's listing as
           the session's first extra system block, ahead of AGENTS.md -
           the tool call the model would open with, answered before the
           first prompt. an unreadable directory leaves the prompt as
           is, the model lists on its own */
        char *ls = builtin_headstart_listing();
        if (ls) {
            headstart_show(&sink.d, ls);
            rc = system_append_block(e, cfg.system, ls);
            free(ls);
            if (rc) goto done;
            cfg.system = NULL; /* already carried: AGENTS.md appends */
        }
    }
    if (o->agents_md) {
        rc = inject_agents_md(e, cfg.system);
        if (rc) goto done;
    }
    if (o->builtin_mcp) {
        /* the built-in server rides the proxies' tools record: one
           record owns the whole server set (engine_apply replaces) */
        cJSON *tools = call_build_agent_tools(&cfg, exe_path);
        char *m = validate_tools(tools);
        if (m) {
            engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
            free(m);
            cJSON_Delete(tools);
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        engine_apply_config_record(e, tools);
        cJSON_Delete(tools);
    }
    if (o->deepwiki_tool) {
        /* the free deepwiki server: wrapped in an mcp-proxy child that
           renames its tools (repo_topics/repo_info/repo_ask). skipped
           whole when a server of that name already exists (a
           --mcp-proxy config may carry its own). non-required: a
           failed connect renders a warning, the session continues */
        cJSON *cur = e->tools_cfg
                         ? cJSON_GetObjectItemCaseSensitive(e->tools_cfg,
                                                            "tools")
                         : NULL;
        bool taken = false;
        if (cJSON_IsArray(cur))
            for (const cJSON *s = cur->child; s && !taken; s = s->next)
                if (!strcmp(rec_str(s, "name"), "deepwiki")) taken = true;
        if (taken) {
            engine_emit_record(
                e, rec_error(EC_INVALID_RECORD,
                             "a server named 'deepwiki' already exists: the "
                             "bundled deepwiki server is skipped",
                             false));
        } else {
            cJSON *dw = NULL;
            rc = repl_deepwiki_build(exe_path, e, deepwiki_cfg,
                                     sizeof deepwiki_cfg, &dw);
            if (rc) goto done;
            if (dw) {
                cJSON *tools =
                    e->tools_cfg
                        ? cJSON_Duplicate(e->tools_cfg, 1)
                        : cJSON_Parse("{\"type\":\"tools\",\"tools\":[]}");
                cJSON_AddItemToArray(
                    cJSON_GetObjectItemCaseSensitive(tools, "tools"), dw);
                char *m = validate_tools(tools);
                if (m) {
                    engine_emit_record(e, rec_error(EC_INVALID_RECORD, m,
                                                    true));
                    free(m);
                    cJSON_Delete(tools);
                    rc = EXIT_INVALID_RECORD;
                    goto done;
                }
                engine_apply_config_record(e, tools);
                cJSON_Delete(tools);
            }
        }
    }
    if (o->helper_tool) {
        /* the helper subagent: this same agent as one `helper(prompt)`
           tool; its seed copies the session's llm and every tool server
           but the helper itself (the recursion cut); its system prompt
           is the helper's own bundled text (system_prompts/helper.txt),
           not the session's */
        cJSON *helper_srv = NULL;
        rc = repl_helper_build(&cfg, prompt_system_prompts_helper,
                               e->tools_cfg, exe_path, o->full_tools,
                               e, helper_seed, sizeof helper_seed,
                               helper_cfg, sizeof helper_cfg, &helper_srv);
        if (rc) goto done;
        cJSON *tools =
            e->tools_cfg
                ? cJSON_Duplicate(e->tools_cfg, 1)
                : cJSON_Parse("{\"type\":\"tools\",\"tools\":[]}");
        cJSON_AddItemToArray(
            cJSON_GetObjectItemCaseSensitive(tools, "tools"), helper_srv);
        char *m = validate_tools(tools);
        if (m) {
            engine_emit_record(e, rec_error(EC_INVALID_RECORD, m, true));
            free(m);
            cJSON_Delete(tools);
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        engine_apply_config_record(e, tools);
        cJSON_Delete(tools);
    }
    if (o->store && *o->store) {
        /* an existing store replays fully - display and transcript -
           before the first prompt; either way the session appends */
        rc = store_load(e, out, o->store);
        if (rc) goto done;
        sink.store = fopen(o->store, "a");
        if (!sink.store) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "cannot open the conversation store '%s'", o->store);
            engine_emit_record(e, rec_error(EC_IO_ERROR, msg, true));
            rc = EXIT_IO_ERROR;
            goto done;
        }
    }

    for (;;) {
        int r;
        /* a SIGINT racing the read start still meets the stage rule: at a
           fresh prompt the line is empty by construction, so it quits */
        if (g_stop_flag) {
            g_stop_flag = 0;
            r = ED_QUIT;
            goto handled;
        }
        if (tty) {
            if (need_rule) dspy_rule(&sink.d, '=');
            r = editor_line(&ed); /* prompt, echo and the closing newline
                                     are the editor's (linenoise) */
            dspy_write_str(&sink.d, st.reset); /* the bold user block closed */
        } else {
            r = plain_line(&pr, &line);
        }
    handled:
        if (r == ED_QUIT) { /* Ctrl-C at a clear prompt: exit 8 */
            rc = EXIT_INTERRUPTED;
            goto done;
        }
        if (r == ED_EOF) { /* EOF: the last ending's code */
            rc = last_ending;
            goto done;
        }
        if (r == ED_BAD) {
            dspy_error_line(&sink.d, EC_INVALID_RECORD,
                            "NUL byte in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        if (r == ED_CLEAR) { /* discarded input, fresh prompt line */
            need_rule = false;
            continue;
        }
        /* ED_SUBMIT: the editor owns its buffer, the loop works on
           line - transfer before the shared checks */
        if (tty) {
            buf_clear(&line);
            if (ed.line.len)
                buf_append(&line, ed.line.data, ed.line.len);
        }
        if (!line.len) { /* empty input: ignored, no user record, no turn */
            need_rule = false;
            continue;
        }
        if (!line_valid(&line)) {
            dspy_error_line(&sink.d, EC_INVALID_RECORD,
                            "invalid UTF-8 in input line");
            rc = EXIT_INVALID_RECORD;
            goto done;
        }
        need_rule = true;
        if (tty) editor_hist_push(&ed, line.data);

        /* slash commands: consumed before any user record - the line is
           the session's own control, not the conversation's */
        if (line.data[0] == '/') {
            slash_dispatch(&sl, line.data, e);
            if (sink.d.io_fail) { /* the command drew, the write failed */
                rc = EXIT_OUT_OF_CHANNEL;
                goto done;
            }
            continue;
        }

        if (!tty) render_user_block(&sink, line.data);

        /* the @ sigil is prompt-side only: a word-initial @ before a
           path drops from the record - the model reads the bare path.
           History keeps the typed form (recall shows what was typed),
           and only the final word's sigil goes - mid-line ones stay */
        {
            bool stripped = false;
            size_t i = 0;
            while (i < line.len) {
                if (line.data[i] == '@' && (i == 0 || line.data[i - 1] == ' ') &&
                    i + 1 < line.len && line.data[i + 1] != ' ') {
                    memmove(line.data + i, line.data + i + 1,
                            line.len - i - 1);
                    line.len--;
                    stripped = true;
                    continue;
                }
                i++;
            }
            if (stripped) {
                /* an @ path entered the line: normalize its separators
                   to '/' - windows typing shows '\', the model record
                   and every tool argument speak '/' */
                path_slashes(line.data);
                line.data[line.len] = '\0';
            }
        }

        {
            cJSON *user = call_build_user(line.data);
            char *m = validate_content(
                cJSON_GetObjectItemCaseSensitive(user, "content"));
            if (m) {
                dspy_error_line(&sink.d, EC_INVALID_RECORD, m);
                free(m);
                cJSON_Delete(user);
                rc = EXIT_INVALID_RECORD;
                goto done;
            }
            store_persist(&sink, user); /* the store's turn opener */
            tlist_ingest(&e->tr, user);
            cJSON_Delete(user);
        }

        /* a turn runs against the cooked terminal: raw mode cycles per
           line now (the editor enters and leaves it), so ctrl-c raises
           the stop signal on both platforms - no byte queues behind it */
        timing_reset(&sink);
        rc = engine_start(e);
        if (rc == 0) rc = engine_run(e);
        render_stats_exit(&sink, rc); /* every end of the request */
        last_ending = rc;
        if (rc == EXIT_INTERRUPTED) {
            g_stop_flag = 0; /* consumed: the session continues */
            continue;
        }
        if (rc == EXIT_OK) continue;
        goto done; /* fatal codes and the terminal 9 end the session */
    }

done:
    buf_free(&line);
    buf_free(&sl.pick);
    /* the dspy sink's strdup'd brief state (agent display) */
    dspy_brief_flush(&sink.d); /* held request lines, not lost */
    free(sink.d.brief_tools);
    dspy_brief_clear(&sink.d);
    free(sink.d.brief_strip);
    if (tty) editor_free(&ed);
    else plain_free(&pr);
    if (sink.store) fclose(sink.store);
    if (sink.store_fail && rc == EXIT_OK) {
        /* the session ran, its transcript did not land whole */
        engine_emit_record(e, rec_error(EC_IO_ERROR,
                                        "the conversation store write failed",
                                        true));
        rc = EXIT_IO_ERROR;
    }
    engine_free(e); /* process teardown: the mcp children die here */
    repl_helper_cleanup(helper_seed, helper_cfg);
    repl_deepwiki_cleanup(deepwiki_cfg);
    if (sink.d.io_fail) rc = EXIT_OUT_OF_CHANNEL;
    return rc;
}

/* ================= command ================= */

static void repl_usage(FILE *out) {
    fputs("usage: llmkit repl (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "                [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "                [--reasoning-effort <value>] "
          "[--system-prompt <text>]\n"
          "                [--header <name=value>]...\n"
          "                [--mcp-proxy <config>]... "
          "[--terminal-tool <name.tool>]...\n",
          out);
}

int cmd_repl(int argc, char **argv) {
    signals_init(); /* SIGINT is an input control here: the stage rule at
                       the prompt, the orderly stop in a turn */
    call_cfg_t c;
    char err[256] = "";
    if (call_parse_ex(argc - 2, argv + 2, &c, err, sizeof err, false) != 0) {
        fprintf(stderr, "llmkit repl: %s\n", err);
        repl_usage(stderr);
        return EXIT_OUT_OF_CHANNEL;
    }
    char exe[4096];
    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
    int rc = repl_run(&c, STDIN_FILENO, stdout, exe, NULL);
    call_cfg_free(&c);
    return rc;
}

/* ================= agent command ================= */

int repl_store_extract(int argc, char **argv, const char **store,
                       char *err, size_t errsz) {
    int n = 0;
    *store = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--conversation-store")) {
            if (*store) {
                snprintf(err, errsz, "--conversation-store given twice");
                return -1;
            }
            if (i + 1 >= argc) {
                snprintf(err, errsz,
                         "missing value for --conversation-store");
                return -1;
            }
            *store = argv[i + 1];
            i++;
            continue;
        }
        argv[n++] = argv[i];
    }
    return n;
}

/* pull --verbose out of the agent flag range, in place; returns 1 when
   seen, 0 when absent, -1 on a duplicate (err filled) */
int repl_verbose_extract(int argc, char **argv, char *err, size_t errsz) {
    bool seen = false;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) {
            if (seen) {
                snprintf(err, errsz, "--verbose given twice");
                return -1;
            }
            seen = true;
            continue;
        }
        argv[n++] = argv[i];
    }
    return seen ? 1 : 0;
}

/* pull --no-helper out of the agent flag range, in place; returns 1
   when seen, 0 when absent, -1 on a duplicate (err filled) */
int repl_nohelper_extract(int argc, char **argv, char *err, size_t errsz) {
    bool seen = false;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--no-helper")) {
            if (seen) {
                snprintf(err, errsz, "--no-helper given twice");
                return -1;
            }
            seen = true;
            continue;
        }
        argv[n++] = argv[i];
    }
    return seen ? 1 : 0;
}

/* pull --no-deepwiki out of the agent flag range, in place; returns 1
   when seen, 0 when absent, -1 on a duplicate (err filled) */
int repl_nodeepwiki_extract(int argc, char **argv, char *err, size_t errsz) {
    bool seen = false;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--no-deepwiki")) {
            if (seen) {
                snprintf(err, errsz, "--no-deepwiki given twice");
                return -1;
            }
            seen = true;
            continue;
        }
        argv[n++] = argv[i];
    }
    return seen ? 1 : 0;
}

static void agent_usage(FILE *out) {
    fputs("usage: llmkit agent (--anthropic|--openai|--openai-responses) "
          "<api_base>\n"
          "                [--key <token>] [--model <name>] "
          "[--max-tokens <n>]\n"
          "                [--reasoning-effort <value>] "
          "[--system-prompt <text>]\n"
          "                [--header <name=value>]...\n"
          "                [--mcp-proxy <config>]... "
          "[--terminal-tool <name.tool>]...\n"
          "                [--conversation-store <file>] [--verbose] "
          "[--no-helper]\n"
          "                [--no-deepwiki]\n",
          out);
}

/* the repl surface plus the agent extras: the built-in mcp server,
   AGENTS.md injected after the system prompt, the bundled agent prompt,
   and the conversation store when --conversation-store names one */
int cmd_agent_repl(int argc, char **argv) {
    signals_init(); /* SIGINT is an input control here, as in repl */
    char err[256] = "";
    const char *store = NULL;
    call_cfg_t c;
    int n = repl_store_extract(argc - 2, argv + 2, &store, err, sizeof err);
    if (n >= 0) {
        int h = repl_nohelper_extract(n, argv + 2, err, sizeof err);
        if (h < 0)
            n = -1;
        else {
            n -= h; /* the flag compacted away when seen */
            int w = repl_nodeepwiki_extract(n, argv + 2, err, sizeof err);
            if (w < 0)
                n = -1;
            else {
            n -= w; /* the deepwiki flag compacted away when seen */
            int v = repl_verbose_extract(n, argv + 2, err, sizeof err);
            if (v < 0)
                n = -1;
            else {
                n -= v; /* the verbose flag compacted away */
                n = call_parse_ex(n, argv + 2, &c, err, sizeof err,
                                  false);
                if (n == 0) {
                    char exe[4096];
                    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
                    repl_opts_t o = { .default_prompt =
                                          prompt_system_prompts_agent,
                                      .builtin_mcp = true,
                                      .agents_md = true,
                                      .headstart = true,
                                      .store = store,
                                      .llm_retry = true,
                                      .full_tools = v == 1,
                                      /* quiet display defaults, --verbose
                                         restores both: thinking text whole,
                                         tool arguments untrimmed */
                                      .quiet_think = v != 1,
                                      .trim_args = v != 1,
                                      /* the helper subagent rides along
                                         unless --no-helper */
                                      .helper_tool = h != 1,
                                      /* the bundled deepwiki server rides
                                         along unless --no-deepwiki */
                                      .deepwiki_tool = w != 1 };
                    int rc = repl_run_ex(&c, &o, STDIN_FILENO, stdout, exe,
                                         NULL);
                    call_cfg_free(&c);
                    return rc;
                }
            }
        }
        }
    }
    fprintf(stderr, "llmkit agent: %s\n", err);
    agent_usage(stderr);
    return EXIT_OUT_OF_CHANNEL;
}
