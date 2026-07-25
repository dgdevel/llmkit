#include "mcp_transport.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ shared helpers ============================ */

/* Growing null-terminated byte buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} growbuf;

static void growbuf_append(growbuf *gb, const char *src, size_t n) {
    if (n == 0) return;
    size_t need = gb->len + n + 1;
    if (need > gb->cap) {
        size_t newcap = gb->cap ? gb->cap : 256;
        while (newcap < need) newcap *= 2;
        char *tmp = realloc(gb->data, newcap);
        if (tmp == NULL) {
            log_activity("[error] OOM in growbuf");
            exit(EXIT_INTERNAL_ERR);
        }
        gb->data = tmp;
        gb->cap = newcap;
    }
    memcpy(gb->data + gb->len, src, n);
    gb->len += n;
    gb->data[gb->len] = '\0';
}

static void growbuf_free(growbuf *gb) {
    free(gb->data);
    gb->data = NULL;
    gb->len = 0;
    gb->cap = 0;
}

/* curl WRITEFUNCTION backed by a growbuf. */
static size_t curl_write_growbuf(char *ptr, size_t size, size_t nmemb, void *userdata) {
    growbuf *gb = userdata;
    size_t n = size * nmemb;
    growbuf_append(gb, ptr, n);
    return n;
}

/* Write all bytes to a pipe, looping over partial writes. */
static int pipe_write_all(platform_pipe *p, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = platform_pipe_write(p, data + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read one newline-terminated message from a pipe into a malloc'd string
 * (excluding the newline). Returns 0 on success, -1 on timeout/EOF/error. */
static int pipe_read_line(platform_pipe *p, int64_t timeout_ms, char **out) {
    growbuf gb = {0};
    char chunk[2048];
    int rc = -1;
    int64_t start = platform_now_ms();

    while (1) {
        int64_t remaining = timeout_ms - (platform_now_ms() - start);
        if (remaining <= 0) break;
        int n = platform_pipe_read(p, chunk, sizeof(chunk), remaining);
        if (n <= 0) break;

        int i = 0;
        while (i < n) {
            if (chunk[i] == '\n') {
                rc = 0;
                goto done;
            }
            int j = i;
            while (j < n && chunk[j] != '\n') j++;
            growbuf_append(&gb, chunk + i, (size_t)(j - i));
            i = j;
        }
    }

done:
    if (rc != 0) {
        growbuf_free(&gb);
        *out = NULL;
        return -1;
    }
    *out = (gb.data != NULL) ? gb.data : util_strdup("");
    return 0;
}

/* Convert config headers ("Key=Value" entries) to a curl slist ("Key: Value").
 * Appends to `base` (may be NULL) and returns the resulting list. */
static struct curl_slist *headers_to_slist(char **cfg_headers, struct curl_slist *base) {
    struct curl_slist *list = base;
    if (cfg_headers == NULL) return list;
    for (int i = 0; cfg_headers[i] != NULL; i++) {
        const char *eq = strchr(cfg_headers[i], '=');
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - cfg_headers[i]);
        const char *val = eq + 1;
        size_t vlen = strlen(val);
        char *h = malloc(klen + 2 + vlen + 1);
        if (h == NULL) {
            log_activity("[error] OOM building headers");
            exit(EXIT_INTERNAL_ERR);
        }
        memcpy(h, cfg_headers[i], klen);
        h[klen] = ':';
        h[klen + 1] = ' ';
        memcpy(h + klen + 2, val, vlen + 1);
        list = curl_slist_append(list, h);
        free(h);
    }
    return list;
}

/* Case-insensitive prefix check. */
static int istarts_with(const char *s, const char *prefix) {
    while (*prefix) {
        char a = *s;
        char b = *prefix;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* Locate the first occurrence of needle in hay[0..hay_len). */
static const char *memfind(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || needle_len > hay_len) return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return hay + i;
    }
    return NULL;
}

/* Call curl_global_init once per process. */
static void ensure_curl_init(void) {
    static int done = 0;
    if (!done) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        done = 1;
    }
}

