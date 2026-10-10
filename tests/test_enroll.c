/* CP-8 federated enrollment client: keypair + audience generation, the "oce1."
 * enrollment code format (matching the control plane's parser), the proof-of-
 * possession signature (verified with the public key — the exact check central
 * runs), and the persistence round-trip. */

#include "check.h"
#include "enroll.h"
#include "dbwriter.h"
#include "jwt.h"   /* oc_base64url_decode */

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract the value of a flat "key":"..." pair from JSON. */
static int json_extract(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return 0;
    size_t len = (size_t)(e - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static void test_generate(mbedtls_ctr_drbg_context *rng) {
    char pk1[1024], aud1[128], pk2[1024], aud2[128];
    CHECK(oc_enroll_generate(pk1, sizeof pk1, aud1, sizeof aud1) == 0);
    CHECK(oc_enroll_generate(pk2, sizeof pk2, aud2, sizeof aud2) == 0);
    CHECK(strncmp(aud1, "ws_", 3) == 0);
    CHECK(strlen(aud1) > 10);
    CHECK(strcmp(aud1, aud2) != 0);   /* opaque + unique per generate */

    mbedtls_pk_context k;
    mbedtls_pk_init(&k);
    CHECK(mbedtls_pk_parse_key(&k, (const unsigned char *)pk1, strlen(pk1) + 1,
                               NULL, 0, mbedtls_ctr_drbg_random, rng) == 0);
    CHECK(mbedtls_pk_get_type(&k) == MBEDTLS_PK_ECKEY);
    CHECK(mbedtls_pk_get_bitlen(&k) == 256);
    mbedtls_pk_free(&k);
}

static void test_code_and_signature(void) {
    char pk[1024], aud[128], code[2048];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    CHECK(oc_enroll_build_code(pk, aud, code, sizeof code) == 0);
    CHECK(strncmp(code, "oce1.", 5) == 0);

    /* Decode the base64url payload back to JSON (as the saas parser does). */
    uint8_t json[1024];
    long jn = oc_base64url_decode(code + 5, strlen(code + 5), json, sizeof json - 1);
    CHECK(jn > 0);
    if (jn <= 0) return;
    json[jn] = '\0';

    char aud_in[128], pk_b64[512];
    CHECK(json_extract((const char *)json, "aud", aud_in, sizeof aud_in));
    CHECK(strcmp(aud_in, aud) == 0);
    CHECK(json_extract((const char *)json, "pk", pk_b64, sizeof pk_b64));

    /* pk is standard-base64 SPKI DER → re-parses as an EC P-256 public key. */
    uint8_t der[256];
    size_t derlen = 0;
    CHECK(mbedtls_base64_decode(der, sizeof der, &derlen,
                                (const unsigned char *)pk_b64, strlen(pk_b64)) == 0);
    mbedtls_pk_context pub;
    mbedtls_pk_init(&pub);
    CHECK(mbedtls_pk_parse_public_key(&pub, der, derlen) == 0);
    CHECK(mbedtls_pk_get_type(&pub) == MBEDTLS_PK_ECKEY);
    CHECK(mbedtls_pk_get_bitlen(&pub) == 256);

    /* Sign a nonce and verify with the public key — the exact central check. */
    const char *nonce = "test-nonce-xyz";
    char sig_b64[256];
    CHECK(oc_enroll_sign_proof(pk, aud, nonce, sig_b64, sizeof sig_b64) == 0);
    uint8_t sig[128];
    size_t siglen = 0;
    CHECK(mbedtls_base64_decode(sig, sizeof sig, &siglen,
                                (const unsigned char *)sig_b64, strlen(sig_b64)) == 0);

    char msg[512];
    int mn = snprintf(msg, sizeof msg, "openchime-enroll-v1|%s|%s", aud, nonce);
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)msg, (size_t)mn, hash, 0);
    CHECK(mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, siglen) == 0);

    /* A different nonce must not verify against the same signature. */
    char msg2[512];
    int mn2 = snprintf(msg2, sizeof msg2, "openchime-enroll-v1|%s|%s", aud, "other-nonce");
    uint8_t hash2[32];
    mbedtls_sha256((const unsigned char *)msg2, (size_t)mn2, hash2, 0);
    CHECK(mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash2, sizeof hash2, sig, siglen) != 0);

    mbedtls_pk_free(&pub);
}

/* A managed box's claim (AUTH.md §8.7): the signature covers the audience and the
 * hashes of the ticket and of the public key — checked here the way central does. */
