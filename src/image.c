/* image.c - image content blocks: path -> block (read, sniff, encode),
   url -> block. */
#include "llmkit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int read_whole(const char *path, buf_t *out, unsigned long long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char tmp[16384];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (out->len + (unsigned long long)n > cap) {
            fclose(f);
            return -2;
        }
        buf_append(out, tmp, n);
    }
    bool bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

static const char *sniff_media_type(const unsigned char *d, size_t n,
                                    const char *path) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A)
        return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return "image/jpeg";
    if (n >= 6 && !memcmp(d, "GIF8", 4) && (d[4] == '7' || d[4] == '9') && d[5] == 'a')
        return "image/gif";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "image/webp";
    /* extension fallback */
    const char *dot = strrchr(path, '.');
    if (dot) {
        if (!strcasecmp(dot, ".png")) return "image/png";
        if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))
            return "image/jpeg";
        if (!strcasecmp(dot, ".gif")) return "image/gif";
        if (!strcasecmp(dot, ".webp")) return "image/webp";
    }
    return NULL;
}

cJSON *image_block_from_path(const char *path, char *err, size_t errsz) {
    buf_t b;
    buf_init(&b);
    int rc = read_whole(path, &b, (unsigned long long)LLMKIT_IMAGE_MAX);
    if (rc == -1) {
        snprintf(err, errsz, "cannot read image '%s'", path);
        buf_free(&b);
        return NULL;
    }
    if (rc == -2) {
        snprintf(err, errsz, "image '%s' exceeds 5 MiB limit", path);
        buf_free(&b);
        return NULL;
    }
    const char *mt = sniff_media_type((const unsigned char *)b.data, b.len, path);
    if (!mt) {
        snprintf(err, errsz, "unrecognized image type '%s'", path);
        buf_free(&b);
        return NULL;
    }
    buf_t b64;
    buf_init(&b64);
    buf_append_b64(&b64, b.data, b.len);
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "image");
    cJSON *src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "type", "base64");
    cJSON_AddStringToObject(src, "media_type", mt);
    cJSON_AddStringToObject(src, "data", b64.data ? b64.data : "");
    cJSON_AddItemToObject(blk, "source", src);
    buf_free(&b64);
    buf_free(&b);
    return blk;
}

cJSON *image_block_from_url(const char *url) {
    if (!url || !*url) return NULL;
    size_t n = strlen(url);
    const char *host;
    size_t sl;
    if (!strncmp(url, "https://", 8)) { host = url + 8; sl = 8; }
    else if (!strncmp(url, "http://", 7)) { host = url + 7; sl = 7; }
    else return NULL;
    size_t hl = n - sl;
    const char *slash = strchr(host, '/');
    if (slash) hl = (size_t)(slash - host);
    if (hl == 0) return NULL;
    cJSON *blk = cJSON_CreateObject();
    cJSON_AddStringToObject(blk, "type", "image");
    cJSON *src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "type", "url");
    cJSON_AddStringToObject(src, "url", url);
    cJSON_AddItemToObject(blk, "source", src);
    return blk;
}

char *image_block_desc(const cJSON *blk) {
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(blk, "source");
    if (!cJSON_IsObject(src)) return strdup("image");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(src, "type");
    if (cJSON_IsString(st) && !strcmp(st->valuestring, "url")) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(src, "url");
        const char *us = cJSON_IsString(u) ? u->valuestring : "?";
        size_t n = strlen(us);
        char *d = malloc(n + 32);
        if (!d) return strdup("image/url");
        snprintf(d, n + 32, "image/url %.60s", us);
        return d;
    }
    const cJSON *mt = cJSON_GetObjectItemCaseSensitive(src, "media_type");
    const cJSON *dt = cJSON_GetObjectItemCaseSensitive(src, "data");
    const char *m = cJSON_IsString(mt) ? mt->valuestring : "image";
    size_t dl = cJSON_IsString(dt) ? strlen(dt->valuestring) : 0;
    unsigned long long bytes = (unsigned long long)dl * 3 / 4;
    char *d = malloc(128);
    if (!d) return strdup(m);
    snprintf(d, 128, "%s, %.1fM b64", m, (double)dl / (1024.0 * 1024.0));
    (void)bytes;
    return d;
}
