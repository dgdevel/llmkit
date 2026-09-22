/* engine.c — streaming block emitter, conversation engine, runner command
   (design §2, §3, §4, §8). */
#include "llmkit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================= blkemit ================= */

void blk_init(blkemit_t *b) {
    memset(b, 0, sizeof *b);
    buf_init(&b->text);
}

void blk_free(blkemit_t *b) {
    buf_free(&b->text);
    cJSON_Delete(b->pending_final);
    memset(b, 0, sizeof *b);
    buf_init(&b->text);
}

void blk_begin_turn(blkemit_t *b, engine_t *e) {
    b->e = e;
    b->have_open = false;
    b->open_kind = -1;
    buf_clear(&b->text);
    cJSON_Delete(b->pending_final);
    b->pending_final = NULL;
    b->interval = e ? e->stream_interval : 1.0;
    b->next_flush = mono_now() + (b->interval > 0 ? b->interval : 0);
}

void blk_emit_pending(blkemit_t *b) {
    if (b->pending_final) {
        engine_emit_record(b->e, b->pending_final);
        b->pending_final = NULL;
    }
}

cJSON *blk_take_pending(blkemit_t *b) {
    cJSON *p = b->pending_final;
    b->pending_final = NULL;
    return p;
}

static const char *blk_type_name(int kind) {
    return kind == 1 ? "thinking" : "response";
}

/* close the open block; the final record is held (the previous held final
   is emitted first, preserving order). A thinking block
   always carries its signature field here — empty when the endpoint does
   not use one (requirements §8) — so streamed and non-streamed turns
   produce identical final records. */
void blk_stop(blkemit_t *b) {
    if (!b->have_open) return;
    blk_emit_pending(b);
    cJSON *rec;
    if (b->open_kind == 1) {
        rec = rec_thinking_final(b->text.data ? b->text.data : "", "");
    } else {
        rec = rec_text(blk_type_name(b->open_kind),
                       b->text.data ? b->text.data : "", false);
    }
    b->pending_final = rec;
    b->have_open = false;
    buf_clear(&b->text);
}

void blk_stop_thinking(blkemit_t *b, const char *signature) {
    if (!b->have_open || b->open_kind != 1) {
        /* a thinking block stop with no open block: nothing streamed */
        return;
    }
    blk_emit_pending(b);
    cJSON *rec = rec_thinking_final(b->text.data ? b->text.data : "",
                                    signature ? signature : "");
    b->pending_final = rec;
    b->have_open = false;
    buf_clear(&b->text);
}

void blk_delta(blkemit_t *b, int kind, const char *text, size_t n) {
    if (!b->have_open || b->open_kind != kind) {
        blk_stop(b);
        b->have_open = true;
        b->open_kind = kind;
        buf_clear(&b->text);
    }
    if (n) buf_append(&b->text, text, n);
    if (!b->streaming || !b->text.len) return;
    double now = mono_now();
    if (b->interval <= 0 || now >= b->next_flush) {
        engine_emit_record(
            b->e, rec_text(blk_type_name(kind), b->text.data, true));
        buf_clear(&b->text);
        b->next_flush = now + (b->interval > 0 ? b->interval : 0);
    }
}

/* external stop: pending final becomes a trailing partial, buffered text is
   flushed as the last partial; nothing keeps a signature or final flag */
void blk_abort(blkemit_t *b) {
    if (b->pending_final) {
        cJSON *r = b->pending_final;
        b->pending_final = NULL;
        cJSON_ReplaceItemInObjectCaseSensitive(r, "partial",
                                               cJSON_CreateBool(true));
        cJSON_DeleteItemFromObject(r, "signature");
        cJSON_DeleteItemFromObject(r, "usage");
        cJSON_DeleteItemFromObject(r, "finish_reason");
        engine_emit_record(b->e, r);
    }
    if (b->have_open && b->text.len) {
        engine_emit_record(
            b->e, rec_text(blk_type_name(b->open_kind), b->text.data, true));
        buf_clear(&b->text);
    }
    b->have_open = false;
}

/* ================= engine basics ================= */

