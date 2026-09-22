/* platform.c — threads, queues, spawn, signals, curl helpers (design §2). */
#include "llmkit.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- signals ---- */

volatile int g_stop_flag = 0;

static void sigint_handler(int sig) {
    (void)sig;
    /* a second SIGINT terminates immediately: the orderly stop may still be
       waiting for an in-flight (silent) endpoint transfer that only the
       write callback can abort, and the user must always be able to kill */
    if (g_stop_flag) _exit(EXIT_INTERRUPTED);
    g_stop_flag = 1;
}

void signals_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: blocking reads must wake up */
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- queue: mutex + condvar, unbounded ---- */

typedef struct qnode {
    struct qnode *next;
    void *data;
} qnode_t;

struct queue {
    pthread_mutex_t m;
    pthread_cond_t c;
    qnode_t *head, *tail;
    bool closed;
};

queue_t *queue_new(void) {
    queue_t *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    return q;
}

static void queue_drop_all(queue_t *q) {
    while (q->head) {
        qnode_t *n = q->head;
        q->head = n->next;
        free(n->data);
        free(n);
    }
    q->tail = NULL;
}

void queue_free(queue_t *q) {
    queue_drop_all(q);
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->c);
    free(q);
}

void queue_push(queue_t *q, void *item) {
    qnode_t *n = calloc(1, sizeof *n);
    n->data = item;
    pthread_mutex_lock(&q->m);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    pthread_cond_signal(&q->c);
    pthread_mutex_unlock(&q->m);
}

void *queue_pop(queue_t *q) {
    pthread_mutex_lock(&q->m);
    while (!q->head && !q->closed)
        pthread_cond_wait(&q->c, &q->m);
    if (!q->head) { pthread_mutex_unlock(&q->m); return NULL; }
    qnode_t *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
    void *d = n->data;
    free(n);
    return d;
}

void *queue_try_pop(queue_t *q) {
    pthread_mutex_lock(&q->m);
    if (!q->head) { pthread_mutex_unlock(&q->m); return NULL; }
    qnode_t *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
    void *d = n->data;
    free(n);
    return d;
}

void *queue_pop_timeout(queue_t *q, double seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)seconds;
    ts.tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1e9);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&q->m);
    while (!q->head && !q->closed) {
        if (pthread_cond_timedwait(&q->c, &q->m, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&q->m);
            return NULL;
        }
    }
    if (!q->head) { pthread_mutex_unlock(&q->m); return NULL; }
    qnode_t *n = q->head;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    pthread_mutex_unlock(&q->m);
    void *d = n->data;
    free(n);
    return d;
}

void queue_close(queue_t *q) {
    pthread_mutex_lock(&q->m);
    q->closed = true;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
}

bool queue_closed(queue_t *q) {
    pthread_mutex_lock(&q->m);
    bool c = q->closed;
    pthread_mutex_unlock(&q->m);
    return c;
}

/* block until the producer side closed the queue (reader teardown wait) */
void queue_wait_closed(queue_t *q) {
    pthread_mutex_lock(&q->m);
    while (!q->closed) pthread_cond_wait(&q->c, &q->m);
    pthread_mutex_unlock(&q->m);
}

int thread_start_detached(thread_fn fn, void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return rc;
}

/* ---- spawn ---- */

int spawn_shell(const char *command_line, spawn_t *out) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe)) return -1;
    if (pipe(out_pipe)) { close(in_pipe[0]); close(in_pipe[1]); return -1; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* child: stderr passes through untouched (it is the log channel) */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", "-c", command_line, (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    out->pid = pid;
    out->to_fd = in_pipe[1];
    out->from_fd = out_pipe[0];
    return 0;
}

/* SIGTERM + reap + close; resets pid and fds so repeated calls are safe */
void spawn_kill(spawn_t *s) {
    if (s->pid > 0) {
        kill(s->pid, SIGTERM);
        waitpid(s->pid, NULL, 0);
    }
    if (s->to_fd >= 0) close(s->to_fd);
    if (s->from_fd >= 0) close(s->from_fd);
    s->pid = -1;
    s->to_fd = -1;
    s->from_fd = -1;
}

