/* Harness: the HTTP request parser (daemon/http.c), as a connection's first
 * bytes reach it. The head, then head and body; on a parsed request, the
 * header values the static routes read. Every field is a view into the input,
 * and the head and body lie inside it. */

#include "fuzz.h"
#include "http.h"

#include <string.h>

#define H "http"

static void inside(const char *buf, size_t len, const char *p, size_t n) {
    if (p && (p < buf || p + n > buf + len)) OC_FUZZ_BROKEN(H, "a field outside the request");
}

static void fields(const char *buf, size_t len, const oc_http_req *r) {
    inside(buf, len, r->method, r->method_len);
    inside(buf, len, r->path, r->path_len);
    inside(buf, len, r->host, r->host_len);
    inside(buf, len, r->origin, r->origin_len);
    inside(buf, len, r->ws_key, r->ws_key_len);
    inside(buf, len, r->ws_proto, r->ws_proto_len);
    inside(buf, len, r->inm, r->inm_len);
    if (r->head_len > len) OC_FUZZ_BROKEN(H, "a head longer than the input");
}

int oc_fuzz_http(const uint8_t *data, size_t len) {
    const char *buf = (const char *)data;
    oc_http_req r;
    memset(&r, 0, sizeof r);
    int rc = oc_http_parse_head(buf, len, &r);
    if (rc < -1 || rc > 1) OC_FUZZ_BROKEN(H, "oc_http_parse_head answered outside -1..1");
    if (rc == 1) fields(buf, len, &r);
    memset(&r, 0, sizeof r);
    rc = oc_http_parse(buf, len, 4096, &r);
    if (rc < -1 || rc > 1) OC_FUZZ_BROKEN(H, "oc_http_parse answered outside -1..1");
    if (rc == 1) {
        fields(buf, len, &r);
        inside(buf, len, r.body, r.body_len);
        if (r.body_len > 4096) OC_FUZZ_BROKEN(H, "a body over the limit");
        const char *text;
        size_t tl;
        if (oc_http_webhook_text(&r, &text, &tl)) inside(buf, len, text, tl);
        if (r.inm) (void)oc_http_etag_listed(r.inm, r.inm_len, "\"abc\"");
    }
    (void)oc_http_accepts_gzip(buf, len);
    (void)oc_http_etag_listed(buf, len, "\"abc\"");
    return 0;
}
