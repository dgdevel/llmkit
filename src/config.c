#include "config.h"
#include "utf8.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

/* Internal parser state */
typedef struct {
    yaml_parser_t *parser;
    yaml_event_t event;
    int error_code;
    char error_msg[256];
    runtime_ctx *ctx;
} cfg_parse;

static int cfg_next(cfg_parse *p) {
    if (!yaml_parser_parse(p->parser, &p->event)) {
        snprintf(p->error_msg, sizeof(p->error_msg), "YAML parse error: %s", p->parser->problem);
        p->error_code = EXIT_CONFIG_ERR;
        return -1;
    }
    return 0;
}

static int cfg_expect(cfg_parse *p, yaml_event_type_t type) {
    if (p->event.type != type) {
        snprintf(p->error_msg, sizeof(p->error_msg), "Expected YAML event type %d, got %d", type,
                 p->event.type);
        p->error_code = EXIT_CONFIG_ERR;
        return -1;
    }
    return 0;
}

static void cfg_event_done(cfg_parse *p) {
    yaml_event_delete(&p->event);
}

static const char *cfg_scalar(cfg_parse *p) {
    if (p->event.type != YAML_SCALAR_EVENT) return NULL;
    return (const char *)p->event.data.scalar.value;
}

static int cfg_skip_mapping(cfg_parse *p) {
    int depth = 1;
    while (depth > 0) {
        if (cfg_next(p) != 0) return -1;
        if (p->event.type == YAML_MAPPING_START_EVENT)
            depth++;
        else if (p->event.type == YAML_MAPPING_END_EVENT)
            depth--;
        cfg_event_done(p);
    }
    return 0;
}

static int cfg_skip_sequence(cfg_parse *p) {
    int depth = 1;
    while (depth > 0) {
        if (cfg_next(p) != 0) return -1;
        if (p->event.type == YAML_SEQUENCE_START_EVENT)
            depth++;
        else if (p->event.type == YAML_SEQUENCE_END_EVENT)
            depth--;
        cfg_event_done(p);
    }
    return 0;
}

static int cfg_skip_value(cfg_parse *p) {
    if (cfg_next(p) != 0) return -1;
    switch (p->event.type) {
    case YAML_SCALAR_EVENT:
        cfg_event_done(p);
        return 0;
    case YAML_MAPPING_START_EVENT:
        cfg_event_done(p);
        return cfg_skip_mapping(p);
    case YAML_SEQUENCE_START_EVENT:
        cfg_event_done(p);
        return cfg_skip_sequence(p);
    default:
        snprintf(p->error_msg, sizeof(p->error_msg), "Expected scalar, mapping, or sequence value");
        p->error_code = EXIT_CONFIG_ERR;
        return -1;
    }
}

static int cfg_read_scalar(cfg_parse *p, char **out) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) return -1;
    const char *v = cfg_scalar(p);
    if (!utf8_validate_c_string(v)) {
        snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in scalar");
        p->error_code = EXIT_CONFIG_ERR;
        return -1;
    }
    *out = util_strdup(v);
    cfg_event_done(p);
    return 0;
}

/* Parse a mapping into char ** array (key=value strings, NULL-terminated) */
static int cfg_parse_headers(cfg_parse *p, char ***out) {
    int count = 0, cap = 0;
    char **arr = NULL;

    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);
        if (!utf8_validate_c_string(key)) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in header key");
            p->error_code = EXIT_CONFIG_ERR;
            goto err;
        }
        size_t klen = strlen(key);

        if (cfg_next(p) != 0) goto err;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *val = cfg_scalar(p);
        if (!utf8_validate_c_string(val)) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in header value");
            p->error_code = EXIT_CONFIG_ERR;
            goto err;
        }
        size_t vlen = strlen(val);

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            char **tmp = realloc(arr, (cap + 1) * sizeof(char *));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            arr = tmp;
        }

        char *entry = malloc(klen + 1 + vlen + 1);
        if (!entry) {
            p->error_code = EXIT_INTERNAL_ERR;
            goto err;
        }
        memcpy(entry, key, klen);
        entry[klen] = '=';
        memcpy(entry + klen + 1, val, vlen + 1);
        arr[count++] = entry;
        arr[count] = NULL;

        cfg_event_done(p);
    }

    cfg_event_done(p);
    *out = arr ? arr : NULL;
    return 0;

err:
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
    *out = NULL;
    return -1;
}

