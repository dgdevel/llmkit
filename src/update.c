/* update.c - llmkit update: check github for a newer release and
   install it. plain installs self-replace via <exe>.new + rename;
   managed installs (a dpkg/pacman/rpm package owns llmkit) hand the
   release package to the system installer. windows prints a download
   hint only: self-replace of a running exe stays out of scope for v1.
   the tarball is verified with an in-tree sha256 and unwrapped with an
   in-tree gzip inflate, so the command needs nothing beyond libc,
   libcjson and libcurl. */
#include "llmkit.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#include <io.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define API_LATEST "https://api.github.com/repos/dgdevel/llmkit/releases/latest"
#define DL_BASE     "https://github.com/dgdevel/llmkit/releases/download/"

/* install flavors, in detection order */
enum { PKG_PLAIN = 0, PKG_DEB, PKG_RPM, PKG_PACMAN };

/* ================= version parsing / comparing ================= */

typedef struct {
    int maj, min, pat;
    bool ok;
} semver_t;

/* "v1.2.3", "1.2.3" and shorter prefixes ("v1", "1.2"); junk fails */
static semver_t parse_ver(const char *tag) {
    semver_t v = {0, 0, 0, false};
    const char *p = tag;
    if (*p == 'v' || *p == 'V') p++;
    if (*p < '0' || *p > '9') return v;
    int *fields[3] = {&v.maj, &v.min, &v.pat};
    for (int i = 0; i < 3; i++) {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || n < 0 || n > 100000) return v;
        *fields[i] = (int)n;
        p = end;
        if (*p == '.') {
            p++;
            continue;
        }
        break; /* shorter prefix: the rest stays 0 */
    }
    v.ok = *p == '\0';
    return v;
}

static int ver_cmp(semver_t a, semver_t b) {
    if (a.maj != b.maj) return a.maj < b.maj ? -1 : 1;
    if (a.min != b.min) return a.min < b.min ? -1 : 1;
    if (a.pat != b.pat) return a.pat < b.pat ? -1 : 1;
    return 0;
}

/* ================= http ================= */

/* one GET into a buf (binary-safe). 0 ok; -1 transport; -2 status. */
static int http_get(const char *url, const char *accept, buf_t *out,
                    char *err, size_t errsz) {
    struct curl_slist *hdrs = NULL;
    if (accept) http_hdr_add(&hdrs, "Accept", accept);
    http_req_t r;
    memset(&r, 0, sizeof r);
    r.url = url;
    r.hdrs = hdrs; /* freed by http_perform */
    r.is_get = true;
    r.ua = "llmkit/" LLMKIT_VERSION;
    r.connect_to = 10;
    r.read_to = 30;
    buf_init(&r.resp);
    buf_init(&r.content_type);
    int rc = http_perform(&r);
    buf_free(&r.content_type);
    if (rc == -1) {
        snprintf(err, errsz, "connect_failed: curl %d", (int)r.curl_res);
        buf_free(&r.resp);
        return -1;
    }
    if (rc != 0) {
        snprintf(err, errsz, "http_error: HTTP %ld", r.status);
        buf_free(&r.resp);
        return -2;
    }
    if (r.resp.data) buf_append(out, r.resp.data, r.resp.len);
    buf_free(&r.resp);
    return 0;
}

/* the latest release's tag ("v1.15.0"); NULL + err on failure */
static char *fetch_tag(char *err, size_t errsz) {
    buf_t resp;
    buf_init(&resp);
    err[0] = '\0';
    int rc = http_get(API_LATEST, "application/vnd.github+json", &resp, err,
                      errsz);
    if (rc != 0) {
        buf_free(&resp);
        return NULL;
    }
    cJSON *rel = cJSON_ParseWithLength(resp.data ? resp.data : "", resp.len);
    buf_free(&resp);
    if (!rel) {
        snprintf(err, errsz, "api_error: bad json from github");
        return NULL;
    }
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(rel, "tag_name");
    char *out = NULL;
    if (cJSON_IsString(tag) && tag->valuestring && tag->valuestring[0])
        out = strdup(tag->valuestring);
    cJSON_Delete(rel);
    if (!out) snprintf(err, errsz, "api_error: no tag_name in response");
    return out;
}

