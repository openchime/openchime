/* Unit tests for the incoming webhook's payload reader (daemon/http.c,
 * oc_http_webhook_payload; ARCH-71): plain text as sent, JSON unescaped, a form's
 * `payload`, the common chat-webhook format's blocks, text and attachments in
 * their order, its link syntax converted to the message format, the fields read
 * past, and the limits. Each expected text is the message format's own
 * (docs/MARKDOWN.md), which is the contract with every client. */

#include "http.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXB 65536u

/* The text a request with `ctype` and `body` posts, or NULL when it posts none. */
static const char *posted(const char *ctype, const char *body) {
    static char req[16384], out[8192];
    size_t bl = strlen(body);
    int n = snprintf(req, sizeof req, "POST /webhook/x HTTP/1.1\r\nHost: h\r\nContent-Type: %s\r\n"
                                      "Content-Length: %zu\r\n\r\n%s", ctype, bl, body);
    oc_http_req r;
    size_t tl = 0;
    if (n < 0 || (size_t)n >= sizeof req || oc_http_parse(req, (size_t)n, MAXB, &r) != 1) return NULL;
    if (!oc_http_webhook_payload(&r, out, sizeof out, &tl)) return NULL;
    if (tl != strlen(out)) return NULL;
    return out;
}
static const char *json(const char *body) { return posted("application/json", body); }
static int is(const char *got, const char *want) { return got && strcmp(got, want) == 0; }

/* Criterion 7: plain text is posted as sent, markup and all. */
static void test_plain(void) {
    /* contract: a text/plain body is the message, unchanged */
    CHECK(is(posted("text/plain", "raw <https://x.example|x> &amp; body"), "raw <https://x.example|x> &amp; body"));
    CHECK(posted("text/plain", "") == NULL);
    /* contract: a NUL byte in the body is dropped, so the text is the C string
     * of the length it says */
    static const char rq[] = "POST /webhook/x HTTP/1.1\r\nContent-Type: text/plain\r\n"
                             "Content-Length: 5\r\n\r\na\0b\0c";
    oc_http_req r;
    char out[16];
    size_t tl = 0;
    CHECK(oc_http_parse(rq, sizeof rq - 1, MAXB, &r) == 1);
    CHECK(oc_http_webhook_payload(&r, out, sizeof out, &tl) == 1 && tl == 3 && strcmp(out, "abc") == 0);
    static const char nul[] = "POST /webhook/x HTTP/1.1\r\nContent-Type: text/plain\r\n"
                              "Content-Length: 1\r\n\r\n\0";
    CHECK(oc_http_parse(nul, sizeof nul - 1, MAXB, &r) == 1);
    CHECK(oc_http_webhook_payload(&r, out, sizeof out, &tl) == 0 && tl == 0);
}

/* Criterion 1: a JSON string is decoded. */
static void test_escapes(void) {
    /* contract: JSON escapes become their characters (ARCH-71) */
    CHECK(is(json("{\"text\":\"line one\\nline \\\"two\\\" caf\\u00e9\"}"), "line one\nline \"two\" caf\xC3\xA9"));
    CHECK(json("{\"other\":\"v\"}") == NULL);                     /* nothing to say */
    CHECK(json("{\"text\":\"\"}") == NULL);
    CHECK(json("[\"text\",\"x\"]") == NULL);                      /* not an object */
    CHECK(json("{\"text\":") == NULL);                            /* not JSON */
}

/* Criterion 3: each piece of the format's syntax. */
static void test_conversions(void) {
    /* contract: the message format's labelled link (MARKDOWN.md) */
    CHECK(is(json("{\"text\":\"see <https://x.example/a|the report>\"}"), "see [the report](https://x.example/a)"));
    /* contract: a bare address autolinks as it is */
    CHECK(is(json("{\"text\":\"<https://x.example/a>\"}"), "https://x.example/a"));
    /* contract: an address the format does not link reads as text */
    CHECK(is(json("{\"text\":\"mail <mailto:a@b.example|a@b.example>\"}"), "mail a@b.example"));
    /* contract: the broadcast mentions of MARKDOWN.md */
    CHECK(is(json("{\"text\":\"<!here> <!channel> <!everyone>\"}"), "@here @channel @everyone"));
    /* contract: a channel reference by name */
    CHECK(is(json("{\"text\":\"in <#C024BE7LR|general>\"}"), "in #general"));
    /* contract: the format's three entities */
    CHECK(is(json("{\"text\":\"a &lt;b&gt; &amp; c\"}"), "a <b> & c"));
    /* contract: a mention by id is left as written */
    CHECK(is(json("{\"text\":\"<@U024BE7LH> look\"}"), "<@U024BE7LH> look"));
    /* contract: emphasis is already the format */
    CHECK(is(json("{\"text\":\"*bold* _it_ ~st~\"}"), "*bold* _it_ ~st~"));
    /* contract: a label the format cannot hold keeps its address beside it */
    CHECK(is(json("{\"text\":\"<https://x.example|a [b]>\"}"), "a [b] (https://x.example)"));
    /* contract: an entity's '<' is a character, never the start of a link */
    CHECK(is(json("{\"text\":\"&lt;https://x.example|y&gt;\"}"), "<https://x.example|y>"));
}

