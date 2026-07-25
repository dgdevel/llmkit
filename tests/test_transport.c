#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include "mcp_transport.h"
#include "util.h"

/* The SSE event parser is internal but non-static so we can unit-test it. */
extern int sse_pop_event(mcp_connection *conn, char **out_event, char **out_data);

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, msg); \
            tests_failed++;                                                   \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                       \
    do {                                                                                          \
        tests_run++;                                                                              \
        if ((a) != (b)) {                                                                         \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected %lld, got %lld\n", __FILE__, __LINE__, \
                    msg, (long long)(b), (long long)(a));                                         \
            tests_failed++;                                                                       \
        }                                                                                         \
    } while (0)

#define CHECK_STR_EQ(a, b, msg)                                                             \
    do {                                                                                    \
        tests_run++;                                                                        \
        if (!(a) || !(b) || strcmp((a), (b)) != 0) {                                        \
            fprintf(stderr, "  FAIL (%s:%d): %s — expected \"%s\", got \"%s\"\n", __FILE__, \
                    __LINE__, msg, (b) ? (b) : "NULL", (a) ? (a) : "NULL");                 \
            tests_failed++;                                                                 \
        }                                                                                   \
    } while (0)

/* ============================ SSE parser unit tests ============================ */

/* Build a scratch connection whose sse_buf holds a copy of `src`. */
static void parser_seed(mcp_connection *conn, const char *src) {
    memset(conn, 0, sizeof(*conn));
    conn->sse_buf = util_strdup(src);
    conn->sse_buf_len = strlen(src);
    conn->sse_buf_cap = conn->sse_buf_len + 1;
}

/* Append more bytes to a seeded buffer, mirroring how the write callback grows it. */
static void parser_append(mcp_connection *conn, const char *more) {
    size_t add = strlen(more);
    size_t need = conn->sse_buf_len + add + 1;
    if (need > conn->sse_buf_cap) {
        char *tmp = realloc(conn->sse_buf, need);
        if (tmp == NULL) return;
        conn->sse_buf = tmp;
        conn->sse_buf_cap = need;
    }
    memcpy(conn->sse_buf + conn->sse_buf_len, more, add);
    conn->sse_buf_len += add;
    conn->sse_buf[conn->sse_buf_len] = '\0';
}

static void test_sse_message_event(void) {
    mcp_connection conn;
    parser_seed(&conn, "event: message\ndata: {\"jsonrpc\":\"2.0\"}\n\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "should pop one event");
    CHECK_STR_EQ(ev, "message", "event type");
    CHECK_STR_EQ(data, "{\"jsonrpc\":\"2.0\"}", "event data");
    CHECK(conn.sse_buf_len == 0, "buffer drained");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_message_event\n");
}

static void test_sse_endpoint_event(void) {
    mcp_connection conn;
    parser_seed(&conn, "event: endpoint\ndata: /message?sid=1\n\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "pop endpoint");
    CHECK_STR_EQ(ev, "endpoint", "type endpoint");
    CHECK_STR_EQ(data, "/message?sid=1", "endpoint path");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_endpoint_event\n");
}

static void test_sse_multiple_events(void) {
    mcp_connection conn;
    parser_seed(&conn, "event: ping\ndata: 1\n\nevent: message\ndata: M\n\n");
    char *ev1 = NULL;
    char *data1 = NULL;
    char *ev2 = NULL;
    char *data2 = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev1, &data1), 1, "first pop");
    CHECK_STR_EQ(ev1, "ping", "first event type");
    CHECK_STR_EQ(data1, "1", "first event data");
    CHECK_EQ(sse_pop_event(&conn, &ev2, &data2), 1, "second pop");
    CHECK_STR_EQ(ev2, "message", "second event type");
    CHECK_STR_EQ(data2, "M", "second event data");
    CHECK(conn.sse_buf_len == 0, "buffer drained after two pops");
    CHECK_EQ(sse_pop_event(&conn, &ev2, &data2), 0, "third pop returns 0");

    free(ev1);
    free(data1);
    free(ev2);
    free(data2);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_multiple_events\n");
}

static void test_sse_crlf_line_endings(void) {
    mcp_connection conn;
    parser_seed(&conn, "event: message\r\ndata: hi\r\n\r\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "pop with CRLF");
    CHECK_STR_EQ(ev, "message", "type with crlf");
    CHECK_STR_EQ(data, "hi", "data with crlf");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_crlf_line_endings\n");
}

static void test_sse_multiline_data(void) {
    mcp_connection conn;
    parser_seed(&conn, "data: line1\ndata: line2\n\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "pop multiline");
    CHECK(ev == NULL, "no event: field -> NULL type");
    CHECK_STR_EQ(data, "line1\nline2", "multiline data joined by newline");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_multiline_data\n");
}

static void test_sse_comment_ignored(void) {
    mcp_connection conn;
    parser_seed(&conn, ": keep-alive comment\ndata: payload\n\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "pop with comment");
    CHECK_STR_EQ(data, "payload", "comment line ignored");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_comment_ignored\n");
}

static void test_sse_incomplete_returns_zero(void) {
    mcp_connection conn;
    parser_seed(&conn, "event: message\ndata: partial");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 0, "no terminator -> 0");
    CHECK(ev == NULL && data == NULL, "no output on incomplete");
    CHECK(conn.sse_buf_len == strlen("event: message\ndata: partial"),
          "buffer unchanged on incomplete");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_incomplete_returns_zero\n");
}

static void test_sse_split_across_chunks(void) {
    /* Simulate receiving an event in two pieces: first the data line, then the
     * terminator arrives later. The parser must not emit until complete. */
    mcp_connection conn;
    parser_seed(&conn, "event: message\ndata: hi\n");
    char *ev = NULL;
    char *data = NULL;
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 0, "partial -> 0");
    free(ev);
    free(data);
    /* now the rest of the stream arrives, appended to the existing buffer */
    parser_append(&conn, "\n");
    CHECK_EQ(sse_pop_event(&conn, &ev, &data), 1, "now complete -> 1");
    CHECK_STR_EQ(ev, "message", "type after completion");
    CHECK_STR_EQ(data, "hi", "data after completion");

    free(ev);
    free(data);
    free(conn.sse_buf);
    fprintf(stderr, "  [ok] test_sse_split_across_chunks\n");
}

/* ============================ mock server helpers ============================ */

static void write_all_fd(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

/* Read a full HTTP request (headers + body by Content-Length) into buf. */
static int read_http_request(int fd, char *buf, size_t n) {
    size_t total = 0;
    char *hdr_end = NULL;
    while (total < n - 1) {
        ssize_t r = read(fd, buf + total, n - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end != NULL) break;
    }
    if (hdr_end == NULL) return (int)total;
    long clen = 0;
    char *cl = strstr(buf, "Content-Length:");
    if (cl == NULL) cl = strstr(buf, "content-length:");
    if (cl != NULL) clen = strtol(cl + 15, NULL, 10);
    char *body = hdr_end + 4;
    size_t body_have = total - (size_t)(body - buf);
    while ((long)body_have < clen && total < n - 1) {
        ssize_t r = read(fd, buf + total, n - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
        buf[total] = '\0';
        body_have = total - (size_t)(body - buf);
    }
    return (int)total;
}

typedef void (*server_fn)(int srv, void *ctx);

/* Fork a child that finds a free port, listens, writes the port to a pipe, then
 * invokes fn(srv, ctx). Returns child pid and fills *out_port (0 on failure). */
static pid_t launch_server(server_fn fn, void *ctx, int *out_port) {
    *out_port = 0;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        int srv = -1;
        int port = 0;
        for (int p = 38000; p < 38100 && srv < 0; p++) {
            srv = platform_tcp_listen("127.0.0.1", p);
            if (srv >= 0) port = p;
        }
        ssize_t w = write(pipefd[1], &port, sizeof(port));
        (void)w;
        close(pipefd[1]);
        if (srv < 0) _exit(1);
        fn(srv, ctx);
        close(srv);
        _exit(0);
    }
    /* parent */
    close(pipefd[1]);
    ssize_t r = read(pipefd[0], out_port, sizeof(int));
    (void)r;
    close(pipefd[0]);
    return pid;
}

/* ============================ stdio integration ============================ */

static void test_stdio_roundtrip(void) {
    const char *mock_path = "/tmp/llmkit_stdio_mock.sh";
    FILE *fp = fopen(mock_path, "wb");
    CHECK(fp != NULL, "open mock script");
    if (fp == NULL) return;
    const char *script = "#!/bin/sh\n"
                         "IFS= read -r line\n"
                         "printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":\"1\","
                         "\"result\":{\"tools\":[]}}'\n";
    fwrite(script, 1, strlen(script), fp);
    fclose(fp);

    mcp_server_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "stdio-mock";
    cfg.transport = MCP_STDIO;
    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "/bin/sh %s", mock_path);
    cfg.cmdline = cmdline;
    cfg.init_timeout_ms = 5000;
    cfg.call_timeout_ms = 5000;

    mcp_connection conn;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_SUCCESS, "stdio transport_open");

    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"tools/list\"}";
    char *resp = NULL;
    CHECK_EQ(transport_send(&conn, req, 5000, &resp), EXIT_SUCCESS, "stdio transport_send");
    CHECK(resp != NULL && strstr(resp, "\"tools\":[]") != NULL, "stdio response body");

    free(resp);
    transport_close(&conn);
    unlink(mock_path);
    fprintf(stderr, "  [ok] test_stdio_roundtrip\n");
}

/* ============================ http integration ============================ */

struct http_ctx {
    const char *resp_body;
    const char *session_id;   /* sent on the first response only */
    const char *req_files[2]; /* capture full request bytes per round (NULL to skip) */
    int n_requests;
};

static void http_server_fn(int srv, void *ctxptr) {
    struct http_ctx *c = ctxptr;
    for (int i = 0; i < c->n_requests; i++) {
        int client = platform_tcp_accept(srv, 10000);
        if (client < 0) continue;
        char req[8192];
        int n = read_http_request(client, req, sizeof(req));
        const char *file = c->req_files[i];
        if (file != NULL && n > 0) {
            FILE *fp = fopen(file, "wb");
            if (fp != NULL) {
                fwrite(req, 1, (size_t)n, fp);
                fclose(fp);
            }
        }
        char resp[16384];
        int off = snprintf(resp, sizeof(resp),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n");
        if (i == 0 && c->session_id != NULL) {
            off +=
                snprintf(resp + off, sizeof(resp) - off, "Mcp-Session-Id: %s\r\n", c->session_id);
        }
        off += snprintf(resp + off, sizeof(resp) - off,
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n%s",
                        strlen(c->resp_body), c->resp_body);
        write_all_fd(client, resp, (size_t)off);
        shutdown(client, SHUT_WR);
        char drain[64];
        ssize_t r = read(client, drain, sizeof(drain));
        (void)r;
        close(client);
    }
}

static char *read_file_str(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    return buf;
}

static void test_http_roundtrip(void) {
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"tools\":[]}}";
    const char *req_file = "/tmp/llmkit_http_req1.txt";
    unlink(req_file);
    struct http_ctx ctx = {.resp_body = body, .session_id = NULL, .n_requests = 1};
    ctx.req_files[0] = req_file;
    ctx.req_files[1] = NULL;

    int port = 0;
    pid_t pid = launch_server(http_server_fn, &ctx, &port);
    CHECK(pid >= 0 && port > 0, "launch http server");
    if (pid < 0 || port == 0) return;

    mcp_server_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "http-mock";
    cfg.transport = MCP_HTTP;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", port);
    cfg.url = url;
    cfg.call_timeout_ms = 5000;

    mcp_connection conn;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_SUCCESS, "http transport_open");

    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"tools/list\"}";
    char *resp = NULL;
    CHECK_EQ(transport_send(&conn, req, 5000, &resp), EXIT_SUCCESS, "http transport_send");
    CHECK(resp != NULL && strstr(resp, "\"tools\":[]") != NULL, "http response body");

    transport_close(&conn);

    int status = 0;
    waitpid(pid, &status, 0);

    char *captured = read_file_str(req_file);
    CHECK(captured != NULL, "request captured to file");
    CHECK(captured != NULL && strstr(captured, "tools/list") != NULL,
          "request body contained method");
    CHECK(captured != NULL && strstr(captured, "Content-Type: application/json") != NULL,
          "request had json content-type");
    free(captured);
    unlink(req_file);

    fprintf(stderr, "  [ok] test_http_roundtrip\n");
}

static void test_http_session_id_echo(void) {
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{}}";
    const char *sid = "sess-abc-123";
    const char *req1_file = "/tmp/llmkit_http_req1.txt";
    const char *req2_file = "/tmp/llmkit_http_req2.txt";
    unlink(req1_file);
    unlink(req2_file);
    struct http_ctx ctx = {.resp_body = body, .session_id = sid, .n_requests = 2};
    ctx.req_files[0] = req1_file;
    ctx.req_files[1] = req2_file;

    int port = 0;
    pid_t pid = launch_server(http_server_fn, &ctx, &port);
    CHECK(pid >= 0 && port > 0, "launch http server (session)");
    if (pid < 0 || port == 0) return;

    mcp_server_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "http-sess";
    cfg.transport = MCP_HTTP;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/mcp", port);
    cfg.url = url;
    cfg.call_timeout_ms = 5000;

    mcp_connection conn;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_SUCCESS, "http open (session)");

    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"initialize\"}";
    char *resp1 = NULL;
    CHECK_EQ(transport_send(&conn, req, 5000, &resp1), EXIT_SUCCESS, "first send");
    free(resp1);

    /* After the first response carried Mcp-Session-Id, the connection must remember it. */
    CHECK_STR_EQ(conn.session_id, sid, "session id captured");

    char *resp2 = NULL;
    CHECK_EQ(transport_send(&conn, req, 5000, &resp2), EXIT_SUCCESS, "second send");
    free(resp2);

    transport_close(&conn);
    int status = 0;
    waitpid(pid, &status, 0);

    /* The first request must NOT carry the header; the second request MUST. */
    char *cap1 = read_file_str(req1_file);
    char *cap2 = read_file_str(req2_file);
    CHECK(cap1 != NULL && strstr(cap1, "MCP-Session-Id") == NULL, "req1 has no session header");
    CHECK(cap2 != NULL && strstr(cap2, "MCP-Session-Id: sess-abc-123") != NULL,
          "req2 echoes session header");
    free(cap1);
    free(cap2);
    unlink(req1_file);
    unlink(req2_file);

    fprintf(stderr, "  [ok] test_http_session_id_echo\n");
}