static void test_claim_signature(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);

    uint8_t ticket[32];
    memset(ticket, 7, sizeof ticket);
    char ticket64[64];
    size_t tl = 0;
    CHECK(mbedtls_base64_encode((unsigned char *)ticket64, sizeof ticket64, &tl, ticket, sizeof ticket) == 0);
    for (size_t i = 0; i < tl; i++) {            /* standard -> url alphabet, unpadded */
        if (ticket64[i] == '+') ticket64[i] = '-';
        else if (ticket64[i] == '/') ticket64[i] = '_';
        else if (ticket64[i] == '=') { ticket64[i] = '\0'; break; }
    }

    char pub_b64[512], sig_b64[256];
    CHECK(oc_enroll_sign_claim(pk, aud, ticket64, pub_b64, sizeof pub_b64, sig_b64, sizeof sig_b64) == 0);

    uint8_t der[256], sig[128];
    size_t derlen = 0, siglen = 0;
    CHECK(mbedtls_base64_decode(der, sizeof der, &derlen, (const unsigned char *)pub_b64, strlen(pub_b64)) == 0);
    CHECK(mbedtls_base64_decode(sig, sizeof sig, &siglen, (const unsigned char *)sig_b64, strlen(sig_b64)) == 0);
    mbedtls_pk_context pub;
    mbedtls_pk_init(&pub);
    CHECK(mbedtls_pk_parse_public_key(&pub, der, derlen) == 0);
    CHECK(mbedtls_pk_get_bitlen(&pub) == 256);

    /* The message, built independently: SHA-256 of the raw ticket and of the DER. */
    uint8_t th[32], kh[32];
    mbedtls_sha256(ticket, sizeof ticket, th, 0);
    mbedtls_sha256(der, derlen, kh, 0);
    char th64[64], kh64[64];
    size_t n = 0;
    mbedtls_base64_encode((unsigned char *)th64, sizeof th64, &n, th, sizeof th);
    mbedtls_base64_encode((unsigned char *)kh64, sizeof kh64, &n, kh, sizeof kh);
    for (char *p = th64; *p; p++) { if (*p == '+') *p = '-'; else if (*p == '/') *p = '_'; else if (*p == '=') { *p = '\0'; break; } }
    for (char *p = kh64; *p; p++) { if (*p == '+') *p = '-'; else if (*p == '/') *p = '_'; else if (*p == '=') { *p = '\0'; break; } }
    char msg[512];
    int mn = snprintf(msg, sizeof msg, "openchime-claim-v1|%s|%s|%s", aud, th64, kh64);
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)msg, (size_t)mn, hash, 0);
    CHECK(mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, siglen) == 0);

    /* Another audience, or another ticket, is another message. */
    mn = snprintf(msg, sizeof msg, "openchime-claim-v1|%s|%s|%s", "ws_someone_else", th64, kh64);
    mbedtls_sha256((const unsigned char *)msg, (size_t)mn, hash, 0);
    CHECK(mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, siglen) != 0);
    mbedtls_pk_free(&pub);

    /* A ticket that is not base64url is refused rather than signed. */
    CHECK(oc_enroll_sign_claim(pk, aud, "not base64!", pub_b64, sizeof pub_b64, sig_b64, sizeof sig_b64) != 0);
    CHECK(oc_enroll_sign_claim(pk, aud, "", pub_b64, sizeof pub_b64, sig_b64, sizeof sig_b64) != 0);
}

/* ---- a canned central, for what the claim does with each answer ------------- */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

struct canned { int listen_fd; int status; char seen[2048]; };

static void *canned_central(void *arg) {
    struct canned *c = arg;
    int fd = accept(c->listen_fd, NULL, NULL);
    if (fd < 0) return NULL;
    size_t got = 0;
    for (;;) {                                   /* headers + the small JSON body */
        ssize_t r = read(fd, c->seen + got, sizeof c->seen - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        c->seen[got] = '\0';
        const char *body = strstr(c->seen, "\r\n\r\n");
        const char *cl = strstr(c->seen, "Content-Length:");
        if (body && cl && strlen(body + 4) >= (size_t)atoi(cl + 15)) break;
    }
    char resp[160];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                     "Connection: close\r\n\r\n{}", c->status);
    ssize_t w = write(fd, resp, (size_t)n); (void)w;
    close(fd);
    return NULL;
}

/* `v6`: central at [::1], named in brackets in the URL. */
static oc_enroll_result claim_at(int status, struct canned *c, const char *pk, const char *aud, int v6) {
    memset(c, 0, sizeof *c);
    c->status = status;
    c->listen_fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t al;
    if (v6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback; al = sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
        a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); al = sizeof *a4;
    }
    if (c->listen_fd < 0 || bind(c->listen_fd, (struct sockaddr *)&ss, al) != 0) {
        if (c->listen_fd >= 0) close(c->listen_fd);
        return (oc_enroll_result)-1;                 /* this host has no such loopback */
    }
    listen(c->listen_fd, 1);
    getsockname(c->listen_fd, (struct sockaddr *)&ss, &al);
    unsigned port = v6 ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port) : ntohs(((struct sockaddr_in *)&ss)->sin_port);
    pthread_t th;
    pthread_create(&th, NULL, canned_central, c);
    char url[96];
    snprintf(url, sizeof url, v6 ? "http://[::1]:%u/api/machine/enroll" : "http://127.0.0.1:%u/api/machine/enroll", port);
    oc_enroll_result r = oc_enroll_claim(url, aud, pk, "BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc");
    pthread_join(th, NULL);
    close(c->listen_fd);
    return r;
}

