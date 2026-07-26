#include "conversation.h"
#include "util.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int write_json_line(FILE *fp, cJSON *root) {
    char *str = cJSON_PrintUnformatted(root);
    if (str == NULL) return EXIT_INTERNAL_ERR;
    int rc = EXIT_SUCCESS;
    if (fprintf(fp, "%s\n", str) < 0) rc = EXIT_FILE_ERR;
    free(str);
    if (rc == EXIT_SUCCESS) fflush(fp);
    return rc;
}

static cJSON *make_entry_base(const char *type_str) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "type", type_str);
    char ts[64];
    util_timestamp_now(ts, sizeof(ts));
    cJSON_AddStringToObject(root, "timestamp", ts);
    return root;
}

/* ------------------------------------------------------------------ */
/*  conversation_open                                                  */
/* ------------------------------------------------------------------ */

int conversation_open(const char *path, FILE **out_fp) {
    if (path == NULL || out_fp == NULL) return EXIT_INTERNAL_ERR;
    *out_fp = NULL;

    char *existing = util_read_file(path);
    if (existing != NULL) {
        size_t len = strlen(existing);
        if (!utf8_validate(existing, len)) {
            log_activity("[error] Conversation file contains invalid UTF-8: %s", path);
            free(existing);
            return EXIT_FILE_ERR;
        }
        free(existing);
    }

    *out_fp = fopen(path, "a");
    if (*out_fp == NULL) {
        log_activity("[error] Cannot open conversation file: %s", path);
        return EXIT_FILE_ERR;
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  conversation_write_meta                                            */
/* ------------------------------------------------------------------ */

int conversation_write_meta(FILE *fp, const char *config_hash, const char *run_id) {
    if (fp == NULL || config_hash == NULL || run_id == NULL) return EXIT_INTERNAL_ERR;

    cJSON *root = make_entry_base("meta");
    if (root == NULL) return EXIT_INTERNAL_ERR;

    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "config_hash", config_hash);
    cJSON_AddStringToObject(root, "run_id", run_id);

    int rc = write_json_line(fp, root);
    cJSON_Delete(root);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  conversation_write_entry (variadic)                                */
/* ------------------------------------------------------------------ */

int conversation_write_entry(FILE *fp, entry_type type, ...) {
    if (fp == NULL) return EXIT_INTERNAL_ERR;

    va_list args;
    va_start(args, type);
    int rc = EXIT_SUCCESS;
    cJSON *root = NULL;

    switch (type) {
    case ENTRY_USER: {
        const char *content = va_arg(args, const char *);
        const char *source = va_arg(args, const char *);
        root = make_entry_base("user");
        if (root == NULL) {
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddStringToObject(root, "content", content ? content : "");
        cJSON_AddStringToObject(root, "source", source ? source : "cli");
        break;
    }

    case ENTRY_ASSISTANT: {
        const char *content = va_arg(args, const char *);
        const char *model = va_arg(args, const char *);
        const usage_info *usage = va_arg(args, const usage_info *);
        root = make_entry_base("assistant");
        if (root == NULL) {
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddStringToObject(root, "content", content ? content : "");
        cJSON_AddStringToObject(root, "model", model ? model : "");
        if (usage != NULL) {
            cJSON *u = cJSON_CreateObject();
            if (u != NULL) {
                cJSON_AddNumberToObject(u, "prompt_tokens", usage->prompt_tokens);
                cJSON_AddNumberToObject(u, "completion_tokens", usage->completion_tokens);
                cJSON_AddNumberToObject(u, "total_tokens", usage->total_tokens);
                cJSON_AddItemToObject(root, "usage", u);
            }
        }
        break;
    }

    case ENTRY_TOOL_CALL: {
        const char *id = va_arg(args, const char *);
        const char *name = va_arg(args, const char *);
        const char *arguments = va_arg(args, const char *);
        const char *server = va_arg(args, const char *);
        root = make_entry_base("tool_call");
        if (root == NULL) {
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddStringToObject(root, "id", id ? id : "");
        cJSON_AddStringToObject(root, "name", name ? name : "");
        cJSON_AddStringToObject(root, "arguments", arguments ? arguments : "{}");
        cJSON_AddStringToObject(root, "mcp_server", server ? server : "");
        break;
    }

    case ENTRY_TOOL_RESULT: {
        const char *call_id = va_arg(args, const char *);
        const char *name = va_arg(args, const char *);
        const char *result = va_arg(args, const char *);
        int is_error = va_arg(args, int);
        int is_timeout = va_arg(args, int);
        const char *server = va_arg(args, const char *);
        root = make_entry_base("tool_result");
        if (root == NULL) {
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddStringToObject(root, "call_id", call_id ? call_id : "");
        cJSON_AddStringToObject(root, "name", name ? name : "");
        cJSON_AddStringToObject(root, "result", result ? result : "");
        cJSON_AddBoolToObject(root, "is_error", is_error ? 1 : 0);
        cJSON_AddBoolToObject(root, "is_timeout", is_timeout ? 1 : 0);
        cJSON_AddStringToObject(root, "mcp_server", server ? server : "");
        break;
    }

    case ENTRY_ERROR: {
        int code = va_arg(args, int);
        const char *message = va_arg(args, const char *);
        int recoverable = va_arg(args, int);
        root = make_entry_base("error");
        if (root == NULL) {
            rc = EXIT_INTERNAL_ERR;
            break;
        }
        cJSON_AddNumberToObject(root, "code", code);
        cJSON_AddStringToObject(root, "message", message ? message : "");
        cJSON_AddBoolToObject(root, "recoverable", recoverable ? 1 : 0);
        break;
    }

    default:
        rc = EXIT_INTERNAL_ERR;
        break;
    }

    va_end(args);

    if (root != NULL) {
        int wr = write_json_line(fp, root);
        cJSON_Delete(root);
        if (rc == EXIT_SUCCESS) rc = wr;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  grow helpers                                                       */
/* ------------------------------------------------------------------ */

static json_message *grow_msgs(json_message *msgs, int *cap, int need) {
    if (need <= *cap) return msgs;
    int new_cap = *cap ? *cap : 16;
    while (new_cap < need) new_cap *= 2;
    json_message *tmp = realloc(msgs, (size_t)new_cap * sizeof(json_message));
    if (tmp == NULL) return NULL;
    memset(tmp + *cap, 0, (size_t)(new_cap - *cap) * sizeof(json_message));
    *cap = new_cap;
    return tmp;
}

static tool_call *grow_tcalls(tool_call *arr, int *cap, int need) {
    if (need <= *cap) return arr;
    int new_cap = *cap ? *cap : 4;
    while (new_cap < need) new_cap *= 2;
    tool_call *tmp = realloc(arr, (size_t)new_cap * sizeof(tool_call));
    if (tmp == NULL) return NULL;
    memset(tmp + *cap, 0, (size_t)(new_cap - *cap) * sizeof(tool_call));
    *cap = new_cap;
    return tmp;
}

static const char *json_str(cJSON *obj, const char *field, const char *def) {
    cJSON *j = cJSON_GetObjectItem(obj, field);
    return (j && cJSON_IsString(j)) ? j->valuestring : def;
}

static void free_entries(void *p, int count) {
    /* p is conv_entry array; each entry owns a cJSON */
    typedef struct {
        entry_type type;
        cJSON *json;
    } ce;
    ce *arr = (ce *)p;
    if (arr == NULL) return;
    for (int i = 0; i < count; i++) {
        if (arr[i].json) cJSON_Delete(arr[i].json);
    }
    free(arr);
}

/* ------------------------------------------------------------------ */
/*  conversation_reconstruct (two-pass)                                */
/* ------------------------------------------------------------------ */
/* Internal entry for the first parse pass. */
typedef struct {
    entry_type type;
    cJSON *json;
} conv_entry;

int conversation_reconstruct(const char *path, json_message **out_msgs, int *out_count) {
    if (path == NULL || out_msgs == NULL || out_count == NULL) return EXIT_INTERNAL_ERR;
    *out_msgs = NULL;
    *out_count = 0;

    char *content = util_read_file(path);
    if (content == NULL) return EXIT_SUCCESS;

    /* ---- Pass 1: parse all lines into conv_entry array ---- */
    int ecap = 32, ecount = 0;
    conv_entry *entries = calloc((size_t)ecap, sizeof(conv_entry));
    if (entries == NULL) {
        free(content);
        return EXIT_INTERNAL_ERR;
    }

    {
        char *line = content;
        while (line != NULL && *line != '\0') {
            char *next = strchr(line, '\n');
            if (next != NULL) *next = '\0';

            size_t ll = strlen(line);
            while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = '\0';

            if (ll > 0) {
                cJSON *j = cJSON_Parse(line);
                if (j != NULL) {
                    cJSON *tj = cJSON_GetObjectItem(j, "type");
                    const char *ts = (tj && cJSON_IsString(tj)) ? tj->valuestring : NULL;
                    if (ts != NULL) {
                        entry_type et;
                        if (strcmp(ts, "user") == 0) {
                            et = ENTRY_USER;
                        } else if (strcmp(ts, "assistant") == 0) {
                            et = ENTRY_ASSISTANT;
                        } else if (strcmp(ts, "tool_call") == 0) {
                            et = ENTRY_TOOL_CALL;
                        } else if (strcmp(ts, "tool_result") == 0) {
                            et = ENTRY_TOOL_RESULT;
                        } else if (strcmp(ts, "error") == 0) {
                            et = ENTRY_ERROR;
                        } else if (strcmp(ts, "meta") == 0) {
                            et = ENTRY_META;
                        } else {
                            cJSON_Delete(j);
                            line = next ? next + 1 : NULL;
                            continue;
                        }

                        /* Skip meta and error -- not part of LLM history. */
                        if (et != ENTRY_META && et != ENTRY_ERROR) {
                            if (ecount >= ecap) {
                                int new_cap = ecap * 2;
                                conv_entry *tmp =
                                    realloc(entries, (size_t)new_cap * sizeof(conv_entry));
                                if (tmp == NULL) {
                                    free_entries(entries, ecount);
                                    free(content);
                                    return EXIT_INTERNAL_ERR;
                                }
                                memset(tmp + ecap, 0,
                                       (size_t)(new_cap - ecap) * sizeof(conv_entry));
                                entries = tmp;
                                ecap = new_cap;
                            }
                            entries[ecount].type = et;
                            entries[ecount].json = j;
                            ecount++;
                            line = next ? next + 1 : NULL;
                            continue;
                        }
                    }
                    cJSON_Delete(j);
                }
            }
            line = next ? next + 1 : NULL;
        }
    }

    /* ---- Pass 2: build json_message array from entries ---- */
    int msg_cap = 16;
    json_message *msgs = calloc((size_t)msg_cap, sizeof(json_message));
    if (msgs == NULL) {
        free_entries(entries, ecount);
        free(content);
        return EXIT_INTERNAL_ERR;
    }
    int msg_count = 0;

    for (int i = 0; i < ecount;) {
        conv_entry *re = &entries[i];

        if (re->type == ENTRY_USER) {
            msgs = grow_msgs(msgs, &msg_cap, msg_count + 1);
            if (msgs == NULL) {
                free_entries(entries, ecount);
                conversation_free_messages(msgs, msg_count);
                free(content);
                return EXIT_INTERNAL_ERR;
            }
            json_message *m = &msgs[msg_count++];
            m->role = util_strdup("user");
            m->content = util_strdup(json_str(re->json, "content", ""));
            i++;
            continue;
        }

        if (re->type == ENTRY_ASSISTANT) {
            msgs = grow_msgs(msgs, &msg_cap, msg_count + 1);
            if (msgs == NULL) {
                free_entries(entries, ecount);
                conversation_free_messages(msgs, msg_count);
                free(content);
                return EXIT_INTERNAL_ERR;
            }
            json_message *m = &msgs[msg_count++];
            m->role = util_strdup("assistant");
            m->content = util_strdup(json_str(re->json, "content", ""));
            int asst_idx = msg_count - 1;
            i++;

            /* Collect tool_call and tool_result entries that follow. */
            int tc_count = 0, tc_cap = 0;
            tool_call *tc_arr = NULL;

            while (i < ecount) {
                conv_entry *pe = &entries[i];
                if (pe->type == ENTRY_TOOL_CALL) {
                    tc_arr = grow_tcalls(tc_arr, &tc_cap, tc_count + 1);
                    if (tc_arr == NULL) {
                        free(tc_arr);
                        free_entries(entries, ecount);
                        conversation_free_messages(msgs, msg_count);
                        free(content);
                        return EXIT_INTERNAL_ERR;
                    }
                    tc_arr[tc_count].id = util_strdup(json_str(pe->json, "id", ""));
                    tc_arr[tc_count].name = util_strdup(json_str(pe->json, "name", ""));
                    tc_arr[tc_count].arguments = util_strdup(json_str(pe->json, "arguments", "{}"));
                    tc_count++;
                    i++;

                } else if (pe->type == ENTRY_TOOL_RESULT) {
                    msgs = grow_msgs(msgs, &msg_cap, msg_count + 1);
                    if (msgs == NULL) {
                        free(tc_arr);
                        free_entries(entries, ecount);
                        conversation_free_messages(msgs, msg_count);
                        free(content);
                        return EXIT_INTERNAL_ERR;
                    }
                    json_message *tm = &msgs[msg_count++];
                    tm->role = util_strdup("tool");
                    tm->tool_call_id = util_strdup(json_str(pe->json, "call_id", ""));
                    tm->content = util_strdup(json_str(pe->json, "result", ""));
                    i++;

                } else {
                    break;
                }
            }

            msgs[asst_idx].tool_calls = tc_arr;
            msgs[asst_idx].tool_call_count = tc_count;
            continue;
        }

        /* Orphan tool_call/tool_result -- skip. */
        i++;
    }

    free_entries(entries, ecount);
    free(content);
    *out_msgs = msgs;
    *out_count = msg_count;
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  conversation_read_last_assistant                                  */
/* ------------------------------------------------------------------ */

int conversation_read_last_assistant(const char *path, char **out_content) {
    if (path == NULL || out_content == NULL) return EXIT_INTERNAL_ERR;
    *out_content = NULL;

    char *content = util_read_file(path);
    if (content == NULL) {
        /* File does not exist or is empty -- no assistant found. */
        *out_content = util_strdup("");
        return *out_content ? EXIT_SUCCESS : EXIT_INTERNAL_ERR;
    }

    /* Walk lines in reverse to find the last "assistant" entry. */
    char *last_content = NULL;

    /* Split into lines.  We'll scan forward and overwrite the last match. */
    char *line = content;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) *next = '\0';

        size_t ll = strlen(line);
        while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = '\0';

        if (ll > 0) {
            cJSON *j = cJSON_Parse(line);
            if (j != NULL) {
                cJSON *tj = cJSON_GetObjectItem(j, "type");
                const char *ts = (tj && cJSON_IsString(tj)) ? tj->valuestring : NULL;
                if (ts != NULL && strcmp(ts, "assistant") == 0) {
                    cJSON *cj = cJSON_GetObjectItem(j, "content");
                    const char *val = (cj && cJSON_IsString(cj)) ? cj->valuestring : "";
                    free(last_content);
                    last_content = util_strdup(val);
                    if (last_content == NULL) {
                        cJSON_Delete(j);
                        free(content);
                        free(last_content);
                        *out_content = NULL;
                        return EXIT_INTERNAL_ERR;
                    }
                }
                cJSON_Delete(j);
            }
        }
        line = next ? next + 1 : NULL;
    }

    free(content);

    if (last_content == NULL) {
        last_content = util_strdup("");
        if (last_content == NULL) return EXIT_INTERNAL_ERR;
    }

    *out_content = last_content;
    return EXIT_SUCCESS;
}

void conversation_free_messages(json_message *msgs, int count) {
    if (msgs == NULL) return;
    for (int i = 0; i < count; i++) {
        free(msgs[i].role);
        free(msgs[i].content);
        free(msgs[i].tool_call_id);
        if (msgs[i].tool_calls != NULL) {
            for (int j = 0; j < msgs[i].tool_call_count; j++) {
                free(msgs[i].tool_calls[j].id);
                free(msgs[i].tool_calls[j].name);
                free(msgs[i].tool_calls[j].arguments);
            }
            free(msgs[i].tool_calls);
        }
    }
    free(msgs);
}