/* Parse a string-to-string mapping (rename/redefine) */
static int cfg_parse_str_map(cfg_parse *p, char ***out) {
    int count = 0, cap = 0;
    char **arr = NULL;

    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);
        if (!utf8_validate_c_string(key)) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in map key");
            p->error_code = EXIT_CONFIG_ERR;
            goto err;
        }
        size_t klen = strlen(key);

        if (cfg_next(p) != 0) goto err;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *val = cfg_scalar(p);
        if (!utf8_validate_c_string(val)) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in map value");
            p->error_code = EXIT_CONFIG_ERR;
            goto err;
        }
        size_t vlen = strlen(val);

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            char **tmp = realloc(arr, (cap + 1) * sizeof(char *));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            arr = tmp;
        }

        char *entry = malloc(klen + 1 + vlen + 1);
        if (!entry) {
            p->error_code = EXIT_INTERNAL_ERR;
            goto err;
        }
        memcpy(entry, key, klen);
        entry[klen] = '=';
        memcpy(entry + klen + 1, val, vlen + 1);
        arr[count++] = entry;
        arr[count] = NULL;

        cfg_event_done(p);
    }

    cfg_event_done(p);
    *out = arr ? arr : NULL;
    return 0;

err:
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
    *out = NULL;
    return -1;
}

/* Parse a string list (whitelist/blacklist) */
static int cfg_parse_str_list(cfg_parse *p, char ***out) {
    int count = 0, cap = 0;
    char **arr = NULL;

    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_SEQUENCE_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_SEQUENCE_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *val = cfg_scalar(p);
        if (!utf8_validate_c_string(val)) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Invalid UTF-8 in list item");
            p->error_code = EXIT_CONFIG_ERR;
            goto err;
        }

        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            char **tmp = realloc(arr, (cap + 1) * sizeof(char *));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            arr = tmp;
        }
        arr[count] = util_strdup(val);
        arr[++count] = NULL;

        cfg_event_done(p);
    }

    cfg_event_done(p);
    *out = arr ? arr : NULL;
    return 0;

err:
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
    *out = NULL;
    return -1;
}

static int mcp_parse_fields(cfg_parse *p, mcp_server_cfg *cfg, bool allow_reference) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->transport = MCP_STDIO;
    cfg->init_timeout_ms = 30000;
    cfg->call_timeout_ms = 600000;
    cfg->call_timeout_beh = TIMEOUT_FAIL;
    cfg->max_reconnect = 3;
    cfg->reconnect_delay_ms = 1000;

    int seen_other = 0; /* any key other than 'name' was present */

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "name") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->name) != 0) goto err;
            if (strlen(cfg->name) == 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "MCP server name must not be empty");
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }

        } else if (strcmp(key, "type") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            if (strcmp(v, "stdio") == 0) {
                cfg->transport = MCP_STDIO;
            } else if (strcmp(v, "http") == 0) {
                cfg->transport = MCP_HTTP;
            } else if (strcmp(v, "sse") == 0) {
                cfg->transport = MCP_SSE;
            } else {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid mcp type '%s'", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            free(v);

        } else if (strcmp(key, "cmdline") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->cmdline) != 0) goto err;

        } else if (strcmp(key, "url") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->url) != 0) goto err;

        } else if (strcmp(key, "headers") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_parse_headers(p, &cfg->headers) != 0) goto err;

        } else if (strcmp(key, "init_timeout") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            int64_t ms = util_parse_duration(v);
            free(v);
            if (ms < 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid init_timeout");
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            cfg->init_timeout_ms = ms;

        } else if (strcmp(key, "call_timeout") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            int64_t ms = util_parse_duration(v);
            free(v);
            if (ms < 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid call_timeout");
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            cfg->call_timeout_ms = ms;

        } else if (strcmp(key, "call_timeout_behavior") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            if (strcmp(v, "fail") == 0) {
                cfg->call_timeout_beh = TIMEOUT_FAIL;
            } else if (strcmp(v, "continue") == 0) {
                cfg->call_timeout_beh = TIMEOUT_CONTINUE;
            } else {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid call_timeout_behavior '%s'",
                         v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            free(v);

        } else if (strcmp(key, "hide") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) {
                cfg->hide = true;
            } else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0) {
                cfg->hide = false;
            } else {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid boolean '%s' for hide", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            free(v);

        } else if (strcmp(key, "namespace") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->namespace) != 0) goto err;

        } else if (strcmp(key, "rename") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_parse_str_map(p, &cfg->rename_keys) != 0) goto err;

        } else if (strcmp(key, "redefine") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_parse_str_map(p, &cfg->redefine_keys) != 0) goto err;

        } else if (strcmp(key, "whitelist") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_parse_str_list(p, &cfg->whitelist) != 0) goto err;

        } else if (strcmp(key, "blacklist") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            if (cfg_parse_str_list(p, &cfg->blacklist) != 0) goto err;

        } else if (strcmp(key, "max_reconnect") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            char *end;
            long n = strtol(v, &end, 10);
            if (end == v || *end != '\0' || n < 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid max_reconnect '%s'", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            cfg->max_reconnect = (int)n;
            free(v);

        } else if (strcmp(key, "reconnect_delay") == 0) {
            seen_other = 1;
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            int64_t ms = util_parse_duration(v);
            free(v);
            if (ms < 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "Invalid reconnect_delay");
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            cfg->reconnect_delay_ms = ms;

        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }

    cfg_event_done(p);

    if (!cfg->name) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "MCP server entry missing required 'name' field");
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }

    /* Subagent reference entry: only a 'name' was given. The specification
     * is resolved against the top-level 'mcps' list (validated after the
     * whole document is parsed), so skip all per-entry requirements. */
    if (allow_reference && !seen_other) {
        cfg->reference = true;
        return 0;
    }

    if (cfg->transport == MCP_STDIO && !cfg->cmdline) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "MCP server '%s' of type stdio missing required 'cmdline' field", cfg->name);
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }

    if ((cfg->transport == MCP_HTTP || cfg->transport == MCP_SSE) && !cfg->url) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "MCP server '%s' of type %s missing required 'url' field", cfg->name,
                 cfg->transport == MCP_HTTP ? "http" : "sse");
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }

    if (!cfg->namespace) {
        cfg->namespace = util_strdup(cfg->name);
    }

    return 0;

