/* llmkit.h - shared declarations. See docs/design.md. */
#ifndef LLMKIT_H
#define LLMKIT_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

/* release builds stamp this: tools/release.sh passes VERSION=<x.y.z> to make */
#ifndef LLMKIT_VERSION
#define LLMKIT_VERSION "1.0"
#endif

struct engine;
typedef struct engine engine_t;

/* ---- exit codes (design sec.11) ---- */
#define EXIT_OK                 0
#define EXIT_OUT_OF_CHANNEL     1
#define EXIT_INVALID_RECORD     2
#define EXIT_CONNECT_FAILED     3
#define EXIT_HTTP_ERROR         4
#define EXIT_API_ERROR          5
#define EXIT_MAX_TOOL_ROUNDS    6
#define EXIT_IO_ERROR           7
#define EXIT_INTERRUPTED        8
#define EXIT_TERMINAL_TOOL      9

/* ---- error codes ---- */
#define EC_INVALID_RECORD   "invalid_record"
#define EC_CONNECT_FAILED   "connect_failed"
#define EC_HTTP_ERROR       "http_error"
#define EC_API_ERROR        "api_error"
#define EC_TOOL_FAILED      "tool_failed"
#define EC_TOOL_TIMEOUT     "tool_timeout"
#define EC_MAX_ROUNDS       "max_tool_rounds_exceeded"
#define EC_IO_ERROR         "io_error"
#define EC_INTERRUPTED      "interrupted"

int exit_code_of(const char *error_code);

/* ---- protocols ---- */
enum { PROTO_OPENAI = 0, PROTO_RESPONSES = 1, PROTO_ANTHROPIC = 2 };
int protocol_of(const char *s); /* -1 unknown */

