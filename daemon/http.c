/* Minimal HTTP/1.1 request reader for the webhook endpoint — see http.h. */

#include "http.h"
#define JSMN_HEADER   /* jwt.c carries the jsmn implementation; we take declarations */
#include "jsmn.h"

#include <stdint.h>
#include <string.h>

/* Bound header bytes so a peer can't make us buffer headers forever before the
 * terminator arrives. Generous for a webhook (a few small headers). */
#define OC_HTTP_MAX_HEADERS 8192u

static int ci_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

/* Trim leading spaces/tabs, then return the length up to CR/LF. */
static void trim_value(const char **v, size_t *n) {
    while (*n && (**v == ' ' || **v == '\t')) { (*v)++; (*n)--; }
    size_t k = 0;
    while (k < *n && (*v)[k] != '\r' && (*v)[k] != '\n') k++;
    *n = k;
}

int oc_http_parse(const char *buf, size_t len, size_t max_body, oc_http_req *req) {
    memset(req, 0, sizeof *req);

    /* Find the end of the header block. */
    const char *hdr_end = NULL;
    if (len >= 4) {
        for (size_t i = 0; i + 4 <= len; i++) {
            if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                hdr_end = buf + i + 4;
                break;
            }
        }
    }
    if (!hdr_end) return len > OC_HTTP_MAX_HEADERS ? -1 : 0;   /* need more (or too big) */

    /* Request line: METHOD SP path SP HTTP/x.y CRLF. */
    const char *line_end = memchr(buf, '\r', (size_t)(hdr_end - buf));
    if (!line_end) return -1;
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) return -1;
    req->method = buf; req->method_len = (size_t)(sp1 - buf);
    req->path = sp1 + 1; req->path_len = (size_t)(sp2 - (sp1 + 1));
    if (req->method_len == 0 || req->path_len == 0) return -1;

    /* Headers we care about: Content-Length, Content-Type. */
    size_t content_length = 0;
    const char *p = line_end + 2;                 /* skip the request line's CRLF */
    while (p < hdr_end - 2) {
        const char *eol = memchr(p, '\r', (size_t)(hdr_end - p));
        if (!eol || eol > hdr_end - 2) break;
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            const char *val = colon + 1; size_t vlen = (size_t)(eol - (colon + 1));
            trim_value(&val, &vlen);
            if (ci_eq(p, nlen, "content-length")) {
                content_length = 0;
                for (size_t i = 0; i < vlen; i++) {
                    if (val[i] < '0' || val[i] > '9') { content_length = 0; break; }
                    content_length = content_length * 10 + (size_t)(val[i] - '0');
                    if (content_length > max_body) return -1;
                }
            } else if (ci_eq(p, nlen, "content-type")) {
                /* application/json, optionally with a ; charset= suffix. */
                if (vlen >= 16 && ci_eq(val, 16, "application/json")) req->is_json = 1;
            }
        }
        p = eol + 2;
    }

    if (content_length > max_body) return -1;
    size_t have = (size_t)(len - (hdr_end - buf));
    if (have < content_length) return 0;          /* body still arriving */

    req->body = hdr_end;
    req->body_len = content_length;
    return 1;
}

int oc_http_webhook_text(const oc_http_req *req, const char **out, size_t *outlen) {
    if (!req->body || req->body_len == 0) return 0;
    if (!req->is_json) { *out = req->body; *outlen = req->body_len; return *outlen > 0; }

    /* JSON: pull the top-level "text" string value. */
    jsmn_parser P; jsmn_init(&P);
    jsmntok_t toks[64];
    int n = jsmn_parse(&P, req->body, req->body_len, toks, (unsigned)(sizeof toks / sizeof toks[0]));
    if (n < 1 || toks[0].type != JSMN_OBJECT) return 0;
    for (int i = 1; i < n - 1; i++) {
        if (toks[i].type == JSMN_STRING) {
            size_t klen = (size_t)(toks[i].end - toks[i].start);
            if (klen == 4 && memcmp(req->body + toks[i].start, "text", 4) == 0 &&
                toks[i + 1].type == JSMN_STRING) {
                *out = req->body + toks[i + 1].start;
                *outlen = (size_t)(toks[i + 1].end - toks[i + 1].start);
                return *outlen > 0;
            }
        }
    }
    return 0;
}