err:
    if (cfg->name) free(cfg->name);
    if (cfg->cmdline) free(cfg->cmdline);
    if (cfg->url) free(cfg->url);
    if (cfg->namespace) free(cfg->namespace);
    if (cfg->headers) {
        for (int i = 0; cfg->headers[i]; i++) free(cfg->headers[i]);
        free(cfg->headers);
    }
    if (cfg->rename_keys) {
        for (int i = 0; cfg->rename_keys[i]; i++) free(cfg->rename_keys[i]);
        free(cfg->rename_keys);
    }
    if (cfg->redefine_keys) {
        for (int i = 0; cfg->redefine_keys[i]; i++) free(cfg->redefine_keys[i]);
        free(cfg->redefine_keys);
    }
    if (cfg->whitelist) {
        for (int i = 0; cfg->whitelist[i]; i++) free(cfg->whitelist[i]);
        free(cfg->whitelist);
    }
    if (cfg->blacklist) {
        for (int i = 0; cfg->blacklist[i]; i++) free(cfg->blacklist[i]);
        free(cfg->blacklist);
    }
    memset(cfg, 0, sizeof(*cfg));
    return -1;
}

static int cfg_parse_mcps(cfg_parse *p, runtime_ctx *ctx) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_SEQUENCE_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    int cap = 0;
    ctx->mcp_count = 0;
    ctx->mcps = NULL;

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_SEQUENCE_END_EVENT) break;

        if (p->event.type != YAML_MAPPING_START_EVENT) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Expected mapping for MCP server entry");
            p->error_code = EXIT_CONFIG_ERR;
            cfg_event_done(p);
            goto err;
        }
        cfg_event_done(p);

        if (ctx->mcp_count >= cap) {
            cap = cap ? cap * 2 : 8;
            mcp_server_cfg *tmp = realloc(ctx->mcps, cap * sizeof(mcp_server_cfg));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            ctx->mcps = tmp;
        }

        if (mcp_parse_fields(p, &ctx->mcps[ctx->mcp_count], false) != 0) goto err;
        ctx->mcp_count++;
    }

    cfg_event_done(p);

    if (ctx->mcp_count == 0) {
        snprintf(p->error_msg, sizeof(p->error_msg), "'mcps' must contain at least one server");
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }

    return 0;

err:
    for (int i = 0; i < ctx->mcp_count; i++) config_free_mcp(&ctx->mcps[i]);
    free(ctx->mcps);
    ctx->mcps = NULL;
    ctx->mcp_count = 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  subagents                                                          */
/* ------------------------------------------------------------------ */

static int cfg_parse_subagents(cfg_parse *p, runtime_ctx *ctx, subagent_spec **out, int *out_count,
                               int depth);

/* Read a boolean scalar ("true"/"yes" or "false"/"no"). Returns 0 on success
 * with *out set, or -1 with p->error set. Declared here for the subagent
 * parsers below. */
static int cfg_read_bool(cfg_parse *p, int *out);

/* Free a subagent tool definition. */
static void cfg_free_subagent_tool(subagent_tool_def *tool) {
    if (!tool) return;
    free(tool->name);
    free(tool->description);
    if (tool->attributes) {
        for (int i = 0; i < tool->attribute_count; i++) {
            free(tool->attributes[i].name);
            free(tool->attributes[i].type);
            free(tool->attributes[i].description);
        }
        free(tool->attributes);
    }
    memset(tool, 0, sizeof(*tool));
}

/* Free an array of subagent specs, recursively. */
static void cfg_free_subagents(subagent_spec *arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) {
        cfg_free_subagent_tool(&arr[i].tool);
        free(arr[i].system_prompt);
        free(arr[i].user_prompt);
        for (int j = 0; j < arr[i].mcp_count; j++) config_free_mcp(&arr[i].mcps[j]);
        free(arr[i].mcps);
        /* Recursive call frees the nested array itself. */
        cfg_free_subagents(arr[i].subagents, arr[i].subagent_count);
    }
    free(arr);
}

