#include "llm.h"
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

static char *build_request_body(const json_message *msgs, int msg_count, const tool_def *tools,
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

/* ------------------------------------------------------------------ */
/*  Response parser                                                    */
/* ------------------------------------------------------------------ */

static int parse_response(const char *body, char **out_content, char **out_reasoning,
                          char **out_model, tool_call **out_calls, int *out_call_count,
                          usage_info *usage) {
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
        }
    }

    cJSON_Delete(root);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  llm_chat_complete                                                  */
/* ------------------------------------------------------------------ */

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

    /* Build URL: {api_base}/chat/completions (strip trailing slash). */
    size_t base_len = strlen(ctx->llm.api_base);
    while (base_len > 0 && ctx->llm.api_base[base_len - 1] == '/') base_len--;
    char url[4096];
    int url_n =
        snprintf(url, sizeof(url), "%.*s/chat/completions", (int)base_len, ctx->llm.api_base);
    if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
        log_activity("[error] LLM API base URL too long");
        return EXIT_LLM_ERR;
    }

    /* Build request body */
    char *body = build_request_body(messages, msg_count, tools, tool_count, &ctx->llm);
    if (body == NULL) return EXIT_INTERNAL_ERR;

    /* cURL setup */
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(body);
        return EXIT_INTERNAL_ERR;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (ctx->llm.api_key != NULL && ctx->llm.api_key[0] != '\0') {
        char auth[1024];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->llm.api_key);
        headers = curl_slist_append(headers, auth);
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

    /* Parse the response body. */
    int rc = parse_response(gb.data ? gb.data : "", out_content, out_reasoning, out_model,
                            out_calls, out_call_count, usage);
    growbuf_free(&gb);
    return rc;
}
