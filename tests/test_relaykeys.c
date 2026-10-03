/* The relay's published keys (relaykeys.h): what a JWKS yields, and the worker
 * that reads it from the relay and hands it to the writer. */

#include "check.h"
#include "issuer.h"
#include "../daemon/dbwriter.h"
#include "../daemon/relaykeys.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A relay that serves /oidc/jwks: `status` and `body` as the test sets them. */
typedef struct {
    int  fd, port, stop, hits, status;
    char body[4096];
    pthread_mutex_t mu;
} fake_relay;

static void *relay_thread(void *arg) {
    fake_relay *f = arg;
    for (;;) {
        int c = accept(f->fd, NULL, NULL);
        if (c < 0) break;
        /* The whole request head before any answer: answering a part and
         * closing with the rest unread resets the connection under the reader. */
        char req[2048];
        size_t got = 0;
        req[0] = '\0';
        while (got < sizeof req - 1 && !strstr(req, "\r\n\r\n")) {
            ssize_t n = read(c, req + got, sizeof req - 1 - got);
            if (n <= 0) break;
            got += (size_t)n;
            req[got] = '\0';
        }
        pthread_mutex_lock(&f->mu);
        if (f->stop) { pthread_mutex_unlock(&f->mu); close(c); break; }
        int ours = strncmp(req, "GET /oidc/jwks ", 15) == 0;
        if (ours) f->hits++;
        char resp[4400];
        int st = ours ? f->status : 404;
        const char *b = ours ? f->body : "no";
        int rl = snprintf(resp, sizeof resp, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", st, strlen(b), b);
        pthread_mutex_unlock(&f->mu);
        ssize_t w = write(c, resp, (size_t)rl); (void)w;
        close(c);
    }
    return NULL;
}

static int relay_hits(fake_relay *f) {
    pthread_mutex_lock(&f->mu);
    int h = f->hits;
    pthread_mutex_unlock(&f->mu);
    return h;
}

int run_relaykeys_tests(void) {
    printf("test_relaykeys: P-256 signing keys from a JWKS, the rest passed over; fetched into the writer\n");
    oc_issuer a, b;
    CHECK(oc_issuer_init(&a, "oc-rk-a") == 0 && oc_issuer_init(&b, "oc-rk-b") == 0);
    char ja[400], jb[400], doc[4096], pem[4096];
    CHECK(oc_jwk_p256(&a.key, ja, sizeof ja) > 0 && oc_jwk_p256(&b.key, jb, sizeof jb) > 0);

    /* Two keys: both, as PEM the verifier reads, the pinned key's text among them. */
    snprintf(doc, sizeof doc, "{\"keys\":[%s,%s]}", ja, jb);
    CHECK(oc_relaykeys_pem(doc, strlen(doc), pem, sizeof pem) == 2);
    CHECK(strstr(pem, a.pem) != NULL && strstr(pem, b.pem) != NULL);

    /* What is not an ES256 signing key on P-256 is passed over: another curve,
     * another type, a key for encryption, another algorithm, a point off the
     * curve. "use":"sig" and "alg":"ES256" are fine; so is their absence. */
    {
        char x[64] = "", y[64] = "";
        const char *px = strstr(ja, "\"x\":\""), *py = strstr(ja, "\"y\":\"");
        CHECK(px && py);
        if (px && py) {
            sscanf(px + 5, "%63[^\"]", x);
            sscanf(py + 5, "%63[^\"]", y);
        }
        char bad_y[64];
        snprintf(bad_y, sizeof bad_y, "%s", y);
        bad_y[5] = bad_y[5] == 'A' ? 'B' : 'A';
        snprintf(doc, sizeof doc,
                 "{\"keys\":["
                 "{\"kty\":\"EC\",\"crv\":\"P-384\",\"x\":\"%s\",\"y\":\"%s\"},"
                 "{\"kty\":\"RSA\",\"n\":\"AQAB\",\"e\":\"AQAB\"},"
                 "{\"kty\":\"EC\",\"crv\":\"P-256\",\"use\":\"enc\",\"x\":\"%s\",\"y\":\"%s\"},"
                 "{\"kty\":\"EC\",\"crv\":\"P-256\",\"alg\":\"ES384\",\"x\":\"%s\",\"y\":\"%s\"},"
                 "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"},"
                 "{\"kty\":\"EC\",\"crv\":\"P-256\",\"use\":\"sig\",\"alg\":\"ES256\",\"kid\":\"k\",\"x\":\"%s\",\"y\":\"%s\"}"
                 "]}", x, y, x, y, x, y, x, bad_y, x, y);
        CHECK(oc_relaykeys_pem(doc, strlen(doc), pem, sizeof pem) == 1);
        CHECK(strcmp(pem, a.pem) == 0 || strncmp(pem, a.pem, strlen(pem)) == 0);
    }

    /* None at all is zero keys; what is not a key set is -1; nothing is cut short. */
    CHECK(oc_relaykeys_pem("{\"keys\":[]}", 11, pem, sizeof pem) == 0);
    CHECK(oc_relaykeys_pem("{\"keys\":{}}", 11, pem, sizeof pem) == -1);
    CHECK(oc_relaykeys_pem("{\"nokeys\":[]}", 13, pem, sizeof pem) == -1);
    CHECK(oc_relaykeys_pem("not json", 8, pem, sizeof pem) == -1);
    snprintf(doc, sizeof doc, "{\"keys\":[%s,%s]}", ja, jb);
    CHECK(oc_relaykeys_pem(doc, strlen(doc), pem, 250) == -1 && pem[0] == '\0');
    /* At most OC_RELAYKEYS_MAX from one document. */
    {
        size_t o = (size_t)snprintf(doc, sizeof doc, "{\"keys\":[");
        for (int i = 0; i < OC_RELAYKEYS_MAX + 2; i++)
            o += (size_t)snprintf(doc + o, sizeof doc - o, "%s%s", i ? "," : "", ja);
        snprintf(doc + o, sizeof doc - o, "]}");
        CHECK(oc_relaykeys_pem(doc, strlen(doc), pem, sizeof pem) == OC_RELAYKEYS_MAX);
    }

    /* The worker: the relay down at first -- it asks again -- then answering. */
    fake_relay f;
    memset(&f, 0, sizeof f);
    pthread_mutex_init(&f.mu, NULL);
    f.fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof sa;
    CHECK(bind(f.fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(f.fd, 8) == 0 &&
          getsockname(f.fd, (struct sockaddr *)&sa, &sl) == 0);
    f.port = ntohs(sa.sin_port);
    f.status = 503;
    snprintf(f.body, sizeof f.body, "{\"keys\":[%s]}", jb);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, relay_thread, &f) == 0);

    const char *path = "build/test_relaykeys.db";
    unlink(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    CHECK(oc_dbwriter_configure_oidc(w, "https://auth.openchime.io", "acme.example", a.pem, "") == 0);
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/oidc/jwks", f.port);
    oc_relaykeys *rk = oc_relaykeys_start(url, w);
    CHECK(rk != NULL);
    /* Waits bound a hang, not a speed: each event is asserted after. */
    for (int i = 0; i < 1500 && relay_hits(&f) < 1; i++) usleep(20000);
    CHECK(relay_hits(&f) == 1 && !oc_relaykeys_fetched(rk));
    oc_relaykeys_stop(rk);

    /* Answering: the writer has the published key. */
    pthread_mutex_lock(&f.mu); f.status = 200; pthread_mutex_unlock(&f.mu);
    rk = oc_relaykeys_start(url, w);
    for (int i = 0; i < 1500 && !oc_relaykeys_fetched(rk); i++) usleep(20000);
    CHECK(oc_relaykeys_fetched(rk));
    oc_relaykeys_stop(rk);
    /* A URL too long to hold is refused at start, not cut. */
    {
        char longu[700];
        memset(longu, 'a', sizeof longu - 1); longu[sizeof longu - 1] = '\0';
        CHECK(oc_relaykeys_start(longu, w) == NULL);
    }

    oc_dbwriter_stop(w);
    unlink(path);
    pthread_mutex_lock(&f.mu); f.stop = 1; pthread_mutex_unlock(&f.mu);
    int k = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(k, (struct sockaddr *)&sa, sizeof sa) == 0) close(k); else close(k);
    pthread_join(th, NULL);
    close(f.fd);
    pthread_mutex_destroy(&f.mu);
    oc_issuer_free(&a); oc_issuer_free(&b);
    return failures;
}