/* Validate a subagent attribute JSON schema type. */
static bool cfg_subagent_attr_type_valid(const char *t) {
    if (strcmp(t, "string") == 0) return true;
    if (strcmp(t, "integer") == 0) return true;
    if (strcmp(t, "number") == 0) return true;
    if (strcmp(t, "boolean") == 0) return true;
    if (strcmp(t, "array") == 0) return true;
    if (strcmp(t, "object") == 0) return true;
    return false;
}

/* Parse the value of one tool_definition.attributes.<name> entry: a mapping
 * with optional type (default "string"), description and required
 * (default true). name must stay valid for the duration of the call; it is
 * duplicated into the attr. */
static int cfg_parse_subagent_attr(cfg_parse *p, subagent_attr *attr, const char *name) {
    memset(attr, 0, sizeof(*attr));
    attr->name = util_strdup(name);
    attr->required = 1;

    if (attr->name == NULL) {
        p->error_code = EXIT_INTERNAL_ERR;
        return -1;
    }

    if (cfg_next(p) != 0) goto err;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) {
        cfg_event_done(p);
        goto err;
    }
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "type") == 0) {
            cfg_event_done(p);
            free(attr->type);
            if (cfg_read_scalar(p, &attr->type) != 0) goto err;
        } else if (strcmp(key, "description") == 0) {
            cfg_event_done(p);
            free(attr->description);
            if (cfg_read_scalar(p, &attr->description) != 0) goto err;
        } else if (strcmp(key, "required") == 0) {
            cfg_event_done(p);
            if (cfg_read_bool(p, &attr->required) != 0) goto err;
        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }
    cfg_event_done(p);

    if (attr->type == NULL) attr->type = util_strdup("string");
    if (attr->type == NULL) {
        p->error_code = EXIT_INTERNAL_ERR;
        goto err;
    }
    if (!cfg_subagent_attr_type_valid(attr->type)) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Invalid type '%s' for subagent attribute '%s' "
                 "(string|integer|number|boolean|array|object)",
                 attr->type, attr->name);
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }
    return 0;

err:
    free(attr->name);
    free(attr->type);
    free(attr->description);
    memset(attr, 0, sizeof(*attr));
    return -1;
}

/* Parse a tool_definition mapping (name, description, attributes). */
static int cfg_parse_subagent_tool_def(cfg_parse *p, subagent_tool_def *tool) {
    memset(tool, 0, sizeof(*tool));

    if (cfg_next(p) != 0) goto err;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) {
        cfg_event_done(p);
        goto err;
    }
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "name") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &tool->name) != 0) goto err;
        } else if (strcmp(key, "description") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &tool->description) != 0) goto err;
        } else if (strcmp(key, "attributes") == 0) {
            cfg_event_done(p);

            if (cfg_next(p) != 0) goto err;
            if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) {
                cfg_event_done(p);
                goto err;
            }
            cfg_event_done(p);

            int cap = 0;
            while (1) {
                if (cfg_next(p) != 0) goto err;
                if (p->event.type == YAML_MAPPING_END_EVENT) break;
                if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
                const char *aname = cfg_scalar(p);
                if (!utf8_validate_c_string(aname)) {
                    snprintf(p->error_msg, sizeof(p->error_msg),
                             "Invalid UTF-8 in subagent attribute name");
                    p->error_code = EXIT_CONFIG_ERR;
                    goto err;
                }
                char *name_copy = util_strdup(aname);
                cfg_event_done(p);
                if (name_copy == NULL) {
                    p->error_code = EXIT_INTERNAL_ERR;
                    goto err;
                }

                if (tool->attribute_count >= cap) {
                    cap = cap ? cap * 2 : 4;
                    subagent_attr *tmp =
                        realloc(tool->attributes, (size_t)cap * sizeof(subagent_attr));
                    if (tmp == NULL) {
                        free(name_copy);
                        p->error_code = EXIT_INTERNAL_ERR;
                        goto err;
                    }
                    tool->attributes = tmp;
                }

                int rc =
                    cfg_parse_subagent_attr(p, &tool->attributes[tool->attribute_count], name_copy);
                free(name_copy);
                if (rc != 0) goto err;
                tool->attribute_count++;
            }
            cfg_event_done(p);
        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }
    cfg_event_done(p);

    if (tool->name == NULL || tool->name[0] == '\0') {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "subagents entry missing required 'tool_definition.name'");
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }
    return 0;

err:
    cfg_free_subagent_tool(tool);
    return -1;
}

/* Parse one 'mcps' list of a subagent. Unlike the top-level list it may be
 * empty and entries may be name-only references to top-level servers. */
