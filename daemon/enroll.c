/* Federated enrollment client (CP-8). See enroll.h. Crypto is mbedTLS; the
 * outbound HTTP mirrors the S3 backend's blocking helpers (daemon/blob_s3.c) over
 * the CA-verifying TLS wrapper (shared/tls.c). JSON is emitted with snprintf and
 * parsed with the vendored jsmn (a self-contained static copy in this TU). */

#include "enroll.h"
#include "tls.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#define JSMN_HEADER   /* declarations only; the impl lives in daemon/jwt.c's TU */
#include "jsmn.h"

#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* --- RNG (the canonical entropy + ctr_drbg recipe) ------------------------- */

typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
} enroll_rng;

static int rng_init(enroll_rng *r) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    const char *pers = "oc-enroll";
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy,
                                 (const unsigned char *)pers, strlen(pers));
}

static void rng_free(enroll_rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

/* --- base64 (standard) + base64url encode ---------------------------------- */

static int b64_std(const uint8_t *in, size_t inlen, char *out, size_t outcap, size_t *outlen) {
    size_t ol = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outcap, &ol, in, inlen) != 0) return -1;
    out[ol] = '\0';
    *outlen = ol;
    return 0;
}

static int b64url(const uint8_t *in, size_t inlen, char *out, size_t outcap, size_t *outlen) {
    if (b64_std(in, inlen, out, outcap, outlen) != 0) return -1;
    size_t w = 0;
    for (size_t i = 0; i < *outlen; i++) {
        char c = out[i];
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        else if (c == '=') continue;   /* url-safe, no padding */
        out[w++] = c;
    }
    out[w] = '\0';
    *outlen = w;
    return 0;
}

/* --- minimal JSON reads (flat top-level object), copied from daemon/jwt.c --- */

#define ENROLL_MAX_TOKENS 32

static int tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int len = t->end - t->start;
    return t->type == JSMN_STRING && len == (int)strlen(s) &&
           strncmp(js + t->start, s, (size_t)len) == 0;
}

static int skip_tok(const jsmntok_t *t, int i, int ntok) {
    if (i >= ntok) return i;
    if (t[i].type == JSMN_STRING || t[i].type == JSMN_PRIMITIVE) return i + 1;
    int j = i + 1;
    if (t[i].type == JSMN_OBJECT) {
        for (int c = 0; c < t[i].size; c++) { j = skip_tok(t, j, ntok); j = skip_tok(t, j, ntok); }
    } else if (t[i].type == JSMN_ARRAY) {
        for (int c = 0; c < t[i].size; c++) j = skip_tok(t, j, ntok);
    }
    return j;
}

static int find_value(const char *js, const jsmntok_t *t, int ntok, const char *key) {
    if (ntok < 1 || t[0].type != JSMN_OBJECT) return -1;
    int i = 1;
    for (int c = 0; c < t[0].size && i + 1 < ntok; c++) {
        int vidx = i + 1;
        if (tok_eq(js, &t[i], key)) return vidx;
        i = skip_tok(t, vidx, ntok);
    }
    return -1;
}

static int json_get_str(const char *js, size_t jlen, const char *key, char *dst, size_t cap) {
    jsmntok_t toks[ENROLL_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, jlen, toks, ENROLL_MAX_TOKENS);
    if (n < 0) return 0;
    int v = find_value(js, toks, n, key);
    if (v < 0 || toks[v].type != JSMN_STRING) return 0;
    size_t len = (size_t)(toks[v].end - toks[v].start);
    if (len >= cap) len = cap - 1;
    memcpy(dst, js + toks[v].start, len);
    dst[len] = '\0';
    return 1;
}

/* --- outbound HTTP over CA-verified TLS (adapted from daemon/blob_s3.c) ----- */

typedef struct {
    char host[256];       /* host only, for connect() + SNI/cert check */
    char endpoint[300];   /* host[:port], as sent in the Host header */
    char port[16];
    int  use_tls;
    oc_tls_client tls;    /* valid iff use_tls; CA-verifying config */
} enroll_ctx;

typedef struct {
    int         fd;
    int         tls;
    oc_tls_conn conn;
} enroll_conn;

static int parse_url(const char *url, enroll_ctx *ctx) {
    memset(ctx, 0, sizeof *ctx);
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { p += 8; ctx->use_tls = 1; snprintf(ctx->port, sizeof ctx->port, "443"); }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; ctx->use_tls = 0; snprintf(ctx->port, sizeof ctx->port, "80"); }
    else return -1;

    char hostport[256];
    size_t i = 0;
    while (*p && *p != '/' && i < sizeof hostport - 1) hostport[i++] = *p++;
    hostport[i] = '\0';
    if (i == 0) return -1;

    char *colon = strchr(hostport, ':');
    if (colon) { *colon = '\0'; snprintf(ctx->port, sizeof ctx->port, "%s", colon + 1); }
    snprintf(ctx->host, sizeof ctx->host, "%s", hostport);

    int is_default = (ctx->use_tls && strcmp(ctx->port, "443") == 0) ||
                     (!ctx->use_tls && strcmp(ctx->port, "80") == 0);
    if (is_default) snprintf(ctx->endpoint, sizeof ctx->endpoint, "%s", ctx->host);
    else snprintf(ctx->endpoint, sizeof ctx->endpoint, "%s:%s", ctx->host, ctx->port);
    return 0;
}

