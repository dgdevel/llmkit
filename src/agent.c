#include "agent.h"
#include "conversation.h"
#include "mcp.h"
#include "llm.h"
#include "util.h"
#include "platform.h"
#include "steering.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* File-scope run mode. Maps to RUN_MODE_QUIET/DEBUG/STREAM strings. */
static const char *g_run_mode = RUN_MODE_QUIET;

/* For OM_QUIET: stores the last assistant content to print at end. */
static char *g_last_content = NULL;

/* Whether steering (--steer) is active for this run. */
static bool g_steering = false;

/* Run-mode constants for internal dispatch. */
#define OM_QUIET  0
#define OM_DEBUG  1
#define OM_STREAM 2

static int resolve_run_mode(const char *mode) {
    if (mode == NULL) return OM_QUIET;
    if (strcmp(mode, RUN_MODE_DEBUG) == 0) return OM_DEBUG;
    if (strcmp(mode, RUN_MODE_STREAM) == 0) return OM_STREAM;
    return OM_QUIET;
}

/* ------------------------------------------------------------------ */
/*  Prompt resolution                                                  */
/* ------------------------------------------------------------------ */
/* If prompt_arg is an existing file path, read its contents.
 * Otherwise treat it as the literal prompt text.
 * Returns a malloc'd string, or NULL on error. */
static char *resolve_prompt(const char *prompt_arg) {
    if (prompt_arg == NULL || prompt_arg[0] == '\0') return NULL;

    char *text = util_read_file(prompt_arg);
    if (text != NULL) {
        /* File existed - trim trailing newline(s). */
        size_t len = strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
            text[--len] = '\0';
        }
        return text;
    }

    /* Not a file - use as literal. */
    return util_strdup(prompt_arg);
}

/* ------------------------------------------------------------------ */
/*  Find tool_def by namespaced name                                   */
/* ------------------------------------------------------------------ */
static const tool_def *find_tool(const tool_def *tools, int tool_count,
                                 const char *namespaced_name) {
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, namespaced_name) == 0) {
            return &tools[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Stdout emit helpers                                                */
/* ------------------------------------------------------------------ */

/* Resolve the mode integer once at startup. */
static int g_om = OM_QUIET;

/* Write a JSONL event to stdout (stream mode only). */
static void emit_event(cJSON *event) {
    if (g_om != OM_STREAM) return;
    char *json = cJSON_PrintUnformatted(event);
    if (json) {
        fputs(json, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(json);
    }
}

/* Write a timestamped human-readable line to stdout (debug mode only). */
static void emit_debug(const char *format, ...) {
    if (g_om != OM_DEBUG) return;
    char ts[64];
    util_timestamp_now(ts, sizeof(ts));
    fprintf(stdout, "[%s] ", ts);
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
}

/* Store assistant content for quiet mode, print it in debug/stream modes. */
static void emit_assistant_content(const char *content) {
    /* Quiet: store for final print. */
    if (g_om == OM_QUIET) {
        free(g_last_content);
        g_last_content = content ? util_strdup(content) : NULL;
        return;
    }
    /* Debug: print as human-readable line. */
    if (g_om == OM_DEBUG) {
        emit_debug("assistant: %s", content ? content : "");
        return;
    }
    /* Stream: handled via emit_assistant_event below -- no plain-text. */
}

static void emit_turn_start(int turn) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "turn_start");
        cJSON_AddNumberToObject(event, "turn", turn);
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("turn_start turn=%d", turn);
    }
    /* quiet: nothing */
}

static void emit_assistant_event(const char *content, const char *model, const usage_info *usage) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "assistant");
        cJSON_AddStringToObject(event, "content", content ? content : "");
        cJSON_AddStringToObject(event, "model", model ? model : "");
        cJSON_AddStringToObject(event, "timestamp", ts);
        if (usage && usage->total_tokens > 0) {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddNumberToObject(u, "prompt_tokens", usage->prompt_tokens);
            cJSON_AddNumberToObject(u, "completion_tokens", usage->completion_tokens);
            cJSON_AddNumberToObject(u, "total_tokens", usage->total_tokens);
            cJSON_AddItemToObject(event, "usage", u);
        }
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("assistant: %s", content ? content : "");
    }
    /* quiet: assistant_content is stored via emit_assistant_content */
}

static void emit_tool_call_event(const char *id, const char *name, const char *arguments,
                                 const char *mcp_server) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "tool_call");
        cJSON_AddStringToObject(event, "id", id ? id : "");
        cJSON_AddStringToObject(event, "name", name ? name : "");
        cJSON_AddStringToObject(event, "arguments", arguments ? arguments : "");
        cJSON_AddStringToObject(event, "mcp_server", mcp_server ? mcp_server : "");
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("tool_call: %s(%s)", name ? name : "", arguments ? arguments : "");
    }
    /* quiet: nothing */
}