static int cfg_parse_subagent_mcps(cfg_parse *p, subagent_spec *spec) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_SEQUENCE_START_EVENT) != 0) {
        cfg_event_done(p);
        return -1;
    }
    cfg_event_done(p);

    int cap = 0;

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_SEQUENCE_END_EVENT) break;

        if (p->event.type != YAML_MAPPING_START_EVENT) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Expected mapping for MCP server entry");
            p->error_code = EXIT_CONFIG_ERR;
            cfg_event_done(p);
            goto err;
        }
        cfg_event_done(p);

        if (spec->mcp_count >= cap) {
            cap = cap ? cap * 2 : 4;
            mcp_server_cfg *tmp = realloc(spec->mcps, (size_t)cap * sizeof(mcp_server_cfg));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            spec->mcps = tmp;
        }

        if (mcp_parse_fields(p, &spec->mcps[spec->mcp_count], true) != 0) goto err;
        spec->mcp_count++;
    }

    cfg_event_done(p);
    return 0;

err:
    for (int i = 0; i < spec->mcp_count; i++) config_free_mcp(&spec->mcps[i]);
    free(spec->mcps);
    spec->mcps = NULL;
    spec->mcp_count = 0;
    return -1;
}

/* Parse a single subagent mapping. The caller has already consumed the
 * YAML_MAPPING_START_EVENT. Frees itself on error. */
static int cfg_parse_subagent(cfg_parse *p, runtime_ctx *ctx, subagent_spec *spec, int depth) {
    memset(spec, 0, sizeof(*spec));

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "tool_definition") == 0) {
            cfg_event_done(p);
            if (cfg_parse_subagent_tool_def(p, &spec->tool) != 0) goto err;
        } else if (strcmp(key, "system_prompt") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &spec->system_prompt) != 0) goto err;
        } else if (strcmp(key, "user_prompt") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &spec->user_prompt) != 0) goto err;
        } else if (strcmp(key, "mcps") == 0) {
            cfg_event_done(p);
            if (cfg_parse_subagent_mcps(p, spec) != 0) goto err;
        } else if (strcmp(key, "subagents") == 0) {
            cfg_event_done(p);
            if (cfg_parse_subagents(p, ctx, &spec->subagents, &spec->subagent_count, depth + 1) !=
                0)
                goto err;
        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }
    cfg_event_done(p);

    if (spec->tool.name == NULL) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "subagents entry missing required 'tool_definition' with a 'name'");
        p->error_code = EXIT_CONFIG_ERR;
        goto err;
    }
    return 0;

err:
    cfg_free_subagent_tool(&spec->tool);
    free(spec->system_prompt);
    free(spec->user_prompt);
    for (int i = 0; i < spec->mcp_count; i++) config_free_mcp(&spec->mcps[i]);
    free(spec->mcps);
    cfg_free_subagents(spec->subagents, spec->subagent_count);
    memset(spec, 0, sizeof(*spec));
    return -1;
}

/* Parse a 'subagents' sequence (root or nested). depth is 1-based. */
static int cfg_parse_subagents(cfg_parse *p, runtime_ctx *ctx, subagent_spec **out, int *out_count,
                               int depth) {
    *out = NULL;
    *out_count = 0;

    if (depth > SUBAGENT_MAX_DEPTH) {
        snprintf(p->error_msg, sizeof(p->error_msg), "subagents nested deeper than %d levels",
                 SUBAGENT_MAX_DEPTH);
        p->error_code = EXIT_CONFIG_ERR;
        return -1;
    }

    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_SEQUENCE_START_EVENT) != 0) {
        cfg_event_done(p);
        return -1;
    }
    cfg_event_done(p);

    int count = 0, cap = 0;
    subagent_spec *arr = NULL;

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_SEQUENCE_END_EVENT) break;

        if (p->event.type != YAML_MAPPING_START_EVENT) {
            snprintf(p->error_msg, sizeof(p->error_msg), "Expected mapping for subagent entry");
            p->error_code = EXIT_CONFIG_ERR;
            cfg_event_done(p);
            goto err;
        }
        cfg_event_done(p);

        if (count >= cap) {
            cap = cap ? cap * 2 : 4;
            subagent_spec *tmp = realloc(arr, (size_t)cap * sizeof(subagent_spec));
            if (!tmp) {
                p->error_code = EXIT_INTERNAL_ERR;
                goto err;
            }
            arr = tmp;
        }

        if (cfg_parse_subagent(p, ctx, &arr[count], depth) != 0) goto err;
        count++;

        /* Tool names must be unique among siblings: the parent dispatches a
         * call by name within this list only. */
        for (int i = 0; i < count - 1; i++) {
            if (strcmp(arr[i].tool.name, arr[count - 1].tool.name) == 0) {
                snprintf(p->error_msg, sizeof(p->error_msg), "Duplicate subagent tool name '%s'",
                         arr[count - 1].tool.name);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
        }
    }

    cfg_event_done(p);
    *out = arr;
    *out_count = count;
    return 0;

err:
    cfg_free_subagents(arr, count);
    return -1;
}