/* ============================ sse integration ============================ */

struct sse_ctx {
    const char *message_body; /* JSON-RPC response returned via the message event */
};

static void sse_server_fn(int srv, void *ctxptr) {
    struct sse_ctx *c = ctxptr;
    int conn1 = platform_tcp_accept(srv, 10000); /* GET stream */
    if (conn1 < 0) return;
    char rb[4096];
    read_http_request(conn1, rb, sizeof(rb)); /* drain GET request line + headers */

    const char *event_block = "event: endpoint\ndata: /message\n\n";
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Connection: keep-alive\r\n\r\n%s",
                        event_block);
    write_all_fd(conn1, hdr, (size_t)hlen);

    /* wait for the POST to arrive on the listening socket */
    struct pollfd pfd = {.fd = srv, .events = POLLIN};
    if (poll(&pfd, 1, 10000) <= 0) {
        close(conn1);
        return;
    }
    int conn2 = platform_tcp_accept(srv, 5000); /* POST */
    if (conn2 < 0) {
        close(conn1);
        return;
    }
    read_http_request(conn2, rb, sizeof(rb));
    /* acknowledge the POST so curl completes cleanly */
    const char *ack = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    write_all_fd(conn2, ack, strlen(ack));
    close(conn2);

    char msg[4096];
    int mlen = snprintf(msg, sizeof(msg), "event: message\ndata: %s\n\n", c->message_body);
    write_all_fd(conn1, msg, (size_t)mlen);
    shutdown(conn1, SHUT_WR);
    char drain[64];
    ssize_t r = read(conn1, drain, sizeof(drain));
    (void)r;
    close(conn1);
}

