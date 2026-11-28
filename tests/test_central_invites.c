/* The invitations central queues (central_invites.h, ARCH-85): a take's answer
 * read, a report's exact body, the waits between takes, and the worker against a
 * stand-in for central -- what it takes, that each invitation is handed over to be
 * made, that the outcomes are reported signed, that a failed report is repeated
 * with the same ids, and that a take that fails is asked again after a wait. */
#include "check.h"
#include "central_invites.h"
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
#include <unistd.h>

static const char ID1[] = "0123456789abcdef0123456789abcdef";
static const char ID2[] = "fedcba9876543210fedcba9876543210";

static void test_parse(void) {
    char ids[4][OC_CENTRAL_INVITES_ID_LEN + 1], addrs[4][OC_CENTRAL_INVITES_ADDR];
    const char *body =
        "{\"invitations\":[{\"id\":\"0123456789abcdef0123456789abcdef\",\"address\":\"alex@example.com\"},"
        "{\"address\":\"sam@example.com\",\"id\":\"fedcba9876543210fedcba9876543210\",\"extra\":[1,{\"x\":2}]}]}";
    CHECK(oc_central_invites_parse(body, strlen(body), ids, addrs, 4) == 2);
    CHECK(strcmp(ids[0], ID1) == 0 && strcmp(addrs[0], "alex@example.com") == 0);
    CHECK(strcmp(ids[1], ID2) == 0 && strcmp(addrs[1], "sam@example.com") == 0);

    /* None waiting is an answer too. */
    CHECK(oc_central_invites_parse("{\"invitations\":[]}", 18, ids, addrs, 4) == 0);

    /* An entry with an id not central's shape, or no address, is skipped. */
    const char *odd = "{\"invitations\":[{\"id\":\"XYZ\",\"address\":\"a@b.example\"},"
                      "{\"id\":\"0123456789abcdef0123456789abcdef\",\"address\":\"\"},"
                      "{\"id\":\"fedcba9876543210fedcba9876543210\",\"address\":\"c@d.example\"}]}";
    CHECK(oc_central_invites_parse(odd, strlen(odd), ids, addrs, 4) == 1);
    CHECK(strcmp(ids[0], ID2) == 0);

    /* At most `max`. */
    CHECK(oc_central_invites_parse(body, strlen(body), ids, addrs, 1) == 1);

    /* Not the shape at all. */
    CHECK(oc_central_invites_parse("[]", 2, ids, addrs, 4) == -1);
    CHECK(oc_central_invites_parse("{\"other\":[]}", 12, ids, addrs, 4) == -1);
    CHECK(oc_central_invites_parse("not json", 8, ids, addrs, 4) == -1);
}

static void test_report_body(void) {
    char out[512];
    const char *ids[] = { ID1, ID2 };
    const int made[] = { 1, 0 };
    const char *why[] = { "", "already a \"member\"" };
    CHECK(oc_central_invites_report_body(ids, made, why, 2, out, sizeof out) == 0);
    /* contract: the report's wire shape, which central's report route reads */
    CHECK(strcmp(out, "{\"outcomes\":[{\"id\":\"0123456789abcdef0123456789abcdef\",\"made\":true,\"reason\":null},"
                      "{\"id\":\"fedcba9876543210fedcba9876543210\",\"made\":false,"
                      "\"reason\":\"already a \\\"member\\\"\"}]}") == 0);
    /* Too small a buffer is a failure, never a truncated body. */
    CHECK(oc_central_invites_report_body(ids, made, why, 2, out, 60) == -1);
}

static void test_wait(void) {
    const oc_central_invites_timing *t = &OC_CENTRAL_INVITES_TIMING;
    /* Central's pace, when it names one; an hour when it does not. */
    CHECK(oc_central_invites_wait_ms(1, 30, 0, t) == 30000u);
    CHECK(oc_central_invites_wait_ms(1, 0, 0, t) == 3600u * 1000u);
    /* A minute after a failure, doubling, to a day. */
    CHECK(oc_central_invites_wait_ms(0, 0, 1, t) == 60u * 1000u);
    CHECK(oc_central_invites_wait_ms(0, 0, 2, t) == 120u * 1000u);
    CHECK(oc_central_invites_wait_ms(0, 0, 3, t) == 240u * 1000u);
    CHECK(oc_central_invites_wait_ms(0, 0, 40, t) == 24ull * 3600u * 1000u);
}

/* --- a stand-in for central ---------------------------------------------- */

#define FAKE_MAX 8

typedef struct {
    int  status;          /* 0 = hang up unanswered */
    const char *body;     /* NULL = none */
    int  retry_after;     /* 0 = no header */
} answer;

