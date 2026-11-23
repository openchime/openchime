/* Outgoing webhooks' delivery (webhooks_out.h, docs/details/WEBHOOKS.md): the
 * signature, the endpoint check and each delivery against a receiver on the
 * loopback, the retry headers and reasons, and the whole path from the writer's
 * queue through the worker -- a failing address retried and, after a day of
 * failing on the writer's injected clock, turned off. */

#include "check.h"
#include "testenv.h"
#include "dbwriter.h"
#include "webhooks_out.h"
#include "totp.h"

#include <mbedtls/md.h>
#include <sqlite3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RX_MAX 12
enum { RX_ECHO, RX_ECHO_JSON, RX_OK, RX_FAIL, RX_WRONG };

/* A receiver on the loopback: answers `n` requests, each as `mode[i]` says,
 * and keeps what came. */
typedef struct {
    int  fd, n, seen;
    int  mode[RX_MAX];
    char head[RX_MAX][2048];
    char body[RX_MAX][2048];
} receiver;

static void header_value(const char *req, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    const char *h = strcasestr(req, name);
    if (!h) return;
    h += strlen(name);
    while (*h == ' ') h++;
    size_t n = strcspn(h, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, h, n);
    out[n] = '\0';
}

static void *receiver_thread(void *arg) {
    receiver *rx = arg;
    while (rx->seen < rx->n) {
        int c = accept(rx->fd, NULL, NULL);
        if (c < 0) break;
        char buf[4096];
        size_t total = 0, head_end = 0;
        long clen = -1;
        for (;;) {
            if (total >= sizeof buf - 1) break;
            ssize_t n = read(c, buf + total, sizeof buf - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
            buf[total] = '\0';
            if (!head_end) {
                char *he = strstr(buf, "\r\n\r\n");
                if (he) {
                    head_end = (size_t)(he - buf) + 4;
                    char *cl = strcasestr(buf, "Content-Length:");
                    if (cl) clen = strtol(cl + 15, NULL, 10);
                }
            }
            if (head_end && clen >= 0 && total >= head_end + (size_t)clen) break;
        }
        int i = rx->seen++;
        snprintf(rx->head[i], sizeof rx->head[i], "%.*s", (int)head_end, buf);
        snprintf(rx->body[i], sizeof rx->body[i], "%s", head_end ? buf + head_end : "");
        char challenge[96] = "";
        const char *ch = strstr(rx->body[i], "\"challenge\":\"");
        if (ch) {
            ch += 13;
            size_t k = strcspn(ch, "\"");
            snprintf(challenge, sizeof challenge, "%.*s", (int)k, ch);
        }
        char reply[256];
        int status = rx->mode[i] == RX_FAIL ? 500 : 200;
        if (rx->mode[i] == RX_ECHO) snprintf(reply, sizeof reply, "%s", challenge);
        else if (rx->mode[i] == RX_ECHO_JSON) snprintf(reply, sizeof reply, "{\"challenge\":\"%s\"}", challenge);
        else if (rx->mode[i] == RX_WRONG) snprintf(reply, sizeof reply, "not-the-challenge");
        else snprintf(reply, sizeof reply, "ok");
        char resp[512];
        int rn = snprintf(resp, sizeof resp, "HTTP/1.1 %d X\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          status, strlen(reply), reply);
        ssize_t wr = write(c, resp, (size_t)rn);
        (void)wr;
        close(c);
    }
    return NULL;
}

static int rx_start(receiver *rx, pthread_t *th, const int *modes, int n, char *url, size_t cap) {
    memset(rx, 0, sizeof *rx);
    rx->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rx->fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(rx->fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(rx->fd, 8) != 0) { close(rx->fd); return -1; }
    socklen_t sl = sizeof sa;
    getsockname(rx->fd, (struct sockaddr *)&sa, &sl);
    memcpy(rx->mode, modes, (size_t)n * sizeof *modes);
    rx->n = n;
    snprintf(url, cap, "http://127.0.0.1:%d/events", ntohs(sa.sin_port));
    return pthread_create(th, NULL, receiver_thread, rx) == 0 ? 0 : -1;
}

static void rx_end(receiver *rx, pthread_t th) {
    pthread_join(th, NULL);   /* it has had all its requests */
    close(rx->fd);
}

/* The worker's next result, waited for on its eventfd. 1, or 0 on a hang. */
static int next_result(oc_webhooks_out *o, oc_owh_result *r) {
    for (int guard = 0; guard < 3000; guard++) {     /* only a guard against a hang */
        if (oc_webhooks_out_next(o, r)) return 1;
        struct pollfd pf = { oc_webhooks_out_eventfd(o), POLLIN, 0 };
        if (poll(&pf, 1, 10) > 0) { uint64_t v; ssize_t rd = read(pf.fd, &v, sizeof v); (void)rd; }
    }
    return 0;
}

/* Whether `head` carries a signature over `body` made with `secret`. */
static int signed_by(const char *head, const char *body, const char *secret) {
    char ts[32], sig[128];
    header_value(head, "X-Slack-Request-Timestamp:", ts, sizeof ts);
    header_value(head, "X-Slack-Signature:", sig, sizeof sig);
    char base[4096];
    int n = snprintf(base, sizeof base, "v0:%s:%s", ts, body);
    unsigned char mac[32];
    if (n < 0 || mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)secret,
                                 strlen(secret), (const unsigned char *)base, (size_t)n, mac) != 0) return 0;
    char want[3 + 64 + 1] = "v0=";
    for (int i = 0; i < 32; i++) snprintf(want + 3 + 2 * i, 3, "%02x", mac[i]);
    return ts[0] && strcmp(sig, want) == 0;
}

