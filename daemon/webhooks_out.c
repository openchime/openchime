/* Outgoing webhooks' delivery worker (webhooks_out.h, docs/details/WEBHOOKS.md). */
#include "webhooks_out.h"

#include "auth.h"           /* oc_rand_bytes */
#include "https_client.h"
#include "json.h"

#include <errno.h>
#include <mbedtls/md.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define OWH_RESULTS_MAX 256

typedef struct task {
    int           check;            /* an endpoint check, not a delivery */
    oc_outwh_due  d;                /* a delivery: its strings heap */
    char          last_reason[24];  /* why the attempt before failed */
    uint64_t      tag;              /* a check's */
    char         *url;              /* a check's, heap */
    uint8_t       secret[OC_OUTWH_SECRET_LEN];
    struct task  *next;
} task;

struct oc_webhooks_out {
    uint8_t         key[OC_FACTOR_KEY_LEN];
    oc_owh_sender   send;
    oc_owh_clock    clock;
    void           *ctx;
    int             evfd;

    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    task           *head, *tail;
    int             pending;        /* deliveries handed over, not finished */
    oc_owh_result   results[OWH_RESULTS_MAX];
    int             r_head, r_count;
    int             stopping;
};

void oc_owh_secret_text(const uint8_t raw[OC_OUTWH_SECRET_LEN], char out[2 * OC_OUTWH_SECRET_LEN + 1]) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < OC_OUTWH_SECRET_LEN; i++) {
        out[2 * i] = hx[raw[i] >> 4];
        out[2 * i + 1] = hx[raw[i] & 15];
    }
    out[2 * OC_OUTWH_SECRET_LEN] = '\0';
}

int oc_owh_sign(const char *secret_text, uint64_t ts, const char *body, size_t len, char out[3 + 64 + 1]) {
    const mbedtls_md_info_t *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    unsigned char mac[32];
    char pre[32];
    int pn = snprintf(pre, sizeof pre, "v0:%llu:", (unsigned long long)ts);
    if (!sha || pn < 0 || (size_t)pn >= sizeof pre) return -1;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, sha, 1) != 0 ||
             mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret_text, strlen(secret_text)) != 0 ||
             mbedtls_md_hmac_update(&ctx, (const unsigned char *)pre, (size_t)pn) != 0 ||
             mbedtls_md_hmac_update(&ctx, (const unsigned char *)body, len) != 0 ||
             mbedtls_md_hmac_finish(&ctx, mac) != 0;
    mbedtls_md_free(&ctx);
    if (rc) return -1;
    static const char hx[] = "0123456789abcdef";
    memcpy(out, "v0=", 3);
    for (size_t i = 0; i < sizeof mac; i++) {
        out[3 + 2 * i] = hx[mac[i] >> 4];
        out[3 + 2 * i + 1] = hx[mac[i] & 15];
    }
    out[3 + 64] = '\0';
    return 0;
}

int oc_owh_echoes(const char *body, size_t len, const char *challenge) {
    if (!body || !challenge || !*challenge) return 0;
    size_t s = 0, e = len;
    while (s < e && (body[s] == ' ' || body[s] == '\r' || body[s] == '\n' || body[s] == '\t')) s++;
    while (e > s && (body[e - 1] == ' ' || body[e - 1] == '\r' || body[e - 1] == '\n' || body[e - 1] == '\t')) e--;
    size_t cl = strlen(challenge);
    if (e - s == cl && memcmp(body + s, challenge, cl) == 0) return 1;
    if (e > s && body[s] == '{') {
        oc_json d;
        if (oc_json_parse(&d, body + s, e - s) != 0) return 0;
        char got[128];
        int ok = oc_json_get_str(&d, 0, "challenge", got, sizeof got) == 0 && strcmp(got, challenge) == 0;
        oc_json_free(&d);
        return ok;
    }
    return 0;
}

/* ---- the real sender and clock ---- */

static int https_send(void *ctx, const char *url, const char *headers, const char *body, size_t len,
                      int timeout_ms, oc_owh_reply *out) {
    (void)ctx;
    memset(out, 0, sizeof *out);
    oc_https_resp r;
    char err[256] = "";
    errno = 0;
    if (oc_https_request("POST", url, "application/json", body, len, headers, timeout_ms, &r, err, sizeof err) != 0) {
        int e = errno;
        out->timed_out = (e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT) && strncmp(err, "read from", 9) == 0;
        return -1;
    }
    out->status = r.status;
    out->body = r.body;           /* taken over */
    out->body_len = r.body_len;
    r.body = NULL;
    oc_https_resp_free(&r);
    return 0;
}

