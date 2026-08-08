#include "compact.h"
#include "conversation.h"
#include "llm.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#define COMPACT_SIDECAR_VERSION 1
#define COMPACT_TAIL_MESSAGES   2 /* recent messages kept verbatim */
#define COMPACT_MIN_MESSAGES    6 /* do not compact tiny conversations */
#define COMPACT_SUMMARY_PROMPT                                             \
    "Summarize the following conversation concisely. Preserve key facts, " \
    "decisions, and the user's original request. Output only the summary text."
#define COMPACT_PLACEHOLDER_SUMMARY                                              \
    "[Earlier conversation compacted: this part of the conversation was folded " \
    "into a single message so the provider-visible prompt prefix stays stable.]"

/* ------------------------------------------------------------------ */
/*  Token estimation                                                   */
/* ------------------------------------------------------------------ */

int64_t compact_estimate_tokens(const json_message *msgs, int msg_count) {
    int64_t bytes = 0;
    for (int i = 0; i < msg_count; i++) {
        const json_message *m = &msgs[i];
        if (m->role) bytes += (int64_t)strlen(m->role);
        if (m->content) bytes += (int64_t)strlen(m->content);
        if (m->reasoning) bytes += (int64_t)strlen(m->reasoning);
        if (m->tool_call_id) bytes += (int64_t)strlen(m->tool_call_id);
        for (int j = 0; j < m->tool_call_count; j++) {
            if (m->tool_calls[j].id) bytes += (int64_t)strlen(m->tool_calls[j].id);
            if (m->tool_calls[j].name) bytes += (int64_t)strlen(m->tool_calls[j].name);
            if (m->tool_calls[j].arguments) bytes += (int64_t)strlen(m->tool_calls[j].arguments);
        }
    }
    /* bytes/4 heuristic; never zero so a trigger check cannot divide by it. */
    return (bytes / 4) + 1;
}

/* ------------------------------------------------------------------ */
/*  Message helpers                                                    */
/* ------------------------------------------------------------------ */

/* Deep-copy a message array (nested tool_calls included). The copy can be
 * freed with conversation_free_messages(). Returns NULL on OOM. */
static json_message *dup_messages(const json_message *src, int count) {
    if (count <= 0) return NULL;
    json_message *out = calloc((size_t)count, sizeof(json_message));
    if (out == NULL) return NULL;
    for (int i = 0; i < count; i++) {
        out[i].role = util_strdup(src[i].role ? src[i].role : "");
        out[i].content = util_strdup(src[i].content ? src[i].content : "");
        if (src[i].reasoning) out[i].reasoning = util_strdup(src[i].reasoning);
        if (src[i].tool_call_id) out[i].tool_call_id = util_strdup(src[i].tool_call_id);
        if (src[i].tool_call_count > 0) {
            out[i].tool_calls = calloc((size_t)src[i].tool_call_count, sizeof(tool_call));
            if (out[i].tool_calls == NULL) {
                conversation_free_messages(out, count);
                return NULL;
            }
            for (int j = 0; j < src[i].tool_call_count; j++) {
                out[i].tool_calls[j].id =
                    util_strdup(src[i].tool_calls[j].id ? src[i].tool_calls[j].id : "");
                out[i].tool_calls[j].name =
                    util_strdup(src[i].tool_calls[j].name ? src[i].tool_calls[j].name : "");
                out[i].tool_calls[j].arguments = util_strdup(
                    src[i].tool_calls[j].arguments ? src[i].tool_calls[j].arguments : "");
            }
            out[i].tool_call_count = src[i].tool_call_count;
        }
    }
    return out;
}

/* Build the projection prefix: the pinned leading messages (system prompt +
 * first user message, verbatim) plus one summary message. The pinned messages
 * are plain system/user messages, so only role/content/reasoning are copied.
 * Returns a new array of length pin + 1, or NULL on OOM. */
static json_message *build_projection(const json_message *all, int pin, const char *summary_text,
                                      int *out_count) {
    json_message *proj = calloc((size_t)pin + 1, sizeof(json_message));
    if (proj == NULL) return NULL;
    for (int i = 0; i < pin; i++) {
        proj[i].role = util_strdup(all[i].role ? all[i].role : "");
        proj[i].content = util_strdup(all[i].content ? all[i].content : "");
        if (all[i].reasoning) proj[i].reasoning = util_strdup(all[i].reasoning);
    }
    proj[pin].role = util_strdup("user");
    proj[pin].content = util_strdup(summary_text);
    if (proj[pin].role == NULL || proj[pin].content == NULL) {
        conversation_free_messages(proj, pin + 1);
        return NULL;
    }
    *out_count = pin + 1;
    return proj;
}