static void emit_tool_result_event(const char *call_id, const char *name, const char *result,
                                   int is_error, int is_timeout, const char *mcp_server) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "tool_result");
        cJSON_AddStringToObject(event, "call_id", call_id ? call_id : "");
        cJSON_AddStringToObject(event, "name", name ? name : "");
        cJSON_AddStringToObject(event, "result", result ? result : "");
        cJSON_AddBoolToObject(event, "is_error", is_error ? 1 : 0);
        cJSON_AddBoolToObject(event, "is_timeout", is_timeout ? 1 : 0);
        cJSON_AddStringToObject(event, "mcp_server", mcp_server ? mcp_server : "");
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        const char *status;
        if (is_error) {
            status = is_timeout ? "TIMEOUT" : "ERROR";
        } else {
            status = "ok";
        }
        emit_debug("tool_result: %s -> %s", name ? name : "", status);
    }
    /* quiet: nothing */
}

static void emit_done(int turns) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "done");
        cJSON_AddNumberToObject(event, "turns", turns);
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("done turns=%d", turns);
    }
    /* quiet: nothing */
}

static void emit_error_event(int code, const char *message) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "error");
        cJSON_AddNumberToObject(event, "code", code);
        cJSON_AddStringToObject(event, "message", message ? message : "");
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("error code=%d %s", code, message ? message : "");
    } else if (om == OM_QUIET) {
        /* Quiet mode: print error to stdout so the caller sees it. */
        fprintf(stdout, "error: %s\n", message ? message : "");
        fflush(stdout);
    }
}