/* ============================ stdio transport ============================ */

static int transport_stdio_open(mcp_server_cfg *cfg, mcp_connection *conn) {
    if (cfg->cmdline == NULL) {
        log_activity("[error] stdio MCP server '%s' has no cmdline", cfg->name);
        return EXIT_MCP_ERR;
    }
    if (platform_process_spawn(cfg->cmdline, &conn->proc, &conn->pipe_in, &conn->pipe_out) != 0) {
        log_activity("[error] failed to spawn MCP server: %s", cfg->cmdline);
        return EXIT_MCP_ERR;
    }
    conn->proc_spawned = true;
    return EXIT_SUCCESS;
}

static int transport_stdio_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms,
                                char **out_resp) {
    *out_resp = NULL;
    if (pipe_write_all(&conn->pipe_in, req_json, strlen(req_json)) != 0) return EXIT_MCP_ERR;
    if (pipe_write_all(&conn->pipe_in, "\n", 1) != 0) return EXIT_MCP_ERR;

    char *line = NULL;
    if (pipe_read_line(&conn->pipe_out, timeout_ms, &line) != 0) {
        log_activity("[error] no response from stdio MCP server '%s'", conn->cfg->name);
        return EXIT_MCP_ERR;
    }
    *out_resp = line;
    return EXIT_SUCCESS;
}

static void transport_stdio_close(mcp_connection *conn) {
    if (!conn->proc_spawned) return;
    conn->proc_spawned = false;
    platform_pipe_close(&conn->pipe_in); /* signal EOF to child stdin */
    if (platform_process_wait(&conn->proc, 1000) != 0) {
        platform_process_kill(&conn->proc);
        platform_process_wait(&conn->proc, 5000);
    }
    platform_pipe_close(&conn->pipe_out);
    platform_process_close(&conn->proc);
}

/* ============================ http transport ============================ */

/* HEADERFUNCTION: capture Mcp-Session-Id from the response. */
static size_t http_capture_session(char *buffer, size_t size, size_t nitems, void *userdata) {
    mcp_connection *conn = userdata;
    size_t total = size * nitems;
    const char *key = "MCP-Session-Id:";
    size_t klen = strlen(key);
    if (total > klen && istarts_with(buffer, key)) {
        const char *v = buffer + klen;
        size_t vlen = total - klen;
        while (vlen > 0 && (*v == ' ' || *v == '\t')) {
            v++;
            vlen--;
        }
        while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' ')) {
            vlen--;
        }
        free(conn->session_id);
        if (vlen == 0) {
            conn->session_id = NULL;
        } else {
            conn->session_id = malloc(vlen + 1);
            if (conn->session_id == NULL) {
                log_activity("[error] OOM capturing session id");
                exit(EXIT_INTERNAL_ERR);
            }
            memcpy(conn->session_id, v, vlen);
            conn->session_id[vlen] = '\0';
        }
    }
    return total;
}

static int transport_http_open(mcp_server_cfg *cfg, mcp_connection *conn) {
    if (cfg->url == NULL) {
        log_activity("[error] http MCP server '%s' has no url", cfg->name);
        return EXIT_MCP_ERR;
    }
    conn->base_url = util_strdup(cfg->url);
    return EXIT_SUCCESS;
}