static int tcp_connect(const enroll_ctx *ctx) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ctx->host, ctx->port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return fd;
}

static int conn_open(enroll_conn *c, enroll_ctx *ctx) {
    memset(c, 0, sizeof *c);
    c->fd = tcp_connect(ctx);
    if (c->fd < 0) return -1;
    if (!ctx->use_tls) return 0;

    if (oc_tls_conn_init(&c->conn, &ctx->tls.conf, c->fd) != 0) { close(c->fd); return -1; }
    if (oc_tls_conn_set_hostname(&c->conn, ctx->host) != 0) { oc_tls_conn_free(&c->conn); close(c->fd); return -1; }
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) break;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        fprintf(stderr, "openchimed: enroll TLS handshake failed for %s\n", ctx->host);
        oc_tls_conn_free(&c->conn); close(c->fd);
        return -1;
    }
    c->tls = 1;
    return 0;
}

static void conn_close(enroll_conn *c) {
    if (!c) return;
    if (c->tls) oc_tls_conn_free(&c->conn);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->tls = 0;
}

static int conn_write(enroll_conn *c, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        if (c->tls) {
            size_t n = 0;
            oc_tls_status st = oc_tls_write(&c->conn, p + sent, len - sent, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st != OC_TLS_OK || n == 0) return -1;
            sent += n;
        } else {
            ssize_t n = write(c->fd, p + sent, len - sent);
            if (n <= 0) return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

static long conn_read(enroll_conn *c, void *buf, size_t cap) {
    if (c->tls) {
        for (;;) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&c->conn, buf, cap, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st == OC_TLS_CLOSED) return 0;
            if (st != OC_TLS_OK) return -1;
            return (long)n;
        }
    }
    ssize_t n = read(c->fd, buf, cap);
    return (n < 0) ? -1 : (long)n;
}

/* POST a JSON body to `path`; fills *status and copies the (small) JSON reply
 * into `resp`. Returns 0 on a completed exchange, -1 on I/O error. */
static int post_json(enroll_ctx *ctx, const char *path, const char *body,
                     int *status, char *resp, size_t resp_cap, size_t *resp_len) {
    enroll_conn c;
    if (conn_open(&c, ctx) != 0) return -1;

    char req[1024];
    size_t blen = strlen(body);
    int n = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        path, ctx->endpoint, blen);
    if (n < 0 || n >= (int)sizeof req || conn_write(&c, req, (size_t)n) != 0 ||
        conn_write(&c, body, blen) != 0) {
        conn_close(&c);
        return -1;
    }

    /* Response head: status line + Content-Length + any spilled body bytes. */
    char hdr[8192];
    size_t hlen = 0;
    const char *term = NULL;
    while (hlen < sizeof hdr - 1) {
        long r = conn_read(&c, hdr + hlen, sizeof hdr - 1 - hlen);
        if (r <= 0) { conn_close(&c); return -1; }
        hlen += (size_t)r;
        hdr[hlen] = '\0';
        term = strstr(hdr, "\r\n\r\n");
        if (term) break;
    }
    if (!term || strncmp(hdr, "HTTP/1.", 7) != 0) { conn_close(&c); return -1; }
    *status = atoi(hdr + 9);

    long long content_length = -1;
    for (const char *p = hdr; p && p < term; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > term) break;
        if (strncasecmp(p, "Content-Length:", 15) == 0) content_length = atoll(p + 15);
        p = eol + 2;
    }

    size_t got = 0;
    const char *body_start = term + 4;
    size_t spilled = hlen - (size_t)(body_start - hdr);
    if (spilled > resp_cap - 1) spilled = resp_cap - 1;
    memcpy(resp, body_start, spilled);
    got = spilled;

    for (;;) {
        if (content_length >= 0 && (long long)got >= content_length) break;
        if (got >= resp_cap - 1) break;
        long r = conn_read(&c, resp + got, resp_cap - 1 - got);
        if (r < 0) { conn_close(&c); return -1; }
        if (r == 0) break;   /* EOF — Connection: close */
        got += (size_t)r;
    }
    resp[got] = '\0';
    *resp_len = got;
    conn_close(&c);
    return 0;
}

/* --- public: generate / build code / activate ------------------------------ */

