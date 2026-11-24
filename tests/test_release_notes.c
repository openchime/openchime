/* Release notes for the clients (release_notes.h, PROTOCOL.md §5.16r), against
 * a stand-in for central: the GET goes to /v1/workspace/releases/build/<id>,
 * signed over the method, that path and an empty body; an answer is kept six
 * hours and a failure five minutes (on an injected clock); askers during a fetch
 * share it; the thirty-third build id drops the least recently used; and a body
 * larger than a frame carries is a failure. */

#include "check.h"
#include "release_notes.h"
#include "enroll.h"
#include "protocol.h"

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int         fd;
    const char *pk;            /* the enrollment key, to check signatures */
    const char *aud;
    pthread_mutex_t mu;
    int         requests, bad_sigs, stop;
    char        last_path[160];
    char       *big;           /* a body over the frame's room */
} fake_central;

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

static int signature_holds(const char *pk_pem, const char *method, const char *path, const char *aud,
                           long ts, const char *sig_b64) {
    uint8_t sig[160]; size_t siglen = 0;
    if (mbedtls_base64_decode(sig, sizeof sig, &siglen, (const unsigned char *)sig_b64, strlen(sig_b64)) != 0)
        return 0;
    mbedtls_pk_context kp;
    mbedtls_pk_init(&kp);
    int ok = 0;
    if (mbedtls_pk_parse_key(&kp, (const unsigned char *)pk_pem, strlen(pk_pem) + 1, NULL, 0, NULL, NULL) == 0) {
        uint8_t bh[32];
        mbedtls_sha256((const unsigned char *)"", 0, bh, 0);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", bh[i]);
        char canon[640];
        int cn = snprintf(canon, sizeof canon, "openchime-machine-v2|%s|%s|%s|%ld|%s", method, path, aud, ts, hex);
        uint8_t h[32];
        mbedtls_sha256((const unsigned char *)canon, (size_t)cn, h, 0);
        ok = mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen) == 0;
    }
    mbedtls_pk_free(&kp);
    return ok;
}

static void *fake_thread(void *arg) {
    fake_central *f = arg;
    for (;;) {
        pthread_mutex_lock(&f->mu);
        int stop = f->stop;
        pthread_mutex_unlock(&f->mu);
        if (stop) break;
        struct pollfd pfd = { f->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 50) <= 0) continue;
        int c = accept(f->fd, NULL, NULL);
        if (c < 0) continue;
        char buf[4096];
        size_t total = 0;
        while (total < sizeof buf - 1) {
            ssize_t n = read(c, buf + total, sizeof buf - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }
        char method[8] = "", path[160] = "", aud[128], ts[32], sig[256];
        sscanf(buf, "%7s %159s", method, path);
        header_value(buf, "X-OpenChime-Audience:", aud, sizeof aud);
        header_value(buf, "X-OpenChime-Timestamp:", ts, sizeof ts);
        header_value(buf, "X-OpenChime-Signature:", sig, sizeof sig);
        int good = !strcmp(method, "GET") && !strcmp(aud, f->aud) &&
                   signature_holds(f->pk, "GET", path, aud, strtol(ts, NULL, 10), sig);
        /* Not over the path before /v1. */
        if (good && signature_holds(f->pk, "GET", "/api/releases/build/rel", aud, strtol(ts, NULL, 10), sig)) good = 0;
        pthread_mutex_lock(&f->mu);
        f->requests++;
        if (!good) f->bad_sigs++;
        snprintf(f->last_path, sizeof f->last_path, "%s", path);
        pthread_mutex_unlock(&f->mu);
        const char *P = "/v1/workspace/releases/build/";
        const char *id = strncmp(path, P, strlen(P)) == 0 ? path + strlen(P) : "";
        int status = 404;
        const char *body = "";
        if (!good) status = 401;
        else if (!strncmp(id, "rel", 3)) { status = 200; body = "{\"release\":21,\"sections\":[]}"; }
        else if (!strcmp(id, "fail")) status = 500;
        else if (!strcmp(id, "big")) { status = 200; body = f->big; }
        size_t blen = strlen(body);
        char head[256];
        int hn = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, blen);
        ssize_t wr = write(c, head, (size_t)hn);
        size_t off = 0;
        while (wr >= 0 && off < blen) {
            wr = write(c, body + off, blen - off);
            if (wr > 0) off += (size_t)wr;
        }
        close(c);
    }
    return NULL;
}

static uint64_t g_now;
static uint64_t test_clock(void *ctx) { (void)ctx; return g_now; }

/* The answer for `id`, from the cache or after the fetch; waits on the worker's
 * eventfd, not on time. */