/* ================= buf.c ================= */
typedef struct buf {
    char *data;
    size_t len, cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_append_str(buf_t *b, const char *s);
void buf_append_byte(buf_t *b, char c);
void buf_appendf(buf_t *b, const char *fmt, ...);
void buf_clear(buf_t *b); /* keep capacity */
char *buf_steal(buf_t *b, size_t *len_out); /* NUL-terminated; resets b */

/* append a JSON-encoded string literal (with quotes) */
void buf_append_jstr(buf_t *b, const char *s);
/* append the serialization of a cJSON tree (unformatted) */
void buf_append_tree(buf_t *b, const cJSON *t);
/* append a JSON number from a double, cJSON-style (deterministic) */
void buf_append_jnum(buf_t *b, double d);

bool utf8_valid(const uint8_t *p, size_t n);

/* ================= platform.c ================= */
typedef struct queue queue_t;

queue_t *queue_new(void);
void queue_free(queue_t *q); /* drops remaining items (freed with free()) */
void queue_push(queue_t *q, void *item);
void *queue_try_pop(queue_t *q);        /* NULL if empty */
void *queue_pop_timeout(queue_t *q, double seconds); /* NULL on timeout/empty */
void queue_close(queue_t *q);
bool queue_closed(queue_t *q);
void queue_wait_closed(queue_t *q);

/* threads (detached readers) */
typedef void *(*thread_fn)(void *);
int thread_start_detached(thread_fn fn, void *arg);

/* monotonic clock seconds */
double mono_now(void);

/* /bin/sh -c spawn with pipes; returns pid or -1 */
typedef struct spawn {
    pid_t pid;
    int to_fd;   /* write to child stdin; -1 if none */
    int from_fd; /* read child stdout; -1 if none */
} spawn_t;
int spawn_shell(const char *command_line, spawn_t *out);
void spawn_kill(spawn_t *s); /* SIGTERM + reap + close; idempotent */

/* signals */
extern volatile int g_stop_flag; /* SIGINT observed */
void signals_init(void);           /* SIGINT handler, SIGPIPE ignore */

/* ---- curl helpers ---- */
typedef struct http_req {
    const char *url;
    struct curl_slist *hdrs; /* caller-built, freed by http_perform */
    const char *body;        /* request body bytes */
    size_t body_len;
    /* response capture (always filled) */
    buf_t resp;
    long status;
    buf_t content_type;
    char session_id[256]; /* Mcp-Session-Id response header, "" if none */
    /* streaming: if set, response bytes go here instead of resp (resp still
       captures when on_data is NULL) */
    void (*on_data)(void *ctx, const char *bytes, size_t n);
    void *cb_ctx;
    bool got_data;
    /* timeouts, seconds; <=0 means unset. whole-second options are floored */
    double connect_to, read_to, total_to;
    CURLcode curl_res; /* set when transport error */
} http_req_t;

/* 0 = 2xx, 1 = non-2xx (body captured), -1 = transport error */
int http_perform(http_req_t *r);
bool http_hdr_add(struct curl_slist **list, const char *name, const char *value);
void http_hdr_add_json(struct curl_slist **list);
/* build a header list from a cJSON object of name->value strings; false on
   bad type. err may be NULL (config-time validation already reports) */
bool http_hdrs_from_json(struct curl_slist **list, const cJSON *obj,
                         char *err, size_t errsz);

/* llm endpoint error mapping shared by both wires (from curl result /
   http status + captured body; fatal records) */
cJSON *http_transport_error(http_req_t *req);
/* with_type: include the openai error.type suffix in the message */
cJSON *http_status_error(http_req_t *req, bool with_type);

/* the per-turn llm request shared by both wires: url (api_base + path),
   headers (content type, bearer key unless overridden, custom, sse
   accept), timeouts, stream callback. The caller sets body/body_len,
   performs and calls llm_http_teardown. */
void llm_http_setup(engine_t *e, const char *path, bool stream,
                    void (*on_data)(void *, const char *, size_t), void *ctx,
                    buf_t *url, struct curl_slist **hdrs, http_req_t *r);
void llm_http_teardown(buf_t *url, struct curl_slist *hdrs, http_req_t *r);

/* ================= sse.c ================= */
typedef struct sse_parser sse_parser_t;
typedef void (*sse_cb)(void *ctx, const char *event, const char *data, size_t data_len);

struct sse_parser {
    sse_cb cb;
    void *ctx;
    buf_t line;    /* current line accumulation */
    buf_t event;   /* pending event name */
    buf_t data;    /* pending data lines (joined with \n) */
    bool have_data;
};

void sse_init(sse_parser_t *p, sse_cb cb, void *ctx);
void sse_free(sse_parser_t *p);
void sse_feed(sse_parser_t *p, const char *bytes, size_t n);
void sse_eof(sse_parser_t *p); /* dispatch any unterminated event */

/* ================= jsonl.c ================= */

/* record classification */
enum {
    R_UNKNOWN = -1,
    R_HEADER = 0, R_LLM, R_TOOLS, R_OPTIONS, R_SYSTEM, R_USER,
    R_FLUSH, R_START, R_ERROR,
    R_THINKING, R_RESPONSE, R_TOOL_REQUEST, R_TOOL_RESPONSE,
    R_AGENT, R_EXPOSE, R_HIDE,
};
int rec_classify(const cJSON *tree); /* R_* or R_UNKNOWN */

/* validators return NULL or a malloc'd message (invalid reason) */
char *validate_llm(const cJSON *t);
char *validate_tools(const cJSON *t);
char *validate_options(const cJSON *t);        /* merged options object */
char *validate_content(const cJSON *content);  /* list of {type:"text",text} */

const char *rec_str(const cJSON *t, const char *field); /* string field or NULL */
bool rec_bool(const cJSON *t, const char *field, bool dflt);
double rec_num(const cJSON *t, const char *field, double dflt);

/* byte-level line pusher: drops 0x0d, splits on 0x0a, holds trailing
   fragment, strict UTF-8, NUL rejection. Returns:
   0 ok, -1 invalid line (callback NOT invoked; caller gets reason) */
typedef void (*jsonl_line_fn)(void *ctx, char *line /*malloc'd*/);
typedef struct jsonl_pusher {
    jsonl_line_fn on_line;
    void *ctx;
    buf_t hold; /* pending fragment without \n */
    bool done;  /* eof processed */
} jsonl_pusher_t;

void jsonl_pusher_init(jsonl_pusher_t *p, jsonl_line_fn fn, void *ctx);
void jsonl_pusher_free(jsonl_pusher_t *p);
/* returns 0 ok, -1 invalid utf-8 / NUL byte in chunk (fatal invalid_record) */
int jsonl_feed(jsonl_pusher_t *p, const char *bytes, size_t n);
/* process held fragment at EOF (if non-empty); returns as jsonl_feed */
int jsonl_eof(jsonl_pusher_t *p);
/* parse one line; NULL on malformed json (or embedded NUL - pre-checked) */
cJSON *jsonl_parse_line(const char *line);

/* feed a whole file through the byte pipeline; false on io error or a
   byte-rule violation (record validity is the callback's business) */
bool jsonl_read_file(const char *path, jsonl_line_fn on_line, void *ctx);

/* output record builders (field order fixed, deterministic) */
cJSON *rec_error(const char *code, const char *message, bool fatal);
cJSON *rec_start(void);
cJSON *rec_text(const char *type /*"response"|"thinking"*/, const char *text,
                bool partial);
cJSON *rec_thinking_final(const char *text, const char *signature);
cJSON *rec_tool_request(const char *tool, const cJSON *arguments, const char *id);
cJSON *rec_tool_response(const char *id, const char *text, bool is_error);
void rec_attach_usage(cJSON *rec, double in, double out);
void rec_attach_finish(cJSON *rec, const char *finish);

/* ---- transcript (internal record view) ---- */
enum { T_USER = 1, T_THINK, T_TEXT, T_TREQ, T_TRESP };

typedef struct trec {
    uint8_t kind;
    cJSON *tree;     /* owning record tree (input or synthesized) */
    /* T_USER: content array borrowed from tree */
    /* T_THINK/T_TEXT: */
    char *text;      /* owned, concatenated block text */
    char *signature; /* T_THINK: owned or NULL */
    bool complete;   /* block closed by a partial:false record */
    /* T_TREQ: */
    char *tool, *id;
    cJSON *args; /* owned (borrowed from tree when input) */
    /* T_TRESP: */
    bool is_error;
} trec_t;

void trec_free(trec_t *r);

/* appends (or extends) transcript records from an input/emitted record tree;
   returns the new tail or NULL (caller owns tree regardless) */
typedef struct tlist {
    trec_t **v;
    size_t n, cap;
} tlist_t;

void tlist_clear(tlist_t *l);
bool tlist_ingest(tlist_t *l, cJSON *record_tree); /* false = wrong type */

/* group iteration over a transcript: groups are
   G_USER (one T_USER) or G_ASSIST (block/req run + following TRESP run) */
enum { G_DONE = 0, G_USER, G_ASSIST };
typedef struct group_iter {
    const tlist_t *l;
    size_t i;
    /* filled per group: */
    size_t start, stop;  /* whole-group span [start, stop) */
    size_t begin, end;   /* blocks + treqs  (G_ASSIST) */
    size_t rbegin, rend; /* tool responses  (G_ASSIST) */
    const trec_t *user;  /* G_USER */
    bool complete;       /* G_ASSIST: last block closed / has responses */
} group_iter_t;

/* streaming block emitter: interval-grouped partial records; the block-final
   record of the last block of a final turn is held for the engine's boundary
   decision (drop-rule ordering, design sec.4) */
typedef struct blkemit blkemit_t;
struct blkemit {
    engine_t *e;
    bool streaming;
    double interval, next_flush;
    bool have_open;
    int open_kind; /* 0 = response, 1 = thinking */
    buf_t text;     /* text since the last flush */
    cJSON *pending_final;
};

void blk_init(blkemit_t *b);
void blk_free(blkemit_t *b);
void blk_begin_turn(blkemit_t *b, engine_t *e); /* reset per-turn state */
void blk_delta(blkemit_t *b, int kind, const char *text, size_t n);
void blk_stop_thinking(blkemit_t *b, const char *signature);
void blk_stop(blkemit_t *b);
void blk_emit_pending(blkemit_t *b);        /* emit held block-final now */
cJSON *blk_take_pending(blkemit_t *b);      /* hand off (NULL if none) */
void blk_abort(blkemit_t *b);               /* external stop: trailing partial */

void group_begin(group_iter_t *g, const tlist_t *l);
int group_next(group_iter_t *g); /* G_DONE / G_USER / G_ASSIST */

/* join of a user record's text blocks with \n; false if no text blocks */
bool user_text_join(const trec_t *u, buf_t *out);

/* ================= engine ================= */
struct engine;
struct wire;

typedef void (*emit_fn)(void *ctx, cJSON *record);

typedef struct turn_out {
    int kind; /* TURN_* */
    cJSON *final_rec; /* TURN_FINAL: held final response (not yet emitted) */
    cJSON *error_rec; /* TURN_FATAL */
} turn_out_t;

enum { TURN_FINAL = 1, TURN_TOOLS, TURN_FATAL, TURN_ABORTED };

typedef struct wire {
    int (*turn)(struct wire *w, struct engine *e, turn_out_t *out);
    /* serialization only: fills last_body, no network (tests, sec.13) */
    int (*build)(struct wire *w, struct engine *e); /* 0 ok, 2 invalid_record */
    void (*destroy)(struct wire *w);
    buf_t last_body; /* request body of the most recent build */
} wire_t;

/* tool execution seam: returns 0 ok, 1 failed (err filled), 2 timeout.
   text_out is the tool payload; on failure it may carry a message. */
typedef int (*tool_exec_fn)(struct engine *e, const char *tool, cJSON *args,
                            buf_t *text_out, bool *is_error, char *err,
                            size_t errsz);

struct engine {
    /* config snapshots (owned trees) */
    cJSON *llm, *options, *system, *tools_cfg;
    size_t llm_mark; /* transcript length when current llm took effect */
    int protocol;
    /* bumped on every llm/system snapshot change: prefix rebuild trigger.
       Pointer identity is unsafe here - a replaced tree can be allocated
       at the address of the freed one (allocator reuse), which would hide
       the change and keep stale request bytes. */
    unsigned long cfg_epoch;

    /* transcript */
    tlist_t tr;

    /* mcp servers (mcp.c) */
    struct mcp_mgr *mcp;
    /* ordered tool listing snapshot: entries {server, tool tree} */
    struct tool_listing *tools;

    /* emit sink */
    emit_fn emit;
    void *emit_ctx;

    /* wire */
    wire_t *wire;

    /* parsed option cache */
    double stream_interval; /* 1 default */
    double tool_call_timeout; /* <0 = none */
    double llm_connect_timeout; /* 5 */
    double llm_read_timeout; /* 1200 */
    long max_tool_rounds;    /* -1 none */

    /* input (runner mode) */
    queue_t *inq;
    bool inq_shared; /* a detached stdin reader still pushes to inq */
    bool stdin_eof;
    bool stdin_ioerr;
    bool running;
    bool first_record_seen;
    bool ever_flushed;
    size_t records_since_flush;
    bool flush_seen;
    cJSON **pending; /* buffered steering candidate trees */
    size_t npending, cappending;
    int fatal_code; /* set when a drained record failed validation */
    char *terminal_id; /* id of the answered terminal tool_request, NULL
                          when none ran (design sec.4; the sinks read it) */
    int wire_proto;
    bool keep_mcp; /* repl: sessions continue after engine_run endings;
                      mcp children die at engine_free only (sec.12) */

    /* seams for tests */
    tool_exec_fn tool_exec;
    wire_t *(*wire_factory)(struct engine *e);
};

/* engine.c */
engine_t *engine_new(emit_fn emit, void *ctx);
void engine_free(engine_t *e);
void engine_emit_record(engine_t *e, cJSON *rec); /* sink + ingest */
void engine_ingest_only(engine_t *e, cJSON *rec); /* ingest, no sink */
void engine_apply_config_record(engine_t *e, cJSON *tree); /* llm/options/system/tools snapshot */
int engine_input_record(engine_t *e, cJSON *tree); /* reading-state dispatch:
                                                      0 ok, 1 start now, 2 fatal(rec emitted) */
int engine_start(engine_t *e); /* validate + connect; 0 ok, exit code on fatal */
int engine_run(engine_t *e);   /* turn loop; returns exit code */
void engine_drain_input(engine_t *e); /* non-blocking drain into pending */
int engine_stop_orderly(engine_t *e, cJSON *held_final); /* returns EXIT_INTERRUPTED */
int engine_pre_start_stop(engine_t *e);  /* SIGINT before start; returns exit code */
void engine_rebuild_options_cache(engine_t *e);
wire_t *wire_factory_default(engine_t *e);

/* stdin reader thread body (arg is the engine) */
void *stdin_reader_thread(void *arg);

/* jsonl.c */
void jsonl_stdout_sink(void *ctx, cJSON *rec);

/* ================= mcp.c ================= */
enum { MCP_STDIO = 0, MCP_HTTP, MCP_SSE };

typedef struct mcp_server {
    char *name;
    cJSON *cfg; /* server object from the tools record */
    int type;
    bool required;
    char protocol_req[32]; /* requested revision */
    bool v2;
    /* stdio */
    spawn_t sp;
    queue_t *q;
    bool dead;
    bool have_reader;
    /* http */
    char *url;     /* request url (sse: from endpoint event) */
    char *session; /* Mcp-Session-Id */
    bool sse_ready;
    pthread_t sse_th;
    bool have_sse_th;
    /* common */
    int next_id;
    cJSON *tools; /* listing array snapshot */
    bool connected;
} mcp_server_t;

typedef struct tool_entry {
    mcp_server_t *srv;
    cJSON *tool; /* owned snapshot of the upstream tool object */
    char *exposed_name;
    bool terminal; /* terminal_tools marks it (design sec.6) */
} tool_entry_t;

typedef struct tool_listing {
    tool_entry_t *v;
    size_t n, cap;
} tool_listing_t;

typedef struct mcp_mgr {
    mcp_server_t **v;
    size_t n, cap;
    tool_listing_t listing; /* ordered snapshot, rebuilt by mcp_reconcile */
} mcp_mgr_t;

mcp_mgr_t *mcp_mgr_new(void);
void mcp_mgr_free(mcp_mgr_t *m);
/* protocol revision accepted on either side of a connection (server
   config, client initialize) */
bool mcp_protocol_supported(const char *rev);
/* reconcile server set with a validated tools record; connects new servers.
   Emits connect_failed error records through e->emit (if set) for
   non-required failures. Returns 0, or exit code 3 if a required server
   failed. */
int mcp_reconcile(mcp_mgr_t *m, engine_t *e, const cJSON *tools_record);
const tool_listing_t *mcp_listing(mcp_mgr_t *m); /* ordered snapshot */
mcp_server_t *mcp_find(mcp_mgr_t *m, const char *name);
/* exposed name (server.tool) marked terminal by its server's
   terminal_tools; false when unknown (design sec.6) */
bool mcp_tool_is_terminal(mcp_mgr_t *m, const char *tool);
/* raw call: 0 ok (result tree owned by caller), 1 failed (err), 2 timeout */
int mcp_call_raw(mcp_server_t *srv, const char *upstream_tool,
                 const cJSON *args, cJSON **result_out, char *err, size_t errsz,
                 double timeout);
/* call: 0 ok (text_out,is_error), 1 failed (err), 2 timeout */
int mcp_call(mcp_mgr_t *m, const char *tool /* server.tool */, cJSON *args,
             buf_t *text_out, bool *is_error, char *err, size_t errsz,
             double timeout /*<0 none*/);
void mcp_kill_all(mcp_mgr_t *m);

/* json-rpc stdio server loop (agent-as-tool + mcp-proxy) */
typedef struct rpc_handler {
    /* return: 0 = reply with result tree (ownership taken), 1 = reply with
       error {code,message} (errmsg malloc'd), 2 = no reply (notification),
       -1 = fatal, stop serving */
    int (*handle)(void *ctx, const char *method, cJSON *params, cJSON *id,
                  cJSON **result_out, char **errmsg_out);
    void *ctx;
} rpc_handler_t;

/* serves json-rpc until EOF or fatal. read_fn returns <=0 on end/error. */
typedef int (*rpc_read_fn)(void *ctx, char *buf, size_t bufsz);
typedef void (*rpc_write_fn)(void *ctx, const char *line, size_t n);
void rpc_serve(rpc_handler_t *h, rpc_read_fn rd, rpc_write_fn wr, void *io_ctx);
int rpc_serve_stdio(rpc_handler_t *h);

/* transcript truncation (agent rollback) */
void tlist_truncate(tlist_t *l, size_t n);

/* engine helpers used by the other entry points */
int engine_validate_start(engine_t *e); /* llm+user presence; 0 or exit code */
void http_global_init(void);

/* test hooks (selfcheck): push input into the engine queue as the stdin
   reader thread would */
void engine_test_push_record(engine_t *e, cJSON *tree); /* takes tree */
void engine_test_push_eof(engine_t *e);

/* ================= wire constructors ================= */
wire_t *wire_openai_new(int proto /*PROTO_OPENAI|PROTO_RESPONSES*/);
wire_t *wire_anthropic_new(void);

/* ================= proxy internals (shared with the selfcheck) ========== */
typedef struct exposed_tool {
    struct mcp_server *srv;
    char *upstream_name;
    char *upstream_srv;
    cJSON *presentation;
    char **from;
    char **to;
    size_t nmap;
} exposed_tool_t;

typedef struct proxy_state {
    mcp_mgr_t *mgr;
    exposed_tool_t *ex;
    size_t nex, capex;
    bool ok;
    bool first;
    cJSON *tools_record;
} proxy_state_t;

void proxy_state_init(proxy_state_t *p);
void proxy_state_free(proxy_state_t *p);
/* feed a config line; false on fatal error (already printed to stderr) */
bool proxy_config_line(proxy_state_t *p, char *line /* stolen */);
int proxy_resolve_and_build(proxy_state_t *p); /* 0 ok, -1 fatal */
/* the json-rpc method handler the stdio server loop serves (selfcheck) */
int proxy_handle(void *ctx, const char *method, cJSON *params, cJSON *id,
                 cJSON **result_out, char **errmsg_out);

/* ================= call.c ================= */

/* parsed `llmkit call` command line; the parser owns CLI shape only
   (design sec.11) */
typedef struct call_cfg {
    int protocol;                    /* PROTO_* */
    char *api_base;                  /* owned */
    char *key, *model, *system, *prompt; /* owned, NULL = absent */
    long max_tokens;                 /* -1 absent, >0 sent */
    char **hdr_names, **hdr_values;  /* owned, parallel arrays */
    size_t nhdrs;
    char **proxies;                  /* owned config paths, argv order */
    size_t nproxies;
    char **terminals;                /* owned --terminal-tool values */
    size_t nterminals;
} call_cfg_t;

void call_cfg_free(call_cfg_t *c);
/* 0 ok, 1 usage error (err filled, cfg freed). with_prompt false is the
   repl surface: --prompt is an unknown flag there (requirements sec.12) */
int call_parse_ex(int argc, char **argv, call_cfg_t *c, char *err,
                  size_t errsz, bool with_prompt);
/* 0 ok, 1 usage error (err filled, cfg freed) */
int call_parse(int argc, char **argv, call_cfg_t *c, char *err, size_t errsz);
cJSON *call_build_llm(const call_cfg_t *c);
cJSON *call_build_system(const call_cfg_t *c); /* NULL when absent */
cJSON *call_build_tools(const call_cfg_t *c, const char *exe_path); /* NULL */
cJSON *call_build_user(const char *prompt);
void call_shell_quote(buf_t *b, const char *s); /* POSIX single-quote */
/* /proc/self/exe with argv0 fallback: the mcp-proxy command line */
void self_exe(char *out, size_t sz, const char *argv0);
/* compile the leading records - llm, optional system, optional tools -
   into the engine, validation included; 0 ok or the exit code, errors
   rendered through the engine sink (design sec.11/12) */
int call_compile(const call_cfg_t *c, engine_t *e, const char *exe_path);
/* compile + run one conversation; returns the exit code (design sec.11) */
int call_run(const call_cfg_t *c, FILE *out, FILE *errf, const char *exe_path,
             wire_t *(*factory)(engine_t *));

/* ================= repl.c ================= */

cJSON *repl_build_options(void); /* stream_interval 0 (requirements sec.12) */
/* run the chat session: records compiled from c, input lines from in_fd,
   rendered transcript on out. Returns the exit code (design sec.12). */
int repl_run(const call_cfg_t *c, int in_fd, FILE *out, const char *exe_path,
             wire_t *(*factory)(engine_t *));

/* ================= commands ================= */

int cmd_runner(void);
int cmd_agent(const char *seed_path);
int cmd_proxy(const char *config_path);
int cmd_call(int argc, char **argv);
int cmd_repl(int argc, char **argv);
int cmd_help(void);
int cmd_version(void);

#endif /* LLMKIT_H */