/* Generate the rolling summary for the covered span all[pin..covered). With
 * agent.compact.summarize the middle of the conversation is summarized via
 * the LLM (same endpoint); otherwise a static placeholder is used. On LLM
 * failure the placeholder is used. Returns a malloc'd string or NULL on OOM. */
static char *generate_summary(runtime_ctx *ctx, const json_message *all, int pin, int covered) {
    if (!ctx->agent.compact_summarize) {
        return util_strdup(COMPACT_PLACEHOLDER_SUMMARY);
    }

    int mid_count = covered - pin;
    json_message *mid = calloc((size_t)mid_count + 1, sizeof(json_message));
    if (mid == NULL) return NULL;
    mid[0].role = util_strdup("system");
    mid[0].content = util_strdup(COMPACT_SUMMARY_PROMPT);
    json_message *body = dup_messages(&all[pin], mid_count);
    if (mid[0].role == NULL || mid[0].content == NULL || body == NULL) {
        conversation_free_messages(mid, 1);
        conversation_free_messages(body, mid_count);
        return NULL;
    }
    memcpy(&mid[1], body, (size_t)mid_count * sizeof(json_message));
    free(body);

    char *content = NULL, *reasoning = NULL, *model = NULL;
    tool_call *calls = NULL;
    int call_count = 0;
    usage_info usage;
    memset(&usage, 0, sizeof(usage));
    int rc = llm_chat_complete(ctx, mid, mid_count + 1, NULL, 0, &content, &reasoning, &model,
                               &calls, &call_count, &usage);
    free(reasoning);
    free(model);
    free(calls);
    conversation_free_messages(mid, mid_count + 1);

    if (rc != EXIT_SUCCESS || content == NULL || content[0] == '\0') {
        log_activity("[compact] summary generation failed, using placeholder");
        free(content);
        return util_strdup(COMPACT_PLACEHOLDER_SUMMARY);
    }
    return content;
}

/* ------------------------------------------------------------------ */
/*  Sidecar persistence                                                */
/* ------------------------------------------------------------------ */

/* Sidecar path: <conversation>.context.json */
static char *sidecar_path(const char *convo_path) {
    size_t len = strlen(convo_path);
    char *p = malloc(len + sizeof(".context.json") + 1);
    if (p == NULL) return NULL;
    memcpy(p, convo_path, len);
    memcpy(p + len, ".context.json", sizeof(".context.json"));
    return p;
}

/* Write the sidecar atomically (temp file + rename). Returns EXIT_SUCCESS or
 * EXIT_FILE_ERR. */
static int write_sidecar(const char *path, const char *json) {
    char *tmp = malloc(strlen(path) + 5);
    if (tmp == NULL) return EXIT_INTERNAL_ERR;
    snprintf(tmp, strlen(path) + 5, "%s.tmp", path);

    FILE *fp = fopen(tmp, "wb");
    if (fp == NULL) {
        free(tmp);
        return EXIT_FILE_ERR;
    }
    int ok = fwrite(json, 1, strlen(json), fp) == strlen(json);
    if (fclose(fp) != 0) ok = 0;
    if (ok) {
        ok = (rename(tmp, path) == 0);
    }
    free(tmp);
    return ok ? EXIT_SUCCESS : EXIT_FILE_ERR;
}

/* Parse a stored projection (a JSON messages array) back into messages.
 * Returns EXIT_SUCCESS with *out set (caller frees with
 * conversation_free_messages) or an error code. */