typedef struct {
    int    fd;
    answer answers[FAKE_MAX];
    int    n_answers;
    int    seen;          /* requests answered; read by the test's thread (fake_seen) */
    char   path[FAKE_MAX][96];
    char   aud[FAKE_MAX][128];
    long   ts[FAKE_MAX];
    char   sig[FAKE_MAX][256];
    char   body[FAKE_MAX][1024];
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

/* Answer the requests a case expects, in turn, then stop. The wait for each is
 * only a guard against a hang, never what ends a case. */
static void *fake_central_thread(void *arg) {
    fake_central *f = arg;
    while (f->seen < f->n_answers) {
        struct pollfd pfd = { f->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 30000) <= 0) break;
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
        int i = f->seen;
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
        f->ts[i] = strtol(ts, NULL, 10);
        if (head_end && total >= head_end)
            snprintf(f->body[i], sizeof f->body[i], "%s", buf + head_end);
        answer a = i < f->n_answers ? f->answers[i] : (answer){ 204, NULL, 0 };
        if (a.status) {
            char resp[2048], ra[48] = "";
            if (a.retry_after) snprintf(ra, sizeof ra, "Retry-After: %d\r\n", a.retry_after);
            const char *b = a.body ? a.body : "";
            int rn = snprintf(resp, sizeof resp,
                              "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\n"
                              "Connection: close\r\n\r\n%s", a.status, ra, strlen(b), b);
            ssize_t wr = write(c, resp, (size_t)rn);
            (void)wr;
        }
        close(c);
        __atomic_store_n(&f->seen, i + 1, __ATOMIC_RELEASE);   /* recorded, then counted */
    }
    return NULL;
}

static int fake_seen(fake_central *f) { return __atomic_load_n(&f->seen, __ATOMIC_ACQUIRE); }

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

/* What the worker handed over to be made. */
typedef struct {
    pthread_mutex_t mu;
    int  n;
    char id[4][OC_CENTRAL_INVITES_ID_LEN + 1];
    char address[4][OC_CENTRAL_INVITES_ADDR];
} made_log;

static void record_make(void *ctx, const char *id, const char *address) {
    made_log *m = ctx;
    pthread_mutex_lock(&m->mu);
    if (m->n < 4) {
        snprintf(m->id[m->n], sizeof m->id[m->n], "%s", id);
        snprintf(m->address[m->n], sizeof m->address[m->n], "%s", address);
        m->n++;
    }
    pthread_mutex_unlock(&m->mu);
}

static int made_count(made_log *m) {
    pthread_mutex_lock(&m->mu);
    int n = m->n;
    pthread_mutex_unlock(&m->mu);
    return n;
}

static const char TAKEN[] =
    "{\"invitations\":[{\"id\":\"0123456789abcdef0123456789abcdef\",\"address\":\"alex@example.com\"},"
    "{\"id\":\"fedcba9876543210fedcba9876543210\",\"address\":\"sam@example.com\"}]}";

/* What a case hands back once both invitations were handed over: nothing; ID1
 * made; or ID1 made and, once central has its report, ID2 refused -- one after
 * the other, so no case rests on how the worker happens to batch them. */
enum { OUT_NONE, OUT_ONE, OUT_TWO };

/* A worker against central answering `answers` in turn, `n` requests and no
 * more. Returns how many requests central saw. */
static int run(fake_central *f, const answer *answers, int n, made_log *m, int outcomes,
               const char *pk, const char *aud) {
    memset(f, 0, sizeof *f);
    int port = 0;
    f->fd = fake_listen(&port);
    CHECK(f->fd >= 0);
    if (f->fd < 0) return -1;
    memcpy(f->answers, answers, (size_t)n * sizeof *answers);
    f->n_answers = n;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fake_central_thread, f) == 0);
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1/workspace/machine/enroll/claim", port);
    /* Waits short enough for a test: a failed take or report is asked again in
     * 5 ms, doubling; with no Retry-After, a minute. */
    oc_central_invites_timing t = { .idle_ms = 60000, .fail_min_ms = 5, .fail_max_ms = 40 };
    oc_central_invites *ci = oc_central_invites_start_timed(url, aud, pk, record_make, m, &t);
    CHECK(ci != NULL);
    if (outcomes != OUT_NONE) {
        CHECK(WAIT_UNTIL(made_count(m) == 2, 3000));
        oc_central_invites_outcome(ci, ID1, 1, NULL);
    }
    if (outcomes == OUT_TWO) {
        CHECK(WAIT_UNTIL(fake_seen(f) >= 2, 3000));      /* ID1's report taken */
        oc_central_invites_outcome(ci, ID2, 0, "already a member");
    }
    pthread_join(th, NULL);           /* central has answered all `n` */
    oc_central_invites_stop(ci);
    close(f->fd);
    return f->seen;
}

