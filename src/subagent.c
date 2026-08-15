#include "subagent.h"
#include "conversation.h"
#include "llm.h"
#include "mcp.h"
#include "platform.h"
#include "util.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Input schema                                                       */
/* ------------------------------------------------------------------ */

char *subagent_build_input_schema(const subagent_tool_def *tool) {
    if (tool == NULL) return NULL;

    cJSON *schema = cJSON_CreateObject();
    if (schema == NULL) return NULL;

    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(schema, "properties");
    cJSON *req = cJSON_AddArrayToObject(schema, "required");
    if (props == NULL || req == NULL) {
        cJSON_Delete(schema);
        return NULL;
    }

    for (int i = 0; i < tool->attribute_count; i++) {
        const subagent_attr *a = &tool->attributes[i];

        cJSON *pa = cJSON_AddObjectToObject(props, a->name ? a->name : "");
        if (pa == NULL) {
            cJSON_Delete(schema);
            return NULL;
        }
        if (cJSON_AddStringToObject(pa, "type", a->type ? a->type : "string") == NULL ||
            (a->description != NULL &&
             cJSON_AddStringToObject(pa, "description", a->description) == NULL)) {
            cJSON_Delete(schema);
            return NULL;
        }

        if (a->required) {
            cJSON *item = cJSON_CreateString(a->name ? a->name : "");
            if (item == NULL || cJSON_AddItemToArray(req, item) == 0) {
                cJSON_Delete(item);
                cJSON_Delete(schema);
                return NULL;
            }
        }
    }

    char *out = cJSON_PrintUnformatted(schema);
    cJSON_Delete(schema);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Tool registration                                                  */
/* ------------------------------------------------------------------ */

int subagent_register_tools_list(runtime_ctx *ctx, const subagent_spec *specs, int count) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;
    if (count == 0) return EXIT_SUCCESS;

    /* Name collisions: a subagent tool must not shadow an MCP-discovered
     * tool. Sibling subagent tool names are already unique (validated at
     * config load). */
    for (int i = 0; i < count; i++) {
        const char *name = specs[i].tool.name;
        for (int j = 0; j < ctx->tool_count; j++) {
            if (ctx->tools[j].name != NULL && strcmp(ctx->tools[j].name, name) == 0) {
                log_activity("[error] subagent tool name '%s' collides with an existing tool",
                             name);
                return EXIT_CONFIG_ERR;
            }
        }
    }

    int old_count = ctx->tool_count;
    int new_count = old_count + count;

    tool_def *merged = realloc(ctx->tools, (size_t)new_count * sizeof(tool_def));
    if (merged == NULL) {
        log_activity("[error] OOM merging subagent tools");
        return EXIT_INTERNAL_ERR;
    }
    ctx->tools = merged;

    for (int i = 0; i < count; i++) {
        const subagent_spec *s = &specs[i];
        tool_def *td = &ctx->tools[old_count + i];
        memset(td, 0, sizeof(*td));

        td->name = util_strdup(s->tool.name);
        td->original = util_strdup(s->tool.name);
        td->description = util_strdup(s->tool.description ? s->tool.description : "");
        td->input_schema = subagent_build_input_schema(&s->tool);
        td->mcp_server = util_strdup(SUBAGENT_TOOL_SERVER);

        if (td->name == NULL || td->original == NULL || td->description == NULL ||
            td->input_schema == NULL || td->mcp_server == NULL) {
            log_activity("[error] OOM building tool definition for subagent '%s'", s->tool.name);
            free(td->name);
            free(td->original);
            free(td->description);
            free(td->input_schema);
            free(td->mcp_server);
            memset(td, 0, sizeof(*td));
            return EXIT_INTERNAL_ERR;
        }
    }
    ctx->tool_count = new_count;

    /* Keep the deterministic ordering of the whole tool block. */
    if (ctx->tool_count > 1) {
        qsort(ctx->tools, (size_t)ctx->tool_count, sizeof(tool_def), mcp_tool_def_cmp);
    }

    return EXIT_SUCCESS;
}

