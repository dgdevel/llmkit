#include "srv.h"
#include "platform.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cJSON.h>

/* ------------------------------------------------------------------ */
/*  JSON-RPC response builders                                         */
/* ------------------------------------------------------------------ */

char *srv_build_response(const cJSON *id_node, cJSON *result, const char *error_msg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    /* Preserve the request id verbatim (number, string, or null). Per
     * JSON-RPC 2.0 the response MUST echo the request id. */
    if (id_node != NULL) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id_node, 1));
    } else {
        cJSON_AddNullToObject(root, "id");
    }
    if (error_msg != NULL) {
        cJSON *err = cJSON_CreateObject();
        if (err != NULL) {
            cJSON_AddNumberToObject(err, "code", -32000);
            cJSON_AddStringToObject(err, "message", error_msg);
            cJSON_AddItemToObject(root, "error", err);
        }
    } else if (result != NULL) {
        cJSON_AddItemToObject(root, "result", result);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *srv_build_success(const cJSON *id_node, const char *result_json) {
    cJSON *result = cJSON_Parse(result_json ? result_json : "{}");
    if (result == NULL) result = cJSON_CreateObject();
    return srv_build_response(id_node, result, NULL);
}

char *srv_build_error(const cJSON *id_node, const char *msg) {
    return srv_build_response(id_node, NULL, msg ? msg : "Internal error");
}

char *srv_rewrite_response_id(const cJSON *id_node, char *backend_resp) {
    if (backend_resp == NULL) return NULL;
    cJSON *resp = cJSON_Parse(backend_resp);
    if (resp == NULL) {
        /* Not valid JSON; leave the raw response as-is. */
        return backend_resp;
    }
    cJSON *existing = cJSON_GetObjectItem(resp, "id");
    if (existing != NULL) {
        cJSON_ReplaceItemInObject(resp, "id", cJSON_Duplicate(id_node, 1));
    } else {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id_node, 1));
    }
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    free(backend_resp);
    return out;
}