engine_t *engine_new(emit_fn emit, void *ctx) {
    engine_t *e = calloc(1, sizeof *e);
    e->emit = emit;
    e->emit_ctx = ctx;
    e->mcp = (struct mcp_mgr *)mcp_mgr_new();
    e->tools = (struct tool_listing *)&e->mcp->listing;
    e->stream_interval = 1.0;
    e->tool_call_timeout = -1;
    e->llm_connect_timeout = 5;
    e->llm_read_timeout = 1200;
    e->max_tool_rounds = -1;
    return e;
}

void engine_free(engine_t *e) {
    if (!e) return;
    cJSON_Delete(e->llm);
    cJSON_Delete(e->options);
    cJSON_Delete(e->system);
    cJSON_Delete(e->tools_cfg);
    tlist_clear(&e->tr);
    mcp_mgr_free((mcp_mgr_t *)e->mcp);
    if (e->wire) e->wire->destroy(e->wire);
    for (size_t i = 0; i < e->npending; i++) cJSON_Delete(e->pending[i]);
    free(e->pending);
    if (e->inq && !e->inq_shared)
        queue_free(e->inq); /* shared: the stdin reader may still push;
                               leaking at exit beats freeing under it
                               (ponytail: a self-pipe wake needs a real
                               caller — only the runner shares, and it is
                               about to _exit) */
    free(e);
}

void engine_emit_record(engine_t *e, cJSON *rec) {
    if (!rec) return;
    tlist_ingest(&e->tr, rec); /* transcript records only; others ignored */
    e->emit(e->emit_ctx, rec); /* sink consumes the record */
}

void engine_ingest_only(engine_t *e, cJSON *rec) { tlist_ingest(&e->tr, rec); }

static void engine_emit_error(engine_t *e, const char *code, const char *msg,
                              bool fatal) {
    e->emit(e->emit_ctx, rec_error(code, msg, fatal));
}

void engine_rebuild_options_cache(engine_t *e) {
    e->max_tool_rounds = -1;
    e->tool_call_timeout = -1;
    e->stream_interval = 1.0;
    e->llm_connect_timeout = 5;
    e->llm_read_timeout = 1200;
    if (!e->options) return;
    const cJSON *f;
    if ((f = cJSON_GetObjectItemCaseSensitive(e->options, "max_tool_rounds")) &&
        cJSON_IsNumber(f))
        e->max_tool_rounds = (long)f->valuedouble;
    if ((f = cJSON_GetObjectItemCaseSensitive(e->options, "tool_call_timeout")) &&
        cJSON_IsNumber(f))
        e->tool_call_timeout = f->valuedouble;
    if ((f = cJSON_GetObjectItemCaseSensitive(e->options, "stream_interval")) &&
        cJSON_IsNumber(f))
        e->stream_interval = f->valuedouble;
    if ((f = cJSON_GetObjectItemCaseSensitive(e->options, "llm_connect_timeout")) &&
        cJSON_IsNumber(f))
        e->llm_connect_timeout = f->valuedouble;
    if ((f = cJSON_GetObjectItemCaseSensitive(e->options, "llm_read_timeout")) &&
        cJSON_IsNumber(f))
        e->llm_read_timeout = f->valuedouble;
}

void engine_apply_config_record(engine_t *e, cJSON *tree) {
    switch (rec_classify(tree)) {
    case R_LLM:
        cJSON_Delete(e->llm);
        e->llm = cJSON_Duplicate(tree, 1);
        e->protocol = protocol_of(rec_str(tree, "endpoint_protocol"));
        e->llm_mark = e->tr.n;
        e->cfg_epoch++;
        break;
    case R_OPTIONS: {
        if (!e->options) e->options = cJSON_CreateObject();
        for (cJSON *f = tree->child; f; f = f->next) {
            if (f->string && !strcmp(f->string, "type")) continue;
            cJSON_DeleteItemFromObject(e->options, f->string);
            cJSON_AddItemToObject(e->options, f->string,
                                  cJSON_Duplicate(f, 1));
        }
        engine_rebuild_options_cache(e);
        break;
    }
    case R_SYSTEM:
        cJSON_Delete(e->system);
        e->system = cJSON_Duplicate(tree, 1);
        e->cfg_epoch++;
        break;
    case R_TOOLS:
        cJSON_Delete(e->tools_cfg);
        e->tools_cfg = cJSON_Duplicate(tree, 1);
        break;
    default:
        break;
    }
}