static int transport_http_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms,
                               char **out_resp) {
    *out_resp = NULL;
    ensure_curl_init();
    if (conn->curl == NULL) {
        conn->curl = curl_easy_init();
        if (conn->curl == NULL) return EXIT_INTERNAL_ERR;
    }
    CURL *c = conn->curl;
    curl_easy_reset(c);

    growbuf gb = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    if (conn->session_id != NULL) {
        char buf[512];
        snprintf(buf, sizeof(buf), "MCP-Session-Id: %s", conn->session_id);
        headers = curl_slist_append(headers, buf);
    }
    headers = headers_to_slist(conn->cfg->headers, headers);

    curl_easy_setopt(c, CURLOPT_URL, conn->base_url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req_json);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(req_json));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_growbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &gb);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, http_capture_session);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, conn);
    if (timeout_ms > 0) {
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    }

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        log_activity("[error] HTTP MCP request failed: %s", curl_easy_strerror(rc));
        growbuf_free(&gb);
        return EXIT_MCP_ERR;
    }
    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        log_activity("[error] MCP server '%s' returned HTTP %ld", conn->cfg->name, http_code);
        growbuf_free(&gb);
        return EXIT_MCP_ERR;
    }
    *out_resp = (gb.data != NULL) ? gb.data : util_strdup("");
    return EXIT_SUCCESS;
}

static void transport_http_close(mcp_connection *conn) {
    if (conn->curl != NULL) {
        curl_easy_cleanup(conn->curl);
        conn->curl = NULL;
    }
    free(conn->base_url);
    conn->base_url = NULL;
    free(conn->session_id);
    conn->session_id = NULL;
}

/* ============================ sse transport ============================ */

static void sse_buf_append(mcp_connection *conn, const char *src, size_t n) {
    if (n == 0) return;
    size_t need = conn->sse_buf_len + n + 1;
    if (need > conn->sse_buf_cap) {
        size_t newcap = conn->sse_buf_cap ? conn->sse_buf_cap : 1024;
        while (newcap < need) newcap *= 2;
        char *tmp = realloc(conn->sse_buf, newcap);
        if (tmp == NULL) {
            log_activity("[error] OOM in sse buffer");
            exit(EXIT_INTERNAL_ERR);
        }
        conn->sse_buf = tmp;
        conn->sse_buf_cap = newcap;
    }
    memcpy(conn->sse_buf + conn->sse_buf_len, src, n);
    conn->sse_buf_len += n;
    conn->sse_buf[conn->sse_buf_len] = '\0';
}

/* curl WRITEFUNCTION for the SSE GET stream: append raw bytes to conn->sse_buf. */
static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    mcp_connection *conn = userdata;
    size_t n = size * nmemb;
    sse_buf_append(conn, ptr, n);
    return n;
}

/* Pop the first complete SSE event out of conn->sse_buf.
 * On success returns 1 and writes the parsed event type (malloc'd, may be NULL)
 * and data (malloc'd, may be NULL) to the out-pointers; the consumed bytes are
 * removed from the buffer. Returns 0 if no complete event is buffered yet.
 * Non-static so the parser can be unit-tested directly. */
