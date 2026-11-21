/* The address report (address_report.h, AUTH.md §3.6): what each answer means,
 * the address read from central's reply, and the report itself against a
 * stand-in for central -- where it goes, that it is signed, that {} is sent as
 * it is, that no answer, 422, 429 and 5xx are tried again, that a refusal ends
 * it, that a round follows a recorded one, and that stopping ends a wait at once. */

#include "check.h"
#include "address_report.h"
#include "enroll.h"

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
#include <time.h>
#include <unistd.h>

static void test_disposition(void) {
    CHECK(oc_address_report_disposition(1, 200) == OC_ADDRESS_RECORDED);
    CHECK(oc_address_report_disposition(1, 204) == OC_ADDRESS_RECORDED);
    CHECK(oc_address_report_disposition(0, 0) == OC_ADDRESS_RETRY);      /* no answer at all */
    CHECK(oc_address_report_disposition(1, 422) == OC_ADDRESS_RETRY);    /* central could not check it */
    CHECK(oc_address_report_disposition(1, 429) == OC_ADDRESS_RETRY);
    CHECK(oc_address_report_disposition(1, 500) == OC_ADDRESS_RETRY);
    CHECK(oc_address_report_disposition(1, 503) == OC_ADDRESS_RETRY);
    static const int REFUSED[] = { 400, 401, 403, 404, 409 };
    for (size_t i = 0; i < sizeof REFUSED / sizeof REFUSED[0]; i++)
        CHECK(oc_address_report_disposition(1, REFUSED[i]) == OC_ADDRESS_REFUSED);
}

static void test_recorded(void) {
    char a[300];
    CHECK(oc_address_report_recorded("{\"address\":\"203.0.113.7\"}", a, sizeof a) == 0 &&
          strcmp(a, "203.0.113.7") == 0);
    CHECK(oc_address_report_recorded("{\"ok\":true,\"address\":\"chat.acme.example\"}", a, sizeof a) == 0 &&
          strcmp(a, "chat.acme.example") == 0);
    /* No address in the reply: the log says "the address this server is reached at". */
    CHECK(oc_address_report_recorded("", a, sizeof a) != 0);
    CHECK(oc_address_report_recorded("{}", a, sizeof a) != 0);
    CHECK(oc_address_report_recorded("{\"address\":7}", a, sizeof a) != 0);
    /* What is logged is printable: a reply cannot write control bytes into the log. */
    CHECK(oc_address_report_recorded("{\"address\":\"a\\nb\"}", a, sizeof a) != 0);
    CHECK(oc_address_report_recorded("{\"address\":\"203.0.113.7\"}", a, 5) != 0);
}

/* --- a stand-in for central ---------------------------------------------- */

#define FAKE_MAX 12

typedef struct {
    int  fd;
    int  statuses[FAKE_MAX];   /* the answer to each request; 0 = hang up unanswered */
    int  n_statuses;
    const char *reply;         /* the body of a 200 */
    int  seen;
    char path[FAKE_MAX][96];
    char aud[FAKE_MAX][128];
    long ts[FAKE_MAX];
    char sig[FAKE_MAX][256];
    char ver[FAKE_MAX][8];
    char body[FAKE_MAX][512];
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

/* Answer requests until none comes for a quarter second: longer than any wait
 * the test's timings leave between requests, short enough to cost little. */
static void *fake_central_thread(void *arg) {
    fake_central *f = arg;
    while (f->seen < FAKE_MAX) {
        struct pollfd pfd = { f->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 250) <= 0) break;
        int c = accept(f->fd, NULL, NULL);
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
        int i = f->seen++;
        if (strncmp(buf, "POST ", 5) == 0) {
            size_t pl = strcspn(buf + 5, " ");
            if (pl >= sizeof f->path[i]) pl = sizeof f->path[i] - 1;
            memcpy(f->path[i], buf + 5, pl);
            f->path[i][pl] = '\0';
        }
        char ts[32];
        header_value(buf, "X-OpenChime-Audience:", f->aud[i], sizeof f->aud[i]);
        header_value(buf, "X-OpenChime-Timestamp:", ts, sizeof ts);
        header_value(buf, "X-OpenChime-Signature:", f->sig[i], sizeof f->sig[i]);
        header_value(buf, "X-OpenChime-Signature-Version:", f->ver[i], sizeof f->ver[i]);
        f->ts[i] = strtol(ts, NULL, 10);
        if (head_end && total >= head_end)
            snprintf(f->body[i], sizeof f->body[i], "%s", buf + head_end);
        int status = i < f->n_statuses ? f->statuses[i] : 200;
        if (status) {
            const char *rb = status == 200 && f->reply ? f->reply : "";
            char resp[512];
            int rn = snprintf(resp, sizeof resp,
                              "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n%s", status, strlen(rb), rb);
            ssize_t wr = write(c, resp, (size_t)rn);
            (void)wr;
        }
        close(c);
    }
    return NULL;
}

static int fake_listen(int *port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(lfd, 8) != 0) { close(lfd); return -1; }
    socklen_t sl = sizeof sa;
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    *port = ntohs(sa.sin_port);
    return lfd;
}