/* ================= record validation (shared by both input paths) ======== */

static char *validate_any_record(cJSON *tree) {
    int k = rec_classify(tree);
    switch (k) {
    case R_LLM: return validate_llm(tree);
    case R_TOOLS: return validate_tools(tree);
    case R_OPTIONS: return validate_options(tree);
    case R_SYSTEM:
    case R_USER: {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(tree, "content");
        return validate_content(c);
    }
    case R_THINKING:
    case R_RESPONSE:
        if (!rec_str(tree, "text")) return strdup("missing text field");
        return NULL;
    case R_TOOL_REQUEST: {
        if (!rec_str(tree, "tool")) return strdup("missing tool field");
        if (!rec_str(tree, "id")) return strdup("missing id field");
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(tree, "arguments");
        if (a && !cJSON_IsObject(a)) return strdup("arguments must be an object");
        return NULL;
    }
    case R_TOOL_RESPONSE: {
        if (!rec_str(tree, "id")) return strdup("missing id field");
        if (!rec_str(tree, "text")) return strdup("missing text field");
        return NULL;
    }
    default:
        return NULL;
    }
}

static void emit_invalid(engine_t *e, const char *msg) {
    char m[512];
    snprintf(m, sizeof m, "%s", msg);
    engine_emit_error(e, EC_INVALID_RECORD, m, true);
}

/* ================= reading-state input dispatch ================= */

int engine_input_record(engine_t *e, cJSON *tree) {
    int k = rec_classify(tree);
    bool first = !e->first_record_seen;
    e->first_record_seen = true;
    switch (k) {
    case R_HEADER:
        if (first) {
            double v = rec_num(tree, "version", -1);
            if (v != 1) {
                emit_invalid(e, "unsupported header version");
                cJSON_Delete(tree);
                return 2;
            }
        }
        cJSON_Delete(tree);
        return 0;
    case R_START:
    case R_ERROR:
        cJSON_Delete(tree);
        return 0;
    case R_FLUSH: {
        cJSON_Delete(tree);
        bool first_flush = !e->ever_flushed;
        if (first_flush || e->records_since_flush > 0) {
            e->ever_flushed = true;
            return 1; /* start now */
        }
        engine_emit_error(
            e, EC_INVALID_RECORD,
            "bare flush: no new records since the previous one", false);
        return 0;
    }
    default: {
        switch (k) {
        case R_LLM:
        case R_TOOLS:
        case R_OPTIONS:
        case R_SYSTEM:
        case R_USER:
        case R_THINKING:
        case R_RESPONSE:
        case R_TOOL_REQUEST:
        case R_TOOL_RESPONSE:
            break;
        default:
            emit_invalid(e, k == R_UNKNOWN || k == R_AGENT || k == R_EXPOSE ||
                                   k == R_HIDE
                               ? "unknown record type"
                               : "record type not accepted here");
            cJSON_Delete(tree);
            return 2;
        }
        char *msg = validate_any_record(tree);
        if (msg) {
            emit_invalid(e, msg);
            free(msg);
            cJSON_Delete(tree);
            return 2;
        }
        if (k == R_LLM || k == R_TOOLS || k == R_OPTIONS || k == R_SYSTEM)
            engine_apply_config_record(e, tree);
        else
            tlist_ingest(&e->tr, tree);
        e->records_since_flush++;
        cJSON_Delete(tree);
        return 0;
    }
    }
}

/* ================= running-state drain + steering ================= */

static void pending_push(engine_t *e, cJSON *tree) {
    if (e->npending == e->cappending) {
        e->cappending = e->cappending ? e->cappending * 2 : 8;
        e->pending = realloc(e->pending, e->cappending * sizeof *e->pending);
    }
    e->pending[e->npending++] = tree;
}

typedef struct qmsg {
    int kind; /* Q_REC, Q_EOF, Q_IOERR, Q_BADLINE */
    cJSON *tree;
    char *msg;
} qmsg_t;
enum { Q_REC = 0, Q_EOF, Q_IOERR, Q_BADLINE };

static qmsg_t *qmsg_new(int kind, cJSON *tree, char *msg) {
    qmsg_t *m = calloc(1, sizeof *m);
    m->kind = kind;
    m->tree = tree;
    m->msg = msg;
    return m;
}

