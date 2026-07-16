#ifndef OC_HTTP_H
#define OC_HTTP_H

#include <stddef.h>

/* Minimal HTTP/1.1 request reader for the incoming-webhook endpoint (ARCH-32,
 * REQ-170). Purpose-built for one small POST endpoint reached by uncontrolled
 * third parties — not a general web server, and deliberately not a vendored
 * parser (picohttpparser remains an option if the HTTP surface ever grows). It
 * parses the request line + the headers it needs (Content-Length, Content-Type)
 * and locates the body; everything is a zero-copy view into the caller's
 * buffer. */

typedef struct {
    const char *method; size_t method_len;
    const char *path;   size_t path_len;
    const char *body;   size_t body_len;
    int         is_json;   /* Content-Type is application/json */
} oc_http_req;

/* Parse a (possibly partial) request from `buf`/`len`. Fields of *req point into
 * `buf`. Returns:
 *    1  a complete request was parsed,
 *    0  incomplete — the caller should read more bytes and retry,
 *   -1  malformed, or the declared body exceeds `max_body`. */
int oc_http_parse(const char *buf, size_t len, size_t max_body, oc_http_req *req);

/* The webhook message text for a parsed request: the JSON `"text"` field when
 * the body is JSON, otherwise the raw body. Writes a view (into the body) to
 * `out` and `outlen`. Returns 1 on success, 0 if there is no usable text. */
int oc_http_webhook_text(const oc_http_req *req, const char **out, size_t *outlen);

#endif /* OC_HTTP_H */
