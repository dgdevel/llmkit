/* models.c - llmkit models: one GET /models, textual or normalized json
   out (design sec.14). No engine, no wire: the raw record channel is not
   involved, errors go to stderr with the call command's code tier. */
#include "llmkit.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ================= parsed command line ================= */

typedef struct {
    int protocol;         /* PROTO_* */
    char *api_base;       /* owned */
    char *key;            /* owned */
    char **hdr_names;     /* owned, --header folds here */
    char **hdr_values;
    size_t nhdrs;
    bool json;
} models_cfg_t;

static void models_cfg_free(models_cfg_t *c) {
    free(c->api_base);
    free(c->key);
    for (size_t i = 0; i < c->nhdrs; i++) {
        free(c->hdr_names[i]);
        free(c->hdr_values[i]);
    }
    free(c->hdr_names);
    free(c->hdr_values);
    memset(c, 0, sizeof *c);
}

static int hdr_add(models_cfg_t *c, const char *name, const char *value) {
    for (size_t i = 0; i < c->nhdrs; i++) {
        if (!strcmp(c->hdr_names[i], name)) { /* later replaces earlier */
            char *nv = strdup(value);
            if (!nv) return -1;
            free(c->hdr_values[i]);
            c->hdr_values[i] = nv;
            return 0;
        }
    }
    char **n = realloc(c->hdr_names, (c->nhdrs + 1) * sizeof *n);
    char **v = realloc(c->hdr_values, (c->nhdrs + 1) * sizeof *v);
    if (n) c->hdr_names = n;
    if (v) c->hdr_values = v;
    char *nn = !n ? NULL : strdup(name);
    char *nv = !v ? NULL : strdup(value);
    if (!nn || !nv) {
        free(nn);
        free(nv);
        return -1;
    }
    c->hdr_names[c->nhdrs] = nn;
    c->hdr_values[c->nhdrs] = nv;
    c->nhdrs++;
    return 0;
}

static void models_usage(FILE *out) {
    fputs("usage: llmkit models\n"
          "          [--openai|--openai-responses|--anthropic]\n"
          "          [--key <key>] [--header <name>=<value>]\n"
          "          [--json]\n"
          "          <api_base>\n",
          out);
}

/* ================= request / parse ================= */

/* the upstream list page: GET api_base + "/models". False on transport
   error (err filled) or non-2xx (err filled, HTTP status + body cut). */
static bool fetch_page(const models_cfg_t *c, const char *after, buf_t *body,
                       long *status, char *err, size_t errsz) {
    buf_t url;
    buf_init(&url);
    buf_append_str(&url, c->api_base);
    buf_append_str(&url, "/models");
    if (after && after[0]) {
        buf_append_str(&url, "?after=");
        buf_append_str(&url, after); /* server-generated id: no quoting */
    }

    struct curl_slist *hdrs = NULL;
    bool have_auth = false;
    for (size_t i = 0; i < c->nhdrs; i++) {
        if (!strcasecmp(c->hdr_names[i], "authorization") ||
            !strcasecmp(c->hdr_names[i], "x-api-key"))
            have_auth = true;
        http_hdr_add(&hdrs, c->hdr_names[i], c->hdr_values[i]);
    }
    http_hdr_add(&hdrs, "Accept", "application/json");
    if (c->key && c->key[0] && !have_auth) {
        if (c->protocol == PROTO_ANTHROPIC) {
            http_hdr_add(&hdrs, "x-api-key", c->key);
            http_hdr_add(&hdrs, "anthropic-version", "2023-06-01");
        } else {
            buf_t auth;
            buf_init(&auth);
            buf_appendf(&auth, "Bearer %s", c->key);
            http_hdr_add(&hdrs, "Authorization", auth.data ? auth.data : "");
            buf_free(&auth);
        }
    }

    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = url.data;
    r.hdrs = hdrs; /* freed by http_perform */
    r.is_get = true;
    buf_init(&r.resp);
    buf_init(&r.content_type);
    int rc = http_perform(&r);
    buf_free(&r.content_type);
    *status = r.status;
    if (rc == 0 && r.status >= 200 && r.status < 300) {
        buf_append(body, r.resp.data, r.resp.len);
    } else if (rc != 0) {
        snprintf(err, errsz, "connect_failed: curl %d", (int)r.curl_res);
    } else {
        size_t n = r.resp.len > 200 ? 200 : r.resp.len;
        char cut[256] = "";
        if (n) memcpy(cut, r.resp.data, n), cut[n] = '\0';
        snprintf(err, errsz, "http_error: HTTP %ld: %s", r.status, cut);
    }
    buf_free(&r.resp);
    buf_free(&url);
    return err[0] == '\0';
}