/* The clock the worker signs with. */
static uint64_t g_clock_ms = 1730295731000ull;
static uint64_t test_clock(void *ctx) { (void)ctx; return g_clock_ms; }

/* A sender that never answers in time. */
static int timeout_sender(void *ctx, const char *url, const char *headers, const char *body, size_t len,
                          int timeout_ms, oc_owh_reply *out) {
    (void)ctx; (void)url; (void)headers; (void)body; (void)len;
    memset(out, 0, sizeof *out);
    out->timed_out = timeout_ms == OC_OUTWH_TIMEOUT_MS;
    return -1;
}

static const uint8_t KEY[OC_FACTOR_KEY_LEN] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
                                                7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };

static void test_signature(void) {
    /* The convention's own worked example. */
    const char *body = "token=xyzz0WbapA4vBCDEFasx0q6G&team_id=T1DC2JH3J&team_domain=testteamnow&channel_id=G8PSS9T3V"
                       "&channel_name=foobar&user_id=U2CERLKJA&user_name=roadrunner&command=%2Fwebhook-collect&text="
                       "&response_url=https%3A%2F%2Fhooks.slack.com%2Fcommands%2FT1DC2JH3J%2F397700885554%2F96rGlfmibIGlgcZRskXaIFfN"
                       "&trigger_id=398738663015.47445629121.803a0bc887a14d10d2c447fce8b6703c";
    char sig[3 + 64 + 1];
    CHECK(oc_owh_sign("8f742231b10e8888abcd99yyyzzz85a5", 1531420618ull, body, strlen(body), sig) == 0);
    CHECK(strcmp(sig, "v0=a2114d57b48eac39b9ad189dd8316235a7b4a8d21a10bd27519666489c69b503") == 0);
    uint8_t raw[OC_OUTWH_SECRET_LEN];
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = (uint8_t)(i * 17);
    char text[2 * OC_OUTWH_SECRET_LEN + 1];
    oc_owh_secret_text(raw, text);
    CHECK(strlen(text) == 2 * OC_OUTWH_SECRET_LEN && strncmp(text, "0011223344", 10) == 0);
    /* What an endpoint's answer to the check may be. */
    CHECK(oc_owh_echoes("abc123", 6, "abc123"));
    CHECK(oc_owh_echoes(" abc123\r\n", 9, "abc123"));
    CHECK(oc_owh_echoes("{\"challenge\":\"abc123\"}", 22, "abc123"));
    CHECK(!oc_owh_echoes("abc12", 5, "abc123"));
    CHECK(!oc_owh_echoes("{\"challenge\":\"nope\"}", 20, "abc123"));
    CHECK(!oc_owh_echoes(NULL, 0, "abc123"));
}