static int cfg_parse_llm(cfg_parse *p, runtime_ctx *ctx) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    ctx->llm.model = util_strdup("gpt-4o-mini");

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "api_base") == 0) {
            cfg_event_done(p);
            free(ctx->llm.api_base);
            if (cfg_read_scalar(p, &ctx->llm.api_base) != 0) goto err;

        } else if (strcmp(key, "api_key") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &ctx->llm.api_key) != 0) goto err;

        } else if (strcmp(key, "model") == 0) {
            cfg_event_done(p);
            free(ctx->llm.model);
            if (cfg_read_scalar(p, &ctx->llm.model) != 0) goto err;

        } else if (strcmp(key, "headers") == 0) {
            cfg_event_done(p);
            if (cfg_parse_headers(p, &ctx->llm.headers) != 0) goto err;

        } else if (strcmp(key, "retain_reasoning") == 0) {
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) {
                ctx->llm.retain_reasoning = true;
            } else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0) {
                ctx->llm.retain_reasoning = false;
            } else {
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Invalid boolean '%s' for retain_reasoning", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            free(v);

        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }

    cfg_event_done(p);
    return 0;

err:
    return -1;
}

/* Read a boolean scalar ("true"/"yes" or "false"/"no"). Returns 0 on success
 * with *out set, or -1 with p->error set. */
static int cfg_read_bool(cfg_parse *p, int *out) {
    char *v = NULL;
    if (cfg_read_scalar(p, &v) != 0) return -1;
    int ok = 0;
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) {
        *out = 1;
        ok = 1;
    } else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0) {
        *out = 0;
        ok = 1;
    } else {
        snprintf(p->error_msg, sizeof(p->error_msg), "Invalid boolean '%s'", v);
        p->error_code = EXIT_CONFIG_ERR;
    }
    free(v);
    return ok ? 0 : -1;
}

/* Parse the agent.compact mapping (prefix-cache-aware compaction settings). */
static int cfg_parse_agent_compact(cfg_parse *p, runtime_ctx *ctx) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "enabled") == 0) {
            cfg_event_done(p);
            if (cfg_read_bool(p, &ctx->agent.compact_enabled) != 0) goto err;
        } else if (strcmp(key, "max_tokens") == 0) {
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            char *end = NULL;
            long n = strtol(v, &end, 10);
            if (end == v || *end != '\0' || n <= 0) {
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Invalid integer '%s' for compact.max_tokens", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            ctx->agent.compact_max_tokens = n;
            free(v);
        } else if (strcmp(key, "threshold") == 0) {
            cfg_event_done(p);
            char *v = NULL;
            if (cfg_read_scalar(p, &v) != 0) goto err;
            char *end = NULL;
            double d = strtod(v, &end);
            if (end == v || *end != '\0' || d <= 0.0 || d > 1.0) {
                snprintf(p->error_msg, sizeof(p->error_msg),
                         "Invalid threshold '%s' for compact.threshold (0 < t <= 1)", v);
                free(v);
                p->error_code = EXIT_CONFIG_ERR;
                goto err;
            }
            ctx->agent.compact_threshold = d;
            free(v);
        } else if (strcmp(key, "summarize") == 0) {
            cfg_event_done(p);
            if (cfg_read_bool(p, &ctx->agent.compact_summarize) != 0) goto err;
        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }

    cfg_event_done(p);
    return 0;

err:
    return -1;
}

static int cfg_parse_agent(cfg_parse *p, runtime_ctx *ctx) {
    if (cfg_next(p) != 0) return -1;
    if (cfg_expect(p, YAML_MAPPING_START_EVENT) != 0) return -1;
    cfg_event_done(p);

    while (1) {
        if (cfg_next(p) != 0) goto err;
        if (p->event.type == YAML_MAPPING_END_EVENT) break;
        if (cfg_expect(p, YAML_SCALAR_EVENT) != 0) goto err;
        const char *key = cfg_scalar(p);

        if (strcmp(key, "system_prompt") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &ctx->agent.system_prompt) != 0) goto err;
        } else if (strcmp(key, "compact") == 0) {
            cfg_event_done(p);
            if (cfg_parse_agent_compact(p, ctx) != 0) goto err;
        } else {
            cfg_event_done(p);
            if (cfg_skip_value(p) != 0) goto err;
        }
    }

    cfg_event_done(p);
    return 0;

err:
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Post-parse subagent validation                                     */
/* ------------------------------------------------------------------ */

/* Is name already present in the seen list? */
static bool name_seen(const char **seen, int seen_count, const char *name) {
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i], name) == 0) return true;
    }
    return false;
}

/* Append a name pointer (not copied) to the seen list. Returns 0 or -1 on
 * allocation failure. */
static int seen_append(const char ***seen, int *seen_count, int *seen_cap, const char *name) {
    if (*seen_count >= *seen_cap) {
        *seen_cap = *seen_cap ? *seen_cap * 2 : 8;
        const char **tmp = realloc(*seen, (size_t)*seen_cap * sizeof(char *));
        if (tmp == NULL) return -1;
        *seen = tmp;
    }
    (*seen)[(*seen_count)++] = name;
    return 0;
}