/* the provider model object, normalized: id always; created as a unix
   second count when the page gives one (openai) or an RFC3339 stamp
   (anthropic); display_name/owned_by when the provider has them; raw
   keeps the provider object verbatim. NULL + err when the object has no
   id string. */
static cJSON *model_normalize(const cJSON *m, char *err, size_t errsz) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0]) {
        snprintf(err, errsz, "api_error: model object without an id string");
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "id", id->valuestring);

    /* created: unix number as-is; RFC3339/ISO stamp -> unix seconds */
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(m, "created");
    double unix_created = 0;
    bool have_created = false;
    if (cJSON_IsNumber(cr)) {
        unix_created = cr->valuedouble;
        have_created = true;
    } else {
        const cJSON *ca = cJSON_GetObjectItemCaseSensitive(m, "created_at");
        if (cJSON_IsString(ca) && ca->valuestring[0]) {
            /* "YYYY-MM-DDTHH:MM:SS[.frac][Z|+HH:MM]": hand-rolled, no
               strptime (glibc needs _GNU_SOURCE, mingw lacks it) */
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
            char tsep = 0;
            struct tm tm;
            const char *rest =
                sscanf(ca->valuestring, "%4d-%2d-%2d%c%2d:%2d:%2d", &y, &mo,
                       &d, &tsep, &h, &mi, &s) == 7 &&
                        (tsep == 'T' || tsep == 't' || tsep == ' ')
                    ? ca->valuestring + 19
                    : NULL;
            if (rest) {
                memset(&tm, 0, sizeof tm);
                tm.tm_year = y - 1900;
                tm.tm_mon = mo - 1;
                tm.tm_mday = d;
                tm.tm_hour = h;
                tm.tm_min = mi;
                tm.tm_sec = s;
                unix_created = (double)timegm(&tm);
                /* fractional seconds: fold into the unix count */
                if (rest[0] == '.') {
                    double frac = 0, scale = 0.1;
                    for (const char *p = rest + 1; *p >= '0' && *p <= '9';
                         p++, scale /= 10)
                        frac += (*p - '0') * scale;
                    unix_created += frac;
                }
                if (rest[0] == '+' || rest[0] == '-') {
                    int hh = 0, mm = 0;
                    if (sscanf(rest + 1, "%2d:%2d", &hh, &mm) == 2) {
                        long off = (long)hh * 3600 + (long)mm * 60;
                        unix_created += rest[0] == '+' ? -off : off;
                    }
                } /* trailing 'Z' or absent offset: already UTC */
                have_created = true;
            }
        }
    }
    if (have_created)
        cJSON_AddNumberToObject(out, "created", unix_created);

    const cJSON *dn = cJSON_GetObjectItemCaseSensitive(m, "display_name");
    if (cJSON_IsString(dn) && dn->valuestring[0])
        cJSON_AddStringToObject(out, "display_name", dn->valuestring);
    const cJSON *ob = cJSON_GetObjectItemCaseSensitive(m, "owned_by");
    if (cJSON_IsString(ob) && ob->valuestring[0])
        cJSON_AddStringToObject(out, "owned_by", ob->valuestring);

    cJSON_AddItemToObject(out, "raw", cJSON_Duplicate(m, 1));
    return out;
}

/* ================= session fetch (the repl's /models) ================= */

/* one GET /models page loop over the live session's llm record. Copies
   every protocol, key and header flag - the endpoint already carries
   the session - and returns the normalized model objects as an array
   (never NULL, empty on none). NULL + err on a transport or parse
   failure (the tier's message, prefixed by its code). */