int subagent_register_tools(runtime_ctx *ctx) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;
    return subagent_register_tools_list(ctx, ctx->subagents, ctx->subagent_count);
}

subagent_spec *subagent_find(subagent_spec *arr, int count, const char *tool_name) {
    if (arr == NULL || tool_name == NULL) return NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(arr[i].tool.name, tool_name) == 0) return &arr[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Prompt interpolation                                               */
/* ------------------------------------------------------------------ */

/* Growable output buffer for interpolation. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} interp_out;

static int interp_append(interp_out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 128;
        while (o->len + n + 1 > cap) cap *= 2;
        char *tmp = realloc(o->buf, cap);
        if (tmp == NULL) return -1;
        o->buf = tmp;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    return 0;
}

/* Text value of one attribute from the call arguments:
 *  - string attributes interpolate the raw text;
 *  - number/boolean/array/object attributes interpolate their JSON literal;
 *  - a missing (optional) attribute interpolates the empty string.
 * Returns a malloc'd string, or NULL on allocation failure. */
static char *attr_value_text(const cJSON *args, const char *name) {
    cJSON *v = cJSON_GetObjectItem(args, name);
    if (v == NULL) return util_strdup("");
    if (cJSON_IsString(v)) return util_strdup(v->valuestring ? v->valuestring : "");
    char *printed = cJSON_PrintUnformatted(v);
    return printed ? printed : util_strdup("");
}

/* Replace {attribute} placeholders in tmpl with values from the tool call
 * arguments. Unknown placeholders are left untouched. Returns a malloc'd
 * string, or NULL on allocation failure. */
char *subagent_interp(const char *tmpl, const subagent_spec *spec, const char *args_json) {
    if (tmpl == NULL) return NULL;

    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    if (args == NULL) return NULL;

    interp_out o = {NULL, 0, 0};

    size_t i = 0;
    while (tmpl[i] != '\0') {
        if (tmpl[i] == '{') {
            const char *close = strchr(&tmpl[i + 1], '}');
            if (close != NULL) {
                size_t name_len = (size_t)(close - &tmpl[i + 1]);
                char name[256];
                if (name_len < sizeof(name)) {
                    memcpy(name, &tmpl[i + 1], name_len);
                    name[name_len] = '\0';
                    const subagent_attr *a = NULL;
                    for (int k = 0; k < spec->tool.attribute_count; k++) {
                        if (strcmp(spec->tool.attributes[k].name, name) == 0) {
                            a = &spec->tool.attributes[k];
                            break;
                        }
                    }
                    if (a != NULL) {
                        char *val = attr_value_text(args, name);
                        if (val == NULL) {
                            free(o.buf);
                            cJSON_Delete(args);
                            return NULL;
                        }
                        int rc = interp_append(&o, val, strlen(val));
                        free(val);
                        if (rc != 0) {
                            free(o.buf);
                            cJSON_Delete(args);
                            return NULL;
                        }
                        i += name_len + 2;
                        continue;
                    }
                }
            }
        }
        if (interp_append(&o, &tmpl[i], 1) != 0) {
            free(o.buf);
            cJSON_Delete(args);
            return NULL;
        }
        i++;
    }

    if (o.buf == NULL) {
        cJSON_Delete(args);
        return util_strdup("");
    }
    o.buf[o.len] = '\0';
    cJSON_Delete(args);
    return o.buf;
}

/* ------------------------------------------------------------------ */
/*  Nested conversation loop                                           */
/* ------------------------------------------------------------------ */

#define SUBAGENT_MAX_TURNS 50

/* Find a tool_def by namespaced name. */
static const tool_def *find_tool(const tool_def *tools, int tool_count, const char *name) {
    for (int i = 0; i < tool_count; i++) {
        if (tools[i].name != NULL && strcmp(tools[i].name, name) == 0) return &tools[i];
    }
    return NULL;
}

/* Run the subagent conversation loop over the child context. On success
 * *out_final receives the last assistant content (malloc'd). Returns
 * EXIT_SUCCESS, an exit code on fatal failure (is_error set with a
 * message for soft LLM failures). */
static int subagent_run_loop(runtime_ctx *child, FILE *fp, subagent_spec *spec, int depth,
                             int max_retries, char **out_final, bool *out_is_error) {
    int rc = EXIT_SUCCESS;

    for (int turn = 0; turn < SUBAGENT_MAX_TURNS; turn++) {
        /* Reconstruct message history from the temp conversation file. */
        json_message *msgs = NULL;
        int msg_count = 0;
        rc = conversation_reconstruct(child->convo_path, &msgs, &msg_count);
        if (rc != EXIT_SUCCESS) {
            log_activity("[error] subagent '%s': failed to reconstruct conversation",
                         spec->tool.name);
            return rc;
        }

        /* Prepend the interpolated system prompt when configured. */
        if (child->agent.system_prompt != NULL && child->agent.system_prompt[0] != '\0') {
            json_message *new_msgs = calloc((size_t)msg_count + 1, sizeof(json_message));
            if (new_msgs == NULL) {
                conversation_free_messages(msgs, msg_count);
                return EXIT_INTERNAL_ERR;
            }
            new_msgs[0].role = util_strdup("system");
            new_msgs[0].content = util_strdup(child->agent.system_prompt);
            if (msg_count > 0) {
                memcpy(&new_msgs[1], msgs, (size_t)msg_count * sizeof(json_message));
            }
            free(msgs);
            msgs = new_msgs;
            msg_count++;
        }

        /* Call the LLM with the same retry policy as the main loop. */
        char *content = NULL;
        char *reasoning = NULL;
        char *model = NULL;
        tool_call *calls = NULL;
        int call_count = 0;
        usage_info usage;
        memset(&usage, 0, sizeof(usage));

        rc = llm_chat_complete(child, msgs, msg_count, child->tools, child->tool_count, &content,
                               &reasoning, &model, &calls, &call_count, &usage);

        int retry = 0;
        while (rc != EXIT_SUCCESS && retry < max_retries) {
            retry++;
            int64_t delay_s = util_fibonacci(retry);
            log_activity("[subagent] '%s' LLM call failed, retry %d/%d after %llds",
                         spec->tool.name, retry, max_retries, (long long)delay_s);
            platform_sleep_ms(delay_s * 1000);

            free(content);
            content = NULL;
            free(reasoning);
            reasoning = NULL;
            free(model);
            model = NULL;
            free(calls);
            calls = NULL;
            call_count = 0;
            memset(&usage, 0, sizeof(usage));

            rc = llm_chat_complete(child, msgs, msg_count, child->tools, child->tool_count,
                                   &content, &reasoning, &model, &calls, &call_count, &usage);
        }

        conversation_free_messages(msgs, msg_count);

        if (rc != EXIT_SUCCESS) {
            free(reasoning);
            free(model);
            free(calls);
            *out_is_error = true;
            *out_final = util_strdup("Subagent LLM API call failed");
            return EXIT_SUCCESS;
        }

        conversation_write_entry(fp, ENTRY_ASSISTANT, content ? content : "",
                                 reasoning ? reasoning : "", model ? model : "", &usage);

        if (call_count == 0) {
            free(reasoning);
            free(model);
            free(calls);
            *out_final = content ? content : util_strdup("");
            return EXIT_SUCCESS;
        }

        /* Execute tool calls sequentially. */
        for (int i = 0; i < call_count; i++) {
            const char *tc_name = calls[i].name ? calls[i].name : "";
            const char *tc_args = calls[i].arguments ? calls[i].arguments : "{}";
            const char *tc_id = calls[i].id ? calls[i].id : "";

            log_activity("[subagent] '%s' tool %s", spec->tool.name, tc_name);

            const tool_def *td = find_tool(child->tools, child->tool_count, tc_name);
            if (td == NULL) {
                conversation_write_entry(fp, ENTRY_TOOL_CALL, tc_id, tc_name, tc_args, "");
                conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                         "Tool definition not found", 1, 0, "");
                continue;
            }

            conversation_write_entry(fp, ENTRY_TOOL_CALL, tc_id, tc_name, tc_args, td->mcp_server);

            char *result = NULL;
            bool is_error = false;

            if (strcmp(td->mcp_server, SUBAGENT_TOOL_SERVER) == 0) {
                /* Nested subagent (agent-as-tool inside this subagent). */
                subagent_spec *nested =
                    subagent_find(spec->subagents, spec->subagent_count, tc_name);
                if (nested == NULL) {
                    is_error = true;
                    result = util_strdup("Subagent definition not found");
                } else {
                    int src = subagent_call(child, nested, tc_args, depth + 1, max_retries, &result,
                                            &is_error);
                    if (src != EXIT_SUCCESS) {
                        free(result);
                        free(content);
                        free(reasoning);
                        free(model);
                        free(calls);
                        return src;
                    }
                }
            } else {
                int mrc =
                    mcp_call_tool(child, td->mcp_server, td->original, tc_args, &result, &is_error);
                if (mrc != EXIT_SUCCESS) {
                    /* Fatal MCP failure (e.g. timeout with fail behavior):
                     * stop the subagent and propagate. */
                    conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name,
                                             result ? result : "Tool call failed", 1, 0,
                                             td->mcp_server);
                    free(result);
                    free(content);
                    free(reasoning);
                    free(model);
                    free(calls);
                    *out_is_error = true;
                    *out_final = util_strdup("Subagent tool call failed");
                    return EXIT_MCP_ERR;
                }
            }

            conversation_write_entry(fp, ENTRY_TOOL_RESULT, tc_id, tc_name, result ? result : "",
                                     (int)is_error, 0, td->mcp_server);
            free(result);
        }

        free(content);
        free(reasoning);
        free(model);
        free(calls);
    }

    log_activity("[error] subagent '%s' reached maximum turn limit (%d)", spec->tool.name,
                 SUBAGENT_MAX_TURNS);
    *out_is_error = true;
    *out_final = util_strdup("Subagent reached maximum turn limit");
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  subagent_call                                                      */
/* ------------------------------------------------------------------ */