/* Print the final assistant content (quiet mode only). */
static void emit_quiet_final(void) {
    if (g_om != OM_QUIET) return;
    if (g_last_content && g_last_content[0] != '\0') {
        fputs(g_last_content, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

/* Emit a steer event: a steering user message was injected into the
 * conversation. Stream mode emits a JSONL event; debug mode a timestamped
 * line; quiet mode is silent. */
static void emit_steer_event(const char *content) {
    int om = g_om;
    if (om == OM_STREAM) {
        char ts[64];
        util_timestamp_now(ts, sizeof(ts));
        cJSON *event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "type", "steer");
        cJSON_AddStringToObject(event, "content", content ? content : "");
        cJSON_AddStringToObject(event, "timestamp", ts);
        emit_event(event);
        cJSON_Delete(event);
    } else if (om == OM_DEBUG) {
        emit_debug("steer: %s", content ? content : "");
    }
    /* quiet: nothing */
}

/* Drain any pending steering messages from stdin and append each as a user
 * entry to the conversation file. Returns the number of messages injected.
 * A no-op when steering is disabled. */
static int drain_steering(FILE *fp) {
    if (!g_steering) return 0;

    steering_poll();

    int count = 0;
    char *msg = NULL;
    while ((msg = steering_take()) != NULL) {
        conversation_write_entry(fp, ENTRY_USER, msg, "steer");
        emit_steer_event(msg);
        log_activity("[steer] injected user message");
        free(msg);
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  Startup sequence                                                   */
/* ------------------------------------------------------------------ */
static int startup_sequence(runtime_ctx *ctx, const char *convo_path, const char *prompt_arg,
                            FILE **out_fp) {
    /* Open conversation file. */
    int rc = conversation_open(convo_path, out_fp);
    if (rc != EXIT_SUCCESS) {
        log_activity("[error] Cannot open conversation file: %s", convo_path);
        return rc;
    }

    /* Write meta entry. "sha256:" prefix for config_hash. */
    {
        char prefixed[80];
        snprintf(prefixed, sizeof(prefixed), "sha256:%s", ctx->config_hash);
        conversation_write_meta(*out_fp, prefixed, ctx->run_id);
    }

    /* Resolve prompt. */
    char *prompt_text = resolve_prompt(prompt_arg);
    /* Spec step 4: reject empty or whitespace-only prompts (exit 2). */
    if (prompt_text != NULL) {
        size_t j = 0;
        unsigned char blank = 1;
        while (prompt_text[j] != '\0') {
            if (!isspace((unsigned char)prompt_text[j])) {
                blank = 0;
                break;
            }
            j++;
        }
        if (blank) {
            log_activity("[error] Empty prompt");
            free(prompt_text);
            return EXIT_ARGS_ERR;
        }
    }
    if (prompt_text == NULL) {
        log_activity("[error] Empty prompt");
        return EXIT_ARGS_ERR;
    }

    /* Write user entry. */
    conversation_write_entry(*out_fp, ENTRY_USER, prompt_text,
                             util_read_file(prompt_arg) ? "file" : "cli");
    free(prompt_text);

    /* Connect MCP servers. */
    log_activity("[init] Connecting to MCP servers...");
    rc = mcp_connect_all(ctx);
    if (rc != EXIT_SUCCESS) {
        log_activity("[error] Failed to connect to MCP servers");
        return rc;
    }

    log_activity("[init] LLM API ready");
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Conversation loop                                                  */
/* ------------------------------------------------------------------ */
static int conversation_loop(runtime_ctx *ctx, FILE *fp) {
    int rc = EXIT_SUCCESS;
    int max_turns = 50; /* safety limit */

    for (int turn = 0; turn < max_turns; turn++) {
        emit_turn_start(turn + 1);

        /* ---- Drain any pending steering messages from stdin ---- */
        /* Written as user entries here so the reconstruct below includes them
         * in the very next LLM call. This is the earliest legal injection
         * point: the OpenAI API forbids interleaving a user message between
         * an assistant's tool_calls and their tool results. */
        drain_steering(fp);

        /* ---- Reconstruct message history ---- */
        json_message *msgs = NULL;
        int msg_count = 0;
        rc = conversation_reconstruct(ctx->convo_path, &msgs, &msg_count);
        if (rc != EXIT_SUCCESS) {
            log_activity("[error] Failed to reconstruct conversation");
            return rc;
        }

        /* Prepend system prompt if configured and no system message exists. */
        int has_system = 0;
        for (int i = 0; i < msg_count; i++) {
            if (strcmp(msgs[i].role, "system") == 0) {
                has_system = 1;
                break;
            }
        }
        if (!has_system && ctx->agent.system_prompt != NULL &&
            ctx->agent.system_prompt[0] != '\0') {
            /* Insert at front - shift everything by one. */
            json_message *new_msgs = calloc((size_t)msg_count + 1, sizeof(json_message));
            if (new_msgs == NULL) {
                conversation_free_messages(msgs, msg_count);
                return EXIT_INTERNAL_ERR;
            }
            new_msgs[0].role = util_strdup("system");
            new_msgs[0].content = util_strdup(ctx->agent.system_prompt);
            if (msg_count > 0) {
                memcpy(&new_msgs[1], msgs, (size_t)msg_count * sizeof(json_message));
            }
            free(msgs);
            msgs = new_msgs;
            msg_count++;
        }

        /* ---- Discover tools ---- */
        rc = mcp_discover_tools(ctx);
        if (rc != EXIT_SUCCESS) {
            log_activity("[error] Failed to discover tools");
            conversation_free_messages(msgs, msg_count);
            return rc;
        }

        /* ---- Call LLM ---- */
        char *content = NULL;
        char *model = NULL;
        tool_call *calls = NULL;
        int call_count = 0;
        usage_info usage;
        int64_t t0 = platform_now_ms();

        rc = llm_chat_complete(ctx, msgs, msg_count, ctx->tools, ctx->tool_count, &content, &model,
                               &calls, &call_count, &usage);
        conversation_free_messages(msgs, msg_count);

        int64_t elapsed_ms = platform_now_ms() - t0;

        if (rc != EXIT_SUCCESS) {
            log_activity("[error] LLM API call failed");
            emit_error_event(EXIT_LLM_ERR, "LLM API call failed");
            free(content);
            free(model);
            free(calls);
            return EXIT_LLM_ERR;
        }

        /* ---- Emit assistant output to stdout ---- */
        emit_assistant_content(content);
        emit_assistant_event(content, model, &usage);

        /* ---- Print response stats to stderr (not in quiet mode) ---- */
        if (g_om != OM_QUIET) {
            if (usage.total_tokens > 0) {
                fprintf(stderr, "[stats] %s | %d tokens (prompt=%d + completion=%d) in %.2fs\n",
                        model ? model : "?", usage.total_tokens, usage.prompt_tokens,
                        usage.completion_tokens, (double)elapsed_ms / 1000.0);
            } else {
                fprintf(stderr, "[stats] %s | tokens N/A in %.2fs\n", model ? model : "?",
                        (double)elapsed_ms / 1000.0);
            }
        }

        /* ---- Write assistant entry ---- */
        conversation_write_entry(fp, ENTRY_ASSISTANT, content ? content : "", model ? model : "",
                                 &usage);

        /* ---- No tool calls: conversation complete ---- */
        if (call_count == 0) {
            /* Drain point B: a steering message may have arrived during the
             * final LLM call. If so, keep the loop going so it is processed
             * rather than dropped. */
            if (drain_steering(fp) > 0) {
                free(content);
                free(model);
                free(calls);
                continue;
            }
            log_activity("[done] Conversation complete");
            emit_done(turn + 1);
            free(content);
            free(model);
            free(calls);
            return EXIT_SUCCESS;
        }

        /* ---- Execute tool calls sequentially ---- */
        for (int i = 0; i < call_count; i++) {
            const char *tc_name = calls[i].name ? calls[i].name : "";
            const char *tc_args = calls[i].arguments ? calls[i].arguments : "{}";
            const char *tc_id = calls[i].id ? calls[i].id : "";

            log_activity("[tool] %s", tc_name);

            /* Look up the tool definition to find the backend server and original name. */
            const tool_def *td = find_tool(ctx->tools, ctx->tool_count, tc_name);
            if (td == NULL) {
                log_activity("[error] Tool '%s' not found in tool definitions", tc_name);
                /* Write tool_call + error result and continue. */
                conversation_write_entry(fp, ENTRY_TOOL_CALL, tc_id, tc_name, tc_args, "");
                emit_tool_call_event(tc_id, tc_name, tc_args, "");
                emit_tool_result_event(tc_id, tc_name, "Tool definition not found", 1, 0, "");
                conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                         "Tool definition not found", 1, 0, "");
                continue;
            }

            /* Write tool_call entry. */
            conversation_write_entry(fp, ENTRY_TOOL_CALL, tc_id, tc_name, tc_args, td->mcp_server);
            emit_tool_call_event(tc_id, tc_name, tc_args, td->mcp_server);

            /* Execute via MCP. */
            char *result = NULL;
            bool is_error = false;
            int mrc = mcp_call_tool(ctx, td->mcp_server, td->original, tc_args, &result, &is_error);

            if (mrc == EXIT_MCP_ERR) {
                /* Timeout with fail behavior - stop the conversation. */
                emit_tool_result_event(tc_id, tc_name, result ? result : "Tool call failed", 1, 0,
                                       td->mcp_server);
                conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                         result ? result : "Tool call failed", 1, 0,
                                         td->mcp_server);
                free(result);
                free(content);
                free(model);
                free(calls);
                log_activity("[error] Tool call failed, stopping conversation");
                emit_error_event(EXIT_MCP_ERR, "Tool call failed");
                return EXIT_MCP_ERR;
            }

            /* Write tool_result entry. */
            conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name, result ? result : "",
                                     (int)is_error, 0, td->mcp_server);
            emit_tool_result_event(tc_id, tc_name, result ? result : "", (int)is_error, 0,
                                   td->mcp_server);
            free(result);
        }

        free(content);
        free(model);
        free(calls);

        /* Check for conversation file write errors (flush). */
        if (ferror(fp)) {
            log_activity("[error] Failed to write to conversation file");
            return EXIT_FILE_ERR;
        }
    }

    log_activity("[error] Conversation reached maximum turn limit (%d)", max_turns);
    emit_error_event(EXIT_INTERNAL_ERR, "Conversation reached maximum turn limit");
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  agent_run                                                          */
/* ------------------------------------------------------------------ */

int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt, const char *run_mode,
              bool steering) {
    if (ctx == NULL || convo_path == NULL || prompt == NULL) return EXIT_INTERNAL_ERR;

    g_run_mode = run_mode ? run_mode : RUN_MODE_QUIET;
    g_om = resolve_run_mode(g_run_mode);
    g_last_content = NULL;
    g_steering = steering;

    /* Quiet mode: silence all progress diagnostics so the agent prints only
     * the final answer. Debug/stream modes route their own structured output
     * to stdout, so keep the stderr chatter there too for interactive use. */
    log_activity_set_enabled(g_om != OM_QUIET);

    /* Debug mode: emit a begin event. */
    if (g_om == OM_DEBUG) {
        emit_debug("begin run_mode=%s", g_run_mode);
    }

    /* Store convo_path in ctx so conversation_loop can use it for reconstruction. */
    ctx->convo_path = util_strdup(convo_path);

    /* ---- Startup sequence ---- */
    FILE *fp = NULL;
    int rc = startup_sequence(ctx, convo_path, prompt, &fp);
    if (rc != EXIT_SUCCESS) {
        mcp_disconnect_all(ctx);
        if (fp) fclose(fp);
        return rc;
    }

    /* ---- Conversation loop ---- */
    rc = conversation_loop(ctx, fp);

    /* Quiet mode: print the final assistant content. */
    emit_quiet_final();

    /* ---- Cleanup ---- */
    free(g_last_content);
    g_last_content = NULL;

    mcp_disconnect_all(ctx);
    if (fp) fclose(fp);

    return rc;
}