/* The endpoint check: signed, and passed only by an endpoint that echoes. */
static void test_check(void) {
    receiver rx;
    pthread_t th;
    char url[96];
    const int modes[] = { RX_ECHO, RX_ECHO_JSON, RX_WRONG, RX_FAIL };
    CHECK(rx_start(&rx, &th, modes, 4, url, sizeof url) == 0);
    oc_webhooks_out *o = oc_webhooks_out_start(KEY, NULL, test_clock, NULL);
    CHECK(o != NULL);
    if (!o) return;
    uint8_t secret[OC_OUTWH_SECRET_LEN];
    memset(secret, 0xa5, sizeof secret);
    char text[2 * OC_OUTWH_SECRET_LEN + 1];
    oc_owh_secret_text(secret, text);
    const int want[] = { 1, 1, 0, 0 };
    for (int i = 0; i < 4; i++) {
        oc_webhooks_out_check(o, 100u + (uint64_t)i, url, secret);
        oc_owh_result r;
        CHECK(next_result(o, &r) && r.check_tag == 100u + (uint64_t)i && r.ok == want[i]);
    }
    oc_webhooks_out_stop(o);
    rx_end(&rx, th);
    CHECK(rx.seen == 4);
    CHECK(strstr(rx.body[0], "\"type\":\"url_verification\"") && strstr(rx.body[0], "\"challenge\":\""));
    CHECK(strncmp(rx.head[0], "POST /events ", 13) == 0);
    CHECK(signed_by(rx.head[0], rx.body[0], text));
    char ts[32];
    header_value(rx.head[0], "X-Slack-Request-Timestamp:", ts, sizeof ts);
    CHECK(strcmp(ts, "1730295731") == 0);                   /* the injected clock, in seconds */
    CHECK(strcmp(rx.body[0], rx.body[1]) != 0);             /* a new challenge each time */
}

static oc_outwh_due due_for(uint64_t id, uint64_t hook, uint32_t attempts, const char *reason, char *url,
                            const char *payload, const uint8_t secret[OC_OUTWH_SECRET_LEN]) {
    oc_outwh_due d;
    memset(&d, 0, sizeof d);
    d.id = id; d.webhook_id = hook; d.attempts = attempts;
    snprintf(d.last_reason, sizeof d.last_reason, "%s", reason);
    d.url = url; d.event_id = (char *)"Ev01"; d.payload = (char *)payload;
    if (oc_factor_seal(KEY, hook, secret, d.sealed) == 0) d.sealed_len = OC_TOTP_SEALED_LEN;
    return d;
}

/* Deliveries: signed with the secret opened from its seal, retries marked,
 * each outcome with the reason the convention names. */