static uint64_t real_clock(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- the worker ---- */

static void wipe(void *p, size_t n) {
    for (volatile unsigned char *z = p; n--; z++) *z = 0;
}

static void push_result(oc_webhooks_out *o, const oc_owh_result *res) {
    pthread_mutex_lock(&o->mu);
    if (o->r_count < OWH_RESULTS_MAX) {
        o->results[(o->r_head + o->r_count) % OWH_RESULTS_MAX] = *res;
        o->r_count++;
    }
    pthread_mutex_unlock(&o->mu);
    uint64_t one = 1;
    ssize_t wr = write(o->evfd, &one, sizeof one);
    (void)wr;
}

/* POST `body`, signed with `secret` (its text); `retry` the headers a retry
 * adds, or "". 1 when a 2xx came back, else 0 with the retry reason, and the
 * reply's body when `reply` asks for it. */
static int post_signed(oc_webhooks_out *o, const char *url, const char *secret, const char *body, size_t len,
                       const char *retry, char reason[24], oc_owh_reply *keep) {
    uint64_t ts = o->clock(o->ctx) / 1000u;
    char sig[3 + 64 + 1];
    if (oc_owh_sign(secret, ts, body, len, sig) != 0) { snprintf(reason, 24, "connection_failed"); return 0; }
    char headers[512];
    snprintf(headers, sizeof headers, "X-Slack-Request-Timestamp: %llu\r\nX-Slack-Signature: %s\r\n%s",
             (unsigned long long)ts, sig, retry);
    oc_owh_reply rep;
    memset(&rep, 0, sizeof rep);
    int sent = o->send(o->ctx, url, headers, body, len, OC_OUTWH_TIMEOUT_MS, &rep);
    int ok = sent == 0 && rep.status >= 200 && rep.status < 300;
    if (!ok) snprintf(reason, 24, "%s", sent != 0 ? (rep.timed_out ? "http_timeout" : "connection_failed") : "http_error");
    if (keep) *keep = rep;
    else free(rep.body);
    return ok;
}

static void run_delivery(oc_webhooks_out *o, task *t) {
    oc_owh_result res;
    memset(&res, 0, sizeof res);
    res.delivery = t->d.id;
    uint8_t raw[OC_OUTWH_SECRET_LEN];
    char secret[2 * OC_OUTWH_SECRET_LEN + 1];
    if (oc_factor_open(o->key, t->d.webhook_id, t->d.sealed, t->d.sealed_len, raw) != 0) {
        snprintf(res.reason, sizeof res.reason, "connection_failed");   /* nothing can be signed */
        push_result(o, &res);
        return;
    }
    oc_owh_secret_text(raw, secret);
    wipe(raw, sizeof raw);
    char retry[96] = "";
    if (t->d.attempts)
        snprintf(retry, sizeof retry, "X-Slack-Retry-Num: %u\r\nX-Slack-Retry-Reason: %s\r\n",
                 (unsigned)t->d.attempts, t->last_reason[0] ? t->last_reason : "http_error");
    res.ok = post_signed(o, t->d.url, secret, t->d.payload, strlen(t->d.payload), retry, res.reason, NULL);
    wipe(secret, sizeof secret);
    push_result(o, &res);
}

static void run_check(oc_webhooks_out *o, task *t) {
    oc_owh_result res;
    memset(&res, 0, sizeof res);
    res.check_tag = t->tag;
    uint8_t rnd[16];
    char challenge[2 * sizeof rnd + 1], secret[2 * OC_OUTWH_SECRET_LEN + 1], body[96];
    if (oc_rand_bytes(rnd, sizeof rnd) == 0) {
        for (size_t i = 0; i < sizeof rnd; i++) snprintf(challenge + 2 * i, 3, "%02x", rnd[i]);
        int n = snprintf(body, sizeof body, "{\"type\":\"url_verification\",\"challenge\":\"%s\"}", challenge);
        oc_owh_secret_text(t->secret, secret);
        oc_owh_reply rep;
        memset(&rep, 0, sizeof rep);
        if (post_signed(o, t->url, secret, body, (size_t)n, "", res.reason, &rep))
            res.ok = oc_owh_echoes(rep.body, rep.body ? rep.body_len : 0, challenge);
        free(rep.body);
        wipe(secret, sizeof secret);
    }
    push_result(o, &res);
}

static void task_free(task *t) {
    if (!t) return;
    free(t->d.url); free(t->d.event_id); free(t->d.payload);
    free(t->url);
    wipe(t->secret, sizeof t->secret);
    free(t);
}

static void *worker_main(void *arg) {
    oc_webhooks_out *o = arg;
    for (;;) {
        pthread_mutex_lock(&o->mu);
        while (!o->head && !o->stopping) pthread_cond_wait(&o->cv, &o->mu);
        if (o->stopping) { pthread_mutex_unlock(&o->mu); break; }
        task *t = o->head;
        o->head = t->next;
        if (!o->head) o->tail = NULL;
        pthread_mutex_unlock(&o->mu);
        if (t->check) run_check(o, t);
        else run_delivery(o, t);
        if (!t->check) {
            pthread_mutex_lock(&o->mu);
            o->pending--;
            pthread_mutex_unlock(&o->mu);
        }
        task_free(t);
    }
    return NULL;
}

oc_webhooks_out *oc_webhooks_out_start(const uint8_t key[OC_FACTOR_KEY_LEN], oc_owh_sender send,
                                       oc_owh_clock clock, void *ctx) {
    if (!key) return NULL;
    oc_webhooks_out *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    memcpy(o->key, key, sizeof o->key);
    o->send = send ? send : https_send;
    o->clock = clock ? clock : real_clock;
    o->ctx = ctx;
    o->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_mutex_init(&o->mu, NULL);
    pthread_cond_init(&o->cv, NULL);
    if (o->evfd < 0 || pthread_create(&o->thread, NULL, worker_main, o) != 0) {
        if (o->evfd >= 0) close(o->evfd);
        pthread_mutex_destroy(&o->mu);
        pthread_cond_destroy(&o->cv);
        wipe(o->key, sizeof o->key);
        free(o);
        return NULL;
    }
    return o;
}

int oc_webhooks_out_eventfd(oc_webhooks_out *o) { return o ? o->evfd : -1; }

int oc_webhooks_out_pending(oc_webhooks_out *o) {
    if (!o) return 0;
    pthread_mutex_lock(&o->mu);
    int n = o->pending;
    pthread_mutex_unlock(&o->mu);
    return n;
}

/* Queue `t`: a check goes first, so a person creating a webhook does not wait
 * behind the deliveries. */
static void enqueue(oc_webhooks_out *o, task *t) {
    pthread_mutex_lock(&o->mu);
    if (o->stopping) { pthread_mutex_unlock(&o->mu); task_free(t); return; }
    if (t->check) {
        t->next = o->head;
        o->head = t;
        if (!o->tail) o->tail = t;
    } else {
        if (o->tail) o->tail->next = t; else o->head = t;
        o->tail = t;
        o->pending++;
    }
    pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

void oc_webhooks_out_deliver(oc_webhooks_out *o, const oc_outwh_due *d) {
    if (!o || !d || !d->url || !d->payload) return;
    task *t = calloc(1, sizeof *t);
    if (!t) return;
    t->d = *d;
    t->d.url = strdup(d->url);
    t->d.event_id = d->event_id ? strdup(d->event_id) : NULL;
    t->d.payload = strdup(d->payload);
    snprintf(t->last_reason, sizeof t->last_reason, "%s", d->last_reason);
    if (!t->d.url || !t->d.payload) { task_free(t); return; }
    enqueue(o, t);
}

void oc_webhooks_out_check(oc_webhooks_out *o, uint64_t tag, const char *url,
                           const uint8_t secret[OC_OUTWH_SECRET_LEN]) {
    if (!o || !url) return;
    task *t = calloc(1, sizeof *t);
    if (!t) return;
    t->check = 1;
    t->tag = tag;
    t->url = strdup(url);
    memcpy(t->secret, secret, sizeof t->secret);
    if (!t->url) { task_free(t); return; }
    enqueue(o, t);
}

int oc_webhooks_out_next(oc_webhooks_out *o, oc_owh_result *r) {
    if (!o) return 0;
    pthread_mutex_lock(&o->mu);
    int got = o->r_count > 0;
    if (got) {
        *r = o->results[o->r_head];
        o->r_head = (o->r_head + 1) % OWH_RESULTS_MAX;
        o->r_count--;
    }
    pthread_mutex_unlock(&o->mu);
    return got;
}

void oc_webhooks_out_stop(oc_webhooks_out *o) {
    if (!o) return;
    pthread_mutex_lock(&o->mu);
    o->stopping = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    pthread_join(o->thread, NULL);
    for (task *t = o->head, *n; t; t = n) { n = t->next; task_free(t); }
    close(o->evfd);
    pthread_mutex_destroy(&o->mu);
    pthread_cond_destroy(&o->cv);
    wipe(o->key, sizeof o->key);
    free(o);
}