void engine_drain_input(engine_t *e) {
    if (!e->inq) return;
    for (;;) {
        qmsg_t *m = queue_try_pop(e->inq);
        if (!m) break;
        switch (m->kind) {
        case Q_EOF:
            e->stdin_eof = true;
            break;
        case Q_IOERR:
            e->stdin_ioerr = true;
            break;
        case Q_BADLINE:
            emit_invalid(e, m->msg ? m->msg : "malformed input");
            free(m->msg);
            e->fatal_code = EXIT_INVALID_RECORD;
            break;
        default: {
            cJSON *tree = m->tree;
            int k = rec_classify(tree);
            if (k == R_FLUSH) {
                cJSON_Delete(tree);
                if (e->records_since_flush > 0) e->flush_seen = true;
                else
                    engine_emit_error(
                        e, EC_INVALID_RECORD,
                        "bare flush: no new records since the previous one",
                        false);
            } else if (k == R_START || k == R_ERROR || k == R_HEADER) {
                cJSON_Delete(tree);
            } else {
                char *msg = validate_any_record(tree);
                if (msg) {
                    emit_invalid(e, msg);
                    free(msg);
                    cJSON_Delete(tree);
                    e->fatal_code = EXIT_INVALID_RECORD;
                } else {
                    pending_push(e, tree);
                    e->records_since_flush++;
                }
            }
            break;
        }
        }
        free(m);
        if (e->fatal_code || e->stdin_ioerr) {
            /* drain the rest silently: the process is stopping */
            while ((m = queue_try_pop(e->inq))) {
                cJSON_Delete(m->tree);
                free(m->msg);
                free(m);
            }
            break;
        }
    }
}

static bool steering_ready(engine_t *e) {
    return e->npending > 0 && (e->flush_seen || e->stdin_eof);
}

static int apply_steering(engine_t *e) {
    bool marker = e->flush_seen;
    e->flush_seen = false;
    e->records_since_flush = 0;
    if (marker) e->emit(e->emit_ctx, rec_start());
    for (size_t i = 0; i < e->npending; i++) {
        cJSON *tree = e->pending[i];
        int k = rec_classify(tree);
        if (k == R_TOOLS) {
            cJSON_Delete(e->tools_cfg);
            e->tools_cfg = cJSON_Duplicate(tree, 1);
            int rc = mcp_reconcile((mcp_mgr_t *)e->mcp, e, e->tools_cfg);
            if (rc) {
                cJSON_Delete(tree);
                e->npending = 0;
                return rc;
            }
        } else if (k == R_LLM || k == R_OPTIONS || k == R_SYSTEM) {
            engine_apply_config_record(e, tree);
        } else {
            tlist_ingest(&e->tr, tree);
        }
        cJSON_Delete(tree);
    }
    e->npending = 0;
    return 0;
}

/* ================= start + turn loop ================= */

int engine_validate_start(engine_t *e) {
    if (!e->llm) {
        engine_emit_error(e, EC_INVALID_RECORD,
                          "missing llm record at conversation start", true);
        return EXIT_INVALID_RECORD;
    }
    bool any_user = false;
    for (size_t i = 0; i < e->tr.n; i++)
        if (e->tr.v[i]->kind == T_USER) any_user = true;
    if (!any_user) {
        engine_emit_error(e, EC_INVALID_RECORD,
                          "no user record in the transcript", true);
        return EXIT_INVALID_RECORD;
    }
    return 0;
}

int engine_start(engine_t *e) {
    int rc = engine_validate_start(e);
    if (rc) return rc;
    if (e->tools_cfg) {
        rc = mcp_reconcile((mcp_mgr_t *)e->mcp, e, e->tools_cfg);
        if (rc) return rc;
    }
    e->running = true;
    return 0;
}

static wire_t *ensure_wire(engine_t *e) {
    if (e->wire && e->wire_proto == e->protocol) return e->wire;
    if (e->wire) {
        e->wire->destroy(e->wire);
        e->wire = NULL;
    }
    e->wire = e->wire_factory ? e->wire_factory(e) : wire_factory_default(e);
    e->wire_proto = e->protocol;
    return e->wire;
}

