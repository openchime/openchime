/*
 * OpenChime client — the release notes for the running build. See releasenotes.h.
 */

#include "releasenotes.h"
#include "https_get.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jsmn's implementation, private to this file: the daemon carries its own copy
 * (daemon/jwt.c), and the test program links both, so its two public functions
 * take names of their own here. */
#define jsmn_init  oc_rn_jsmn_init
#define jsmn_parse oc_rn_jsmn_parse
#include "jsmn.h"

typedef struct { const char *js; jsmntok_t *t; int n; } rn_doc;

static int rn_skip(const rn_doc *d, int i) {
    int end = i + 1;
    if (d->t[i].type == JSMN_OBJECT || d->t[i].type == JSMN_ARRAY) {
        int kids = d->t[i].size * (d->t[i].type == JSMN_OBJECT ? 2 : 1);
        for (int k = 0; k < kids && end < d->n; k++) end = rn_skip(d, end);
    }
    return end;
}

static int rn_get(const rn_doc *d, int obj, const char *key) {
    if (obj < 0 || obj >= d->n || d->t[obj].type != JSMN_OBJECT) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int k = 0; k < d->t[obj].size && i + 1 < d->n; k++) {
        const jsmntok_t *kt = &d->t[i];
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == kl &&
            !memcmp(d->js + kt->start, key, kl)) return i + 1;
        i = rn_skip(d, i + 1);
    }
    return -1;
}

static int rn_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int rn_hex4(const char *p, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) { int h = rn_hex(p[i]); if (h < 0) return 0; *v = (*v << 4) | (uint32_t)h; }
    return 1;
}

/* The string at token `i`, unescaped into `out`: 0, or -1 when it is not a
 * string, carries a bad escape or a NUL, or does not fit whole. */
static int rn_str(const rn_doc *d, int i, char *out, size_t cap) {
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_STRING || !cap) return -1;
    const char *s = d->js + d->t[i].start;
    size_t n = (size_t)(d->t[i].end - d->t[i].start), o = 0;
    for (size_t k = 0; k < n; k++) {
        uint32_t cp;
        if (s[k] != '\\') { if (!s[k] || o + 1 >= cap) return -1; out[o++] = s[k]; continue; }
        if (++k >= n) return -1;
        switch (s[k]) {
        case '"': cp = '"'; break;  case '\\': cp = '\\'; break; case '/': cp = '/'; break;
        case 'b': cp = '\b'; break; case 'f': cp = '\f'; break;  case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break; case 't': cp = '\t'; break;
        case 'u':
            if (k + 4 >= n || !rn_hex4(s + k + 1, &cp)) return -1;
            k += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (k + 6 >= n || s[k + 1] != '\\' || s[k + 2] != 'u' || !rn_hex4(s + k + 3, &lo) ||
                    lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                k += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) return -1;
            break;
        default: return -1;
        }
        if (!cp) return -1;
        unsigned char u[4]; size_t ul;
        if (cp < 0x80)         { u[0] = (unsigned char)cp; ul = 1; }
        else if (cp < 0x800)   { u[0] = (unsigned char)(0xC0 | (cp >> 6)); u[1] = (unsigned char)(0x80 | (cp & 0x3F)); ul = 2; }
        else if (cp < 0x10000) { u[0] = (unsigned char)(0xE0 | (cp >> 12)); u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | (cp & 0x3F)); ul = 3; }
        else                   { u[0] = (unsigned char)(0xF0 | (cp >> 18)); u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (unsigned char)(0x80 | (cp & 0x3F)); ul = 4; }
        if (o + ul >= cap) return -1;
        memcpy(out + o, u, ul);
        o += ul;
    }
    out[o] = '\0';
    return 0;
}

/* The k-th element of the array at `arr`, or -1. */
static int rn_elem(const rn_doc *d, int arr, int k) {
    if (arr < 0 || arr >= d->n || d->t[arr].type != JSMN_ARRAY || k >= d->t[arr].size) return -1;
    int i = arr + 1;
    for (int q = 0; q < k && i < d->n; q++) i = rn_skip(d, i);
    return i < d->n ? i : -1;
}

/* Only an https address is offered as a link: anything else in an item's url
 * would be a thing the client opened on the control plane's say-so. */
static int rn_https(const char *u) { return strncmp(u, "https://", 8) == 0; }

