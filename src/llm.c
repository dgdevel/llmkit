#include "llm.h"
#include "platform.h"
#include "util.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>
#include <curl/curl.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Write callback for libcurl - appends to a growbuf. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} growbuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    growbuf *gb = userdata;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    size_t need = gb->len + n + 1;
    if (need > gb->cap) {
        size_t newcap = gb->cap ? gb->cap : 1024;
        while (newcap < need) newcap *= 2;
        char *tmp = realloc(gb->data, newcap);
        if (tmp == NULL) return 0;
        gb->data = tmp;
        gb->cap = newcap;
    }
    memcpy(gb->data + gb->len, ptr, n);
    gb->len += n;
    gb->data[gb->len] = '\0';
    return n;
}

static void growbuf_free(growbuf *gb) {
    free(gb->data);
    gb->data = NULL;
    gb->len = 0;
    gb->cap = 0;
}

/* Convert config "Key=Value" entries to curl "Key: Value" list. */
static struct curl_slist *headers_to_slist(char **cfg_hdrs, struct curl_slist *base) {
    struct curl_slist *list = base;
    if (cfg_hdrs == NULL) return list;
    for (int i = 0; cfg_hdrs[i] != NULL; i++) {
        const char *eq = strchr(cfg_hdrs[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg_hdrs[i]);
        const char *val = eq + 1;
        size_t vlen = strlen(val);
        char *h = malloc(klen + 2 + vlen + 1);
        if (h == NULL) {
            log_activity("[error] OOM");
            exit(EXIT_INTERNAL_ERR);
        }
        memcpy(h, cfg_hdrs[i], klen);
        h[klen] = ':';
        h[klen + 1] = ' ';
        memcpy(h + klen + 2, val, vlen + 1);
        list = curl_slist_append(list, h);
        free(h);
    }
    return list;
}

/* ------------------------------------------------------------------ */
/*  Request body builder                                               */
/* ------------------------------------------------------------------ */

char *llm_build_request_body(const json_message *msgs, int msg_count, const tool_def *tools,
                             int tool_count, const llm_cfg *cfg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    /* model */
    const char *model = (cfg->model && cfg->model[0]) ? cfg->model : "gpt-4o-mini";
    cJSON_AddStringToObject(root, "model", model);

    /* messages */
    cJSON *msgs_arr = cJSON_AddArrayToObject(root, "messages");
    if (msgs_arr == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    for (int i = 0; i < msg_count; i++) {
        const json_message *m = &msgs[i];
        cJSON *mj = cJSON_CreateObject();
        if (mj == NULL) {
            cJSON_Delete(root);
            return NULL;
        }

        const char *role = m->role ? m->role : "";
        cJSON_AddStringToObject(mj, "role", role);

        if (m->content != NULL && m->content[0] != '\0') {
            cJSON_AddStringToObject(mj, "content", m->content);
        } else if (strcmp(role, "tool") == 0) {
            cJSON_AddStringToObject(mj, "content", m->content ? m->content : "");
        } else {
            cJSON_AddStringToObject(mj, "content", "");
        }

        /* reasoning_content for assistant messages, only when retain_reasoning is set */
        if (strcmp(role, "assistant") == 0 && cfg->retain_reasoning && m->reasoning != NULL &&
            m->reasoning[0] != '\0') {
            cJSON_AddStringToObject(mj, "reasoning_content", m->reasoning);
        }

        /* tool_call_id for tool messages */
        if (strcmp(role, "tool") == 0 && m->tool_call_id != NULL) {
            cJSON_AddStringToObject(mj, "tool_call_id", m->tool_call_id);
        }

        /* tool_calls for assistant messages */
        if (strcmp(role, "assistant") == 0 && m->tool_call_count > 0 && m->tool_calls != NULL) {
            cJSON *tc_arr = cJSON_AddArrayToObject(mj, "tool_calls");
            if (tc_arr != NULL) {
                for (int j = 0; j < m->tool_call_count; j++) {
                    cJSON *tcj = cJSON_CreateObject();
                    if (tcj == NULL) {
                        cJSON_Delete(root);
                        return NULL;
                    }
                    cJSON_AddStringToObject(tcj, "id",
                                            m->tool_calls[j].id ? m->tool_calls[j].id : "");
                    cJSON_AddStringToObject(tcj, "type", "function");

                    cJSON *func = cJSON_AddObjectToObject(tcj, "function");
                    if (func != NULL) {
                        cJSON_AddStringToObject(func, "name",
                                                m->tool_calls[j].name ? m->tool_calls[j].name : "");
                        cJSON_AddStringToObject(
                            func, "arguments",
                            m->tool_calls[j].arguments ? m->tool_calls[j].arguments : "{}");
                    }
                    cJSON_AddItemToArray(tc_arr, tcj);
                }
            }
        }

        cJSON_AddItemToArray(msgs_arr, mj);
    }

    /* tools (if any) */
    if (tools != NULL && tool_count > 0) {
        cJSON *tools_arr = cJSON_AddArrayToObject(root, "tools");
        if (tools_arr == NULL) {
            cJSON_Delete(root);
            return NULL;
        }

        for (int i = 0; i < tool_count; i++) {
            cJSON *td = cJSON_CreateObject();
            if (td == NULL) {
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddStringToObject(td, "type", "function");

            cJSON *func = cJSON_AddObjectToObject(td, "function");
            if (func != NULL) {
                cJSON_AddStringToObject(func, "name", tools[i].name ? tools[i].name : "");
                cJSON_AddStringToObject(func, "description",
                                        tools[i].description ? tools[i].description : "");

                if (tools[i].input_schema != NULL && tools[i].input_schema[0] != '\0') {
                    cJSON *schema = cJSON_Parse(tools[i].input_schema);
                    if (schema != NULL) {
                        cJSON_AddItemToObject(func, "parameters", schema);
                    } else {
                        cJSON *fallback = cJSON_CreateObject();
                        if (fallback) cJSON_AddItemToObject(func, "parameters", fallback);
                    }
                } else {
                    cJSON *empty = cJSON_CreateObject();
                    if (empty) cJSON_AddItemToObject(func, "parameters", empty);
                }
            }
            cJSON_AddItemToArray(tools_arr, td);
        }
    }

    if (tool_count > 0) {
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Serialize just the "messages" array of a chat-completion request body,
 * using the exact same (deterministic) serialization as the request itself.
 * Used by prefix-cache compaction to hash the covered prefix and to persist
 * the projection. Returns a malloc'd JSON string, or NULL on failure. */
char *llm_serialize_messages(const json_message *msgs, int msg_count, const llm_cfg *cfg) {
    char *body = llm_build_request_body(msgs, msg_count, NULL, 0, cfg);
    if (body == NULL) return NULL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) return NULL;
    cJSON *arr = cJSON_GetObjectItem(root, "messages");
    char *out = (arr != NULL) ? cJSON_PrintUnformatted(arr) : NULL;
    cJSON_Delete(root);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Anthropic Messages API request body builder                        */
/* ------------------------------------------------------------------ */

/* Anthropic requires max_tokens on every request; used when llm.max_tokens
 * is unset (<= 0). */
#define ANTHROPIC_DEFAULT_MAX_TOKENS 4096

static cJSON *anthropic_text_block(const char *text) {
    cJSON *block = cJSON_CreateObject();
    if (block == NULL) return NULL;
    if (!cJSON_AddStringToObject(block, "type", "text") ||
        !cJSON_AddStringToObject(block, "text", text ? text : "")) {
        cJSON_Delete(block);
        return NULL;
    }
    return block;
}

/* Join the content of every "system" message with blank lines into one
 * malloc'd string for the Anthropic top-level "system" parameter. Returns
 * NULL when there is no system content (the parameter must be omitted, not
 * empty). */
static char *anthropic_join_system(const json_message *msgs, int msg_count) {
    size_t total = 0;
    int count = 0;
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].role == NULL || strcmp(msgs[i].role, "system") != 0) continue;
        if (msgs[i].content == NULL || msgs[i].content[0] == '\0') continue;
        total += strlen(msgs[i].content);
        count++;
    }
    if (count == 0) return NULL;
    total += ((size_t)(count - 1) * 2) + 1; /* "\n\n" separators + NUL */
    char *out = malloc(total);
    if (out == NULL) {
        log_activity("[error] OOM");
        exit(EXIT_INTERNAL_ERR);
    }
    char *p = out;
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].role == NULL || strcmp(msgs[i].role, "system") != 0) continue;
        if (msgs[i].content == NULL || msgs[i].content[0] == '\0') continue;
        if (p != out) {
            *p++ = '\n';
            *p++ = '\n';
        }
        size_t len = strlen(msgs[i].content);
        memcpy(p, msgs[i].content, len);
        p += len;
    }
    *p = '\0';
    return out;
}