static int parse_projection(const char *json, json_message **out, int *out_count) {
    *out = NULL;
    *out_count = 0;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return EXIT_FILE_ERR;
    }
    int n = cJSON_GetArraySize(root);
    json_message *msgs = calloc((size_t)n, sizeof(json_message));
    if (msgs == NULL) {
        cJSON_Delete(root);
        return EXIT_INTERNAL_ERR;
    }
    for (int i = 0; i < n; i++) {
        cJSON *mj = cJSON_GetArrayItem(root, i);
        if (mj == NULL) continue;
        cJSON *r = cJSON_GetObjectItem(mj, "role");
        cJSON *c = cJSON_GetObjectItem(mj, "content");
        cJSON *rc = cJSON_GetObjectItem(mj, "reasoning_content");
        cJSON *tci = cJSON_GetObjectItem(mj, "tool_call_id");
        msgs[i].role = util_strdup((r && cJSON_IsString(r)) ? r->valuestring : "");
        msgs[i].content = util_strdup((c && cJSON_IsString(c)) ? c->valuestring : "");
        if (rc && cJSON_IsString(rc)) msgs[i].reasoning = util_strdup(rc->valuestring);
        if (tci && cJSON_IsString(tci)) msgs[i].tool_call_id = util_strdup(tci->valuestring);

        cJSON *tcs = cJSON_GetObjectItem(mj, "tool_calls");
        if (tcs && cJSON_IsArray(tcs)) {
            int tc_n = cJSON_GetArraySize(tcs);
            msgs[i].tool_calls = calloc((size_t)tc_n, sizeof(tool_call));
            if (msgs[i].tool_calls != NULL) {
                for (int j = 0; j < tc_n; j++) {
                    cJSON *tcj = cJSON_GetArrayItem(tcs, j);
                    if (tcj == NULL) continue;
                    cJSON *idj = cJSON_GetObjectItem(tcj, "id");
                    cJSON *fnj = cJSON_GetObjectItem(tcj, "function");
                    msgs[i].tool_calls[j].id =
                        util_strdup((idj && cJSON_IsString(idj)) ? idj->valuestring : "");
                    if (fnj != NULL) {
                        cJSON *nm = cJSON_GetObjectItem(fnj, "name");
                        cJSON *ar = cJSON_GetObjectItem(fnj, "arguments");
                        msgs[i].tool_calls[j].name =
                            util_strdup((nm && cJSON_IsString(nm)) ? nm->valuestring : "");
                        msgs[i].tool_calls[j].arguments =
                            util_strdup((ar && cJSON_IsString(ar)) ? ar->valuestring : "");
                    }
                    msgs[i].tool_call_count++;
                }
            }
        }
    }
    cJSON_Delete(root);
    *out = msgs;
    *out_count = n;
    return EXIT_SUCCESS;
}

/* Try to load a valid projection sidecar for (convo_path, model). On success
 * sets *out to the projection messages and *covered. Returns 1 when valid and
 * applied, 0 when no usable sidecar exists (caller may compact), or a
 * negative EXIT_* error code on IO/OOM failure. */