char *srv_id_to_str(const cJSON *id_node) {
    if (id_node == NULL) return NULL;
    if (cJSON_IsString(id_node)) return util_strdup(id_node->valuestring);
    if (cJSON_IsNumber(id_node)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", id_node->valuedouble);
        return util_strdup(buf);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Namespace / filter helpers                                         */
/* ------------------------------------------------------------------ */

const char *srv_get_ns(const mcp_server_cfg *cfg) {
    return (cfg->namespace != NULL && cfg->namespace[0]) ? cfg->namespace : cfg->name;
}

/* Check if a namespaced name starts with a given namespace prefix + dot. */
static int has_namespace(const char *name, const char *ns) {
    size_t nlen = strlen(ns);
    return (strncmp(name, ns, nlen) == 0 && name[nlen] == '.');
}

const char *srv_local_name(const char *name, const char *ns) {
    if (!has_namespace(name, ns)) return NULL;
    return name + strlen(ns) + 1;
}

int srv_check_filters(const mcp_server_cfg *cfg, const char *namespaced_name) {
    /* Whitelist: if non-empty, only items in the list pass. */
    if (cfg->whitelist != NULL && cfg->whitelist[0] != NULL) {
        bool found = false;
        for (int i = 0; cfg->whitelist[i] != NULL; i++) {
            if (strcmp(cfg->whitelist[i], namespaced_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return 0;
    }
    /* Blacklist: if non-empty, items in the list are excluded. */
    if (cfg->blacklist != NULL && cfg->blacklist[0] != NULL) {
        for (int i = 0; cfg->blacklist[i] != NULL; i++) {
            if (strcmp(cfg->blacklist[i], namespaced_name) == 0) return 0;
        }
    }
    return 1;
}

void srv_apply_rename(const mcp_server_cfg *cfg, const char *namespaced_name, const char **name) {
    if (cfg->rename_keys == NULL) return;
    for (int i = 0; cfg->rename_keys[i] != NULL; i++) {
        const char *eq = strchr(cfg->rename_keys[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg->rename_keys[i]);
        if (strlen(namespaced_name) == klen &&
            strncmp(cfg->rename_keys[i], namespaced_name, klen) == 0) {
            *name = eq + 1;
            return;
        }
    }
}

void srv_apply_redefine(const mcp_server_cfg *cfg, const char *namespaced_name, const char **desc) {
    if (cfg->redefine_keys == NULL) return;
    for (int i = 0; cfg->redefine_keys[i] != NULL; i++) {
        const char *eq = strchr(cfg->redefine_keys[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg->redefine_keys[i]);
        if (strlen(namespaced_name) == klen &&
            strncmp(cfg->redefine_keys[i], namespaced_name, klen) == 0) {
            *desc = eq + 1;
            return;
        }
    }
}

char *srv_make_namespaced(const char *ns, const char *original) {
    size_t ns_len = strlen(ns);
    size_t on_len = strlen(original);
    char *out = malloc(ns_len + 1 + on_len + 1);
    if (out == NULL) {
        log_activity("[error] OOM");
        exit(EXIT_INTERNAL_ERR);
    }
    memcpy(out, ns, ns_len);
    out[ns_len] = '.';
    memcpy(out + ns_len + 1, original, on_len);
    out[ns_len + 1 + on_len] = '\0';
    return out;
}

/* Cap on HTTP request bodies accepted from the network; larger requests
 * are rejected before any allocation (DoS mitigation). */
#define MAX_HTTP_BODY (16 * 1024 * 1024)

/* Receive timeout per accepted HTTP client; stalled senders are dropped. */
#define HTTP_CLIENT_READ_TIMEOUT_MS 30000

/* ------------------------------------------------------------------ */
/*  stdio serve loop                                                   */
/* ------------------------------------------------------------------ */

static int srv_loop_stdio(runtime_ctx *ctx, srv_handler_fn handler) {
    /* Growable line buffer: a JSON-RPC request larger than 64 KiB must stay
     * on one logical line or the handler cannot parse it. */
    size_t cap = 65536;
    char *line = malloc(cap);
    if (line == NULL) return EXIT_INTERNAL_ERR;

    while (fgets(line, cap, stdin) != NULL) {
        /* If the buffer was filled without a newline, keep reading into a
         * larger buffer (up to MAX_HTTP_BODY) until the line ends. */
        size_t len = strlen(line);
        while (len == cap - 1 && line[len - 1] != '\n') {
            if (cap >= MAX_HTTP_BODY) {
                log_activity("[error] stdio request line too large (>%zu bytes)", cap - 1);
                free(line);
                return EXIT_MCP_ERR;
            }
            cap *= 2;
            if (cap > MAX_HTTP_BODY) cap = MAX_HTTP_BODY + 1;
            char *tmp = realloc(line, cap);
            if (tmp == NULL) {
                free(line);
                return EXIT_INTERNAL_ERR;
            }
            line = tmp;
            if (fgets(line + len, cap - len, stdin) == NULL) break;
            len = strlen(line);
        }

        /* Trim trailing newline/whitespace. */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len == 0) continue;

        char *resp = NULL;
        int rc = handler(ctx, line, &resp);
        if (rc != EXIT_SUCCESS) {
            free(line);
            return rc;
        }

        if (resp != NULL && resp[0] != '\0') {
            fprintf(stdout, "%s\n", resp);
            fflush(stdout);
        }
        free(resp);
    }
    free(line);
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  HTTP serve helpers                                                 */
/* ------------------------------------------------------------------ */

/* Parse "host:port" into host string and port. Returns 0 on success. */
static int parse_listen_addr(const char *addr, char *host_out, int host_size, int *port_out) {
    if (addr == NULL) return -1;
    const char *colon = strrchr(addr, ':');
    if (colon == NULL) return -1;

    size_t host_len = (size_t)(colon - addr);
    if (host_len >= (size_t)host_size) host_len = (size_t)(host_size - 1);
    memcpy(host_out, addr, host_len);
    host_out[host_len] = '\0';

    char *end = NULL;
    long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || p <= 0 || p > 65535) return -1;
    *port_out = (int)p;
    return 0;
}

/* Read an HTTP request body from a socket FILE*. Returns EXIT_SUCCESS
 * or an error code.  This is a minimal HTTP parser. */
static int read_http_request(FILE *client, char **out_body) {
    *out_body = NULL;

    /* Read headers until \r\n\r\n or \n\n. */
    char header_buf[8192];
    size_t hdr_len = 0;
    int blank_line = 0;

    while (fgets(header_buf + hdr_len, (int)(sizeof(header_buf) - hdr_len), client) != NULL) {
        size_t chunk = strlen(header_buf + hdr_len);
        hdr_len += chunk;

        if (hdr_len >= 2 && memcmp(header_buf + hdr_len - 2, "\n\n", 2) == 0) {
            blank_line = 1;
            break;
        }
        if (hdr_len >= 4 && memcmp(header_buf + hdr_len - 4, "\r\n\r\n", 4) == 0) {
            blank_line = 1;
            break;
        }
        if (hdr_len >= sizeof(header_buf) - 1) break;
    }

    if (!blank_line) return EXIT_MCP_ERR;

    /* Parse Content-Length from headers. */
    long content_length = 0;
    {
        /* Simple search for Content-Length: */
        const char *cl = strstr(header_buf, "Content-Length:");
        if (cl == NULL) cl = strstr(header_buf, "content-length:");
        if (cl != NULL) {
            cl += 15; /* skip past "Content-Length:" */
            while (*cl == ' ' || *cl == '\t') cl++;
            char *end = NULL;
            long parsed = strtol(cl, &end, 10);
            if (end == cl || parsed < 0) {
                content_length = 0;
            } else {
                content_length = parsed;
            }
        }
    }

    if (content_length <= 0) return EXIT_SUCCESS;

    /* Reject oversized bodies: 16 MiB cap. */
    if (content_length > (long)MAX_HTTP_BODY) {
        log_activity("[error] HTTP request body too large: %ld bytes", content_length);
        return EXIT_MCP_ERR;
    }

    /* Read body. */
    *out_body = malloc((size_t)content_length + 1);
    if (*out_body == NULL) return EXIT_INTERNAL_ERR;

    size_t total = 0;
    while (total < (size_t)content_length) {
        size_t n = fread(*out_body + total, 1, (size_t)(content_length - total), client);
        if (n == 0) break;
        total += n;
    }
    (*out_body)[total] = '\0';
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  HTTP serve loop                                                    */
/* ------------------------------------------------------------------ */

static int srv_loop_http(runtime_ctx *ctx, srv_handler_fn handler, const char *addr,
                         const char *server_label) {
    char host[256];
    int port = 0;

    if (parse_listen_addr(addr, host, sizeof(host), &port) != 0) {
        log_activity("[error] Invalid listen address: %s", addr);
        return EXIT_ARGS_ERR;
    }

    int listen_fd = platform_tcp_listen(host, port);
    if (listen_fd < 0) {
        log_activity("[error] Failed to bind to %s:%d", host, port);
        return EXIT_FILE_ERR; /* server spec: 3 = Server error (bind failure) */
    }

    log_activity("[init] %s listening on %s:%d", server_label, host, port);

    while (1) {
        int client_fd = platform_tcp_accept(listen_fd, -1);
        if (client_fd < 0) {
            log_activity("[error] Accept failed");
            continue;
        }

        /* Bound the time a single client can stall the loop: drop
         * connections that stop sending mid-request (slowloris). */
        platform_socket_set_read_timeout(client_fd, HTTP_CLIENT_READ_TIMEOUT_MS);

        FILE *client = fdopen(client_fd, "r+");
        if (client == NULL) {
            close(client_fd);
            continue;
        }

        char *body = NULL;
        int rc = read_http_request(client, &body);
        if (rc != EXIT_SUCCESS || body == NULL) {
            free(body);
            fclose(client);
            continue;
        }

        char *resp = NULL;
        int hrc = handler(ctx, body, &resp);
        if (hrc != EXIT_SUCCESS) {
            log_activity("[error] %s handler failed (code %d) for one request", server_label, hrc);
        }
        free(body);

        /* Write HTTP response. */
        if (resp != NULL && resp[0] != '\0') {
            fprintf(client,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    strlen(resp), resp);
        } else {
            fprintf(client, "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: 2\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{}");
        }
        fflush(client);
        free(resp);
        fclose(client);
    }

    /* unreachable */
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  srv_serve                                                          */
/* ------------------------------------------------------------------ */

int srv_serve(runtime_ctx *ctx, const char *listen_addr, srv_handler_fn handler,
              const char *server_label) {
    if (ctx == NULL || handler == NULL) return EXIT_INTERNAL_ERR;

    if (listen_addr != NULL && listen_addr[0] != '\0') {
        return srv_loop_http(ctx, handler, listen_addr, server_label);
    }
    return srv_loop_stdio(ctx, handler);
}
