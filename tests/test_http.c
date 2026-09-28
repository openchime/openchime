/* Unit tests for the daemon's HTTP stack (daemon/http.c, ARCH-32): parsing over
 * picohttpparser -- request line, headers, body framing by Content-Length,
 * partial input, size limits, and what is refused (a chunked body, two lengths
 * that disagree, a head too long or with too many headers) -- the JSON-vs-plain
 * webhook text (REQ-170), the router, and the response writer. */

#include "http.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

#define MAXB 65536u

/* Build "POST <path> HTTP/1.1" + headers + body with a correct Content-Length. */
static size_t build(char *buf, size_t cap, const char *method, const char *path,
                    const char *ctype, const char *body) {
    size_t blen = body ? strlen(body) : 0;
    return (size_t)snprintf(buf, cap,
        "%s %s HTTP/1.1\r\nHost: h\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n%s",
        method, path, ctype, blen, body ? body : "");
}

static void test_parse_json(void) {
    char buf[512];
    size_t n = build(buf, sizeof buf, "POST", "/webhook/deadbeef",
                     "application/json", "{\"text\":\"hello world\"}");
    oc_http_req req;
    CHECK(oc_http_parse(buf, n, MAXB, &req) == 1);
    CHECK(req.method_len == 4 && memcmp(req.method, "POST", 4) == 0);
    CHECK(req.path_len == 17 && memcmp(req.path, "/webhook/deadbeef", 17) == 0);
    CHECK(req.is_json == 1);
    const char *text = NULL; size_t tlen = 0;
    CHECK(oc_http_webhook_text(&req, &text, &tlen) == 1);
    CHECK(tlen == 11 && memcmp(text, "hello world", 11) == 0);
}

static void test_parse_plain(void) {
    char buf[256];
    size_t n = build(buf, sizeof buf, "POST", "/webhook/abcd", "text/plain", "raw body");
    oc_http_req req;
    CHECK(oc_http_parse(buf, n, MAXB, &req) == 1);
    CHECK(req.is_json == 0);
    const char *text = NULL; size_t tlen = 0;
    CHECK(oc_http_webhook_text(&req, &text, &tlen) == 1);
    CHECK(tlen == 8 && memcmp(text, "raw body", 8) == 0);
}

static void test_incomplete(void) {
    /* No header terminator yet -> need more. */
    const char *partial = "POST /webhook/x HTTP/1.1\r\nContent-Length: 5\r\n";
    oc_http_req req;
    CHECK(oc_http_parse(partial, strlen(partial), MAXB, &req) == 0);

    /* Headers complete but body short -> need more. */
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "POST /webhook/x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");
    CHECK(oc_http_parse(buf, (size_t)n, MAXB, &req) == 0);
}

static void test_body_too_large(void) {
    const char *buf =
        "POST /webhook/x HTTP/1.1\r\nContent-Length: 100\r\n\r\n";
    oc_http_req req;
    CHECK(oc_http_parse(buf, strlen(buf), 10, &req) == -1);   /* declared 100 > max 10 */
}

static void test_case_insensitive_headers(void) {
    const char *body = "hi";
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "post /webhook/z HTTP/1.1\r\ncOnTeNt-TyPe: application/json\r\n"
        "CONTENT-LENGTH: %zu\r\n\r\n%s", strlen(body), body);
    oc_http_req req;
    CHECK(oc_http_parse(buf, (size_t)n, MAXB, &req) == 1);
    CHECK(req.is_json == 1);
}

static void test_json_no_text_field(void) {
    char buf[256];
    size_t n = build(buf, sizeof buf, "POST", "/webhook/x",
                     "application/json", "{\"other\":\"v\"}");
    oc_http_req req;
    CHECK(oc_http_parse(buf, n, MAXB, &req) == 1);
    const char *text = NULL; size_t tlen = 0;
    CHECK(oc_http_webhook_text(&req, &text, &tlen) == 0);   /* no usable text */
}

static void test_get_and_paths(void) {
    char buf[256];
    size_t n = build(buf, sizeof buf, "GET", "/healthz", "text/plain", "");
    oc_http_req req;
    CHECK(oc_http_parse(buf, n, MAXB, &req) == 1);   /* parser is method-agnostic */
    CHECK(req.method_len == 3 && memcmp(req.method, "GET", 3) == 0);
    CHECK(req.body_len == 0);

    /* Malformed request line (no path) -> -1. */
    const char *bad = "POST\r\n\r\n";
    CHECK(oc_http_parse(bad, strlen(bad), MAXB, &req) == -1);
}