/* Is 'name' defined in the top-level 'mcps' list? */
static bool top_level_mcp_exists(const runtime_ctx *ctx, const char *name) {
    for (int i = 0; i < ctx->mcp_count; i++) {
        if (strcmp(ctx->mcps[i].name, name) == 0) return true;
    }
    return false;
}

/* Validate the subagent tree now that the whole document is parsed:
 *  - reference MCP entries must resolve to a top-level 'mcps' server;
 *  - fully-specified MCP entries must not reuse a name defined anywhere
 *    else in the config (MCP names are the runtime routing key, so a
 *    duplicate would make tool calls ambiguous).
 * seen[] holds every MCP name defined so far (top-level entries first,
 * then full subagent entries in document order). */
static int validate_subagents(const subagent_spec *arr, int count, const runtime_ctx *ctx,
                              const char ***seen, int *seen_count, int *seen_cap, bool *oom) {
    for (int i = 0; i < count; i++) {
        const subagent_spec *s = &arr[i];
        for (int j = 0; j < s->mcp_count; j++) {
            const mcp_server_cfg *m = &s->mcps[j];
            if (m->reference) {
                if (!top_level_mcp_exists(ctx, m->name)) {
                    log_activity("[error] subagent '%s' references MCP server '%s' which is not "
                                 "defined in the top-level 'mcps' list",
                                 s->tool.name, m->name);
                    return -1;
                }
                continue;
            }
            if (name_seen(*seen, *seen_count, m->name)) {
                log_activity("[error] Duplicate MCP server name '%s' (names must be unique "
                             "across the whole config)",
                             m->name);
                return -1;
            }
            if (seen_append(seen, seen_count, seen_cap, m->name) != 0) {
                *oom = true;
                return -1;
            }
        }
        if (validate_subagents(s->subagents, s->subagent_count, ctx, seen, seen_count, seen_cap,
                               oom) != 0)
            return -1;
    }
    return 0;
}

/* Enforce MCP-name uniqueness and subagent reference resolution.
 * Returns EXIT_SUCCESS, EXIT_CONFIG_ERR, or EXIT_INTERNAL_ERR. */
static int validate_mcp_names(const runtime_ctx *ctx) {
    const char **seen = NULL;
    int seen_count = 0, seen_cap = 0;
    bool oom = false;
    int rc = EXIT_SUCCESS;

    for (int i = 0; i < ctx->mcp_count; i++) {
        if (name_seen(seen, seen_count, ctx->mcps[i].name)) {
            log_activity("[error] Duplicate MCP server name '%s' in top-level 'mcps' list",
                         ctx->mcps[i].name);
            rc = EXIT_CONFIG_ERR;
            goto done;
        }
        if (seen_append(&seen, &seen_count, &seen_cap, ctx->mcps[i].name) != 0) {
            rc = EXIT_INTERNAL_ERR;
            goto done;
        }
    }

    if (validate_subagents(ctx->subagents, ctx->subagent_count, ctx, &seen, &seen_count, &seen_cap,
                           &oom) != 0) {
        if (oom) {
            rc = EXIT_INTERNAL_ERR;
        } else {
            rc = EXIT_CONFIG_ERR;
        }
        goto done;
    }

done:
    free(seen);
    return rc;
}