static int rn_parse(const char *js, size_t len, oc_releasenotes *out) {
    jsmn_parser p;
    int cap = 64;
    rn_doc d = { js, NULL, 0 };
    for (;;) {
        jsmntok_t *t = realloc(d.t, (size_t)cap * sizeof *t);
        if (!t) { free(d.t); return OC_RN_FAILED; }
        d.t = t;
        jsmn_init(&p);
        int n = jsmn_parse(&p, js, len, d.t, (unsigned)cap);
        if (n == JSMN_ERROR_NOMEM && cap < 32768) { cap *= 4; continue; }
        if (n <= 0) { free(d.t); return OC_RN_FAILED; }
        d.n = n;
        break;
    }
    int rc = OC_RN_FAILED;
    if (d.t[0].type != JSMN_OBJECT) goto done;
    {   /* release: a positive number */
        int r = rn_get(&d, 0, "release");
        if (r < 0 || d.t[r].type != JSMN_PRIMITIVE) goto done;
        long v = 0;
        for (int k = d.t[r].start; k < d.t[r].end; k++) {
            if (js[k] < '0' || js[k] > '9' || v > 100000000) goto done;
            v = v * 10 + (js[k] - '0');
        }
        if (v <= 0) goto done;
        out->release = (int)v;
    }
    {   /* published_at: its date, the first ten characters of the timestamp */
        char ts[64];
        if (rn_str(&d, rn_get(&d, 0, "published_at"), ts, sizeof ts) == 0 && strlen(ts) >= 10) {
            memcpy(out->date, ts, 10);
            out->date[10] = '\0';
        }
    }
    int secs = rn_get(&d, 0, "sections");
    if (secs < 0 || d.t[secs].type != JSMN_ARRAY || d.t[secs].size > OC_RN_SECTIONS) goto done;
    for (int s = 0, si; (si = rn_elem(&d, secs, s)) >= 0; s++) {
        oc_rn_section *sec = &out->sections[out->n_sections];
        if (d.t[si].type != JSMN_OBJECT || rn_str(&d, rn_get(&d, si, "title"), sec->title, sizeof sec->title) != 0)
            goto done;
        int items = rn_get(&d, si, "items");
        if (items < 0 || d.t[items].type != JSMN_ARRAY || d.t[items].size > OC_RN_ITEMS) goto done;
        for (int k = 0, ii; (ii = rn_elem(&d, items, k)) >= 0; k++) {
            oc_rn_item *it = &sec->items[sec->n_items];
            if (d.t[ii].type != JSMN_OBJECT || rn_str(&d, rn_get(&d, ii, "title"), it->title, sizeof it->title) != 0)
                goto done;
            if (rn_str(&d, rn_get(&d, ii, "url"), it->url, sizeof it->url) != 0 || !rn_https(it->url))
                it->url[0] = '\0';
            sec->n_items++;
        }
        out->n_sections++;
    }
    rc = OC_RN_OK;
done:
    free(d.t);
    return rc;
}

int oc_releasenotes_askable(const char *b) {
    if (!b || !b[0] || !strcmp(b, "dev") || strlen(b) > 64) return 0;
    for (const char *p = b; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '.' || *p == '_')) return 0;
    return 1;
}

int oc_releasenotes_read(int status, const char *body, size_t len, oc_releasenotes *out) {
    if (!out) return OC_RN_FAILED;
    memset(out, 0, sizeof *out);
    if (status == 404) return OC_RN_NOT_RELEASE;
    if (status != 200 || !body || !len || len > OC_RN_MAX_BODY) return OC_RN_FAILED;
    int rc = rn_parse(body, len, out);
    if (rc != OC_RN_OK) memset(out, 0, sizeof *out);
    return rc;
}

int oc_releasenotes_read_response(const char *resp, size_t len, oc_releasenotes *out) {
    const char *body = NULL;
    int st = oc_http_response_status(resp, len, &body);
    if (st == 200 && !body) { if (out) memset(out, 0, sizeof *out); return OC_RN_FAILED; }
    return oc_releasenotes_read(st, body, body ? len - (size_t)(body - resp) : 0, out);
}

int oc_releasenotes_fetch(const char *build_id, oc_releasenotes *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!out) return OC_RN_FAILED;
    if (!oc_releasenotes_askable(build_id)) return OC_RN_NOT_RELEASE;
    char path[160];
    snprintf(path, sizeof path, "%s%s", OC_RN_API_PATH, build_id);
    char *buf = malloc(OC_RN_MAX_BODY + 4096);
    if (!buf) return OC_RN_FAILED;
    long n = oc_https_get(OC_RN_HOST, path, "application/json", 8000, buf, OC_RN_MAX_BODY + 4096);
    int rc = n > 0 ? oc_releasenotes_read_response(buf, (size_t)n, out) : OC_RN_FAILED;
    free(buf);
    return rc;
}