static void test_sse_roundtrip(void) {
    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"tools\":[]}}";
    struct sse_ctx ctx = {.message_body = body};

    int port = 0;
    pid_t pid = launch_server(sse_server_fn, &ctx, &port);
    CHECK(pid >= 0 && port > 0, "launch sse server");
    if (pid < 0 || port == 0) return;

    mcp_server_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "sse-mock";
    cfg.transport = MCP_SSE;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/sse", port);
    cfg.url = url;
    cfg.init_timeout_ms = 5000;
    cfg.call_timeout_ms = 5000;
    cfg.max_reconnect = 1;
    cfg.reconnect_delay_ms = 100;

    mcp_connection conn;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_SUCCESS, "sse transport_open (endpoint received)");

    const char *req = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"tools/list\"}";
    char *resp = NULL;
    CHECK_EQ(transport_send(&conn, req, 5000, &resp), EXIT_SUCCESS, "sse transport_send");
    CHECK(resp != NULL && strstr(resp, "\"tools\":[]") != NULL, "sse message body");

    free(resp);
    transport_close(&conn);
    int status = 0;
    waitpid(pid, &status, 0);

    fprintf(stderr, "  [ok] test_sse_roundtrip\n");
}

/* ============================ open/close edge cases ============================ */

static void test_close_uninitialized(void) {
    /* transport_close must be safe on a zeroed/never-opened connection. */
    mcp_connection conn;
    memset(&conn, 0, sizeof(conn));
    transport_close(&conn);
    fprintf(stderr, "  [ok] test_close_uninitialized\n");
}