#ifndef _WIN32

/* ================= sha256 (fips 180-4, self-contained) ================= */

typedef struct {
    uint32_t h[8];
    uint64_t bits; /* message length in bits, before padding */
    uint8_t block[64];
    size_t fill;
} sha256_t;

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
    0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
    0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) +
                      ((e & f) ^ ((~e) & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(sha256_t *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->bits = 0;
    s->fill = 0;
}

static void sha_update(sha256_t *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t k = 64 - s->fill;
        if (k > n) k = n;
        memcpy(s->block + s->fill, p, k);
        s->fill += k;
        p += k;
        n -= k;
        if (s->fill == 64) {
            sha_block(s, s->block);
            s->fill = 0;
        }
    }
}

/* pads, flushes the last blocks and writes the 32 digest bytes */
static void sha_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->bits;
    sha_update(s, "\x80", 1);
    uint8_t zero = 0;
    while (s->fill != 56) sha_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha_update(s, lenb, 8); /* fills 56 -> 64 and flushes */
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

/* the one-shot form the command needs: lowercase hex digest */
static void sha256_hex(const void *data, size_t n, char out[65]) {
    sha256_t s;
    sha_init(&s);
    sha_update(&s, data, n);
    uint8_t d[32];
    sha_final(&s, d);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexd[d[i] >> 4];
        out[i * 2 + 1] = hexd[d[i] & 0xf];
    }
    out[64] = '\0';
}

/* ================= gzip inflate (rfc 1951/1952, self-contained) ======
   the release tarball only, so the subset is the one github's gzip
   produces: one stream, deflate blocks, no preset dict. the trailing
   crc32 and isize are not checked - the published sha256 already
   gates the archive. */

typedef struct {
    const uint8_t *p; /* input */
    size_t n, pos;    /* length, read cursor */
    uint32_t bitbuf;  /* lsb-first bit accumulator */
    int bitcnt;
} inflate_t;

static int inf_bits(inflate_t *z, int want) {
    while (z->bitcnt < want) {
        if (z->pos >= z->n) return -1; /* truncated input */
        z->bitbuf |= (uint32_t)z->p[z->pos++] << z->bitcnt;
        z->bitcnt += 8;
    }
    int v = (int)(z->bitbuf & ((1u << want) - 1));
    z->bitbuf >>= want;
    z->bitcnt -= want;
    return v;
}

/* canonical huffman table: codes per length plus the symbols sorted by
   (length, symbol); decoded without materializing a tree */
typedef struct {
    int count[16];
    int symbol[288];
} huff_t;

static int huff_build(huff_t *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0; /* unused symbols are not codes */
    int offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = i;
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1; /* over-subscribed */
    }
    return 0;
}

static int huff_decode(inflate_t *z, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        int b = inf_bits(z, 1);
        if (b < 0) return -1;
        code |= b;
        int cnt = h->count[l];
        if (code - cnt < first) return h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
    4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13};