static int load_sidecar(runtime_ctx *ctx, const char *convo_path, const json_message *canonical,
                        int msg_count, json_message **out, int *out_proj_count, int *out_covered) {
    *out = NULL;
    *out_proj_count = 0;
    *out_covered = 0;

    char *path = sidecar_path(convo_path);
    if (path == NULL) return -EXIT_INTERNAL_ERR;
    char *raw = util_read_file(path);
    free(path);
    if (raw == NULL) return 0;

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (root == NULL) return 0;

    int ok = 0;
    int pcount = 0;
    cJSON *vj = cJSON_GetObjectItem(root, "version");
    cJSON *ccj = cJSON_GetObjectItem(root, "covered_count");
    cJSON *hj = cJSON_GetObjectItem(root, "covered_prefix_hash");
    cJSON *kj = cJSON_GetObjectItem(root, "prompt_cache_key");
    cJSON *pj = cJSON_GetObjectItem(root, "projection");
    if (vj && cJSON_IsNumber(vj) && vj->valueint == COMPACT_SIDECAR_VERSION && ccj &&
        cJSON_IsNumber(ccj) && hj && cJSON_IsString(hj) && kj && cJSON_IsString(kj) && pj &&
        cJSON_IsArray(pj)) {
        int covered = ccj->valueint;
        /* Fail-closed: model key must match and the canonical covered span
         * must hash to the stored value (append-only growth is fine, edits
         * are not). */
        if (covered > 0 && covered <= msg_count) {
            /* prompt_cache_key: sha256(convo_path "|" model) */
            char key_input[4096];
            snprintf(key_input, sizeof(key_input), "%s|%s", convo_path,
                     ctx->llm.model ? ctx->llm.model : "");
            char key_hash[65];
            util_sha256(key_input, strlen(key_input), key_hash);
            char key_prefixed[80];
            snprintf(key_prefixed, sizeof(key_prefixed), "sha256:%s", key_hash);

            char *ser = llm_serialize_messages(canonical, covered, &ctx->llm);
            char hash[65];
            if (ser != NULL) {
                util_sha256(ser, strlen(ser), hash);
                free(ser);
                char hash_prefixed[80];
                snprintf(hash_prefixed, sizeof(hash_prefixed), "sha256:%s", hash);
                if (strcmp(key_prefixed, kj->valuestring) == 0 &&
                    strcmp(hash_prefixed, hj->valuestring) == 0) {
                    char *proj_json = cJSON_PrintUnformatted(pj);
                    if (proj_json != NULL) {
                        if (parse_projection(proj_json, out, &pcount) == EXIT_SUCCESS &&
                            pcount > 0) {
                            *out_proj_count = pcount;
                            *out_covered = covered;
                            ok = 1;
                        }
                        free(proj_json);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    if (ok) return 1;
    if (*out != NULL) {
        conversation_free_messages(*out, pcount);
        *out = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  compact_apply                                                      */
/* ------------------------------------------------------------------ */

int compact_apply(runtime_ctx *ctx, const char *convo_path, json_message **in_out_msgs,
                  int *in_out_count, int *out_covered) {
    if (ctx == NULL || convo_path == NULL || in_out_msgs == NULL || in_out_count == NULL ||
        out_covered == NULL) {
        return EXIT_INTERNAL_ERR;
    }
    *out_covered = 0;
    if (!ctx->agent.compact_enabled) return EXIT_SUCCESS;

    json_message *msgs = *in_out_msgs;
    int msg_count = *in_out_count;
    if (msgs == NULL || msg_count < COMPACT_MIN_MESSAGES) return EXIT_SUCCESS;

    /* Reuse a valid sidecar projection when present. */
    json_message *proj = NULL;
    int pcount = 0, covered = 0;
    int lr = load_sidecar(ctx, convo_path, msgs, msg_count, &proj, &pcount, &covered);
    if (lr < 0) return -lr;
    if (lr == 1 && proj != NULL) {
        int tail = msg_count - covered;
        json_message *merged = calloc((size_t)pcount + (size_t)tail, sizeof(json_message));
        if (merged == NULL) {
            conversation_free_messages(proj, pcount);
            return EXIT_INTERNAL_ERR;
        }
        memcpy(merged, proj, (size_t)pcount * sizeof(json_message));
        free(proj);
        /* The tail must be deep-copied: the canonical array is freed below. */
        json_message *tail_copy = dup_messages(&msgs[covered], tail);
        if (tail_copy == NULL) {
            /* Leave the canonical array untouched on error: the caller frees
             * it (freeing it here would double-free at the call site). */
            conversation_free_messages(merged, pcount);
            return EXIT_INTERNAL_ERR;
        }
        memcpy(&merged[pcount], tail_copy, (size_t)tail * sizeof(json_message));
        free(tail_copy);
        conversation_free_messages(msgs, msg_count);
        *in_out_msgs = merged;
        *in_out_count = pcount + tail;
        *out_covered = covered;
        log_activity("[compact] reusing projection (covered=%d of %d messages)", covered,
                     msg_count);
        return EXIT_SUCCESS;
    }

    /* Otherwise: compact only when the estimate exceeds the budget. */
    int64_t max_tokens = ctx->agent.compact_max_tokens > 0 ? ctx->agent.compact_max_tokens : 16384;
    double threshold = ctx->agent.compact_threshold;
    if (threshold <= 0.0 || threshold > 1.0) threshold = 0.8;
    int64_t estimate = compact_estimate_tokens(msgs, msg_count);
    if (estimate <= (int64_t)((double)max_tokens * threshold)) {
        return EXIT_SUCCESS;
    }

    int pin = (msgs[0].role != NULL && strcmp(msgs[0].role, "system") == 0) ? 1 : 0;
    pin += 1; /* first user message (the original prompt) stays verbatim */
    covered = msg_count - COMPACT_TAIL_MESSAGES;
    /* Never split an assistant tool_call from its tool result at the tail
     * boundary: fold leading tool-result messages into the covered span so
     * each call/result pair is either fully summarized or fully kept. */
    while (covered < msg_count - 1 && msgs[covered].role != NULL &&
           strcmp(msgs[covered].role, "tool") == 0) {
        covered++;
    }
    if (covered <= pin) return EXIT_SUCCESS;

    char *summary = generate_summary(ctx, msgs, pin, covered);
    if (summary == NULL) return EXIT_INTERNAL_ERR;

    json_message *projection = build_projection(msgs, pin, summary, &pcount);
    free(summary);
    if (projection == NULL) return EXIT_INTERNAL_ERR;

    /* Persist the sidecar before switching the request to the projection. */
    char *ser_proj = llm_serialize_messages(projection, pcount, &ctx->llm);
    char *ser_covered = llm_serialize_messages(msgs, covered, &ctx->llm);
    if (ser_proj == NULL || ser_covered == NULL) {
        free(ser_proj);
        free(ser_covered);
        conversation_free_messages(projection, pcount);
        return EXIT_INTERNAL_ERR;
    }
    char covered_hash[65], key_hash[65];
    util_sha256(ser_covered, strlen(ser_covered), covered_hash);
    char key_input[4096];
    snprintf(key_input, sizeof(key_input), "%s|%s", convo_path,
             ctx->llm.model ? ctx->llm.model : "");
    util_sha256(key_input, strlen(key_input), key_hash);
    free(ser_covered);

    char *sidecar = NULL;
    {
        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            free(ser_proj);
            conversation_free_messages(projection, pcount);
            return EXIT_INTERNAL_ERR;
        }
        cJSON_AddNumberToObject(root, "version", COMPACT_SIDECAR_VERSION);
        cJSON_AddNumberToObject(root, "covered_count", covered);
        char hp[80], kp[80];
        snprintf(hp, sizeof(hp), "sha256:%s", covered_hash);
        snprintf(kp, sizeof(kp), "sha256:%s", key_hash);
        cJSON_AddStringToObject(root, "covered_prefix_hash", hp);
        cJSON_AddStringToObject(root, "prompt_cache_key", kp);
        cJSON *pj = cJSON_Parse(ser_proj);
        if (pj != NULL) {
            cJSON_AddItemToObject(root, "projection", pj);
        }
        cJSON_AddStringToObject(root, "cache_state", "unknown");
        sidecar = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    free(ser_proj);
    if (sidecar == NULL) {
        conversation_free_messages(projection, pcount);
        return EXIT_INTERNAL_ERR;
    }

    char *spath = sidecar_path(convo_path);
    if (spath == NULL) {
        free(sidecar);
        conversation_free_messages(projection, pcount);
        return EXIT_INTERNAL_ERR;
    }
    int wr = write_sidecar(spath, sidecar);
    free(spath);
    free(sidecar);
    if (wr != EXIT_SUCCESS) {
        /* Fail-closed: without a persisted sidecar the projection cannot be
         * reused consistently across turns, so do not compact. */
        log_activity("[compact] failed to write sidecar, skipping compaction");
        conversation_free_messages(projection, pcount);
        return EXIT_SUCCESS;
    }

    int tail = msg_count - covered;
    json_message *merged = calloc((size_t)pcount + (size_t)tail, sizeof(json_message));
    if (merged == NULL) {
        conversation_free_messages(projection, pcount);
        return EXIT_INTERNAL_ERR;
    }
    memcpy(merged, projection, (size_t)pcount * sizeof(json_message));
    free(projection);
    /* The tail must be deep-copied: the canonical array is freed below. */
    json_message *tail_copy = dup_messages(&msgs[covered], tail);
    if (tail_copy == NULL) {
        /* Leave the canonical array untouched on error: the caller frees
         * it (freeing it here would double-free at the call site). */
        conversation_free_messages(merged, pcount);
        return EXIT_INTERNAL_ERR;
    }
    memcpy(&merged[pcount], tail_copy, (size_t)tail * sizeof(json_message));
    free(tail_copy);
    conversation_free_messages(msgs, msg_count);
    *in_out_msgs = merged;
    *in_out_count = pcount + tail;
    *out_covered = covered;
    log_activity("[compact] projected %d messages into %d (est. %lld tokens > %lld)", msg_count,
                 pcount + tail, (long long)estimate, (long long)((double)max_tokens * threshold));
    return EXIT_SUCCESS;
}