static void test_open_missing_fields(void) {
    mcp_server_cfg cfg;
    mcp_connection conn;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "stdio-no-cmd";
    cfg.transport = MCP_STDIO;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_MCP_ERR, "stdio without cmdline fails");
    transport_close(&conn);

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "http-no-url";
    cfg.transport = MCP_HTTP;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_MCP_ERR, "http without url fails");
    transport_close(&conn);

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "sse-no-url";
    cfg.transport = MCP_SSE;
    CHECK_EQ(transport_open(&cfg, &conn), EXIT_MCP_ERR, "sse without url fails");
    transport_close(&conn);

    fprintf(stderr, "  [ok] test_open_missing_fields\n");
}

int main(void) {
    /* allow forked children to be reaped without interfering */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "=== test_transport ===\n");

    test_sse_message_event();
    test_sse_endpoint_event();
    test_sse_multiple_events();
    test_sse_crlf_line_endings();
    test_sse_multiline_data();
    test_sse_comment_ignored();
    test_sse_incomplete_returns_zero();
    test_sse_split_across_chunks();

    test_stdio_roundtrip();
    test_http_roundtrip();
    test_http_session_id_echo();
    test_sse_roundtrip();

    test_close_uninitialized();
    test_open_missing_fields();

    fprintf(stderr, "\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