cJSON *models_fetch(int protocol, const char *api_base, const char *key,
                    const cJSON *headers, char *err, size_t errsz) {
    models_cfg_t c;
    memset(&c, 0, sizeof c);
    c.protocol = protocol;
    c.api_base = strdup(api_base ? api_base : "");
    if (!c.api_base) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }
    c.key = key ? strdup(key) : NULL;
    bool oom = !key || c.key;
    cJSON *h;
    if (headers && cJSON_IsObject(headers))
        cJSON_ArrayForEach(h, (cJSON *)headers) {
            if (!h->string || !cJSON_IsString(h) || !h->valuestring) continue;
            if (hdr_add(&c, h->string, h->valuestring)) oom = false;
        }
    if (!oom) {
        snprintf(err, errsz, "out of memory");
        models_cfg_free(&c);
        return NULL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        snprintf(err, errsz, "out of memory");
        models_cfg_free(&c);
        return NULL;
    }
    /* the endpoint's own cursor, one page per round (cmd_models) */
    char after[512] = "";
    while (1) {
        buf_t body;
        buf_init(&body);
        long status = 0;
        err[0] = '\0';
        bool ok = fetch_page(&c, after, &body, &status, err, sizeof err);
        cJSON *page = ok ? cJSON_ParseWithLength(body.data ? body.data : "",
                                                 body.len)
                         : NULL;
        buf_free(&body);
        if (!ok) {
            cJSON_Delete(arr);
            models_cfg_free(&c);
            return NULL;
        }
        if (!page) {
            snprintf(err, errsz, "api_error: bad json from endpoint");
            cJSON_Delete(arr);
            models_cfg_free(&c);
            return NULL;
        }
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(page, "data");
        if (!cJSON_IsArray(data)) {
            snprintf(err, errsz, "api_error: no data array in response");
            cJSON_Delete(page);
            cJSON_Delete(arr);
            models_cfg_free(&c);
            return NULL;
        }
        const cJSON *m;
        cJSON_ArrayForEach(m, data) {
            err[0] = '\0';
            cJSON *nm = model_normalize(m, err, sizeof err);
            if (!nm) {
                cJSON_Delete(nm);
                cJSON_Delete(page);
                cJSON_Delete(arr);
                models_cfg_free(&c);
                return NULL;
            }
            cJSON_AddItemToArray(arr, nm);
        }
        const cJSON *hm = cJSON_GetObjectItemCaseSensitive(page, "has_more");
        const cJSON *lid =
            cJSON_GetObjectItemCaseSensitive(page, "last_id");
        after[0] = '\0';
        if (cJSON_IsTrue(hm) && cJSON_IsString(lid) &&
            lid->valuestring[0] && strlen(lid->valuestring) < sizeof after)
            snprintf(after, sizeof after, "%s", lid->valuestring);
        cJSON_Delete(page);
        if (!after[0]) break;
    }
    models_cfg_free(&c);
    return arr;
}

/* ================= output ================= */

static void print_table(const cJSON *data) {
    /* column widths from the data, then one aligned line per model */
    size_t idw = 2, dnw = 5, obw = 5;
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        const cJSON *v;
        size_t n;
        if ((v = cJSON_GetObjectItemCaseSensitive(it, "id")) &&
            cJSON_IsString(v) && (n = strlen(v->valuestring)) > idw)
            idw = n;
        if ((v = cJSON_GetObjectItemCaseSensitive(it, "display_name")) &&
            cJSON_IsString(v) && (n = strlen(v->valuestring)) > dnw)
            dnw = n;
        if ((v = cJSON_GetObjectItemCaseSensitive(it, "owned_by")) &&
            cJSON_IsString(v) && (n = strlen(v->valuestring)) > obw)
            obw = n;
    }
    printf("%-*s  %-*s  %-*s  %s\n", (int)idw, "id", (int)dnw,
           "display", (int)obw, "owner", "created");
    cJSON_ArrayForEach(it, data) {
        char created[32] = "-";
        const cJSON *cr =
            cJSON_GetObjectItemCaseSensitive(it, "created");
        if (cJSON_IsNumber(cr)) {
            time_t t = (time_t)cr->valuedouble;
            struct tm tm;
            gmtime_r(&t, &tm);
            strftime(created, sizeof created, "%Y-%m-%d", &tm);
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *dn =
            cJSON_GetObjectItemCaseSensitive(it, "display_name");
        const cJSON *ob = cJSON_GetObjectItemCaseSensitive(it, "owned_by");
        printf("%-*s  %-*s  %-*s  %s\n", (int)idw,
               cJSON_IsString(id) ? id->valuestring : "",
               (int)dnw, cJSON_IsString(dn) ? dn->valuestring : "",
               (int)obw, cJSON_IsString(ob) ? ob->valuestring : "", created);
    }
}

/* ================= command ================= */

