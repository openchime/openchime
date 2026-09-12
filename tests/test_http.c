/* Unit tests for the minimal HTTP/1.1 request reader (daemon/http.c) that backs
 * the incoming-webhook endpoint (REQ-170). Exercises request-line parsing,
 * header handling, body framing by Content-Length, incremental (partial) input,
 * size limits, and the JSON-vs-plain message-text extraction. */

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

int run_http_tests(void) {
    printf("test_http: request line, headers, body framing, partial input,\n");
    printf("           size limits, JSON/plain webhook text\n");
    test_parse_json();
    test_parse_plain();
    test_incomplete();
    test_body_too_large();
    test_case_insensitive_headers();
    test_json_no_text_field();
    test_get_and_paths();
    return failures;
}
