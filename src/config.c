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

static int mcp_parse_fields(cfg_parse *p, mcp_server_cfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->transport = MCP_STDIO;
    cfg->init_timeout_ms = 30000;
    cfg->call_timeout_ms = 600000;
    cfg->call_timeout_beh = TIMEOUT_FAIL;
    cfg->max_reconnect = 3;
    cfg->reconnect_delay_ms = 1000;

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
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->cmdline) != 0) goto err;

        } else if (strcmp(key, "url") == 0) {
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->url) != 0) goto err;

        } else if (strcmp(key, "headers") == 0) {
            cfg_event_done(p);
            if (cfg_parse_headers(p, &cfg->headers) != 0) goto err;

        } else if (strcmp(key, "init_timeout") == 0) {
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
            cfg_event_done(p);
            if (cfg_read_scalar(p, &cfg->namespace) != 0) goto err;

        } else if (strcmp(key, "rename") == 0) {
            cfg_event_done(p);
            if (cfg_parse_str_map(p, &cfg->rename_keys) != 0) goto err;

        } else if (strcmp(key, "redefine") == 0) {
            cfg_event_done(p);
            if (cfg_parse_str_map(p, &cfg->redefine_keys) != 0) goto err;

        } else if (strcmp(key, "whitelist") == 0) {
            cfg_event_done(p);
            if (cfg_parse_str_list(p, &cfg->whitelist) != 0) goto err;

        } else if (strcmp(key, "blacklist") == 0) {
            cfg_event_done(p);
            if (cfg_parse_str_list(p, &cfg->blacklist) != 0) goto err;

        } else if (strcmp(key, "max_reconnect") == 0) {
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

        if (mcp_parse_fields(p, &ctx->mcps[ctx->mcp_count]) != 0) goto err;
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
        } else {
            cfg_event_done(&p);
            if (cfg_skip_value(&p) != 0) {
                result = p.error_code;
                goto done;
            }
        }
    }
    cfg_event_done(&p);

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
    memset(ctx, 0, sizeof(*ctx));
}