/* the fixed literal/length and distance tables */
static void fixed_tables(huff_t *lit, huff_t *dist) {
    uint8_t lens[288];
    int i = 0;
    for (; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    huff_build(lit, lens, 288);
    for (i = 0; i < 30; i++) lens[i] = 5;
    huff_build(dist, lens, 30);
}

/* one compressed block appended to out; false on malformed input */
static bool inf_block(inflate_t *z, buf_t *out, const huff_t *lit,
                      const huff_t *dist) {
    for (;;) {
        int sym = huff_decode(z, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            buf_append_byte(out, (char)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int le = inf_bits(z, len_extra[sym]);
            if (le < 0) return false;
            size_t len = len_base[sym] + (size_t)le;
            int ds = huff_decode(z, dist);
            if (ds < 0 || ds >= 30) return false;
            int de = inf_bits(z, dist_extra[ds]);
            if (de < 0) return false;
            size_t d = dist_base[ds] + (size_t)de;
            if (!out->data || d > out->len) return false; /* pre-history */
            for (size_t k = 0; k < len; k++)
                buf_append_byte(out, out->data[out->len - d]);
        }
    }
}

/* the dynamic block header: the rle-coded code length list */
static bool inf_dynamic(inflate_t *z, huff_t *lit, huff_t *dist) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int hlit = inf_bits(z, 5), hdist = inf_bits(z, 5), hclen = inf_bits(z, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    uint8_t clens[19] = {0};
    for (int i = 0; i < hclen; i++) {
        int l = inf_bits(z, 3);
        if (l < 0) return false;
        clens[order[i]] = (uint8_t)l;
    }
    huff_t cl;
    if (huff_build(&cl, clens, 19) != 0) return false;
    uint8_t lens[288 + 32] = {0};
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(z, &cl);
        if (sym < 0) return false;
        if (sym < 16) {
            lens[n++] = (uint8_t)sym;
        } else if (sym == 16) { /* repeat the previous length */
            if (!n) return false;
            int rep = inf_bits(z, 2);
            if (rep < 0) return false;
            uint8_t prev = lens[n - 1];
            for (int k = 0; k < rep + 3 && n < hlit + hdist; k++)
                lens[n++] = prev;
        } else if (sym == 17) { /* short zero run */
            int rep = inf_bits(z, 3);
            if (rep < 0) return false;
            for (int k = 0; k < rep + 3 && n < hlit + hdist; k++)
                lens[n++] = 0;
        } else { /* 18: long zero run */
            int rep = inf_bits(z, 7);
            if (rep < 0) return false;
            for (int k = 0; k < rep + 11 && n < hlit + hdist; k++)
                lens[n++] = 0;
        }
    }
    if (huff_build(lit, lens, hlit) != 0) return false;
    if (huff_build(dist, lens + hlit, hdist) != 0) return false;
    return true;
}

/* one gzip member (rfc 1952 header + deflate stream) into out */
static bool gunzip(const uint8_t *p, size_t n, buf_t *out) {
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return false;
    size_t pos = 10;
    uint8_t flg = p[3];
    if (flg & 0x04) { /* FEXTRA */
        if (pos + 2 > n) return false;
        pos += 2 + (size_t)p[pos] + ((size_t)p[pos + 1] << 8);
    }
    if (flg & 0x08) { /* FNAME */
        while (pos < n && p[pos]) pos++;
        pos++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (pos < n && p[pos]) pos++;
        pos++;
    }
    if (flg & 0x02) pos += 2; /* FHCRC */
    if (pos >= n) return false;
    inflate_t z;
    memset(&z, 0, sizeof z);
    z.p = p;
    z.n = n;
    z.pos = pos;
    huff_t lit, dist;
    fixed_tables(&lit, &dist);
    for (;;) {
        int final = inf_bits(&z, 1);
        int type = inf_bits(&z, 2);
        if (final < 0 || type < 0) return false;
        if (type == 0) {
            /* stored: byte aligned. drop the sub-byte remainder, then
               unread the whole bytes still in the accumulator */
            z.pos -= (size_t)z.bitcnt / 8;
            z.bitbuf = 0;
            z.bitcnt = 0;
            if (z.pos + 4 > z.n) return false;
            size_t len = z.p[z.pos] | ((size_t)z.p[z.pos + 1] << 8);
            size_t nlen = z.p[z.pos + 2] | ((size_t)z.p[z.pos + 3] << 8);
            if ((len ^ 0xffff) != nlen) return false;
            z.pos += 4;
            if (z.pos + len > z.n) return false;
            buf_append(out, z.p + z.pos, len);
            z.pos += len;
        } else if (type == 1) {
            if (!inf_block(&z, out, &lit, &dist)) return false;
        } else if (type == 2) {
            huff_t dl, dd;
            if (!inf_dynamic(&z, &dl, &dd)) return false;
            if (!inf_block(&z, out, &dl, &dd)) return false;
        } else {
            return false;
        }
        if (final) break;
    }
    return true;
}

/* ================= tar ================= */

/* the first member whose path ends in "/llmkit" (the release tarball
   has exactly one directory level, no pax/gnu long names). 0 + slice
   on success, -1 when absent or truncated; the bytes stay owned by
   the archive buf. */
static int tar_member(const buf_t *tar, const char **out, size_t *out_len) {
    size_t off = 0;
    while (off + 512 <= tar->len) {
        const char *hdr = tar->data + off;
        if (hdr[0] == '\0') return -1; /* end-of-archive blocks */
        char name[300];
        size_t nl = 0;
        while (nl < 100 && hdr[nl]) nl++;
        if (!memcmp(hdr + 257, "ustar", 5) && hdr[345]) {
            /* posix magic + non-empty prefix: prefix/name */
            size_t pl = 0;
            while (pl < 155 && hdr[345 + pl]) pl++;
            if (pl + 1 + nl >= sizeof name) return -1;
            memcpy(name, hdr + 345, pl);
            name[pl] = '/';
            memcpy(name + pl + 1, hdr, nl);
            name[pl + 1 + nl] = '\0';
            nl += pl + 1;
        } else {
            memcpy(name, hdr, nl);
            name[nl] = '\0';
        }
        /* size: 12 octal bytes at offset 124, space/NUL terminated */
        size_t size = 0;
        bool ok = true;
        for (int i = 0; i < 12; i++) {
            char c = hdr[124 + i];
            if (c == ' ' || c == '\0') break;
            if (c < '0' || c > '7') {
                ok = false;
                break;
            }
            size = size * 8 + (size_t)(c - '0');
        }
        if (!ok) return -1;
        if (off + 512 + size > tar->len) return -1; /* truncated member */
        if (nl > 7 && !memcmp(name + nl - 7, "/llmkit", 7)) {
            *out = tar->data + off + 512;
            *out_len = size;
            return 0;
        }
        off += 512 + ((size + 511) & ~(size_t)511);
    }
    return -1;
}

/* ================= prompting ================= */

/* one yes/no question on stdin: y/Y/yes/YES count, anything else no */
static bool confirm(const char *question) {
    printf("%s [y/N] ", question);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof line, stdin)) return false;
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *e = p;
    while (*e && *e != '\r' && *e != '\n') e++;
    *e = '\0';
    return !strcasecmp(p, "y") || !strcasecmp(p, "yes");
}