static void test_deliveries(void) {
    receiver rx;
    pthread_t th;
    char url[96];
    const int modes[] = { RX_OK, RX_FAIL };
    CHECK(rx_start(&rx, &th, modes, 2, url, sizeof url) == 0);
    oc_webhooks_out *o = oc_webhooks_out_start(KEY, NULL, test_clock, NULL);
    CHECK(o != NULL);
    if (!o) return;
    uint8_t secret[OC_OUTWH_SECRET_LEN];
    memset(secret, 0x3c, sizeof secret);
    char text[2 * OC_OUTWH_SECRET_LEN + 1];
    oc_owh_secret_text(secret, text);
    const char *payload = "{\"type\":\"event_callback\",\"event_id\":\"Ev01\"}";

    oc_outwh_due d = due_for(41, 5, 0, "", url, payload, secret);
    oc_webhooks_out_deliver(o, &d);
    oc_owh_result r;
    CHECK(next_result(o, &r) && r.delivery == 41 && r.ok == 1);
    d = due_for(42, 5, 2, "http_timeout", url, payload, secret);
    oc_webhooks_out_deliver(o, &d);
    CHECK(next_result(o, &r) && r.delivery == 42 && r.ok == 0 && strcmp(r.reason, "http_error") == 0);
    rx_end(&rx, th);
    CHECK(rx.seen == 2);
    CHECK(strcmp(rx.body[0], payload) == 0);
    CHECK(signed_by(rx.head[0], rx.body[0], text));
    CHECK(strcasestr(rx.head[0], "Content-Type: application/json") != NULL);   /* contract: the envelope is JSON */
    CHECK(strcasestr(rx.head[0], "X-Slack-Retry-Num") == NULL);   /* a first attempt is no retry */
    char v[32];
    header_value(rx.head[1], "X-Slack-Retry-Num:", v, sizeof v);
    CHECK(strcmp(v, "2") == 0);
    header_value(rx.head[1], "X-Slack-Retry-Reason:", v, sizeof v);
    CHECK(strcmp(v, "http_timeout") == 0);

    /* Nobody listening: connection_failed. */
    int port = oc_test_free_port();
    char closed[96];
    snprintf(closed, sizeof closed, "http://127.0.0.1:%d/events", port);
    d = due_for(43, 5, 0, "", closed, payload, secret);
    oc_webhooks_out_deliver(o, &d);
    CHECK(next_result(o, &r) && r.delivery == 43 && r.ok == 0 && strcmp(r.reason, "connection_failed") == 0);
    /* A secret sealed for another webhook does not open: nothing is sent. */
    d = due_for(44, 5, 0, "", closed, payload, secret);
    d.webhook_id = 6;
    oc_webhooks_out_deliver(o, &d);
    CHECK(next_result(o, &r) && r.delivery == 44 && r.ok == 0);
    oc_webhooks_out_stop(o);

    /* No answer in time: http_timeout. */
    o = oc_webhooks_out_start(KEY, timeout_sender, test_clock, NULL);
    CHECK(o != NULL);
    if (!o) return;
    d = due_for(45, 5, 0, "", url, payload, secret);
    oc_webhooks_out_deliver(o, &d);
    CHECK(next_result(o, &r) && r.delivery == 45 && r.ok == 0 && strcmp(r.reason, "http_timeout") == 0);
    oc_webhooks_out_stop(o);
}

/* ---- the whole path: the writer's queue, the worker, the receiver ---- */

static oc_dbres *wait_type(oc_dbwriter *w, int type) {
    for (int guard = 0; guard < 3000; guard++) {     /* only a guard against a hang */
        oc_dbres *r;
        while ((r = oc_dbwriter_next_result(w)) != NULL) {
            if (r->type == type) return r;
            oc_dbres_free(r);
        }
        struct pollfd pf = { oc_dbwriter_eventfd(w), POLLIN, 0 };
        if (poll(&pf, 1, 10) > 0) { uint64_t v; ssize_t rd = read(pf.fd, &v, sizeof v); (void)rd; }
    }
    return NULL;
}

/* Take what is due and hand it to the worker, as the net loop does; then hand
 * the outcomes back. How many were delivered. */
static size_t pass(oc_dbwriter *w, oc_webhooks_out *o, oc_owh_result *last) {
    oc_dbwriter_submit(w, oc_job_new(OC_JOB_OUTWH_TAKE, 0));
    oc_dbres *r = wait_type(w, OC_RES_OUTWH_DUE);
    size_t n = r ? r->n_owh_due : 0;
    for (size_t i = 0; i < n; i++) oc_webhooks_out_deliver(o, &r->owh_due[i]);
    oc_dbres_free(r);
    for (size_t i = 0; i < n; i++) {
        oc_owh_result res;
        if (!next_result(o, &res)) break;
        if (last) *last = res;
        oc_job *j = oc_job_new(OC_JOB_OUTWH_DONE, 0);
        j->owh_delivery = res.delivery;
        j->owh_ok = (uint8_t)res.ok;
        j->owh_reason = strdup(res.reason);
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_type(w, OC_RES_OUTWH_DONE));
    }
    return n;
}