int oc_enroll_generate(char *privkey_pem, size_t privkey_cap,
                       char *audience, size_t audience_cap) {
    enroll_rng rng;
    if (rng_init(&rng) != 0) { rng_free(&rng); return -1; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto done;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                            mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;
    if (mbedtls_pk_write_key_pem(&pk, (unsigned char *)privkey_pem, privkey_cap) != 0) goto done;

    uint8_t rnd[16];
    if (mbedtls_ctr_drbg_random(&rng.drbg, rnd, sizeof rnd) != 0) goto done;
    char b[32];
    size_t bl = 0;
    if (b64url(rnd, sizeof rnd, b, sizeof b, &bl) != 0) goto done;
    if (snprintf(audience, audience_cap, "ws_%s", b) >= (int)audience_cap) goto done;
    rc = 0;

done:
    mbedtls_pk_free(&pk);
    rng_free(&rng);
    return rc;
}

int oc_enroll_build_code(const char *privkey_pem, const char *audience,
                         char *code, size_t code_cap) {
    enroll_rng rng;
    if (rng_init(&rng) != 0) { rng_free(&rng); return -1; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)privkey_pem, strlen(privkey_pem) + 1,
                             NULL, 0, mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;

    uint8_t der[256];
    int dl = mbedtls_pk_write_pubkey_der(&pk, der, sizeof der);
    if (dl < 0) goto done;
    const uint8_t *spki = der + sizeof der - dl;   /* written at the END of the buffer */

    char pk_b64[512];
    size_t pl = 0;
    if (b64_std(spki, (size_t)dl, pk_b64, sizeof pk_b64, &pl) != 0) goto done;

    char json[768];
    int jn = snprintf(json, sizeof json, "{\"aud\":\"%s\",\"pk\":\"%s\"}", audience, pk_b64);
    if (jn < 0 || jn >= (int)sizeof json) goto done;

    char payload[1100];
    size_t payl = 0;
    if (b64url((const uint8_t *)json, (size_t)jn, payload, sizeof payload, &payl) != 0) goto done;
    if (snprintf(code, code_cap, "oce1.%s", payload) >= (int)code_cap) goto done;
    rc = 0;

done:
    mbedtls_pk_free(&pk);
    rng_free(&rng);
    return rc;
}

/* Sign SHA-256("openchime-enroll-v1|<aud>|<nonce>") → ASN.1 DER → base64. */
int oc_enroll_sign_proof(const char *privkey_pem, const char *audience, const char *nonce,
                         char *sig_b64, size_t sig_cap) {
    enroll_rng rng;
    if (rng_init(&rng) != 0) { rng_free(&rng); return -1; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)privkey_pem, strlen(privkey_pem) + 1,
                             NULL, 0, mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;

    char msg[768];
    int mn = snprintf(msg, sizeof msg, "openchime-enroll-v1|%s|%s", audience, nonce);
    if (mn < 0 || mn >= (int)sizeof msg) goto done;

    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)msg, (size_t)mn, hash, 0) != 0) goto done;

    uint8_t der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_len = 0;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, der, sizeof der, &der_len,
                        mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;

    size_t sl = 0;
    if (b64_std(der, der_len, sig_b64, sig_cap, &sl) != 0) goto done;
    rc = 0;

done:
    mbedtls_pk_free(&pk);
    rng_free(&rng);
    return rc;
}

oc_enroll_result oc_enroll_activate(const char *central_url, const char *ca_bundle,
                                    const char *audience, const char *privkey_pem) {
    enroll_ctx ctx;
    if (parse_url(central_url, &ctx) != 0) return OC_ENROLL_FAILED;
    if (ctx.use_tls && oc_tls_client_init_ca(&ctx.tls, ca_bundle) != 0) return OC_ENROLL_FAILED;

    oc_enroll_result result = OC_ENROLL_FAILED;

    /* 1. challenge → nonce (404 = not yet reserved by the operator). */
    char body[256];
    if (snprintf(body, sizeof body, "{\"audienceId\":\"%s\"}", audience) >= (int)sizeof body) goto done;
    int status = 0;
    char resp[2048];
    size_t rlen = 0;
    if (post_json(&ctx, "/api/machine/enroll/challenge", body, &status, resp, sizeof resp, &rlen) != 0) goto done;
    if (status == 404) { result = OC_ENROLL_PENDING; goto done; }
    if (status != 200) goto done;

    char nonce[256];
    if (!json_get_str(resp, rlen, "nonce", nonce, sizeof nonce)) goto done;

    /* 2. sign the proof. */
    char sig_b64[256];
    if (oc_enroll_sign_proof(privkey_pem, audience, nonce, sig_b64, sizeof sig_b64) != 0) goto done;

    /* 3. confirm. */
    char body2[768];
    if (snprintf(body2, sizeof body2, "{\"audienceId\":\"%s\",\"nonce\":\"%s\",\"signature\":\"%s\"}",
                 audience, nonce, sig_b64) >= (int)sizeof body2) goto done;
    int status2 = 0;
    char resp2[1024];
    size_t rlen2 = 0;
    if (post_json(&ctx, "/api/machine/enroll/confirm", body2, &status2, resp2, sizeof resp2, &rlen2) != 0) goto done;
    result = (status2 == 200) ? OC_ENROLL_ACTIVE : OC_ENROLL_FAILED;

done:
    if (ctx.use_tls) oc_tls_client_free(&ctx.tls);
    return result;
}