/* ================= install detection ================= */

/* quiet command run via system(): a terminal cli command, no streaming
   involved. returns the raw wait status */
static int run_quiet(const char *cmd) {
    char full[256];
    snprintf(full, sizeof full, "%s >/dev/null 2>&1", cmd);
    return system(full);
}

static bool have_cmd(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

/* the first line of a command's stdout, trimmed; "" when it fails */
static void capture_line(const char *cmd, char *out, size_t outsz) {
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return;
    if (!fgets(out, (int)outsz, f)) {
        pclose(f);
        return;
    }
    char *e = out + strlen(out);
    while (e > out && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    pclose(f);
}

/* uname -m mapped to the deb architecture naming */
static void deb_arch(char *out, size_t outsz) {
    char m[64];
    capture_line("uname -m", m, sizeof m);
    if (!strcmp(m, "x86_64")) snprintf(out, outsz, "amd64");
    else if (!strcmp(m, "aarch64")) snprintf(out, outsz, "arm64");
    else if (m[0]) snprintf(out, outsz, "%s", m);
    else snprintf(out, outsz, "amd64");
}

/* the rpm distro serie: el9 for the enterprise family, else fc<N> from
   os-release VERSION_ID, else fc41 */
static void rpm_serie(char *out, size_t outsz) {
    snprintf(out, outsz, "fc41");
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;
    char variant[128] = "", version[128] = "";
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *val = NULL;
        if (!strncmp(line, "VARIANT_ID=", 11)) val = variant;
        else if (!strncmp(line, "VERSION_ID=", 11)) val = version;
        if (!val) continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        snprintf(val, 128, "%.116s", line + 11);
    }
    fclose(f);
    if (variant[0] && (!strncmp(variant, "el", 2) ||
                       !strcmp(variant, "rocky") ||
                       !strcmp(variant, "almalinux"))) {
        snprintf(out, outsz, "el9");
        return;
    }
    if (version[0]) {
        int maj = atoi(version);
        if (maj > 0) snprintf(out, outsz, "fc%d", maj);
    }
}

/* the release asset of one flavor for plain version "1.15.0" */
static const char *asset_name(int flavor, const char *plain) {
    static char name[128];
    switch (flavor) {
    case PKG_DEB: {
        char arch[16];
        deb_arch(arch, sizeof arch);
        snprintf(name, sizeof name, "llmkit_%s-1_%s.deb", plain, arch);
        return name;
    }
    case PKG_RPM: {
        char serie[16];
        rpm_serie(serie, sizeof serie);
        snprintf(name, sizeof name, "llmkit-%s-1.%s.x86_64.rpm", plain, serie);
        return name;
    }
    case PKG_PACMAN:
        snprintf(name, sizeof name, "llmkit-%s-1-x86_64.pkg.tar.zst", plain);
        return name;
    default:
        snprintf(name, sizeof name, "llmkit-v%s-linux-x86_64.tar.gz", plain);
        return name;
    }
}

/* how this llmkit was installed: the package manager queries first,
   plain rename-over otherwise - also for an exe under /usr or
   /usr/local that no known manager claims */
static int detect_install(void) {
    if (run_quiet("dpkg -s llmkit") == 0) return PKG_DEB;
    if (run_quiet("pacman -Qi llmkit") == 0) return PKG_PACMAN;
    if (run_quiet("rpm -q llmkit") == 0) return PKG_RPM;
    return PKG_PLAIN;
}

/* ================= the plain self-replace ================= */

static int install_plain(const char *exe, const char *payload,
                         size_t payload_len, char *err, size_t errsz) {
    char newpath[4096 + 8];
    snprintf(newpath, sizeof newpath, "%s.new", exe);
    FILE *f = fopen(newpath, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot write %s: %s", path_basename(newpath),
                 strerror(errno));
        return -1;
    }
    size_t wrote = payload_len ? fwrite(payload, 1, payload_len, f) : 0;
    int bad = ferror(f) ? 1 : 0;
    if (fclose(f) != 0) bad = 1;
    if (bad || wrote != payload_len) {
        remove(newpath);
        snprintf(err, errsz, "short write on %s", path_basename(newpath));
        return -1;
    }
    if (chmod(newpath, 0755) != 0) {
        remove(newpath);
        snprintf(err, errsz, "cannot chmod %s: %s", path_basename(newpath),
                 strerror(errno));
        return -1;
    }
    if (rename(newpath, exe) != 0) { /* same dir: same filesystem */
        remove(newpath);
        snprintf(err, errsz, "cannot replace %s: %s", path_basename(exe),
                 strerror(errno));
        return -1;
    }
    return 0;
}