/* Does `sig_b64` verify over central's canonical string for `body`? */
static int signature_holds(const char *pk_pem, const char *path, const char *aud, long ts, const char *body,
                           const char *sig_b64) {
    uint8_t sig[160]; size_t siglen = 0;
    if (mbedtls_base64_decode(sig, sizeof sig, &siglen, (const unsigned char *)sig_b64,
                              strlen(sig_b64)) != 0) return 0;
    mbedtls_pk_context kp;
    mbedtls_pk_init(&kp);
    int ok = 0;
    if (mbedtls_pk_parse_key(&kp, (const unsigned char *)pk_pem, strlen(pk_pem) + 1,
                             NULL, 0, NULL, NULL) == 0) {
        uint8_t bh[32];
        mbedtls_sha256((const unsigned char *)body, strlen(body), bh, 0);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", bh[i]);
        char canon[640];
        int cn = snprintf(canon, sizeof canon, "openchime-machine-v2|POST|%s|%s|%ld|%s", path, aud, ts, hex);
        uint8_t h[32];
        mbedtls_sha256((const unsigned char *)canon, (size_t)cn, h, 0);
        ok = mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen) == 0;
    }
    mbedtls_pk_free(&kp);
    return ok;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Milliseconds for seconds: retries from 5 ms doubling to 20, a ten-second
 * round, and a round every 100 ms -- each shorter than the stand-in's quarter
 * second of silence, so it sees every request the report makes. */
static const oc_address_report_timing FAST_T = { 5, 20, 10000, 100 };

/* Report `body` against central answering `statuses` in turn, until central has
 * gone a quarter second without a request; returns how many it saw. */
static int run_report(fake_central *f, const int *statuses, int n, const char *body,
                      const char *pk, const char *aud) {
    memset(f, 0, sizeof *f);
    int port = 0;
    f->fd = fake_listen(&port);
    CHECK(f->fd >= 0);
    if (f->fd < 0) return -1;
    memcpy(f->statuses, statuses, (size_t)n * sizeof *statuses);
    f->n_statuses = n;
    f->reply = "{\"address\":\"203.0.113.7\"}";
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fake_central_thread, f) == 0);
    /* The enrollment URL's path is not where the report goes: its origin is. */
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/api/machine/enroll", port);
    oc_address_report *r = oc_address_report_start(url, aud, pk, body, &FAST_T);
    CHECK(r != NULL);
    pthread_join(th, NULL);
    oc_address_report_stop(r);
    close(f->fd);
    return f->seen;
}

static void test_report(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    fake_central *f = calloc(1, sizeof *f);
    CHECK(f != NULL);
    if (!f) return;

    /* No name: {} as it is. No answer, then 422, 429 and 503 are each tried
     * again; the 200 ends the round; the next round, 100 ms on, is refused, and
     * nothing follows the refusal. Every request is the same body, signed. */
    {
        static const int S[] = { 0, 422, 429, 503, 200, 403 };
        CHECK(run_report(f, S, 6, "{}", pk, aud) == 6);
        for (int i = 0; i < 6 && i < f->seen; i++) {
            CHECK(strcmp(f->path[i], OC_ADDRESS_REPORT_PATH) == 0);
            CHECK(strcmp(f->aud[i], aud) == 0);
            CHECK(strcmp(f->body[i], "{}") == 0);
            CHECK(strcmp(f->ver[i], "2") == 0);
            CHECK(signature_holds(pk, f->path[i], aud, f->ts[i], f->body[i], f->sig[i]));
        }
        CHECK(!signature_holds(pk, f->path[0], aud, f->ts[0], "{\"address\":\"x.example\"}", f->sig[0]));
    }

    /* A name: its body as built, and recorded rounds repeating until a refusal. */
    {
        static const int S[] = { 200, 200, 404 };
        CHECK(run_report(f, S, 3, "{\"address\":\"chat.acme.example\"}", pk, aud) == 3);
        for (int i = 0; i < 3 && i < f->seen; i++)
            CHECK(strcmp(f->body[i], "{\"address\":\"chat.acme.example\"}") == 0);
    }

    /* A refusal at once is the only request. */
    {
        static const int S[] = { 400 };
        CHECK(run_report(f, S, 1, "{}", pk, aud) == 1);
    }

    /* Stopping during the wait between rounds returns at once, not two hours on. */
    {
        memset(f, 0, sizeof *f);
        int port = 0;
        f->fd = fake_listen(&port);
        CHECK(f->fd >= 0);
        if (f->fd >= 0) {
            f->n_statuses = 0;   /* every request: 200 */
            pthread_t th;
            CHECK(pthread_create(&th, NULL, fake_central_thread, f) == 0);
            char url[96];
            snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
            oc_address_report *r = oc_address_report_start(url, aud, pk, "{}", NULL);
            CHECK(r != NULL);
            pthread_join(th, NULL);           /* one recorded round, then the two-hour wait */
            CHECK(f->seen == 1);
            double t0 = now_s();
            oc_address_report_stop(r);
            CHECK(now_s() - t0 < 1.0);
            close(f->fd);
        }
    }

    /* Nothing to report with: no report, never a crash. */
    CHECK(oc_address_report_start(NULL, aud, pk, "{}", NULL) == NULL);
    CHECK(oc_address_report_start("http://127.0.0.1:1", aud, pk, NULL, NULL) == NULL);
    oc_address_report_stop(NULL);
    free(f);
}

int run_address_report_tests(void) {
    printf("test_address_report: what each answer means, the address read from central's "
           "reply, and the report against a stand-in central ({} and a name as sent, signed, "
           "retried on no answer, 422, 429 and 5xx, ended by a refusal, repeated after a "
           "recorded round, stopped at once)\n");
    test_disposition();
    test_recorded();
    test_report();
    return failures;
}