int cmd_models(int argc, char **argv) {
    models_cfg_t c;
    memset(&c, 0, sizeof c);
    c.protocol = -1;
    char err[256] = "";

    /* argv[0]=cmd argv[1]="models": the flags start at argv+2, like the
       proxy and call parsers */
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--anthropic") || !strcmp(a, "--openai") ||
            !strcmp(a, "--openai-responses")) {
            if (c.protocol != -1) {
                usage_err(err, sizeof err, "protocol flag given twice");
                goto fail;
            }
            c.protocol = !strcmp(a, "--anthropic")     ? PROTO_ANTHROPIC
                         : !strcmp(a, "--openai")      ? PROTO_OPENAI
                                                       : PROTO_RESPONSES;
        } else if (!strcmp(a, "--key") || !strcmp(a, "--header")) {
            if (i + 1 >= argc) {
                usage_err(err, sizeof err, "missing value for %s", a);
                goto fail;
            }
            const char *v = argv[++i];
            if (!strcmp(a, "--key")) {
                free(c.key);
                c.key = strdup(v);
                if (!c.key) {
                    usage_err(err, sizeof err, "out of memory");
                    goto fail;
                }
            } else { /* --header */
                const char *eq = strchr(v, '=');
                if (!eq || eq == v || !eq[1]) {
                    usage_err(err, sizeof err,
                              "--header wants <name>=<value>");
                    goto fail;
                }
                char *name = strndup(v, (size_t)(eq - v));
                if (!name) {
                    usage_err(err, sizeof err, "out of memory");
                    goto fail;
                }
                int rc = hdr_add(&c, name, eq + 1);
                free(name);
                if (rc) {
                    usage_err(err, sizeof err, "out of memory");
                    goto fail;
                }
            }
        } else if (!strcmp(a, "--json")) {
            c.json = true;
        } else if (a[0] == '-' && a[1] == '-') {
            usage_err(err, sizeof err, "unknown flag '%s'", a);
            goto fail;
        } else {
            if (c.api_base) {
                usage_err(err, sizeof err, "unexpected extra argument '%s'",
                          a);
                goto fail;
            }
            c.api_base = strdup(a);
            if (!c.api_base) {
                usage_err(err, sizeof err, "out of memory");
                goto fail;
            }
        }
    }
    if (c.protocol == -1) {
        usage_err(err, sizeof err,
                  "missing protocol flag "
                  "(--anthropic, --openai or --openai-responses)");
        goto fail;
    }
    if (!c.api_base) {
        usage_err(err, sizeof err, "missing <api_base>");
        goto fail;
    }

    /* paginate: the endpoint's own cursor, one page per round */
    cJSON *list = cJSON_CreateObject();
    cJSON *data = cJSON_AddArrayToObject(list, "data");
    cJSON_AddStringToObject(list, "object", "list");
    size_t nmodels = 0;
    char after[512] = ""; /* endpoint-generated id */
    while (1) {
        buf_t body;
        buf_init(&body);
        long status = 0;
        err[0] = '\0';
        if (!fetch_page(&c, after, &body, &status, err, sizeof err)) {
            buf_free(&body);
            fprintf(stderr, "llmkit models: %s\n", err);
            cJSON_Delete(list);
            models_cfg_free(&c);
            return exit_code_of(strncmp(err, "connect_failed",
                                        sizeof "connect_failed" - 1) == 0
                                    ? EC_CONNECT_FAILED
                                    : EC_HTTP_ERROR);
        }
        cJSON *page = cJSON_ParseWithLength(body.data, body.len);
        buf_free(&body);
        if (!page) {
            fprintf(stderr, "llmkit models: api_error: bad json from "
                            "endpoint\n");
            cJSON_Delete(list);
            models_cfg_free(&c);
            return EXIT_API_ERROR;
        }
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(page, "data");
        if (!cJSON_IsArray(arr)) {
            fprintf(stderr, "llmkit models: api_error: no data array in "
                            "response\n");
            cJSON_Delete(page);
            cJSON_Delete(list);
            models_cfg_free(&c);
            return EXIT_API_ERROR;
        }
        const cJSON *m;
        cJSON_ArrayForEach(m, arr) {
            err[0] = '\0';
            cJSON *nm = model_normalize(m, err, sizeof err);
            if (!nm) {
                fprintf(stderr, "llmkit models: %s\n", err);
                cJSON_Delete(nm);
                cJSON_Delete(page);
                cJSON_Delete(list);
                models_cfg_free(&c);
                return EXIT_API_ERROR;
            }
            cJSON_AddItemToArray(data, nm);
            nmodels++;
        }
        /* cursor: openai "has_more"+"last id", anthropic "has_more"+
           "last id" ("first_id"/"last_id" are ids, not cursors - both
           providers page by an id in ?after=) */
        const cJSON *hm = cJSON_GetObjectItemCaseSensitive(page, "has_more");
        const cJSON *lid = cJSON_GetObjectItemCaseSensitive(page, "last_id");
        after[0] = '\0';
        if (cJSON_IsTrue(hm) && cJSON_IsString(lid) &&
            lid->valuestring[0] && strlen(lid->valuestring) < sizeof after)
            snprintf(after, sizeof after, "%s", lid->valuestring);
        cJSON_Delete(page);
        if (!after[0]) break;
    }

    if (c.json) {
        cJSON_AddNumberToObject(list, "count", (double)nmodels);
        char *s = cJSON_PrintUnformatted(list);
        if (!s) {
            fprintf(stderr, "llmkit models: out of memory\n");
            cJSON_Delete(list);
            models_cfg_free(&c);
            return EXIT_IO_ERROR;
        }
        puts(s);
        free(s);
    } else {
        print_table(data);
    }
    cJSON_Delete(list);
    models_cfg_free(&c);
    return EXIT_OK;

fail:
    fprintf(stderr, "llmkit models: %s\n", err);
    models_usage(stderr);
    models_cfg_free(&c);
    return EXIT_OUT_OF_CHANNEL;
}
