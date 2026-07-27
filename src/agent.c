#include "agent.h"
#include "conversation.h"
#include "mcp.h"
#include "llm.h"
#include "util.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
            free(content);
            free(model);
            free(calls);
            return EXIT_LLM_ERR;
        }

        /* ---- Print response stats to stderr ---- */
        if (usage.total_tokens > 0) {
            fprintf(stderr, "[stats] %s | %d tokens (prompt=%d + completion=%d) in %.2fs\n",
                    model ? model : "?", usage.total_tokens, usage.prompt_tokens,
                    usage.completion_tokens, (double)elapsed_ms / 1000.0);
        } else {
            fprintf(stderr, "[stats] %s | tokens N/A in %.2fs\n", model ? model : "?",
                    (double)elapsed_ms / 1000.0);
        }

        /* ---- Write assistant entry ---- */
        conversation_write_entry(fp, ENTRY_ASSISTANT, content ? content : "", model ? model : "",
                                 &usage);

        /* ---- No tool calls: conversation complete ---- */
        if (call_count == 0) {
            log_activity("[done] Conversation complete");
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
                conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                         "Tool definition not found", 1, 0, "");
                continue;
            }

            /* Write tool_call entry. */
            conversation_write_entry(fp, ENTRY_TOOL_CALL, tc_id, tc_name, tc_args, td->mcp_server);

            /* Execute via MCP. */
            char *result = NULL;
            bool is_error = false;
            int mrc = mcp_call_tool(ctx, td->mcp_server, td->original, tc_args, &result, &is_error);

            if (mrc == EXIT_MCP_ERR) {
                /* Timeout with fail behavior - stop the conversation. */
                conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                         result ? result : "Tool call failed", 1, 0,
                                         td->mcp_server);
                free(result);
                free(content);
                free(model);
                free(calls);
                log_activity("[error] Tool call failed, stopping conversation");
                return EXIT_MCP_ERR;
            }

            /* Write tool_result entry. */
            conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name, result ? result : "",
                                     (int)is_error, 0, td->mcp_server);
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
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  agent_run                                                          */
/* ------------------------------------------------------------------ */

int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt) {
    if (ctx == NULL || convo_path == NULL || prompt == NULL) return EXIT_INTERNAL_ERR;

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

    /* ---- Cleanup ---- */
    mcp_disconnect_all(ctx);
    if (fp) fclose(fp);

    return rc;
}
