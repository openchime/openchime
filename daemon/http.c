/* The daemon's HTTP stack -- see http.h. */

#include "http.h"
#include "picohttpparser.h"
#define JSMN_HEADER   /* jwt.c carries the jsmn implementation; we take declarations */
#include "jsmn.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int oc_http_parse_head(const char *buf, size_t len, oc_http_req *req) {
    memset(req, 0, sizeof *req);
    struct phr_header h[OC_HTTP_MAX_HEADERS];
    size_t nh = OC_HTTP_MAX_HEADERS;
    int minor = 0;
    int r = phr_parse_request(buf, len, &req->method, &req->method_len, &req->path,
                              &req->path_len, &minor, h, &nh, 0);
    /* Incomplete is only incomplete while it could still become a head we take:
     * a peer must not make us hold header bytes without end. */
    if (r == -2) return len > OC_HTTP_MAX_HEAD ? -1 : 0;
    if (r < 0 || (size_t)r > OC_HTTP_MAX_HEAD) return -1;
    if (req->method_len == 0 || req->path_len == 0) return -1;
    req->head_len = (size_t)r;

    int have_len = 0;
    for (size_t i = 0; i < nh; i++) {
        if (h[i].name && ci_eq(h[i].name, h[i].name_len, "host")) {
            if (req->host) return -1;                   /* one Host (RFC 9112 §3.2) */
            req->host = h[i].value; req->host_len = h[i].value_len;
            continue;
        }
        if (h[i].name && ci_eq(h[i].name, h[i].name_len, "origin")) {
            if (req->origin) return -1;
            req->origin = h[i].value; req->origin_len = h[i].value_len;
            continue;
        }
        if (!h[i].name) continue;                       /* a folded continuation line */
        if (ci_eq(h[i].name, h[i].name_len, "upgrade")) {
            if (h[i].value_len == 9 && ci_eq(h[i].value, 9, "websocket")) req->upgrade_ws = 1;
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "sec-websocket-key")) {
            req->ws_key = h[i].value; req->ws_key_len = h[i].value_len;
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "sec-websocket-protocol")) {
            /* The first one offered is the one taken (ioloop.c echoes it). */
            const char *v = h[i].value; size_t vl = h[i].value_len;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            size_t k = 0;
            while (k < vl && v[k] != ',' && v[k] != ' ' && v[k] != '\t') k++;
            if (k && !req->ws_proto) { req->ws_proto = v; req->ws_proto_len = k; }
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "content-length")) {
            if (h[i].value_len == 0 || h[i].value_len > 19) return -1;
            size_t v = 0;
            for (size_t k = 0; k < h[i].value_len; k++) {
                char ch = h[i].value[k];
                if (ch < '0' || ch > '9') return -1;
                v = v * 10 + (size_t)(ch - '0');
            }
            /* Two lengths that disagree are how a request is smuggled past a
             * proxy that believes the other one. */
            if (have_len && v != req->content_length) return -1;
            req->content_length = v;
            have_len = 1;
        } else if (ci_eq(h[i].name, h[i].name_len, "transfer-encoding")) {
            return -1;   /* no route takes a chunked body; see http.h */
        } else if (ci_eq(h[i].name, h[i].name_len, "content-type")) {
            /* application/json, optionally with a ; charset= suffix. */
            if (h[i].value_len >= 16 && ci_eq(h[i].value, 16, "application/json") &&
                (h[i].value_len == 16 || h[i].value[16] == ';' || h[i].value[16] == ' '))
                req->is_json = 1;
            if (h[i].value_len >= 33 && ci_eq(h[i].value, 33, "application/x-www-form-urlencoded") &&
                (h[i].value_len == 33 || h[i].value[33] == ';' || h[i].value[33] == ' '))
                req->is_form = 1;
        }
    }
    return 1;
}

int oc_http_parse(const char *buf, size_t len, size_t max_body, oc_http_req *req) {
    int r = oc_http_parse_head(buf, len, req);
    if (r <= 0) return r;
    if (req->content_length > max_body) return -1;
    if (len - req->head_len < req->content_length) return 0;   /* body still arriving */
    req->body = buf + req->head_len;
    req->body_len = req->content_length;
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

/* --- routes ------------------------------------------------------------------ */

static int path_matches(const oc_http_route *rt, const char *p, size_t plen) {
    size_t n = strlen(rt->path);
    if (rt->prefix) return plen > n && memcmp(p, rt->path, n) == 0;
    return plen == n && memcmp(p, rt->path, n) == 0;
}

const oc_http_route *oc_http_route_find(const oc_http_site *site, const oc_http_req *req,
                                        int *status) {
    const char *q = memchr(req->path, '?', req->path_len);
    size_t plen = q ? (size_t)(q - req->path) : req->path_len;
    int named = 0;
    for (size_t i = 0; site && i < site->n; i++) {
        const oc_http_route *rt = &site->routes[i];
        if (!path_matches(rt, req->path, plen)) continue;
        named = 1;
        if (!rt->method || (strlen(rt->method) == req->method_len &&
                            memcmp(rt->method, req->method, req->method_len) == 0))
            return rt;
    }
    if (named) { *status = 405; return NULL; }
    if (site && site->fallback) return site->fallback;
    *status = 404;
    return NULL;
}

/* --- responses --------------------------------------------------------------- */

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

size_t oc_http_head(char *out, size_t cap, int status, const char *ctype, size_t body_len) {
    return oc_http_head_ex(out, cap, status, ctype, body_len, NULL);
}

size_t oc_http_head_ex(char *out, size_t cap, int status, const char *ctype, size_t body_len,
                       const char *extra) {
    int n = snprintf(out, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, reason(status), ctype, extra ? extra : "", body_len);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

const char *oc_http_error_body(int status, size_t *len) {
    const char *b;
    switch (status) {
    case 400: b = "bad request\n"; break;
    case 404: b = "not found\n"; break;
    case 405: b = "method not allowed\n"; break;
    case 408: b = "request timeout\n"; break;
    case 413: b = "too large\n"; break;
    case 429: b = "rate limited\n"; break;
    default:  b = "error\n"; break;
    }
    *len = strlen(b);
    return b;
}