/* ---- curl ---- */

typedef struct http_aux {
    http_req_t *r;
    bool saw_status;
    bool is_json; /* application/json body: buffer even when streaming */
} http_aux_t;

static size_t hdr_cb(char *buf, size_t sz, size_t nm, void *ud) {
    http_aux_t *a = ud;
    size_t n = sz * nm;
    /* the status line is delivered to the header callback too */
    if (n > 5 && memcmp(buf, "HTTP/", 5) == 0) {
        const char *sp = memchr(buf, ' ', n);
        if (sp) {
            a->r->status = strtol(sp + 1, NULL, 10);
            a->saw_status = true;
        }
        return n;
    }
    if (n >= 14 && strncasecmp(buf, "Content-Type:", 13) == 0) {
        const char *v = buf + 13;
        size_t vn = n - 13;
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ')) vn--;
        buf_clear(&a->r->content_type);
        buf_append(&a->r->content_type, v, vn);
        if (vn >= strlen("application/json") &&
            strncasecmp(v, "application/json", strlen("application/json")) == 0)
            a->is_json = true;
    } else if (n >= 15 && strncasecmp(buf, "Mcp-Session-Id:", 15) == 0) {
        const char *v = buf + 15;
        size_t vn = n - 15;
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ')) vn--;
        if (vn < sizeof a->r->session_id) {
            memcpy(a->r->session_id, v, vn);
            a->r->session_id[vn] = '\0';
        }
    }
    return n;
}

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    http_aux_t *a = ud;
    size_t n = sz * nm;
    a->r->got_data = true;
    if (a->r->on_data && !a->is_json && a->r->status >= 200 &&
        a->r->status < 300) {
        a->r->on_data(a->r->cb_ctx, ptr, n);
    } else {
        buf_append(&a->r->resp, ptr, n);
    }
    if (g_stop_flag) return 0; /* abort the transfer (short count) */
    return n;
}

int http_perform(http_req_t *r) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    http_aux_t aux;
    memset(&aux, 0, sizeof aux);
    aux.r = r;

    curl_easy_setopt(c, CURLOPT_URL, r->url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r->body_len);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, r->hdrs);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &aux);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &aux);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "llmkit/" LLMKIT_VERSION);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    if (r->connect_to > 0)
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)r->connect_to);
    if (r->read_to > 0) { /* first byte and inter-chunk alike (design §5) */
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)r->read_to);
    }
    if (r->total_to > 0)
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(r->total_to * 1000.0));

    CURLcode res = curl_easy_perform(c);
    r->curl_res = res;
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    r->status = status;
    curl_easy_cleanup(c);

    if (res == CURLE_OK) {
        if (status >= 200 && status < 300) return 0;
        return 1;
    }
    return -1;
}

void http_hdr_add(struct curl_slist **list, const char *name, const char *value) {
    buf_t b;
    buf_init(&b);
    buf_appendf(&b, "%s: %s", name, value);
    *list = curl_slist_append(*list, b.data);
    buf_free(&b);
}

void http_hdr_add_json(struct curl_slist **list) {
    *list = curl_slist_append(*list, "Content-Type: application/json");
}

bool http_hdrs_from_json(struct curl_slist **list, const cJSON *obj,
                         char *err, size_t errsz) {
    if (!obj) return true;
    if (!cJSON_IsObject(obj)) {
        snprintf(err, errsz, "headers must be an object");
        return false;
    }
    for (const cJSON *it = obj->child; it; it = it->next) {
        if (!cJSON_IsString(it) || !it->valuestring) {
            snprintf(err, errsz, "header value of '%s' must be a string", it->string);
            return false;
        }
        http_hdr_add(list, it->string, it->valuestring);
    }
    return true;
}