static int ask(oc_release_notes_worker *w, const char *id, oc_rn_answer *a) {
    int r = oc_release_notes_ask(w, id, 7, a);
    if (r == 1) return 1;
    if (r != 0) return -1;
    for (;;) {
        if (oc_release_notes_next(w, a)) return 0;
        struct pollfd pfd = { oc_release_notes_eventfd(w), POLLIN, 0 };
        if (poll(&pfd, 1, 5000) <= 0) return -1;
        uint64_t cnt; ssize_t rd = read(pfd.fd, &cnt, sizeof cnt); (void)rd;
    }
}

static int requests(fake_central *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->requests;
    pthread_mutex_unlock(&f->mu);
    return n;
}

int run_release_notes_tests(void) {
    printf("test_release_notes: the signed GET and its path, the six-hour cache, the five-minute failure, "
           "the 32-entry LRU, the oversized body\n");
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);

    fake_central f;
    memset(&f, 0, sizeof f);
    pthread_mutex_init(&f.mu, NULL);
    f.pk = pk; f.aud = aud;
    f.big = malloc(OC_RELEASE_NOTES_MAX + 11);
    memset(f.big, 'x', OC_RELEASE_NOTES_MAX + 10);
    f.big[OC_RELEASE_NOTES_MAX + 10] = '\0';
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 16) == 0);
    socklen_t sl = sizeof sa;
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    f.fd = lfd;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fake_thread, &f) == 0);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", ntohs(sa.sin_port));

    g_now = 1000000;
    oc_release_notes_worker *w = oc_release_notes_start(url, aud, pk, test_clock, NULL);
    CHECK(w != NULL);
    if (!w) return failures;
    oc_rn_answer a;

    /* A release: fetched once, signed, at the new path. */
    CHECK(ask(w, "rel1", &a) == 0);
    CHECK(a.ok && a.status == 200 && a.body && strstr(a.body, "\"release\":21") && !strcmp(a.build_id, "rel1"));
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 1 && f.bad_sigs == 0 && !strcmp(f.last_path, "/v1/workspace/releases/build/rel1"));

    /* Within six hours, from the cache; after, fetched again. */
    g_now += OC_RELEASE_NOTES_CACHE_MS - 1;
    CHECK(ask(w, "rel1", &a) == 1 && a.ok && a.status == 200);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 1);
    g_now += 2;
    CHECK(ask(w, "rel1", &a) == 0 && a.ok);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 2);

    /* A build that is no release: 404, kept as an answer. */
    CHECK(ask(w, "dev7", &a) == 0 && a.ok && a.status == 404 && !a.body);
    oc_rn_answer_free(&a);
    CHECK(ask(w, "dev7", &a) == 1 && a.ok && a.status == 404);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 3);

    /* A failure: unavailable, held five minutes, then tried again. */
    CHECK(ask(w, "fail", &a) == 0 && !a.ok);
    oc_rn_answer_free(&a);
    g_now += OC_RELEASE_NOTES_FAIL_MS - 1;
    CHECK(ask(w, "fail", &a) == 1 && !a.ok);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 4);
    g_now += 2;
    CHECK(ask(w, "fail", &a) == 0 && !a.ok);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 5);

    /* A body larger than a frame carries is a failure. */
    CHECK(ask(w, "big", &a) == 0 && !a.ok && !a.body);
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == 6);

    /* Thirty-two build ids are held; a thirty-third drops the least recently
     * used. "rel1" is asked last, so it stays; "dev7" is dropped. */
    char id[16];
    for (int i = 0; i < OC_RELEASE_NOTES_CACHE_MAX - 4; i++) {
        g_now += 1;
        snprintf(id, sizeof id, "rel-%d", i);
        CHECK(ask(w, id, &a) == 0 && a.ok);
        oc_rn_answer_free(&a);
    }
    g_now += 1;
    CHECK(ask(w, "rel1", &a) == 1);   /* touched: now the most recently used */
    oc_rn_answer_free(&a);
    int before = requests(&f);
    g_now += 1;
    CHECK(ask(w, "rel-new", &a) == 0 && a.ok);   /* the 33rd */
    oc_rn_answer_free(&a);
    CHECK(ask(w, "rel1", &a) == 1);              /* still held */
    oc_rn_answer_free(&a);
    CHECK(ask(w, "dev7", &a) == 0);              /* dropped: fetched again */
    oc_rn_answer_free(&a);
    CHECK(requests(&f) == before + 2);

    oc_release_notes_stop(w);
    pthread_mutex_lock(&f.mu); f.stop = 1; pthread_mutex_unlock(&f.mu);
    pthread_join(th, NULL);
    close(lfd);
    free(f.big);
    pthread_mutex_destroy(&f.mu);
    return failures;
}