int sse_pop_event(mcp_connection *conn, char **out_event, char **out_data) {
    *out_event = NULL;
    *out_data = NULL;
    char *buf = conn->sse_buf;
    size_t len = conn->sse_buf_len;
    if (len == 0) return 0;

    const char *rn = memfind(buf, len, "\r\n\r\n", 4);
    const char *nn = memfind(buf, len, "\n\n", 2);
    const char *sep = NULL;
    size_t sep_len = 0;
    if (rn != NULL && (nn == NULL || rn < nn)) {
        sep = rn;
        sep_len = 4;
    } else if (nn != NULL) {
        sep = nn;
        sep_len = 2;
    }
    if (sep == NULL) return 0;

    size_t block_len = (size_t)(sep - buf);
    char *event = NULL;
    growbuf data = {0};
    size_t i = 0;
    while (i < block_len) {
        size_t start = i;
        while (i < block_len && buf[i] != '\n') i++;
        size_t line_end = i;
        if (line_end > start && buf[line_end - 1] == '\r') line_end--;
        const char *line = buf + start;
        size_t ll = line_end - start;

        if (ll > 0 && line[0] == ':') {
            /* comment */
        } else if (ll >= 6 && memcmp(line, "event:", 6) == 0) {
            const char *v = line + 6;
            size_t vl = ll - 6;
            while (vl > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            free(event);
            event = malloc(vl + 1);
            if (event == NULL) {
                growbuf_free(&data);
                log_activity("[error] OOM parsing SSE event");
                exit(EXIT_INTERNAL_ERR);
            }
            memcpy(event, v, vl);
            event[vl] = '\0';
        } else if (ll >= 5 && memcmp(line, "data:", 5) == 0) {
            const char *v = line + 5;
            size_t vl = ll - 5;
            while (vl > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vl--;
            }
            growbuf_append(&data, v, vl);
            growbuf_append(&data, "\n", 1);
        }
        if (i < block_len && buf[i] == '\n') i++;
    }

    /* strip the single trailing newline that data lines added */
    if (data.len > 0 && data.data[data.len - 1] == '\n') {
        data.data[--data.len] = '\0';
    }

    /* remove consumed bytes from the accumulator */
    size_t consumed = block_len + sep_len;
    size_t remaining = len - consumed;
    if (remaining > 0) {
        memmove(buf, buf + consumed, remaining);
    }
    conn->sse_buf_len = remaining;
    if (conn->sse_buf_cap > 0) {
        buf[remaining] = '\0';
    }

    *out_event = event;
    *out_data = data.data;
    return 1;
}

/* Drive the multi handle: perform pending I/O then wait up to timeout_ms for
 * activity. Returns 0 if activity occurred, 1 if the connection is alive but
 * idle within the slice, -1 if the GET transfer has completed/closed. */
static int sse_pump(mcp_connection *conn, int64_t timeout_ms) {
    int running = 0;
    CURLMcode mrc = curl_multi_perform(conn->multi, &running);
    if (mrc != CURLM_OK) return -1;
    if (running == 0) return -1;

    int numfds = 0;
    mrc = curl_multi_poll(conn->multi, NULL, 0, (unsigned int)timeout_ms, &numfds);
    if (mrc != CURLM_OK) return -1;
    curl_multi_perform(conn->multi, &running);
    if (running == 0) return -1;
    return (numfds > 0) ? 0 : 1;
}

/* Remove and clean up the GET easy handle (keeps the multi handle). */
static void sse_teardown_get(mcp_connection *conn) {
    if (conn->sse_easy != NULL) {
        curl_multi_remove_handle(conn->multi, conn->sse_easy);
        curl_easy_cleanup(conn->sse_easy);
        conn->sse_easy = NULL;
    }
    if (conn->sse_hdrs != NULL) {
        curl_slist_free_all(conn->sse_hdrs);
        conn->sse_hdrs = NULL;
    }
}

/* Wait until an SSE event of the given type arrives. On success returns 0 and
 * writes the event data (malloc'd, may be NULL) to *out_data. Returns -1 on
 * timeout or if the GET stream closed without delivering the event. */
static int sse_wait_for_event(mcp_connection *conn, const char *event_type, int64_t timeout_ms,
                              char **out_data) {
    *out_data = NULL;
    int64_t start = platform_now_ms();
    while (1) {
        char *ev = NULL;
        char *data = NULL;
        while (sse_pop_event(conn, &ev, &data) == 1) {
            if (ev != NULL && strcmp(ev, event_type) == 0) {
                free(ev);
                *out_data = data;
                return 0;
            }
            free(ev);
            free(data);
            ev = NULL;
            data = NULL;
        }
        int64_t remaining = timeout_ms - (platform_now_ms() - start);
        if (remaining <= 0) return -1;
        int rc = sse_pump(conn, remaining);
        if (rc < 0) {
            /* stream closed: flush any events delivered right before close */
            while (sse_pop_event(conn, &ev, &data) == 1) {
                if (ev != NULL && strcmp(ev, event_type) == 0) {
                    free(ev);
                    *out_data = data;
                    return 0;
                }
                free(ev);
                free(data);
                ev = NULL;
                data = NULL;
            }
            sse_teardown_get(conn);
            return -1;
        }
    }
}

/* Build a POST endpoint URL from the base SSE url and the endpoint event data. */
static char *resolve_endpoint(const char *base_url, const char *endpoint) {
    if (endpoint == NULL) return NULL;
    if (istarts_with(endpoint, "http://") || istarts_with(endpoint, "https://")) {
        return util_strdup(endpoint);
    }
    const char *scheme_end = strstr(base_url, "://");
    if (scheme_end == NULL) return util_strdup(endpoint);
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');

    if (endpoint[0] == '/') {
        size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
        size_t n = origin_len + strlen(endpoint) + 1;
        char *out = malloc(n);
        if (out == NULL) {
            log_activity("[error] OOM resolving endpoint");
            exit(EXIT_INTERNAL_ERR);
        }
        snprintf(out, n, "%.*s%s", (int)origin_len, base_url, endpoint);
        return out;
    }

    const char *last_slash = strrchr(base_url, '/');
    size_t base_len = (last_slash != NULL) ? (size_t)(last_slash - base_url) + 1 : strlen(base_url);
    size_t n = base_len + strlen(endpoint) + 1;
    char *out = malloc(n);
    if (out == NULL) {
        log_activity("[error] OOM resolving endpoint");
        exit(EXIT_INTERNAL_ERR);
    }
    snprintf(out, n, "%.*s%s", (int)base_len, base_url, endpoint);
    return out;
}

/* (Re)create the GET easy handle for the SSE stream and add it to the multi. */
static int sse_setup_get(mcp_connection *conn) {
    CURL *e = curl_easy_init();
    if (e == NULL) return -1;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = headers_to_slist(conn->cfg->headers, headers);

    curl_easy_setopt(e, CURLOPT_URL, conn->base_url);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, conn);
    conn->sse_hdrs = headers;
    conn->sse_easy = e;
    curl_multi_add_handle(conn->multi, e);
    return 0;
}