/* Return the last wire message in msgs_arr if it is a user message, else
 * NULL. The Messages API requires strictly alternating roles, so new user-
 * side content (plain user turns as well as flushed tool_result blocks) is
 * folded into the previous user message rather than emitted side by side. */
static cJSON *anthropic_last_user_message(cJSON *msgs_arr) {
    int n = cJSON_GetArraySize(msgs_arr);
    if (n == 0) return NULL;
    cJSON *last = cJSON_GetArrayItem(msgs_arr, n - 1);
    cJSON *role = cJSON_GetObjectItem(last, "role");
    if (role != NULL && cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
        return last;
    }
    return NULL;
}

/* Append a content block to the last user wire message (creating one when
 * the previous wire message is not a user message). Takes ownership of
 * block. Returns 0 on success, -1 on allocation failure. */
static int anthropic_append_user_block(cJSON *msgs_arr, cJSON *block) {
    cJSON *target = anthropic_last_user_message(msgs_arr);
    if (target != NULL) {
        cJSON *content = cJSON_GetObjectItem(target, "content");
        if (content == NULL) {
            cJSON_Delete(block);
            return -1;
        }
        cJSON_AddItemToArray(content, block);
        return 0;
    }
    cJSON *uj = cJSON_CreateObject();
    cJSON *blocks = (uj != NULL) ? cJSON_CreateArray() : NULL;
    if (uj == NULL || blocks == NULL) {
        cJSON_Delete(block);
        cJSON_Delete(uj);
        return -1;
    }
    cJSON_AddStringToObject(uj, "role", "user");
    cJSON_AddItemToArray(blocks, block);
    cJSON_AddItemToObject(uj, "content", blocks);
    cJSON_AddItemToArray(msgs_arr, uj);
    return 0;
}