/* Criterion 2: a form's payload field. */
static void test_form(void) {
    const char *f = "token=t&payload=%7B%22text%22%3A%22hi+%3Chttps%3A%2F%2Fx.example%7Cthere%3E%22%7D";
    /* contract: the form's payload, decoded, read as the JSON body */
    CHECK(is(posted("application/x-www-form-urlencoded", f), "hi [there](https://x.example)"));
    CHECK(posted("application/x-www-form-urlencoded", "token=t&other=1") == NULL);
}

/* Criterion 4: the order of what is composed. */
static void test_order(void) {
    const char *blocks =
        "{\"text\":\"fallback\",\"blocks\":["
        "{\"type\":\"header\",\"text\":{\"type\":\"plain_text\",\"text\":\"Deploy\"}},"
        "{\"type\":\"divider\"},"
        "{\"type\":\"section\",\"text\":{\"type\":\"mrkdwn\",\"text\":\"*web* is live\"},"
        " \"fields\":[{\"type\":\"mrkdwn\",\"text\":\"*Env*\\nprod\"}]},"
        "{\"type\":\"image\",\"image_url\":\"https://x.example/i.png\",\"alt_text\":\"chart\"},"
        "{\"type\":\"context\",\"elements\":[{\"type\":\"image\",\"image_url\":\"https://x.example/a.png\"},"
        " {\"type\":\"mrkdwn\",\"text\":\"by ada\"}]}]}";
    /* contract: blocks win over text, a block to a line, unknown kinds passed over */
    CHECK(is(json(blocks), "Deploy\n*web* is live\n*Env*\nprod\nby ada"));
    const char *att =
        "{\"attachments\":[{\"pretext\":\"Heads up\",\"title\":\"Build 41\",\"title_link\":\"https://ci.example/41\","
        "\"text\":\"failed on <!here>\",\"fields\":[{\"title\":\"Branch\",\"value\":\"main\",\"short\":true}]},"
        "{\"fallback\":\"Second, by fallback\",\"color\":\"#f00\"}]}";
    /* contract: each attachment's lines, or its fallback */
    CHECK(is(json(att), "Heads up\n[Build 41](https://ci.example/41)\nfailed on @here\n*Branch*: main\nSecond, by fallback"));
    /* contract: text, then the attachments */
    CHECK(is(json("{\"text\":\"Alert\",\"attachments\":[{\"title\":\"CPU\",\"text\":\"95%\"}]}"), "Alert\nCPU\n95%"));
    CHECK(json("{\"blocks\":[{\"type\":\"divider\"}],\"attachments\":[]}") == NULL);   /* 400: empty */
}

/* Criterion 5: fields read past leave the text as it is. */
static void test_ignored(void) {
    /* contract: the webhook's label stays the author; these change nothing */
    CHECK(is(json("{\"text\":\"hi\",\"username\":\"bot\",\"icon_url\":\"https://x.example/i\",\"icon_emoji\":\":ghost:\","
                  "\"channel\":\"#other\",\"mrkdwn\":true,\"unfurl_links\":false,\"unfurl_media\":false}"), "hi"));
}