#endif /* !_WIN32 */

/* ================= the command ================= */

static void update_usage(FILE *out) {
    fputs("usage: llmkit update [--yes]\n"
          "  check github for a newer release and install it\n"
          "  --yes   skip the confirmation prompt\n",
          out);
}

int cmd_update(int argc, char **argv) {
    bool yes = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--yes")) yes = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            update_usage(stdout);
            return EXIT_OK;
        } else {
            fprintf(stderr, "llmkit update: unknown flag '%s'\n", argv[i]);
            update_usage(stderr);
            return EXIT_OUT_OF_CHANNEL;
        }
    }

    /* 1. where are we, and can we touch it */
    char exe[4096];
    self_exe(exe, sizeof exe, argv ? argv[0] : NULL);
#ifndef _WIN32
    struct stat st;
    if (stat(exe, &st) != 0) {
        fprintf(stderr, "llmkit update: cannot stat %s: %s\n", exe,
                strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "llmkit update: %s is not a regular file\n", exe);
        return EXIT_IO_ERROR;
    }
    /* not writable is fatal only when no package manager owns the
       install - the managed flow below runs sudo instead */
    bool writable = access(exe, W_OK) == 0;
#endif

    /* 2. the latest release */
    char err[256] = "";
    char *tag = fetch_tag(err, sizeof err);
    if (!tag) {
        fprintf(stderr, "llmkit update: %s\n", err);
        return strstr(err, "connect_failed") ? EXIT_CONNECT_FAILED
                                             : EXIT_API_ERROR;
    }
    semver_t cur = parse_ver(LLMKIT_VERSION);
    semver_t latest = parse_ver(tag);
    if (!latest.ok) {
        fprintf(stderr, "llmkit update: api_error: unparseable tag '%s'\n",
                tag);
        free(tag);
        return EXIT_API_ERROR;
    }
    if (cur.ok && ver_cmp(cur, latest) >= 0) {
        printf("llmkit %s is up to date\n", LLMKIT_VERSION);
        free(tag);
        return EXIT_OK;
    }
    const char *plain = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;