wire_t *wire_factory_default(engine_t *e) {
    switch (e->protocol) {
    case PROTO_RESPONSES: return wire_openai_new(PROTO_RESPONSES);
    case PROTO_ANTHROPIC: return wire_anthropic_new();
    default: return wire_openai_new(PROTO_OPENAI);
    }
}

static int default_tool_exec(engine_t *e, const char *tool, cJSON *args,
                             buf_t *text_out, bool *is_error, char *err,
                             size_t errsz) {
    return mcp_call((mcp_mgr_t *)e->mcp, tool, args, text_out, is_error, err,
                    errsz, e->tool_call_timeout);
}

static void run_tool(engine_t *e, trec_t *r) {
    buf_t text;
    buf_init(&text);
    bool is_err = false;
    char err[512] = "";
    if (!e->tool_exec) e->tool_exec = default_tool_exec;
    int rc = e->tool_exec(e, r->tool, r->args, &text, &is_err, err, sizeof err);
    if (rc == 2) {
        engine_emit_record(e, rec_tool_response(
                                  r->id,
                                  text.len ? text.data
                                           : (err[0] ? err : "tool timed out"),
                                  true));
        engine_emit_error(e, EC_TOOL_TIMEOUT, err[0] ? err : "tool timed out",
                          false);
    } else if (rc == 1) {
        engine_emit_record(
            e, rec_tool_response(r->id,
                                 text.len ? text.data
                                          : (err[0] ? err : "tool failed"),
                                 true));
        engine_emit_error(e, EC_TOOL_FAILED, err[0] ? err : "tool failed",
                          false);
    } else {
        engine_emit_record(e, rec_tool_response(r->id, text.data ? text.data : "",
                                                is_err));
        if (is_err)
            engine_emit_error(e, EC_TOOL_FAILED,
                              text.data ? text.data : "tool failed", false);
    }
    buf_free(&text);
}

static void synth_tool_response(engine_t *e, trec_t *r, const char *msg) {
    engine_emit_record(e, rec_tool_response(r->id, msg, true));
}

static void emit_held_final(engine_t *e, cJSON *rec) {
    if (!rec) return;
    tlist_ingest(&e->tr, rec);
    e->emit(e->emit_ctx, rec);
}

int engine_stop_orderly(engine_t *e, cJSON *held_final) {
    emit_held_final(e, held_final);
    if (e->npending) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "%zu record(s) received without a flush were dropped",
                 e->npending);
        engine_emit_error(e, EC_IO_ERROR, msg, false);
        for (size_t i = 0; i < e->npending; i++) cJSON_Delete(e->pending[i]);
        e->npending = 0;
    }
    engine_emit_error(e, EC_INTERRUPTED, "conversation stopped externally",
                      true);
    mcp_kill_all((mcp_mgr_t *)e->mcp);
    return EXIT_INTERRUPTED;
}

int engine_pre_start_stop(engine_t *e) {
    if (e->records_since_flush) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "%zu record(s) received without a flush were dropped",
                 e->records_since_flush);
        engine_emit_error(e, EC_IO_ERROR, msg, false);
    }
    mcp_kill_all((mcp_mgr_t *)e->mcp);
    return EXIT_INTERRUPTED;
}