/* POST one request to the SSE endpoint. Returns 0 on success, -1 on failure. */
static int sse_post(mcp_connection *conn, const char *req_json, int64_t timeout_ms) {
    CURL *post = curl_easy_init();
    if (post == NULL) return -1;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = headers_to_slist(conn->cfg->headers, headers);
    growbuf body = {0};
    curl_easy_setopt(post, CURLOPT_URL, conn->sse_endpoint);
    curl_easy_setopt(post, CURLOPT_POST, 1L);
    curl_easy_setopt(post, CURLOPT_POSTFIELDS, req_json);
    curl_easy_setopt(post, CURLOPT_POSTFIELDSIZE, (long)strlen(req_json));
    curl_easy_setopt(post, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(post, CURLOPT_WRITEFUNCTION, curl_write_growbuf);
    curl_easy_setopt(post, CURLOPT_WRITEDATA, &body);
    if (timeout_ms > 0) {
        curl_easy_setopt(post, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    }
    CURLcode rc = curl_easy_perform(post);
    curl_slist_free_all(headers);
    growbuf_free(&body);
    curl_easy_cleanup(post);
    if (rc != CURLE_OK) {
        log_activity("[error] SSE POST failed: %s", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

/* Establish the GET stream + endpoint, retrying up to cfg->max_reconnect times.
 * On success conn->sse_endpoint is set and conn->sse_easy is active. */
static int sse_reconnect(mcp_connection *conn) {
    int attempts = conn->cfg->max_reconnect > 0 ? conn->cfg->max_reconnect : 1;
    for (int a = 0; a < attempts; a++) {
        if (sse_setup_get(conn) != 0) {
            if (a + 1 < attempts) platform_sleep_ms(conn->cfg->reconnect_delay_ms);
            continue;
        }
        char *ep = NULL;
        if (sse_wait_for_event(conn, "endpoint", conn->cfg->init_timeout_ms, &ep) == 0) {
            free(conn->sse_endpoint);
            conn->sse_endpoint = resolve_endpoint(conn->base_url, ep);
            free(ep);
            if (conn->sse_endpoint != NULL) return 0;
        } else {
            free(ep);
        }
        sse_teardown_get(conn);
        if (a + 1 < attempts) platform_sleep_ms(conn->cfg->reconnect_delay_ms);
    }
    return -1;
}

static int transport_sse_open(mcp_server_cfg *cfg, mcp_connection *conn) {
    if (cfg->url == NULL) {
        log_activity("[error] sse MCP server '%s' has no url", cfg->name);
        return EXIT_MCP_ERR;
    }
    ensure_curl_init();
    conn->base_url = util_strdup(cfg->url);
    conn->multi = curl_multi_init();
    if (conn->multi == NULL) return EXIT_INTERNAL_ERR;
    if (sse_reconnect(conn) != 0) {
        log_activity("[error] SSE MCP server '%s': no endpoint event", cfg->name);
        return EXIT_MCP_ERR;
    }
    return EXIT_SUCCESS;
}

static int transport_sse_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms,
                              char **out_resp) {
    *out_resp = NULL;
    if (conn->sse_easy == NULL) {
        if (sse_reconnect(conn) != 0) return EXIT_MCP_ERR;
    }
    if (sse_post(conn, req_json, timeout_ms) != 0) return EXIT_MCP_ERR;

    char *data = NULL;
    if (sse_wait_for_event(conn, "message", timeout_ms, &data) == 0) {
        *out_resp = (data != NULL) ? data : util_strdup("");
        return EXIT_SUCCESS;
    }
    free(data);
    log_activity("[error] SSE MCP server '%s': no message event", conn->cfg->name);
    return EXIT_MCP_ERR;
}

static void transport_sse_close(mcp_connection *conn) {
    sse_teardown_get(conn);
    if (conn->multi != NULL) {
        curl_multi_cleanup(conn->multi);
        conn->multi = NULL;
    }
    free(conn->sse_endpoint);
    conn->sse_endpoint = NULL;
    free(conn->sse_buf);
    conn->sse_buf = NULL;
    conn->sse_buf_len = 0;
    conn->sse_buf_cap = 0;
    free(conn->base_url);
    conn->base_url = NULL;
}

/* ============================ dispatch ============================ */

int transport_open(mcp_server_cfg *cfg, mcp_connection *conn) {
    if (cfg == NULL || conn == NULL) return EXIT_INTERNAL_ERR;
    memset(conn, 0, sizeof(*conn));
    conn->cfg = cfg;
    conn->transport = cfg->transport;
    switch (cfg->transport) {
    case MCP_STDIO:
        return transport_stdio_open(cfg, conn);
    case MCP_HTTP:
        return transport_http_open(cfg, conn);
    case MCP_SSE:
        return transport_sse_open(cfg, conn);
    }
    log_activity("[error] unknown transport type");
    return EXIT_INTERNAL_ERR;
}

int transport_send(mcp_connection *conn, const char *req_json, int64_t timeout_ms,
                   char **out_resp) {
    if (conn == NULL || req_json == NULL || out_resp == NULL) return EXIT_INTERNAL_ERR;
    switch (conn->transport) {
    case MCP_STDIO:
        return transport_stdio_send(conn, req_json, timeout_ms, out_resp);
    case MCP_HTTP:
        return transport_http_send(conn, req_json, timeout_ms, out_resp);
    case MCP_SSE:
        return transport_sse_send(conn, req_json, timeout_ms, out_resp);
    }
    return EXIT_INTERNAL_ERR;
}

void transport_close(mcp_connection *conn) {
    if (conn == NULL) return;
    switch (conn->transport) {
    case MCP_STDIO:
        transport_stdio_close(conn);
        break;
    case MCP_HTTP:
        transport_http_close(conn);
        break;
    case MCP_SSE:
        transport_sse_close(conn);
        break;
    }
}