static void test_refused(void) {
    oc_http_req req;
    /* A chunked body is never taken: no route reads one. */
    const char *te = "POST /webhook/x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(oc_http_parse_head(te, strlen(te), &req) == -1);
    /* Two lengths that disagree are refused; the same one twice is not. */
    const char *two = "POST /x HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd";
    CHECK(oc_http_parse_head(two, strlen(two), &req) == -1);
    const char *same = "POST /x HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc";
    CHECK(oc_http_parse(same, strlen(same), MAXB, &req) == 1 && req.body_len == 3);
    const char *nan = "POST /x HTTP/1.1\r\nContent-Length: 3x\r\n\r\n";
    CHECK(oc_http_parse_head(nan, strlen(nan), &req) == -1);
    const char *neg = "POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    CHECK(oc_http_parse_head(neg, strlen(neg), &req) == -1);
    /* A head that has not ended by OC_HTTP_MAX_HEAD bytes never will. */
    static char big[OC_HTTP_MAX_HEAD + 32];
    int hl = snprintf(big, sizeof big, "GET / HTTP/1.1\r\nX-Pad: ");
    memset(big + hl, 'x', sizeof big - (size_t)hl);
    CHECK(oc_http_parse_head(big, OC_HTTP_MAX_HEAD, &req) == 0);
    CHECK(oc_http_parse_head(big, sizeof big, &req) == -1);
    /* More headers than OC_HTTP_MAX_HEADERS. */
    static char many[4096];
    size_t n = (size_t)snprintf(many, sizeof many, "GET / HTTP/1.1\r\n");
    for (int i = 0; i <= OC_HTTP_MAX_HEADERS; i++)
        n += (size_t)snprintf(many + n, sizeof many - n, "X-%d: y\r\n", i);
    n += (size_t)snprintf(many + n, sizeof many - n, "\r\n");
    CHECK(oc_http_parse_head(many, n, &req) == -1);
    /* A JSON type is JSON only when it is exactly that. */
    const char *js = "POST /x HTTP/1.1\r\nContent-Type: application/jsonx\r\n\r\n";
    CHECK(oc_http_parse_head(js, strlen(js), &req) == 1 && req.is_json == 0);
    const char *jc = "POST /x HTTP/1.1\r\nContent-Type: application/json; charset=utf-8\r\n\r\n";
    CHECK(oc_http_parse_head(jc, strlen(jc), &req) == 1 && req.is_json == 1);
}

static void test_head_fields(void) {
    const char *r = "POST /a?b=1 HTTP/1.0\r\nContent-Length: 2\r\n\r\nhi";
    oc_http_req req;
    CHECK(oc_http_parse_head(r, strlen(r), &req) == 1);
    CHECK(req.head_len == strlen(r) - 2 && req.content_length == 2 && req.body == NULL);
    CHECK(req.path_len == 6 && memcmp(req.path, "/a?b=1", 6) == 0);
}

static void test_router(void) {
    static const oc_http_route R[] = {
        { "POST", "/webhook/", 1, OC_HTTP_LOOP,   64, NULL, NULL, 0 },
        { "GET",  "/page",     0, OC_HTTP_STATIC, 0, "text/plain", "p", 1 },
        { NULL,   "/any",      0, OC_HTTP_STATIC, 0, "text/plain", "a", 1 },
    };
    static const oc_http_route FB = { NULL, "/", 1, OC_HTTP_STATIC, 0, "text/html", "f", 1 };
    oc_http_site site = { R, 3, NULL }, site_fb = { R, 3, &FB };
    oc_http_req req;
    int st = 0;
#define ROUTE(site_, line) (oc_http_parse_head(line, strlen(line), &req) == 1 ? \
                            (st = 0, oc_http_route_find(&(site_), &req, &st)) : NULL)
    CHECK(ROUTE(site, "POST /webhook/abc HTTP/1.1\r\n\r\n") == &R[0]);
    CHECK(ROUTE(site, "POST /webhook/ HTTP/1.1\r\n\r\n") == NULL && st == 404);   /* a prefix needs more */
    CHECK(ROUTE(site, "GET /webhook/abc HTTP/1.1\r\n\r\n") == NULL && st == 405);
    CHECK(ROUTE(site, "GET /page?x=1 HTTP/1.1\r\n\r\n") == &R[1]);           /* the query is not the path */
    CHECK(ROUTE(site, "GET /page/ HTTP/1.1\r\n\r\n") == NULL && st == 404);    /* exact is exact */
    CHECK(ROUTE(site, "HEAD /page HTTP/1.1\r\n\r\n") == NULL && st == 405);    /* methods match as sent */
    CHECK(ROUTE(site, "DELETE /any HTTP/1.1\r\n\r\n") == &R[2]);             /* NULL: any method */
    CHECK(ROUTE(site, "GET /elsewhere HTTP/1.1\r\n\r\n") == NULL && st == 404);
    CHECK(ROUTE(site_fb, "GET /elsewhere HTTP/1.1\r\n\r\n") == &FB);
    CHECK(ROUTE(site_fb, "GET /webhook/abc HTTP/1.1\r\n\r\n") == NULL && st == 405); /* named beats fallback */
#undef ROUTE
}

static void test_writer(void) {
    char out[OC_HTTP_HEAD_MAX];
    size_t n = oc_http_head(out, sizeof out, 404, "text/plain", 10);
    const char *want = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                       "Content-Length: 10\r\nConnection: close\r\n\r\n";
    CHECK(n == strlen(want) && memcmp(out, want, n) == 0);
    CHECK(oc_http_head(out, 16, 200, "text/plain", 2) == 0);                   /* too small: nothing */
    static const int codes[] = { 400, 404, 405, 408, 413 };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        size_t bl = 0;
        const char *b = oc_http_error_body(codes[i], &bl);
        CHECK(b && bl > 0 && bl == strlen(b) && b[bl - 1] == '\n');
        n = oc_http_head(out, sizeof out, codes[i], "text/plain", bl);
        char code[8]; snprintf(code, sizeof code, " %d ", codes[i]);
        CHECK(n > 0 && strstr(out, code) != NULL && strstr(out, " Error\r\n") == NULL);
    }
}

int run_http_tests(void) {
    printf("test_http: request line, headers, body framing, partial input, size limits,\n");
    printf("           refusals, JSON/plain webhook text, the router, the response writer\n");
    test_parse_json();
    test_parse_plain();
    test_incomplete();
    test_body_too_large();
    test_case_insensitive_headers();
    test_json_no_text_field();
    test_get_and_paths();
    test_refused();
    test_head_fields();
    test_router();
    test_writer();
    return failures;
}