int engine_run(engine_t *e) {
    long turns = 0;
    if (!e->tool_exec) e->tool_exec = default_tool_exec;
    for (;;) {
        if (g_stop_flag) {
            /* records received before the stop still get the drop rule */
            engine_drain_input(e);
            return engine_stop_orderly(e, NULL);
        }
        if (e->max_tool_rounds >= 0 && turns >= e->max_tool_rounds) {
            engine_emit_error(e, EC_MAX_ROUNDS,
                              "max_tool_rounds exceeded: the turn that would "
                              "exceed the limit is not started",
                              true);
            return EXIT_MAX_TOOL_ROUNDS;
        }
        wire_t *w = ensure_wire(e);
        turn_out_t out;
        size_t tr_before = e->tr.n;
        int kind = w->turn(w, e, &out);
        if (kind == TURN_FATAL) {
            const char *code = rec_str(out.error_rec, "code");
            int rc = exit_code_of(code);
            e->emit(e->emit_ctx, out.error_rec);
            mcp_kill_all((mcp_mgr_t *)e->mcp);
            return rc;
        }
        if (kind == TURN_ABORTED) {
            engine_drain_input(e); /* drop rule for pre-stop records */
            return engine_stop_orderly(e, NULL);
        }
        turns++;

        if (kind == TURN_TOOLS) {
            bool susp_int = false, susp_steer = false;
            for (size_t i = tr_before; i < e->tr.n; i++) {
                trec_t *r = e->tr.v[i];
                if (r->kind != T_TREQ) continue;
                if (susp_int) {
                    synth_tool_response(e, r, "conversation interrupted");
                    continue;
                }
                if (susp_steer) {
                    synth_tool_response(e, r,
                                        "the user steered the conversation");
                    continue;
                }
                engine_drain_input(e);
                if (e->fatal_code) {
                    mcp_kill_all((mcp_mgr_t *)e->mcp);
                    return e->fatal_code;
                }
                if (e->stdin_ioerr) {
                    engine_emit_error(e, EC_IO_ERROR, "stdin read failed", true);
                    mcp_kill_all((mcp_mgr_t *)e->mcp);
                    return EXIT_IO_ERROR;
                }
                if (g_stop_flag) {
                    susp_int = true;
                    synth_tool_response(e, r, "conversation interrupted");
                    continue;
                }
                if (e->npending && e->flush_seen) {
                    susp_steer = true;
                    synth_tool_response(e, r,
                                        "the user steered the conversation");
                    continue;
                }
                run_tool(e, r);
            }
            engine_drain_input(e);
            if (e->fatal_code) {
                mcp_kill_all((mcp_mgr_t *)e->mcp);
                return e->fatal_code;
            }
            if (e->stdin_ioerr) {
                engine_emit_error(e, EC_IO_ERROR, "stdin read failed", true);
                mcp_kill_all((mcp_mgr_t *)e->mcp);
                return EXIT_IO_ERROR;
            }
            if (g_stop_flag) return engine_stop_orderly(e, NULL);
            if (steering_ready(e)) {
                int rc = apply_steering(e);
                if (rc) {
                    mcp_kill_all((mcp_mgr_t *)e->mcp);
                    return rc;
                }
            }
            continue; /* next turn */
        }

        /* TURN_FINAL */
        engine_drain_input(e);
        if (e->fatal_code) {
            emit_held_final(e, out.final_rec);
            mcp_kill_all((mcp_mgr_t *)e->mcp);
            return e->fatal_code;
        }
        if (e->stdin_ioerr) {
            engine_emit_error(e, EC_IO_ERROR, "stdin read failed", true);
            emit_held_final(e, out.final_rec);
            mcp_kill_all((mcp_mgr_t *)e->mcp);
            return EXIT_IO_ERROR;
        }
        if (g_stop_flag) {
            emit_held_final(e, out.final_rec);
            return engine_stop_orderly(e, NULL);
        }
        if (steering_ready(e)) {
            emit_held_final(e, out.final_rec);
            int rc = apply_steering(e);
            if (rc) {
                mcp_kill_all((mcp_mgr_t *)e->mcp);
                return rc;
            }
            continue;
        }
        if (e->npending) {
            /* drop rule: records that never received a flush (stdin open) */
            char msg[128];
            snprintf(msg, sizeof msg,
                     "%zu record(s) received without a flush were dropped",
                     e->npending);
            engine_emit_error(e, EC_IO_ERROR, msg, false);
            emit_held_final(e, out.final_rec);
            mcp_kill_all((mcp_mgr_t *)e->mcp);
            return EXIT_OK;
        }
        emit_held_final(e, out.final_rec);
        mcp_kill_all((mcp_mgr_t *)e->mcp);
        return EXIT_OK;
    }
}

/* ================= stdin reader thread ================= */

static void reader_on_line(void *ctx, char *line) {
    stdin_reader_ctx_t *rc = ctx;
    cJSON *t = jsonl_parse_line(line);
    free(line);
    if (t) queue_push(rc->e->inq, qmsg_new(Q_REC, t, NULL));
    else
        queue_push(rc->e->inq,
                   qmsg_new(Q_BADLINE, NULL, strdup("malformed json line")));
}