static int has_outcome(const char *body, const char *id, const char *rest) {
    char want[160];
    snprintf(want, sizeof want, "{\"id\":\"%s\",%s}", id, rest);
    return strstr(body, want) != NULL;
}

static void test_against_central(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    fake_central *f = calloc(1, sizeof *f);
    made_log *m = calloc(1, sizeof *m);
    CHECK(f && m);
    if (!f || !m) { free(f); free(m); return; }
    pthread_mutex_init(&m->mu, NULL);

    /* Two waiting, then the pace central names: both handed over to be made, and
     * each outcome reported, signed, as it comes. (Several in one body:
     * test_report_body.) */
    {
        const answer A[] = { { 200, TAKEN, 30 }, { 204, NULL, 0 }, { 204, NULL, 0 } };
        CHECK(run(f, A, 3, m, OUT_TWO, pk, aud) == 3);
        CHECK(strcmp(m->id[0], ID1) == 0 && strcmp(m->address[0], "alex@example.com") == 0);
        CHECK(strcmp(m->id[1], ID2) == 0 && strcmp(m->address[1], "sam@example.com") == 0);
        CHECK(strcmp(f->path[0], "/v1/workspace/machine/invitations/take") == 0);
        CHECK(strcmp(f->path[1], "/v1/workspace/machine/invitations/report") == 0);
        CHECK(strcmp(f->path[2], "/v1/workspace/machine/invitations/report") == 0);
        for (int i = 0; i < 3; i++) {
            CHECK(strcmp(f->aud[i], aud) == 0);
            CHECK(signature_holds(pk, f->path[i], aud, f->ts[i], f->body[i], f->sig[i]));
        }
        CHECK(strcmp(f->body[0], "{}") == 0);
        CHECK(has_outcome(f->body[1], ID1, "\"made\":true,\"reason\":null"));
        CHECK(strstr(f->body[1], ID2) == NULL);
        CHECK(has_outcome(f->body[2], ID2, "\"made\":false,\"reason\":\"already a member\""));
        CHECK(strstr(f->body[2], ID1) == NULL);
    }

    /* A report central could not take is sent again, the same outcomes under the
     * same ids. */
    m->n = 0;
    {
        const answer A[] = { { 200, TAKEN, 30 }, { 503, NULL, 0 }, { 204, NULL, 0 } };
        CHECK(run(f, A, 3, m, OUT_ONE, pk, aud) == 3);
        CHECK(strcmp(f->path[1], "/v1/workspace/machine/invitations/report") == 0);
        CHECK(strcmp(f->path[2], "/v1/workspace/machine/invitations/report") == 0);
        CHECK(strcmp(f->body[1], f->body[2]) == 0);
        CHECK(has_outcome(f->body[2], ID1, "\"made\":true,\"reason\":null"));
    }

    /* A take that fails is asked again after a wait, and what it then gives is
     * handed over; with none waiting, nothing is. */
    m->n = 0;
    {
        const answer A[] = { { 503, NULL, 0 }, { 0, NULL, 0 }, { 200, "{\"invitations\":[]}", 30 } };
        CHECK(run(f, A, 3, m, OUT_NONE, pk, aud) == 3);
        for (int i = 0; i < 3; i++)
            CHECK(strcmp(f->path[i], "/v1/workspace/machine/invitations/take") == 0);
        CHECK(made_count(m) == 0);
    }

    /* Without an enrollment there is nothing to ask with: no worker. */
    CHECK(oc_central_invites_start(NULL, aud, pk, record_make, m) == NULL);
    CHECK(oc_central_invites_start("http://127.0.0.1:1", aud, NULL, record_make, m) == NULL);
    /* An outcome for no worker, or a malformed id, is dropped quietly. */
    oc_central_invites_outcome(NULL, ID1, 1, NULL);

    pthread_mutex_destroy(&m->mu);
    free(m);
    free(f);
}

int run_central_invites_tests(void) {
    printf("test_central_invites: a take's answer read, a report's exact body, the waits "
           "between takes, and the worker against a stand-in central (each invitation "
           "handed over, the outcomes reported signed, a failed report repeated with the "
           "same ids, a failed take asked again)\n");
    test_parse();
    test_report_body();
    test_wait();
    test_against_central();
    return failures;
}