#ifdef _WIN32
    /* hint-only: self-replace of a running exe is fragile */
    printf("llmkit %s -> %s available\n", LLMKIT_VERSION, plain);
    printf("windows self-update is not implemented; download and extract "
           "manually:\n  %s%s/llmkit-%s-windows-x86_64.zip\n",
           DL_BASE, tag, plain);
    free(tag);
    return EXIT_OK;
#else
    /* how the install is owned decides the asset and the installer;
       a read-only exe with no manager behind it is the one dead end */
    int flavor = detect_install();
    if (!writable && flavor == PKG_PLAIN) {
        fprintf(stderr, "llmkit update: %s is not writable; use the package "
                        "manager or install to a user-writable path\n",
                exe);
        free(tag);
        return EXIT_IO_ERROR;
    }

    /* 3. confirm; a piped stdin cannot answer */
    char q[96];
    snprintf(q, sizeof q, "update %s -> %s?", LLMKIT_VERSION, plain);
    if (!yes) {
        if (!isatty(0)) {
            fprintf(stderr,
                    "llmkit update: update %s -> %s available; stdin is not "
                    "a terminal, rerun with --yes to apply\n",
                    LLMKIT_VERSION, plain);
            free(tag);
            return 2;
        }
        if (!confirm(q)) {
            printf("declined\n");
            free(tag);
            return EXIT_OK;
        }
    }

    /* 4. the checksum of the asset this machine wants */
    const char *asset = asset_name(flavor, plain);
    char url[512], csum_url[512];
    snprintf(url, sizeof url, DL_BASE "%s/%s", tag, asset);
    snprintf(csum_url, sizeof csum_url, DL_BASE "%s/checksums-%s.txt", tag,
             tag);
    buf_t csums;
    buf_init(&csums);
    int rc = http_get(csum_url, NULL, &csums, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "llmkit update: %s\n", err);
        buf_free(&csums);
        free(tag);
        return rc == -1 ? EXIT_CONNECT_FAILED : EXIT_HTTP_ERROR;
    }
    /* find "<sha256>  <asset>" among the lines (sha256sum format) */
    char want[65] = "";
    {
        const char *line = csums.data ? csums.data : "";
        size_t alen = strlen(asset);
        while (*line) {
            const char *nl = strchr(line, '\n');
            size_t ll = nl ? (size_t)(nl - line) : strlen(line);
            if (ll == 64 + 2 + alen && line[64] == ' ' && line[65] == ' ' &&
                !memcmp(line + 66, asset, alen)) {
                memcpy(want, line, 64);
                want[64] = '\0';
                break;
            }
            line = nl ? nl + 1 : line + ll;
        }
    }
    buf_free(&csums);
    if (!want[0]) {
        fprintf(stderr, "llmkit update: no checksum line for %s\n", asset);
        free(tag);
        return EXIT_API_ERROR;
    }

    /* 5. download and verify */
    buf_t pkg;
    buf_init(&pkg);
    rc = http_get(url, NULL, &pkg, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "llmkit update: %s\n", err);
        buf_free(&pkg);
        free(tag);
        return rc == -1 ? EXIT_CONNECT_FAILED : EXIT_HTTP_ERROR;
    }
    char got[65];
    sha256_hex(pkg.data, pkg.len, got);
    if (strcmp(want, got)) {
        fprintf(stderr, "llmkit update: checksum mismatch for %s\n", asset);
        buf_free(&pkg);
        free(tag);
        return EXIT_API_ERROR;
    }

    /* 6a. plain install: gunzip, find the member, self-replace */
    if (flavor == PKG_PLAIN) {
        buf_t tar;
        buf_init(&tar);
        if (!gunzip((const uint8_t *)(pkg.data ? pkg.data : ""), pkg.len,
                    &tar)) {
            buf_free(&tar);
            buf_free(&pkg);
            free(tag);
            fprintf(stderr, "llmkit update: cannot decompress %s\n", asset);
            return EXIT_API_ERROR;
        }
        const char *payload;
        size_t payload_len;
        if (tar_member(&tar, &payload, &payload_len) != 0) {
            buf_free(&tar);
            buf_free(&pkg);
            fprintf(stderr, "llmkit update: no llmkit member in %s\n", asset);
            free(tag);
            return EXIT_API_ERROR;
        }
        char ierr[256] = "";
        int irc = install_plain(exe, payload, payload_len, ierr, sizeof ierr);
        buf_free(&tar);
        buf_free(&pkg);
        if (irc != 0) {
            fprintf(stderr, "llmkit update: %s\n", ierr);
            free(tag);
            return EXIT_IO_ERROR;
        }
        printf("llmkit %s installed. run `llmkit version` to confirm\n",
               plain);
        free(tag);
        return EXIT_OK;
    }

    /* 6b. managed install: the package goes to the system installer */
    const char *verb = flavor == PKG_DEB     ? "dpkg -i"
                       : flavor == PKG_RPM   ? "dnf install"
                                             : "pacman -U";
    bool sudo = geteuid() != 0 && have_cmd("sudo");
    printf("package install detected (%s)\n", asset);
    printf("download: %s\n", url);
    if (!confirm("run the installer now?")) {
        buf_free(&pkg);
        free(tag);
        printf("declined; run it yourself:\n  %s%s <file>\n",
               sudo ? "sudo " : "", verb);
        return EXIT_OK;
    }
    /* the package is verified in memory; the installer wants a file */
    char tmp[] = "/tmp/llmkit-update-pkg.XXXXXX";
    int fd = mkstemp(tmp);
    FILE *pf = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!pf) {
        if (fd >= 0) close(fd);
        buf_free(&pkg);
        free(tag);
        fprintf(stderr, "llmkit update: cannot create %s\n", tmp);
        return EXIT_IO_ERROR;
    }
    fwrite(pkg.data, 1, pkg.len, pf);
    int bad = ferror(pf) ? 1 : 0;
    fclose(pf);
    if (bad) {
        remove(tmp);
        buf_free(&pkg);
        free(tag);
        fprintf(stderr, "llmkit update: short write on %s\n", tmp);
        return EXIT_IO_ERROR;
    }
    char syscmd[600];
    snprintf(syscmd, sizeof syscmd, "%s%s %s", sudo ? "sudo " : "", verb, tmp);
    printf("%s\n", syscmd);
    fflush(stdout);
    int status = system(syscmd);
    remove(tmp);
    buf_free(&pkg);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "llmkit update: installer failed\n");
        free(tag);
        return EXIT_IO_ERROR;
    }
    printf("llmkit %s installed. run `llmkit version` to confirm\n", plain);
    free(tag);
    return EXIT_OK;
#endif /* _WIN32 */
}