static oc_enroll_result claim_against(int status, struct canned *c, const char *pk, const char *aud) {
    return claim_at(status, c, pk, aud, 0);
}

static void test_claim_answers(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    struct canned c;

    CHECK(claim_against(200, &c, pk, aud) == OC_ENROLL_ACTIVE);
    CHECK(strstr(c.seen, "POST /api/machine/enroll/claim ") == c.seen);
    CHECK(strstr(c.seen, "\"audienceId\":\"ws_") != NULL);
    CHECK(strstr(c.seen, "\"ticket\":\"BwcHBwcH") != NULL);
    CHECK(strstr(c.seen, "\"publicKey\":\"") != NULL && strstr(c.seen, "\"signature\":\"") != NULL);

    CHECK(claim_against(404, &c, pk, aud) == OC_ENROLL_FAILED);    /* refused: do not retry */
    CHECK(claim_against(400, &c, pk, aud) == OC_ENROLL_FAILED);
    CHECK(claim_against(429, &c, pk, aud) == OC_ENROLL_PENDING);   /* busy: try again */
    CHECK(claim_against(503, &c, pk, aud) == OC_ENROLL_PENDING);
    /* Central at an IPv6 address, bracketed in its URL: reached, and asked with
     * a bracketed Host header. */
    oc_enroll_result r6 = claim_at(200, &c, pk, aud, 1);
    if (r6 == (oc_enroll_result)-1) {
        printf("  (no IPv6 loopback on this host: central at [::1] is skipped)\n");
    } else {
        CHECK(r6 == OC_ENROLL_ACTIVE);
        CHECK(strstr(c.seen, "Host: [::1]:") != NULL);
    }
    /* Nobody there at all. */
    CHECK(oc_enroll_claim("http://127.0.0.1:1/api/machine/enroll", aud, pk,
                          "BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc") == OC_ENROLL_PENDING);
}

static void test_persistence(void) {
    oc_dbwriter *w = oc_dbwriter_start(":memory:");
    if (w) oc_dbwriter_set_pw_iterations(w, 2048);   /* fast PBKDF2 for tests */
    CHECK(w != NULL);
    if (!w) return;

    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    CHECK(oc_dbwriter_store_enrollment(w, pk, aud, 0) == 1);

    char *lpk = NULL, *laud = NULL;
    int active = 99;
    CHECK(oc_dbwriter_load_enrollment(w, &lpk, &laud, &active) == 1);
    CHECK(lpk && strcmp(lpk, pk) == 0);
    CHECK(laud && strcmp(laud, aud) == 0);
    CHECK(active == 0);
    free(lpk); free(laud);

    /* Activation flips the state. */
    CHECK(oc_dbwriter_store_enrollment(w, pk, aud, 1) == 1);
    lpk = NULL; laud = NULL; active = 99;
    CHECK(oc_dbwriter_load_enrollment(w, &lpk, &laud, &active) == 1);
    CHECK(active == 1);
    free(lpk); free(laud);

    oc_dbwriter_stop(w);
}

/* Plain http to anything but loopback is refused before a connection is made:
 * the claim and the activation would otherwise cross the network in the clear.
 * 127.0.0.2 reaches this host but is not one of the loopback names. */
static void test_plain_http_refused(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t sl = sizeof sa;
    CHECK(lfd >= 0 && bind(lfd, (struct sockaddr *)&sa, sl) == 0 && listen(lfd, 4) == 0);
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.2:%d/api/machine/enroll", ntohs(sa.sin_port));
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    CHECK(oc_enroll_activate(url, aud, pk) == OC_ENROLL_FAILED);
    CHECK(oc_enroll_claim(url, aud, pk, "AAAA") == OC_ENROLL_FAILED);
    struct timeval tv = { 0, 200000 };
    fd_set rf; FD_ZERO(&rf); FD_SET(lfd, &rf);
    CHECK(select(lfd + 1, &rf, NULL, NULL, &tv) == 0);   /* nobody connected */
    close(lfd);
}

int run_enroll_tests(void) {
    printf("test_enroll: keygen + opaque audience, oce1 code format + SPKI key, "
           "proof-signature verify, persistence round-trip\n");

    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, (const unsigned char *)"test-enroll", 11);

    test_generate(&rng);
    test_code_and_signature();
    test_claim_signature();
    test_plain_http_refused();
    test_claim_answers();
    test_persistence();

    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);
    return failures;
}
