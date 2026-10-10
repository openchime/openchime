#ifndef OC_HTTP_H
#define OC_HTTP_H

#include <stddef.h>

/* The daemon's HTTP stack (ARCH-32): parsing, routing and the response writer,
 * used by the I/O threads (ioloop.c) for every HTTP connection -- a webhook
 * sender on the TLS port (ALPN, ARCH-54) and an orchestrator on the plaintext
 * health port (ARCH-25).
 *
 * Parsing is picohttpparser's (third_party/picohttpparser, MIT): it parses the
 * request line and headers and does nothing else. What is ours is small: which
 * headers matter (Content-Length, Content-Type, and Host and Origin for the
 * sign-in pages' same-origin check; a Transfer-Encoding is refused,
 * since no route takes a chunked body), the routes, and the bytes of a
 * response. One request per connection, answered with `Connection: close`. */

/* The most header bytes a request may send before its blank line, and the most
 * headers it may have. Past either it is refused (400). */
#define OC_HTTP_MAX_HEAD    8192u
#define OC_HTTP_MAX_HEADERS 32

typedef struct {
    const char *method; size_t method_len;
    const char *path;   size_t path_len;     /* as sent, query included */
    const char *body;   size_t body_len;     /* set once the body is whole */
    int         is_json;                     /* Content-Type is application/json */
    int         is_form;                     /* ...application/x-www-form-urlencoded */
    const char *host;   size_t host_len;     /* the Host header; NULL if none */
    const char *origin; size_t origin_len;   /* the Origin header; NULL if none */
    /* A WebSocket opening handshake (RFC 6455 §4): `Upgrade: websocket` with
     * its key, and the first subprotocol it asked for, if any. The route of
     * kind OC_HTTP_WS takes it; every other route ignores these. */
    int         upgrade_ws;
    const char *ws_key;   size_t ws_key_len;
    const char *ws_proto; size_t ws_proto_len;
    size_t      head_len;                    /* request line + headers + blank line */
    size_t      content_length;              /* declared; 0 when absent */
} oc_http_req;

/* Parse the request line and headers from `buf`/`len`. Fields of *req point
 * into `buf`; the body is not looked at. Returns:
 *    1  the head is whole: head_len and content_length are set,
 *    0  incomplete -- read more and call again,
 *   -1  malformed: a bad request line or header, a head over OC_HTTP_MAX_HEAD
 *       or with more than OC_HTTP_MAX_HEADERS headers, a Content-Length that is
 *       not a number, or any Transfer-Encoding. */
int oc_http_parse_head(const char *buf, size_t len, oc_http_req *req);

/* The head and then the body: 1 with body/body_len set once the declared body
 * is all here, 0 while it is not, -1 as above or when the declared body exceeds
 * `max_body`. */
int oc_http_parse(const char *buf, size_t len, size_t max_body, oc_http_req *req);

/* The webhook message text for a parsed request: the JSON `"text"` field when
 * the body is JSON, otherwise the raw body. Writes a view (into the body) to
 * `out` and `outlen`. Returns 1 on success, 0 if there is no usable text. */
int oc_http_webhook_text(const oc_http_req *req, const char **out, size_t *outlen);

/* --- routes ------------------------------------------------------------------ */

typedef enum {
    OC_HTTP_STATIC,   /* answered on the I/O thread with `body`, no loop involved */
    OC_HTTP_LOOP,     /* reported to the event loop, which answers (it touches state) */
    OC_HTTP_WS,       /* a WebSocket upgrade: from here the connection carries the
                       * binary protocol in WebSocket frames (ioloop.c), for the
                       * browser client, which has no socket of its own */
    OC_HTTP_REDIRECT  /* a 302 to `body` (the Location), answered on the I/O thread */
} oc_http_kind;

typedef struct {
    const char  *method;      /* NULL: any method */
    const char  *path;        /* matched against the path without its query */
    int          prefix;      /* 1: `path` is a prefix; 0: the whole path */
    oc_http_kind kind;
    size_t       max_body;    /* a declared body over this is refused (413) */
    const char  *ctype;       /* OC_HTTP_STATIC: the answer */
    const char  *body;
    size_t       body_len;
    const char  *extra;       /* OC_HTTP_STATIC: headers added to the answer ("A: b\r\n"), or NULL */
} oc_http_route;

/* What one listener serves: its routes, in order, and what a path none of them
 * names gets -- NULL for 404. */
typedef struct {
    const oc_http_route *routes;
    size_t               n;
    const oc_http_route *fallback;
} oc_http_site;

/* The route for a parsed head. NULL with *status set when there is none: 404
 * for a path nothing names, 405 for a path named only under other methods. */
const oc_http_route *oc_http_route_find(const oc_http_site *site, const oc_http_req *req,
                                        int *status);

/* --- responses --------------------------------------------------------------- */

/* Write a response head -- status line, Content-Type, Content-Length and
 * `Connection: close` -- into `out`. Returns its length, or 0 if `cap` is too
 * small. OC_HTTP_HEAD_MAX always suffices for a Content-Type under 64 bytes. */
#define OC_HTTP_HEAD_MAX 512u
size_t oc_http_head(char *out, size_t cap, int status, const char *ctype, size_t body_len);
/* The same with `extra` -- whole header lines, each ending "\r\n", or NULL --
 * after Content-Type: a Location, the pages' security headers. */
size_t oc_http_head_ex(char *out, size_t cap, int status, const char *ctype, size_t body_len,
                       const char *extra);

/* The small fixed page an error status is answered with (text/plain). */
const char *oc_http_error_body(int status, size_t *len);

#endif /* OC_HTTP_H */