static void test_path(void) {
    char dir[256], path[300];
    CHECK(oc_test_mkdtemp("webhooks-out", dir, sizeof dir) == 0);
    oc_test_path(path, sizeof path, dir, "w.db");
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) return;
    oc_dbwriter_set_pw_mem(w, 2048);
    oc_dbwriter_set_factor_key(w, KEY);
    uint64_t own = oc_dbwriter_register_local(w, "path-own", "pw-own-1", OC_ROLE_OWNER, 2048);
    CHECK(own != 0);
    oc_job *j = oc_job_new(OC_JOB_CREATE_CHANNEL, 1);
    j->user_id = own; j->ch_is_public = 1; j->ch_name = strdup("path-hooks");
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_type(w, OC_RES_CHANNEL_INFO);
    uint64_t ch = r ? r->channel_id : 0;
    oc_dbres_free(r);
    CHECK(ch != 0);

    receiver rx;
    pthread_t th;
    char url[96];
    /* The check; a first attempt and two retries failing; the third retry
     * answered; then, a day on, one more failure. */
    const int modes[] = { RX_ECHO, RX_FAIL, RX_FAIL, RX_FAIL, RX_OK, RX_FAIL, RX_FAIL };
    CHECK(rx_start(&rx, &th, modes, 7, url, sizeof url) == 0);
    oc_webhooks_out *o = oc_webhooks_out_start(KEY, NULL, test_clock, NULL);
    CHECK(o != NULL);
    if (!o) { oc_dbwriter_stop(w); return; }

    /* Created as the net loop does: checked by the writer, then the endpoint,
     * then saved. A loopback address is a test's: the writer wants https, so
     * the saved address is rewritten to the receiver after. */
    j = oc_job_new(OC_JOB_OUTWH_CREATE, 2);
    j->user_id = own; j->owh_url = strdup("https://receiver.test/events"); j->owh_events = strdup("message");
    j->owh_n_channels = 1; j->owh_channels[0] = ch;
    oc_dbwriter_submit(w, j);
    r = wait_type(w, OC_RES_OUTWH_CHECK);
    CHECK(r != NULL);
    uint8_t secret[OC_OUTWH_SECRET_LEN] = { 0 };
    if (r) memcpy(secret, r->owh_secret, sizeof secret);
    oc_dbres_free(r);
    oc_webhooks_out_check(o, 9, url, secret);
    oc_owh_result res;
    CHECK(next_result(o, &res) && res.check_tag == 9 && res.ok == 1);
    j = oc_job_new(OC_JOB_OUTWH_CREATE, 2);
    j->user_id = own; j->owh_url = strdup("https://receiver.test/events"); j->owh_events = strdup("message");
    j->owh_n_channels = 1; j->owh_channels[0] = ch; j->owh_verified = 1;
    memcpy(j->owh_secret, secret, sizeof secret);
    oc_dbwriter_submit(w, j);
    r = wait_type(w, OC_RES_OUTWH_SECRET);
    uint64_t hook = r ? r->message_id : 0;
    oc_dbres_free(r);
    CHECK(hook != 0);
    {
        sqlite3 *db = NULL;
        sqlite3_stmt *st = NULL;
        if (sqlite3_open(path, &db) == SQLITE_OK &&
            sqlite3_prepare_v2(db, "UPDATE outgoing_webhooks SET url=?1 WHERE id=?2;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, url, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)hook);
            CHECK(sqlite3_step(st) == SQLITE_DONE);
        }
        sqlite3_finalize(st);
        sqlite3_close(db);
    }

    /* A message: queued, delivered, failing, retried, then answered. */
    j = oc_job_new(OC_JOB_SEND, 3);
    j->user_id = own; j->channel_id = ch;
    memset(j->idem, 0x51, OC_IDEM_LEN);
    oc_job_set_body(j, "ship it", 7);
    oc_dbwriter_submit(w, j);
    oc_dbres_free(wait_type(w, OC_RES_SEND_OK));
    CHECK(pass(w, o, &res) == 1 && !res.ok);
    for (int a = 1; a <= 3; a++) {
        oc_dbwriter_advance_clock_ms((uint64_t)a * 120000u);
        CHECK(pass(w, o, &res) == 1 && res.ok == (a == 3));
    }
    CHECK(pass(w, o, NULL) == 0);                           /* done: nothing more due */

    /* Another, failing for a day: the webhook is turned off. */
    j = oc_job_new(OC_JOB_SEND, 3);
    j->user_id = own; j->channel_id = ch;
    memset(j->idem, 0x52, OC_IDEM_LEN);
    oc_job_set_body(j, "again", 5);
    oc_dbwriter_submit(w, j);
    oc_dbres_free(wait_type(w, OC_RES_SEND_OK));
    CHECK(pass(w, o, &res) == 1 && !res.ok);
    oc_dbwriter_advance_clock_ms(OC_OUTWH_DISABLE_MS + 600000u);
    oc_dbwriter_submit(w, oc_job_new(OC_JOB_OUTWH_TAKE, 0));
    r = wait_type(w, OC_RES_OUTWH_DUE);
    CHECK(r && r->n_owh_due == 1);
    if (r && r->n_owh_due == 1) oc_webhooks_out_deliver(o, &r->owh_due[0]);
    oc_dbres_free(r);
    CHECK(next_result(o, &res) && !res.ok);
    j = oc_job_new(OC_JOB_OUTWH_DONE, 0);
    j->owh_delivery = res.delivery; j->owh_reason = strdup(res.reason);
    oc_dbwriter_submit(w, j);
    r = wait_type(w, OC_RES_OUTWH_DONE);
    CHECK(r && r->owh_disabled == 1);
    oc_dbres_free(r);
    CHECK(pass(w, o, NULL) == 0);                           /* off: nothing is taken */

    oc_webhooks_out_stop(o);
    rx_end(&rx, th);
    CHECK(rx.seen == 7);
    char text[2 * OC_OUTWH_SECRET_LEN + 1];
    oc_owh_secret_text(secret, text);
    char v[32];
    for (int i = 1; i < 7; i++) CHECK(signed_by(rx.head[i], rx.body[i], text));
    CHECK(strcasestr(rx.head[1], "X-Slack-Retry-Num") == NULL);
    header_value(rx.head[2], "X-Slack-Retry-Num:", v, sizeof v);
    CHECK(strcmp(v, "1") == 0);
    header_value(rx.head[4], "X-Slack-Retry-Num:", v, sizeof v);
    CHECK(strcmp(v, "3") == 0);
    header_value(rx.head[4], "X-Slack-Retry-Reason:", v, sizeof v);
    CHECK(strcmp(v, "http_error") == 0);
    /* The same event each time it was tried. */
    char e1[64], e4[64];
    const char *p1 = strstr(rx.body[1], "\"event_id\":\""), *p4 = strstr(rx.body[4], "\"event_id\":\"");
    CHECK(p1 && p4);
    if (p1 && p4) {
        snprintf(e1, sizeof e1, "%.*s", (int)strcspn(p1 + 12, "\""), p1 + 12);
        snprintf(e4, sizeof e4, "%.*s", (int)strcspn(p4 + 12, "\""), p4 + 12);
        CHECK(strcmp(e1, e4) == 0);
    }
    CHECK(strstr(rx.body[1], "\"text\":\"ship it\"") != NULL);   /* contract: the message as sent */

    oc_dbwriter_stop(w);
    oc_dbwriter_advance_clock_ms(0);
    oc_test_rmtree(dir);
}

int run_webhooks_out_tests(void) {
    printf("test_webhooks_out: the signature, the endpoint check, deliveries and their retries, the path from the queue\n");
    test_signature();
    test_check();
    test_deliveries();
    test_path();
    return failures;
}
