/* The invitation mail report (invite_mail.h, ARCH-85, REQ-280): the exact body,
 * the reply policy, and the report itself against a stand-in for central --
 * where it is sent, that it is signed as push is, and that a retry repeats the
 * same inviteId while a refusal is final. */

#include "check.h"
#include "enroll.h"
#include "invite_mail.h"

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

static void test_body(void) {
    char out[512];
    CHECK(oc_invite_mail_build_body("0123456789abcdef0123456789abcdef", "lee@partner.example",
                                    1760000000ull, out, sizeof out) == 0);
    CHECK(strcmp(out, "{\"inviteId\":\"0123456789abcdef0123456789abcdef\","
                      "\"email\":\"lee@partner.example\",\"expiresAt\":1760000000}") == 0);
    /* An address is text from a person: what JSON needs escaping is escaped. */
    CHECK(oc_invite_mail_build_body("0123456789abcdef0123456789abcdef", "a\"b\\c\x01@x.example",
                                    1, out, sizeof out) == 0);
    CHECK(strstr(out, "\"email\":\"a\\\"b\\\\c\\u0001@x.example\"") != NULL);
    /* Too small a buffer is a failure, never a truncated body. */
    CHECK(oc_invite_mail_build_body("0123456789abcdef0123456789abcdef", "lee@partner.example",
                                    1, out, 40) == -1);
}

static void test_disposition(void) {
    CHECK(oc_invite_mail_disposition(1, 200) == OC_INVITE_MAIL_DONE);
    CHECK(oc_invite_mail_disposition(1, 202) == OC_INVITE_MAIL_DONE);
    CHECK(oc_invite_mail_disposition(1, 204) == OC_INVITE_MAIL_DONE);
    static const int FINAL[] = { 400, 401, 404, 409, 429 };
    for (size_t i = 0; i < sizeof FINAL / sizeof FINAL[0]; i++)
        CHECK(oc_invite_mail_disposition(1, FINAL[i]) == OC_INVITE_MAIL_FINAL);
    CHECK(oc_invite_mail_disposition(1, 500) == OC_INVITE_MAIL_RETRY);
    CHECK(oc_invite_mail_disposition(1, 503) == OC_INVITE_MAIL_RETRY);
    CHECK(oc_invite_mail_disposition(0, 0) == OC_INVITE_MAIL_RETRY);   /* no answer at all */
}

/* --- a stand-in for central ---------------------------------------------- */

#define FAKE_MAX 8

typedef struct {
    int  fd;
    int  statuses[FAKE_MAX];   /* the answer to each request; 0 = hang up unanswered */
    int  n_statuses;
    int  seen;
    char path[FAKE_MAX][96];
    char aud[FAKE_MAX][128];
    long ts[FAKE_MAX];
    char sig[FAKE_MAX][256];
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

/* Answer requests until none comes for a second: long enough for every retry
 * the test's short backoff makes, short enough to prove none follows. */
static void *fake_central_thread(void *arg) {
    fake_central *f = arg;
    while (f->seen < FAKE_MAX) {
        struct pollfd pfd = { f->fd, POLLIN, 0 };
        if (poll(&pfd, 1, 1000) <= 0) break;
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
        f->ts[i] = strtol(ts, NULL, 10);
        if (head_end && total >= head_end)
            snprintf(f->body[i], sizeof f->body[i], "%s", buf + head_end);
        int status = i < f->n_statuses ? f->statuses[i] : 200;
        if (status) {
            char resp[128];
            int rn = snprintf(resp, sizeof resp,
                              "HTTP/1.1 %d X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
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
static int signature_holds(const char *pk_pem, const char *aud, long ts, const char *body,
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
        int cn = snprintf(canon, sizeof canon, "openchime-machine-v1|%s|%ld|%s", aud, ts, hex);
        uint8_t h[32];
        mbedtls_sha256((const unsigned char *)canon, (size_t)cn, h, 0);
        ok = mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen) == 0;
    }
    mbedtls_pk_free(&kp);
    return ok;
}

static const char ID[] = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

/* One report against central answering `statuses` in turn; returns how many
 * requests central saw, with `f` holding them. */
static int run_report(fake_central *f, const int *statuses, int n, const char *pk, const char *aud) {
    memset(f, 0, sizeof *f);
    int port = 0;
    f->fd = fake_listen(&port);
    CHECK(f->fd >= 0);
    if (f->fd < 0) return -1;
    memcpy(f->statuses, statuses, (size_t)n * sizeof *statuses);
    f->n_statuses = n;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fake_central_thread, f) == 0);
    /* The enrollment URL's path is not where the report goes: its origin is. */
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/api/machine/enroll", port);
    oc_invite_mail *m = oc_invite_mail_start_backoff(url, NULL, aud, pk, 20);
    CHECK(m != NULL);
    oc_invite_mail_report(m, ID, "lee@partner.example", 1760000000ull);
    pthread_join(th, NULL);          /* central has gone a second without a request */
    oc_invite_mail_stop(m);
    close(f->fd);
    return f->seen;
}

static void test_report(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    char want[512];
    CHECK(oc_invite_mail_build_body(ID, "lee@partner.example", 1760000000ull, want, sizeof want) == 0);
    fake_central *f = calloc(1, sizeof *f);
    CHECK(f != NULL);
    if (!f) return;

    /* Busy, then no answer, then accepted: three tries of one report, each the
     * same body -- the same inviteId -- each signed, and nothing after. */
    {
        static const int S[] = { 503, 0, 202 };
        CHECK(run_report(f, S, 3, pk, aud) == 3);
        for (int i = 0; i < 3 && i < f->seen; i++) {
            CHECK(strcmp(f->path[i], "/api/machine/invite/notify") == 0);
            CHECK(strcmp(f->aud[i], aud) == 0);
            CHECK(strcmp(f->body[i], want) == 0);
            CHECK(signature_holds(pk, aud, f->ts[i], f->body[i], f->sig[i]));
        }
        /* A signature is over the body it came with, not over any body. */
        CHECK(!signature_holds(pk, aud, f->ts[0], "{\"inviteId\":\"x\"}", f->sig[0]));
    }

    /* A refusal is final, whichever one: asked again, central says the same. */
    static const int REFUSALS[] = { 400, 401, 404, 409, 429 };
    for (size_t i = 0; i < sizeof REFUSALS / sizeof REFUSALS[0]; i++)
        CHECK(run_report(f, &REFUSALS[i], 1, pk, aud) == 1);

    /* Central down for good: a bounded number of tries, then the report is let go. */
    {
        static const int S[] = { 500, 502, 503, 504, 500, 500, 500 };
        CHECK(run_report(f, S, 7, pk, aud) == OC_INVITE_MAIL_ATTEMPTS);
    }

    /* Fire and forget: a missing piece is dropped quietly, never a crash. */
    oc_invite_mail_report(NULL, ID, "lee@partner.example", 1);
    free(f);
}

int run_invite_mail_tests(void) {
    printf("test_invite_mail: the report's exact body, its reply policy, and the report "
           "against a stand-in central (origin, path, signing, the same inviteId on "
           "every retry, a 4xx final, bounded retries)\n");
    test_body();
    test_disposition();
    test_report();
    return failures;
}