int config_load(const char *path, runtime_ctx *ctx) {
    char *raw = util_read_file(path);
    if (!raw) {
        log_activity("[error] Cannot read config file: %s", path);
        return EXIT_CONFIG_ERR;
    }

    if (!utf8_validate_c_string(raw)) {
        log_activity("[error] Invalid UTF-8 in config file: %s", path);
        free(raw);
        return EXIT_CONFIG_ERR;
    }

    /* Defaults for agent.compact.* (overridable under the agent: section). */
    ctx->agent.compact_enabled = 0;
    ctx->agent.compact_max_tokens = 16384;
    ctx->agent.compact_threshold = 0.8;
    ctx->agent.compact_summarize = 0;

    cfg_parse p;
    memset(&p, 0, sizeof(p));
    p.ctx = ctx;
    p.error_code = EXIT_SUCCESS;

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    p.parser = &parser;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_activity("[error] Cannot open config file: %s", path);
        free(raw);
        yaml_parser_delete(&parser);
        return EXIT_CONFIG_ERR;
    }
    yaml_parser_set_input_file(&parser, fp);

    int result = EXIT_SUCCESS;

    if (cfg_next(&p) != 0) {
        result = p.error_code;
        goto done;
    }
    if (p.event.type != YAML_STREAM_START_EVENT) {
        snprintf(p.error_msg, sizeof(p.error_msg), "Expected YAML stream");
        result = EXIT_CONFIG_ERR;
        cfg_event_done(&p);
        goto done;
    }
    cfg_event_done(&p);

    if (cfg_next(&p) != 0) {
        result = p.error_code;
        goto done;
    }
    if (p.event.type != YAML_DOCUMENT_START_EVENT) {
        snprintf(p.error_msg, sizeof(p.error_msg), "Expected YAML document");
        result = EXIT_CONFIG_ERR;
        cfg_event_done(&p);
        goto done;
    }
    cfg_event_done(&p);

    if (cfg_next(&p) != 0) {
        result = p.error_code;
        goto done;
    }
    if (p.event.type != YAML_MAPPING_START_EVENT) {
        snprintf(p.error_msg, sizeof(p.error_msg), "Root must be a YAML mapping");
        result = EXIT_CONFIG_ERR;
        cfg_event_done(&p);
        goto done;
    }
    cfg_event_done(&p);

    while (1) {
        if (cfg_next(&p) != 0) {
            result = p.error_code;
            goto done;
        }
        if (p.event.type == YAML_MAPPING_END_EVENT) break;

        if (p.event.type != YAML_SCALAR_EVENT) {
            snprintf(p.error_msg, sizeof(p.error_msg), "Expected scalar key in root mapping");
            result = EXIT_CONFIG_ERR;
            cfg_event_done(&p);
            goto done;
        }
        const char *key = cfg_scalar(&p);

        if (strcmp(key, "llm") == 0) {
            cfg_event_done(&p);
            if (cfg_parse_llm(&p, ctx) != 0) {
                result = p.error_code;
                goto done;
            }
        } else if (strcmp(key, "mcps") == 0) {
            cfg_event_done(&p);
            if (cfg_parse_mcps(&p, ctx) != 0) {
                result = p.error_code;
                goto done;
            }
        } else if (strcmp(key, "agent") == 0) {
            cfg_event_done(&p);
            if (cfg_parse_agent(&p, ctx) != 0) {
                result = p.error_code;
                goto done;
            }
        } else if (strcmp(key, "subagents") == 0) {
            cfg_event_done(&p);
            if (cfg_parse_subagents(&p, ctx, &ctx->subagents, &ctx->subagent_count, 1) != 0) {
                result = p.error_code;
                goto done;
            }
        } else {
            cfg_event_done(&p);
            if (cfg_skip_value(&p) != 0) {
                result = p.error_code;
                goto done;
            }
        }
    }
    cfg_event_done(&p);

    /* Whole document parsed: validate MCP names and subagent references.
     * MCP names are the runtime routing key, so duplicates anywhere in the
     * config (top level or any subagent list) are rejected. */
    result = validate_mcp_names(ctx);
    if (result != EXIT_SUCCESS) goto done;

    if (cfg_next(&p) != 0) {
        result = p.error_code;
        goto done;
    }
    cfg_event_done(&p);

    if (cfg_next(&p) != 0) {
        result = p.error_code;
        goto done;
    }
    cfg_event_done(&p);

done:
    fclose(fp);
    yaml_parser_delete(&parser);

    if (result != EXIT_SUCCESS && p.error_msg[0]) {
        log_activity("[error] %s", p.error_msg);
    }

    free(raw);
    return result;
}

void config_free_mcp(mcp_server_cfg *cfg) {
    if (!cfg) return;
    free(cfg->name);
    free(cfg->cmdline);
    free(cfg->url);
    free(cfg->namespace);
    if (cfg->headers) {
        for (int i = 0; cfg->headers[i]; i++) free(cfg->headers[i]);
        free(cfg->headers);
    }
    if (cfg->rename_keys) {
        for (int i = 0; cfg->rename_keys[i]; i++) free(cfg->rename_keys[i]);
        free(cfg->rename_keys);
    }
    if (cfg->redefine_keys) {
        for (int i = 0; cfg->redefine_keys[i]; i++) free(cfg->redefine_keys[i]);
        free(cfg->redefine_keys);
    }
    if (cfg->whitelist) {
        for (int i = 0; cfg->whitelist[i]; i++) free(cfg->whitelist[i]);
        free(cfg->whitelist);
    }
    if (cfg->blacklist) {
        for (int i = 0; cfg->blacklist[i]; i++) free(cfg->blacklist[i]);
        free(cfg->blacklist);
    }
}

void config_free(runtime_ctx *ctx) {
    if (!ctx) return;
    free(ctx->llm.api_base);
    free(ctx->llm.api_key);
    free(ctx->llm.model);
    if (ctx->llm.headers) {
        for (int i = 0; ctx->llm.headers[i]; i++) free(ctx->llm.headers[i]);
        free(ctx->llm.headers);
    }
    for (int i = 0; i < ctx->mcp_count; i++) {
        config_free_mcp(&ctx->mcps[i]);
    }
    free(ctx->mcps);
    free(ctx->agent.system_prompt);
    cfg_free_subagents(ctx->subagents, ctx->subagent_count);
    memset(ctx, 0, sizeof(*ctx));
}