void *stdin_reader_thread(void *arg) {
    stdin_reader_ctx_t *rc = arg;
    engine_t *e = rc->e;
    jsonl_pusher_t p;
    jsonl_pusher_init(&p, reader_on_line, rc);
    char bbuf[8192];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, bbuf, sizeof bbuf);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop_flag) break; /* records after the stop are not read */
                continue;
            }
            queue_push(e->inq, qmsg_new(Q_IOERR, NULL, NULL));
            break;
        }
        if (n == 0) {
            if (jsonl_eof(&p) != 0)
                queue_push(e->inq, qmsg_new(
                                       Q_BADLINE, NULL,
                                       strdup("invalid utf-8 or NUL byte in input")));
            queue_push(e->inq, qmsg_new(Q_EOF, NULL, NULL));
            break;
        }
        if (g_stop_flag) break; /* discard: the stop is already committed */
        if (jsonl_feed(&p, bbuf, (size_t)n) != 0) {
            queue_push(e->inq,
                       qmsg_new(Q_BADLINE, NULL,
                                strdup("invalid utf-8 or NUL byte in input")));
            break;
        }
    }
    jsonl_pusher_free(&p);
    free(rc);
    return NULL;
}

/* ================= test hooks ================= */

void engine_test_push_record(engine_t *e, cJSON *tree) {
    if (!e->inq) e->inq = queue_new();
    queue_push(e->inq, qmsg_new(Q_REC, tree, NULL));
}

void engine_test_push_eof(engine_t *e) {
    if (!e->inq) e->inq = queue_new();
    queue_push(e->inq, qmsg_new(Q_EOF, NULL, NULL));
}

void engine_test_push_ioerr(engine_t *e) {
    if (!e->inq) e->inq = queue_new();
    queue_push(e->inq, qmsg_new(Q_IOERR, NULL, NULL));
}

/* ================= runner command ================= */

static int runner_attempt_start(engine_t *e, bool via_flush) {
    int rc = engine_start(e);
    if (rc) return rc;
    e->records_since_flush = 0;
    if (via_flush) e->emit(e->emit_ctx, rec_start());
    return 0;
}

int cmd_runner(void) {
    signals_init();
    engine_t *e = engine_new(jsonl_stdout_sink, NULL);
    e->emit(e->emit_ctx, cJSON_Parse("{\"type\":\"header\",\"version\":1}"));
    e->inq = queue_new();

    stdin_reader_ctx_t *rc = calloc(1, sizeof *rc);
    rc->e = e;
    e->inq_shared = true;
    thread_start_detached(stdin_reader_thread, rc);

    int code = 0;
    bool started = false;
    while (!started) {
        /* interruptible wait: a SIGINT before the start must reach the
           pre-start stop path, and the condvar wait does not wake on a
           signal — poll with a short timeout and watch the flag */
        qmsg_t *m = NULL;
        while (!m && !g_stop_flag && !queue_closed(e->inq))
            m = queue_pop_timeout(e->inq, 0.2);
        if (!m) break; /* stop flag, or queue closed and empty */
        int kind = m->kind;
        cJSON *tree = m->tree;
        char *msg = m->msg;
        free(m);
        if (g_stop_flag) {
            cJSON_Delete(tree);
            free(msg);
            code = engine_pre_start_stop(e);
            goto done;
        }
        switch (kind) {
        case Q_EOF: {
            int src = runner_attempt_start(e, false);
            if (src) {
                code = src;
                goto done;
            }
            started = true;
            break;
        }
        case Q_IOERR:
            engine_emit_error(e, EC_IO_ERROR, "stdin read failed", true);
            code = EXIT_IO_ERROR;
            goto done;
        case Q_BADLINE:
            emit_invalid(e, msg ? msg : "malformed input");
            free(msg);
            code = EXIT_INVALID_RECORD;
            goto done;
        default: {
            int r = engine_input_record(e, tree); /* consumes tree */
            if (r == 1) {
                int src = runner_attempt_start(e, true);
                if (src) {
                    code = src;
                    goto done;
                }
                started = true;
            } else if (r == 2) {
                code = EXIT_INVALID_RECORD;
                goto done;
            }
            break;
        }
        }
    }
    if (g_stop_flag && !started) {
        code = engine_pre_start_stop(e);
        goto done;
    }
    if (started) code = engine_run(e);
done:
    mcp_kill_all((mcp_mgr_t *)e->mcp);
    engine_free(e);
    return code;
}