/* Criterion 6: capped at its room, and a payload too large to read is refused. */
static void test_limits(void) {
    enum { BIG = 12000 };
    char *body = malloc(BIG + 64), out[100];
    if (!body) { CHECK(0); return; }
    int o = snprintf(body, BIG + 64, "{\"text\":\"");
    for (int i = 0; i < BIG; i++) body[o++] = (i % 50 == 49) ? ' ' : 'a';
    snprintf(body + o, 64, "\"}");
    char *req = malloc(BIG + 256);
    if (!req) { free(body); CHECK(0); return; }
    int n = snprintf(req, BIG + 256, "POST /webhook/x HTTP/1.1\r\nContent-Type: application/json\r\n"
                                     "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    oc_http_req r;
    size_t tl = 0;
    CHECK(oc_http_parse(req, (size_t)n, MAXB, &r) == 1);
    CHECK(oc_http_webhook_payload(&r, out, sizeof out, &tl) == 1 && tl == sizeof out - 1 && strlen(out) == tl);
    free(req);
    free(body);
    /* A cut never ends inside a character: room for "abc" and half an é. */
    char small[5];
    const char *e = "{\"text\":\"abc\\u00e9\\u00e9\"}";
    char rq[256];
    n = snprintf(rq, sizeof rq, "POST /w HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                 strlen(e), e);
    CHECK(oc_http_parse(rq, (size_t)n, MAXB, &r) == 1);
    CHECK(oc_http_webhook_payload(&r, small, sizeof small, &tl) == 1 && tl == 3);
    /* More tokens than the reader takes: refused, not cut short. */
    enum { TOKS = 20000 };
    char *many = malloc((size_t)TOKS * 4 + 64);
    if (!many) { CHECK(0); return; }
    int m = snprintf(many, 64, "{\"text\":\"x\",\"a\":[");
    for (int i = 0; i < TOKS; i++) { many[m++] = '1'; many[m++] = ','; }
    many[m - 1] = ']'; many[m++] = '}'; many[m] = '\0';
    char *rq2 = malloc((size_t)m + 256);
    if (rq2) {
        int n2 = snprintf(rq2, (size_t)m + 256, "POST /w HTTP/1.1\r\nContent-Type: application/json\r\n"
                                               "Content-Length: %d\r\n\r\n%s", m, many);
        CHECK(oc_http_parse(rq2, (size_t)n2, (size_t)m + 1, &r) == 1);
        CHECK(oc_http_webhook_payload(&r, out, sizeof out, &tl) == 0);
        free(rq2);
    }
    free(many);
}

/* Payloads in the shapes common senders post, each with the text it makes. */
static void test_senders(void) {
    /* An Alertmanager-style text with a link and a list. */
    /* contract: the message as the format writes it */
    CHECK(is(json("{\"text\":\"*[FIRING:1] HighLatency*\\n- api p99 > 2s <https://am.example/#/alerts|View>\"}"),
             "*[FIRING:1] HighLatency*\n- api p99 > 2s [View](https://am.example/#/alerts)"));
    /* A Grafana-style attachment with a title link and fields. */
    /* contract: the message as the format writes it */
    CHECK(is(json("{\"attachments\":[{\"title\":\"[Alerting] CPU\",\"title_link\":\"https://g.example/d/1\","
                  "\"text\":\"CPU above 90%\",\"fields\":[{\"title\":\"host\",\"value\":\"web-1\"}]}]}"),
             "[Alerting] CPU (https://g.example/d/1)\nCPU above 90%\n*host*: web-1"));
    /* A Sentry-style block message. */
    /* contract: the message as the format writes it */
    CHECK(is(json("{\"text\":\"New issue\",\"blocks\":[{\"type\":\"section\",\"text\":{\"type\":\"mrkdwn\","
                  "\"text\":\"<https://s.example/i/7|TypeError: x is undefined>\"}}]}"),
             "[TypeError: x is undefined](https://s.example/i/7)"));
    /* A Jenkins-style attachment with a colour and a fallback only. */
    /* contract: the message as the format writes it */
    CHECK(is(json("{\"attachments\":[{\"color\":\"good\",\"fallback\":\"Build twelve passed\"}]}"), "Build twelve passed"));
    /* A GitLab-style text with escapes and a channel reference. */
    /* contract: the message as the format writes it */
    CHECK(is(json("{\"text\":\"ada pushed to <https://gl.example/p/-/tree/main|main> of &lt;p&gt; in <#C1|dev>\"}"),
             "ada pushed to [main](https://gl.example/p/-/tree/main) of <p> in #dev"));
}

int run_webhook_payload_tests(void) {
    printf("test_webhook_payload: plain text, JSON escapes, a form's payload, the chat-webhook format's\n");
    printf("           syntax, blocks, text and attachments in order, ignored fields, limits, senders\n");
    test_plain();
    test_escapes();
    test_conversions();
    test_form();
    test_order();
    test_ignored();
    test_limits();
    test_senders();
    return failures;
}