int subagent_call(const runtime_ctx *parent, subagent_spec *spec, const char *args_json, int depth,
                  int max_retries, char **out_result, bool *out_is_error) {
    if (parent == NULL || spec == NULL || out_result == NULL || out_is_error == NULL)
        return EXIT_INTERNAL_ERR;

    *out_result = NULL;
    *out_is_error = false;

    if (depth < 1 || depth > SUBAGENT_MAX_DEPTH) {
        *out_is_error = true;
        *out_result = util_strdup("Subagent nesting too deep");
        return EXIT_SUCCESS;
    }

    log_activity("[subagent] starting '%s' (depth %d)", spec->tool.name, depth);

    /* Parse the tool call arguments for interpolation. */
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    if (args == NULL || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        *out_is_error = true;
        *out_result = util_strdup("Subagent received invalid tool arguments");
        return EXIT_SUCCESS;
    }

    /* Interpolate the prompts. */
    char *system_prompt = subagent_interp(spec->system_prompt, spec, args_json);
    char *user_prompt = subagent_interp(spec->user_prompt, spec, args_json);
    cJSON_Delete(args);
    if ((spec->system_prompt != NULL && system_prompt == NULL) ||
        (spec->user_prompt != NULL && user_prompt == NULL)) {
        free(system_prompt);
        free(user_prompt);
        return EXIT_INTERNAL_ERR;
    }

    /* Child context: shares the LLM config (same LLM as the main agent) and
     * the config hash; owns its system prompt, tools and temp conversation.
     * It must never be passed to config_free. */
    runtime_ctx child;
    memset(&child, 0, sizeof(child));
    child.llm = parent->llm; /* shallow copy: strings are read-only shared */
    memcpy(child.config_hash, parent->config_hash, sizeof(child.config_hash));
    child.agent.system_prompt = system_prompt;
    util_uuid_v4(child.run_id);

    /* Temp conversation file for this subagent run. */
    const char *tmpdir = platform_temp_dir();
    size_t plen = strlen(tmpdir) + sizeof("/llmkit-sub-.jsonl") + sizeof(child.run_id);
    char *convo_path = malloc(plen);
    if (convo_path == NULL) {
        free(system_prompt);
        free(user_prompt);
        return EXIT_INTERNAL_ERR;
    }
    snprintf(convo_path, plen, "%s/llmkit-sub-%s.jsonl", tmpdir, child.run_id);
    child.convo_path = convo_path;

    int rc = EXIT_SUCCESS;
    FILE *fp = NULL;
    char *final = NULL;

    /* Lazily connect this subagent's private MCP servers (first use).
     * Reference entries and names already connected are no-ops. */
    for (int i = 0; i < spec->mcp_count; i++) {
        int crc = mcp_connect_one(&spec->mcps[i]);
        if (crc != EXIT_SUCCESS) {
            log_activity("[error] subagent '%s': failed to connect MCP server '%s' (lazy)",
                         spec->tool.name, spec->mcps[i].name);
            *out_is_error = true;
            *out_result = util_strdup("Subagent failed to connect its MCP server");
            rc = EXIT_SUCCESS;
            goto done;
        }
    }

    /* Open the conversation and write meta + interpolated user prompt. */
    rc = conversation_open(convo_path, &fp);
    if (rc != EXIT_SUCCESS) {
        *out_is_error = true;
        *out_result = util_strdup("Subagent failed to open its conversation file");
        rc = EXIT_SUCCESS;
        goto done;
    }
    {
        char prefixed[80];
        snprintf(prefixed, sizeof(prefixed), "sha256:%s", child.config_hash);
        conversation_write_meta(fp, prefixed, child.run_id);
    }
    conversation_write_entry(fp, ENTRY_USER, user_prompt ? user_prompt : "", "subagent");

    /* Discover tools only from this subagent's MCP list... */
    const char **allowed = NULL;
    if (spec->mcp_count > 0) {
        allowed = calloc((size_t)spec->mcp_count, sizeof(char *));
        if (allowed == NULL) {
            rc = EXIT_INTERNAL_ERR;
            goto done;
        }
        for (int i = 0; i < spec->mcp_count; i++) allowed[i] = spec->mcps[i].name;
    }
    rc = mcp_discover_tools_for(&child, allowed, spec->mcp_count);
    free(allowed);
    if (rc != EXIT_SUCCESS) {
        *out_is_error = true;
        *out_result = util_strdup("Subagent failed to discover tools");
        rc = EXIT_SUCCESS;
        goto done;
    }

    /* ... and register its own subagents as tools. */
    rc = subagent_register_tools_list(&child, spec->subagents, spec->subagent_count);
    if (rc != EXIT_SUCCESS) {
        *out_is_error = true;
        *out_result = util_strdup("Subagent failed to register its nested subagents");
        rc = EXIT_SUCCESS;
        goto done;
    }

    /* Run the nested loop. */
    rc = subagent_run_loop(&child, fp, spec, depth, max_retries, &final, out_is_error);
    if (rc == EXIT_SUCCESS) {
        *out_result = final;
        final = NULL;
        log_activity("[subagent] '%s' finished (error=%d)", spec->tool.name, (int)*out_is_error);
    }

done:
    free(final);
    if (fp != NULL) fclose(fp);
    platform_delete_file(convo_path);
    mcp_free_tool_defs(child.tools, child.tool_count); /* frees the array too */
    free(convo_path);
    free(system_prompt);
    free(user_prompt);
    return rc;
}