char *llm_build_request_body_anthropic(const json_message *msgs, int msg_count,
                                       const tool_def *tools, int tool_count, const llm_cfg *cfg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    /* model */
    const char *model = (cfg->model && cfg->model[0]) ? cfg->model : "";
    cJSON_AddStringToObject(root, "model", model);

    /* max_tokens (required by the Messages API) */
    long long max_tokens = (cfg->max_tokens > 0) ? (long long)cfg->max_tokens
                                                 : (long long)ANTHROPIC_DEFAULT_MAX_TOKENS;
    cJSON_AddNumberToObject(root, "max_tokens", (double)max_tokens);

    /* System prompts are a top-level parameter, not a message. */
    char *system = anthropic_join_system(msgs, msg_count);
    if (system != NULL) {
        cJSON_AddStringToObject(root, "system", system);
        free(system);
    }

    cJSON *msgs_arr = cJSON_AddArrayToObject(root, "messages");
    if (msgs_arr == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *pending_results = NULL; /* tool_result blocks awaiting flush */

    for (int i = 0; i < msg_count; i++) {
        const json_message *m = &msgs[i];
        const char *role = m->role ? m->role : "";

        if (strcmp(role, "system") == 0) continue; /* hoisted above */

        if (strcmp(role, "tool") == 0) {
            /* Accumulate consecutive tool results; the Messages API carries
             * them inside a single user message. */
            if (pending_results == NULL) {
                pending_results = cJSON_CreateArray();
                if (pending_results == NULL) {
                    cJSON_Delete(root);
                    return NULL;
                }
            }
            cJSON *tr = cJSON_CreateObject();
            if (tr == NULL || !cJSON_AddStringToObject(tr, "type", "tool_result") ||
                !cJSON_AddStringToObject(tr, "tool_use_id",
                                         m->tool_call_id ? m->tool_call_id : "") ||
                !cJSON_AddStringToObject(tr, "content", m->content ? m->content : "")) {
                cJSON_Delete(tr);
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddItemToArray(pending_results, tr);
            /* Flush when the next message is not another tool result. The
             * blocks fold into the previous user wire message if there is
             * one (roles must alternate in the Messages API). */
            const json_message *next = (i + 1 < msg_count) ? &msgs[i + 1] : NULL;
            if (next == NULL || next->role == NULL || strcmp(next->role, "tool") != 0) {
                cJSON *target = anthropic_last_user_message(msgs_arr);
                if (target != NULL) {
                    cJSON *content = cJSON_GetObjectItem(target, "content");
                    if (content == NULL) {
                        cJSON_Delete(root);
                        return NULL;
                    }
                    while (cJSON_GetArraySize(pending_results) > 0) {
                        cJSON *blk = cJSON_DetachItemFromArray(pending_results, 0);
                        cJSON_AddItemToArray(content, blk);
                    }
                    cJSON_Delete(pending_results);
                } else {
                    cJSON *uj = cJSON_CreateObject();
                    if (uj == NULL) {
                        cJSON_Delete(root);
                        return NULL;
                    }
                    cJSON_AddStringToObject(uj, "role", "user");
                    cJSON_AddItemToObject(uj, "content", pending_results);
                    cJSON_AddItemToArray(msgs_arr, uj);
                }
                pending_results = NULL;
            }
            continue;
        }

        if (strcmp(role, "assistant") == 0) {
            cJSON *aj = cJSON_CreateObject();
            cJSON *blocks = (aj != NULL) ? cJSON_CreateArray() : NULL;
            if (aj == NULL || blocks == NULL) {
                cJSON_Delete(aj);
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddStringToObject(aj, "role", "assistant");

            if (m->content != NULL && m->content[0] != '\0') {
                cJSON *tb = anthropic_text_block(m->content);
                if (tb == NULL) {
                    cJSON_Delete(aj);
                    cJSON_Delete(root);
                    return NULL;
                }
                cJSON_AddItemToArray(blocks, tb);
            }

            /* tool_calls become tool_use blocks; the OpenAI wire format
             * carries arguments as a JSON string, Anthropic as an object. */
            for (int j = 0; j < m->tool_call_count; j++) {
                cJSON *block = cJSON_CreateObject();
                if (block == NULL) {
                    cJSON_Delete(aj);
                    cJSON_Delete(root);
                    return NULL;
                }
                if (!cJSON_AddStringToObject(block, "type", "tool_use") ||
                    !cJSON_AddStringToObject(block, "id",
                                             m->tool_calls[j].id ? m->tool_calls[j].id : "") ||
                    !cJSON_AddStringToObject(block, "name",
                                             m->tool_calls[j].name ? m->tool_calls[j].name : "")) {
                    cJSON_Delete(block);
                    cJSON_Delete(aj);
                    cJSON_Delete(root);
                    return NULL;
                }
                cJSON *input =
                    (m->tool_calls[j].arguments != NULL && m->tool_calls[j].arguments[0] != '\0')
                        ? cJSON_Parse(m->tool_calls[j].arguments)
                        : NULL;
                if (input == NULL) input = cJSON_CreateObject();
                if (input == NULL) {
                    cJSON_Delete(block);
                    cJSON_Delete(aj);
                    cJSON_Delete(root);
                    return NULL;
                }
                cJSON_AddItemToObject(block, "input", input);
                cJSON_AddItemToArray(blocks, block);
            }

            /* Defensive: the API rejects assistant messages without content. */
            if (cJSON_GetArraySize(blocks) == 0) {
                cJSON *tb = anthropic_text_block("");
                if (tb == NULL) {
                    cJSON_Delete(aj);
                    cJSON_Delete(root);
                    return NULL;
                }
                cJSON_AddItemToArray(blocks, tb);
            }

            cJSON_AddItemToObject(aj, "content", blocks);
            cJSON_AddItemToArray(msgs_arr, aj);
            continue;
        }

        /* user (and any unrecognized role) -> plain user text block, folded
         * into the previous user wire message when roles would otherwise
         * repeat. */
        cJSON *tb = anthropic_text_block(m->content ? m->content : "");
        if (tb == NULL || anthropic_append_user_block(msgs_arr, tb) != 0) {
            cJSON_Delete(tb);
            cJSON_Delete(root);
            return NULL;
        }
    }

    /* tools (if any): flat {name, description, input_schema} shape. */
    if (tools != NULL && tool_count > 0) {
        cJSON *tools_arr = cJSON_AddArrayToObject(root, "tools");
        if (tools_arr == NULL) {
            cJSON_Delete(root);
            return NULL;
        }

        for (int i = 0; i < tool_count; i++) {
            cJSON *td = cJSON_CreateObject();
            if (td == NULL) {
                cJSON_Delete(root);
                return NULL;
            }
            if (!cJSON_AddStringToObject(td, "name", tools[i].name ? tools[i].name : "") ||
                !cJSON_AddStringToObject(td, "description",
                                         tools[i].description ? tools[i].description : "")) {
                cJSON_Delete(td);
                cJSON_Delete(root);
                return NULL;
            }
            cJSON *schema = NULL;
            if (tools[i].input_schema != NULL && tools[i].input_schema[0] != '\0') {
                schema = cJSON_Parse(tools[i].input_schema);
            }
            if (schema == NULL) schema = cJSON_CreateObject();
            if (schema == NULL) {
                cJSON_Delete(td);
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddItemToObject(td, "input_schema", schema);
            cJSON_AddItemToArray(tools_arr, td);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Response parser                                                    */
/* ------------------------------------------------------------------ */

int llm_parse_response(const char *body, char **out_content, char **out_reasoning, char **out_model,
                       tool_call **out_calls, int *out_call_count, usage_info *usage) {
    if (body == NULL) return EXIT_LLM_ERR;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return EXIT_LLM_ERR;

    /* Check for top-level API error. */
    cJSON *api_error = cJSON_GetObjectItem(root, "error");
    if (api_error != NULL) {
        cJSON *msg_j = cJSON_GetObjectItem(api_error, "message");
        const char *err =
            (msg_j && cJSON_IsString(msg_j)) ? msg_j->valuestring : "Unknown API error";
        log_activity("[error] LLM API error: %s", err);
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }

    /* model */
    cJSON *model_j = cJSON_GetObjectItem(root, "model");
    if (model_j && cJSON_IsString(model_j) && model_j->valuestring) {
        *out_model = util_strdup(model_j->valuestring);
    }

    /* choices[0].message */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if (choice0 == NULL) {
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }
    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (message == NULL) {
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }

    /* content (nullable) */
    cJSON *content_j = cJSON_GetObjectItem(message, "content");
    if (content_j != NULL && cJSON_IsString(content_j) && content_j->valuestring != NULL) {
        *out_content = util_strdup(content_j->valuestring);
    } else {
        *out_content = util_strdup("");
    }

    /* reasoning_content (nullable; emitted by reasoning-capable models) */
    cJSON *reasoning_j = cJSON_GetObjectItem(message, "reasoning_content");
    if (reasoning_j != NULL && cJSON_IsString(reasoning_j) && reasoning_j->valuestring != NULL) {
        *out_reasoning = util_strdup(reasoning_j->valuestring);
    } else {
        *out_reasoning = util_strdup("");
    }

    /* tool_calls */
    cJSON *tc_arr_j = cJSON_GetObjectItem(message, "tool_calls");
    if (tc_arr_j != NULL && cJSON_IsArray(tc_arr_j)) {
        int tc_count = cJSON_GetArraySize(tc_arr_j);
        *out_calls = calloc((size_t)tc_count, sizeof(tool_call));
        if (*out_calls == NULL) {
            cJSON_Delete(root);
            return EXIT_INTERNAL_ERR;
        }
        *out_call_count = 0;

        for (int i = 0; i < tc_count; i++) {
            cJSON *tc_j = cJSON_GetArrayItem(tc_arr_j, i);
            if (tc_j == NULL) continue;

            cJSON *id_j = cJSON_GetObjectItem(tc_j, "id");
            tool_call *tcp = &(*out_calls)[*out_call_count];

            if (id_j && cJSON_IsString(id_j)) tcp->id = util_strdup(id_j->valuestring);

            cJSON *func_j = cJSON_GetObjectItem(tc_j, "function");
            if (func_j != NULL) {
                cJSON *fn_j = cJSON_GetObjectItem(func_j, "name");
                if (fn_j && cJSON_IsString(fn_j)) tcp->name = util_strdup(fn_j->valuestring);
                cJSON *fa_j = cJSON_GetObjectItem(func_j, "arguments");
                if (fa_j && cJSON_IsString(fa_j)) tcp->arguments = util_strdup(fa_j->valuestring);
            }

            if (tcp->id != NULL) (*out_call_count)++;
        }
    }

    /* usage */
    if (usage != NULL) {
        cJSON *usage_j = cJSON_GetObjectItem(root, "usage");
        if (usage_j != NULL) {
            cJSON *pt = cJSON_GetObjectItem(usage_j, "prompt_tokens");
            if (pt && cJSON_IsNumber(pt)) usage->prompt_tokens = pt->valueint;
            cJSON *ct = cJSON_GetObjectItem(usage_j, "completion_tokens");
            if (ct && cJSON_IsNumber(ct)) usage->completion_tokens = ct->valueint;
            cJSON *tt = cJSON_GetObjectItem(usage_j, "total_tokens");
            if (tt && cJSON_IsNumber(tt)) usage->total_tokens = tt->valueint;

            /* Prefix-cache telemetry. DeepSeek reports the top-level fields;
             * OpenAI nests cached_tokens under prompt_tokens_details. */
            cJSON *pch = cJSON_GetObjectItem(usage_j, "prompt_cache_hit_tokens");
            if (pch && cJSON_IsNumber(pch)) usage->prompt_cache_hit_tokens = pch->valueint;
            cJSON *pcm = cJSON_GetObjectItem(usage_j, "prompt_cache_miss_tokens");
            if (pcm && cJSON_IsNumber(pcm)) usage->prompt_cache_miss_tokens = pcm->valueint;
            cJSON *ptd = cJSON_GetObjectItem(usage_j, "prompt_tokens_details");
            if (ptd != NULL) {
                cJSON *cached = cJSON_GetObjectItem(ptd, "cached_tokens");
                if (cached && cJSON_IsNumber(cached)) usage->cached_tokens = cached->valueint;
            }
        }
    }

    cJSON_Delete(root);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Anthropic Messages API response parser                             */
/* ------------------------------------------------------------------ */

/* Append s to gb, inserting a "\n" separator when gb already holds content
 * (used when a response carries multiple text/thinking blocks). */
static void gb_append_joined(growbuf *gb, const char *s) {
    if (s == NULL || s[0] == '\0') return;
    if (gb->len > 0) write_cb((char *)"\n", 1, 1, gb);
    write_cb((char *)s, 1, strlen(s), gb);
}

int llm_parse_response_anthropic(const char *body, char **out_content, char **out_reasoning,
                                 char **out_model, tool_call **out_calls, int *out_call_count,
                                 usage_info *usage) {
    if (body == NULL) return EXIT_LLM_ERR;

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) return EXIT_LLM_ERR;

    /* Top-level API error: {"type":"error","error":{"type":...,"message":...}} */
    cJSON *api_error = cJSON_GetObjectItem(root, "error");
    if (api_error != NULL) {
        cJSON *msg_j = cJSON_GetObjectItem(api_error, "message");
        const char *err =
            (msg_j && cJSON_IsString(msg_j)) ? msg_j->valuestring : "Unknown API error";
        log_activity("[error] LLM API error: %s", err);
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }

    /* model */
    cJSON *model_j = cJSON_GetObjectItem(root, "model");
    if (model_j && cJSON_IsString(model_j) && model_j->valuestring) {
        *out_model = util_strdup(model_j->valuestring);
    }

    /* content: array of typed blocks. */
    cJSON *content_arr = cJSON_GetObjectItem(root, "content");
    if (content_arr == NULL || !cJSON_IsArray(content_arr)) {
        cJSON_Delete(root);
        return EXIT_LLM_ERR;
    }

    growbuf content_gb = {0};
    growbuf reasoning_gb = {0};
    tool_call *calls = NULL;
    int call_count = 0;
    int tc_capacity = 0;
    int rc = EXIT_SUCCESS;
    int n_blocks = cJSON_GetArraySize(content_arr);

    for (int i = 0; i < n_blocks; i++) {
        cJSON *block = cJSON_GetArrayItem(content_arr, i);
        if (block == NULL) continue;
        cJSON *type_j = cJSON_GetObjectItem(block, "type");
        const char *type = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

        if (strcmp(type, "text") == 0) {
            cJSON *text_j = cJSON_GetObjectItem(block, "text");
            if (text_j && cJSON_IsString(text_j)) {
                gb_append_joined(&content_gb, text_j->valuestring);
            }
        } else if (strcmp(type, "thinking") == 0) {
            cJSON *think_j = cJSON_GetObjectItem(block, "thinking");
            if (think_j && cJSON_IsString(think_j)) {
                gb_append_joined(&reasoning_gb, think_j->valuestring);
            }
        } else if (strcmp(type, "tool_use") == 0) {
            if (call_count == tc_capacity) {
                int newcap = tc_capacity ? tc_capacity * 2 : 4;
                tool_call *tmp = realloc(calls, (size_t)newcap * sizeof(tool_call));
                if (tmp == NULL) {
                    rc = EXIT_INTERNAL_ERR;
                    goto done;
                }
                calls = tmp;
                tc_capacity = newcap;
            }
            tool_call *tcp = &calls[call_count];
            memset(tcp, 0, sizeof(*tcp));

            cJSON *id_j = cJSON_GetObjectItem(block, "id");
            cJSON *name_j = cJSON_GetObjectItem(block, "name");
            if (id_j && cJSON_IsString(id_j)) tcp->id = util_strdup(id_j->valuestring);
            if (name_j && cJSON_IsString(name_j)) tcp->name = util_strdup(name_j->valuestring);

            /* Anthropic carries the arguments as a JSON object; re-serialize
             * it into the string form used across llmkit. */
            cJSON *input_j = cJSON_GetObjectItem(block, "input");
            if (input_j != NULL && cJSON_IsObject(input_j)) {
                tcp->arguments = cJSON_PrintUnformatted(input_j);
            }
            if (tcp->arguments == NULL) tcp->arguments = util_strdup("{}");

            call_count++;
        }
        /* Other block types (e.g. redacted_thinking) are ignored. */
    }

    /* usage: Anthropic excludes cache tokens from input_tokens; fold them
     * into the prompt-side totals so the fields stay comparable with the
     * OpenAI naming. cache_read maps onto cached_tokens. */
    if (usage != NULL) {
        cJSON *usage_j = cJSON_GetObjectItem(root, "usage");
        if (usage_j != NULL && cJSON_IsObject(usage_j)) {
            cJSON *it = cJSON_GetObjectItem(usage_j, "input_tokens");
            if (it && cJSON_IsNumber(it)) usage->prompt_tokens = it->valueint;
            cJSON *ot = cJSON_GetObjectItem(usage_j, "output_tokens");
            if (ot && cJSON_IsNumber(ot)) usage->completion_tokens = ot->valueint;
            cJSON *cr = cJSON_GetObjectItem(usage_j, "cache_read_input_tokens");
            if (cr && cJSON_IsNumber(cr)) usage->cached_tokens = cr->valueint;
            cJSON *cc = cJSON_GetObjectItem(usage_j, "cache_creation_input_tokens");
            if (cc && cJSON_IsNumber(cc)) usage->cache_creation_tokens = cc->valueint;
            usage->prompt_tokens += usage->cached_tokens + usage->cache_creation_tokens;
            usage->total_tokens = usage->prompt_tokens + usage->completion_tokens;
        }
    }

    *out_content = (content_gb.data != NULL) ? content_gb.data : util_strdup("");
    *out_reasoning = (reasoning_gb.data != NULL) ? reasoning_gb.data : util_strdup("");
    if (call_count > 0) {
        *out_calls = calls;
        *out_call_count = call_count;
    } else {
        free(calls);
        *out_calls = NULL;
        *out_call_count = 0;
    }

done:
    cJSON_Delete(root);
    if (rc != EXIT_SUCCESS) {
        for (int i = 0; i < call_count; i++) {
            free(calls[i].id);
            free(calls[i].name);
            free(calls[i].arguments);
        }
        free(calls);
        growbuf_free(&content_gb);
        growbuf_free(&reasoning_gb);
        return rc;
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  llm_chat_complete                                                  */
/* ------------------------------------------------------------------ */

/* Progress callback: abort the transfer promptly when an interrupt is
 * pending (SIGINT handling in platform.c), so Ctrl-C does not have to wait
 * for the LLM response or the 120s timeout. A non-zero return makes
 * curl_easy_perform fail with CURLE_ABORTED_BY_CALLBACK. */
static int llm_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                           curl_off_t ulnow) {
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return platform_sigint_pending() ? 1 : 0;
}

int llm_chat_complete(runtime_ctx *ctx, const json_message *messages, int msg_count,
                      const tool_def *tools, int tool_count, char **out_content,
                      char **out_reasoning, char **out_model, tool_call **out_calls,
                      int *out_call_count, usage_info *usage) {
    if (ctx == NULL) return EXIT_INTERNAL_ERR;
    if (out_content == NULL || out_reasoning == NULL || out_model == NULL || out_calls == NULL ||
        out_call_count == NULL)
        return EXIT_INTERNAL_ERR;

    *out_content = NULL;
    *out_reasoning = NULL;
    *out_model = NULL;
    *out_calls = NULL;
    *out_call_count = 0;
    if (usage != NULL) memset(usage, 0, sizeof(*usage));

    if (ctx->llm.api_base == NULL || ctx->llm.api_base[0] == '\0') {
        log_activity("[error] LLM API base URL not configured");
        return EXIT_LLM_ERR;
    }

    /* Build URL: {api_base}/{provider endpoint} (strip trailing slash).
     * OpenAI -> /chat/completions, Anthropic -> /messages. */
    size_t base_len = strlen(ctx->llm.api_base);
    while (base_len > 0 && ctx->llm.api_base[base_len - 1] == '/') base_len--;
    const char *endpoint =
        (ctx->llm.provider == LLM_PROVIDER_ANTHROPIC) ? "/messages" : "/chat/completions";
    char url[4096];
    int url_n = snprintf(url, sizeof(url), "%.*s%s", (int)base_len, ctx->llm.api_base, endpoint);
    if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
        log_activity("[error] LLM API base URL too long");
        return EXIT_LLM_ERR;
    }

    /* Build request body */
    char *body =
        (ctx->llm.provider == LLM_PROVIDER_ANTHROPIC)
            ? llm_build_request_body_anthropic(messages, msg_count, tools, tool_count, &ctx->llm)
            : llm_build_request_body(messages, msg_count, tools, tool_count, &ctx->llm);
    if (body == NULL) return EXIT_INTERNAL_ERR;

    /* cURL setup */
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(body);
        return EXIT_INTERNAL_ERR;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (ctx->llm.provider == LLM_PROVIDER_ANTHROPIC) {
        if (ctx->llm.api_key != NULL && ctx->llm.api_key[0] != '\0') {
            size_t n = strlen("x-api-key: ") + strlen(ctx->llm.api_key) + 1;
            char *auth = malloc(n);
            if (auth == NULL) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                free(body);
                return EXIT_INTERNAL_ERR;
            }
            snprintf(auth, n, "x-api-key: %s", ctx->llm.api_key);
            headers = curl_slist_append(headers, auth);
            free(auth);
        }
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    } else {
        if (ctx->llm.api_key != NULL && ctx->llm.api_key[0] != '\0') {
            size_t n = strlen("Authorization: Bearer ") + strlen(ctx->llm.api_key) + 1;
            char *auth = malloc(n);
            if (auth == NULL) {
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                free(body);
                return EXIT_INTERNAL_ERR;
            }
            snprintf(auth, n, "Authorization: Bearer %s", ctx->llm.api_key);
            headers = curl_slist_append(headers, auth);
            free(auth);
        }
    }
    headers = headers_to_slist(ctx->llm.headers, headers);

    growbuf gb = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &gb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "llmkit/" LLMKIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, llm_xferinfo_cb);

    log_activity("[progress] Waiting for LLM response...");
    CURLcode cc = curl_easy_perform(curl);

    /* Capture HTTP status code before cleanup. */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (cc != CURLE_OK) {
        log_activity("[error] LLM API request failed: %s", curl_easy_strerror(cc));
        growbuf_free(&gb);
        return EXIT_LLM_ERR;
    }

    if (http_code < 200 || http_code >= 300) {
        log_activity("[error] LLM API returned HTTP %ld", http_code);
        if (gb.data != NULL) {
            log_activity("[error] Response body: %.500s", gb.data);
        }
        growbuf_free(&gb);
        return EXIT_LLM_ERR;
    }

    /* Validate UTF-8 of the response body. */
    if (gb.data != NULL && !utf8_validate(gb.data, gb.len)) {
        log_activity("[error] LLM API response contains invalid UTF-8");
        growbuf_free(&gb);
        return EXIT_LLM_ERR;
    }

    /* Parse the response body with the provider's parser. */
    int rc = (ctx->llm.provider == LLM_PROVIDER_ANTHROPIC)
                 ? llm_parse_response_anthropic(gb.data ? gb.data : "", out_content, out_reasoning,
                                                out_model, out_calls, out_call_count, usage)
                 : llm_parse_response(gb.data ? gb.data : "", out_content, out_reasoning, out_model,
                                      out_calls, out_call_count, usage);
    growbuf_free(&gb);
    return rc;
}
