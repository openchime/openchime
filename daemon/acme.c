#include "acme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/asn1.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#define JSMN_HEADER
#include "jsmn.h"

#include "https_client.h"
#include "jwt.h"      /* oc_base64url_encode, oc_jwk_p256, oc_jwk_thumbprint, oc_jws_es256_sign */

#define HTTP_TIMEOUT_MS 30000
#define POLLS           60       /* a pending object is asked about this often at most */

void oc_cert_issued_free(oc_cert_issued *c) {
    if (!c) return;
    if (c->key_pem) { mbedtls_platform_zeroize(c->key_pem, strlen(c->key_pem)); free(c->key_pem); }
    free(c->chain_pem); free(c->names);
    memset(c, 0, sizeof *c);
}

static void fail(char *err, size_t cap, const char *what, const char *detail) {
    if (err && cap) snprintf(err, cap, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
}

/* --- randomness, keys ------------------------------------------------------ */

typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
} rng;

static int rng_init(rng *r) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    static const char pers[] = "openchimed-acme";
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy,
                                 (const unsigned char *)pers, sizeof pers - 1);
}

static void rng_free(rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

static int new_p256(mbedtls_pk_context *pk, rng *r) {
    return mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
           mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk),
                               mbedtls_ctr_drbg_random, &r->drbg) ? -1 : 0;
}

static char *key_pem_of(mbedtls_pk_context *pk) {
    unsigned char buf[2048];
    if (mbedtls_pk_write_key_pem(pk, buf, sizeof buf) != 0) return NULL;
    char *out = strdup((char *)buf);
    mbedtls_platform_zeroize(buf, sizeof buf);
    return out;
}

/* "a.example,b.example" -> the SAN list mbedTLS writes, over `store` (the
 * names split in place). NULL if there are none or too many. */
#define MAX_NAMES 16
static mbedtls_x509_san_list *san_list(char *store, mbedtls_x509_san_list nodes[MAX_NAMES],
                                       const char **first) {
    int n = 0;
    *first = NULL;
    for (char *tok = strtok(store, ", "); tok; tok = strtok(NULL, ", ")) {
        if (n == MAX_NAMES) return NULL;
        memset(&nodes[n], 0, sizeof nodes[n]);
        nodes[n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        nodes[n].node.san.unstructured_name.p = (unsigned char *)tok;
        nodes[n].node.san.unstructured_name.len = strlen(tok);
        if (n) nodes[n - 1].next = &nodes[n];
        if (!*first) *first = tok;
        n++;
    }
    return n ? &nodes[0] : NULL;
}

/* --- the TLS-ALPN-01 certificate and the CSR ------------------------------ */

int oc_acme_challenge_cert(const char *name, const char *keyauth, char **cert_pem, char **key_pem) {
    *cert_pem = *key_pem = NULL;
    rng r; mbedtls_pk_context pk; mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk); mbedtls_x509write_crt_init(&crt);
    int rc = -1;
    if (rng_init(&r) != 0 || new_p256(&pk, &r) != 0) goto out;

    /* id-pe-acmeIdentifier (1.3.6.1.5.5.7.1.31), critical: the DER of
     * OCTET STRING (SIZE (32)) holding SHA-256 of the key authorization. */
    static const char OID_ACME_ID[] = "\x2b\x06\x01\x05\x05\x07\x01\x1f";
    unsigned char ext[34] = { 0x04, 0x20 };
    if (mbedtls_sha256((const unsigned char *)keyauth, strlen(keyauth), ext + 2, 0) != 0) goto out;

    char subj[300];
    snprintf(subj, sizeof subj, "CN=%s", name);
    unsigned char serial[16];
    mbedtls_ctr_drbg_random(&r.drbg, serial, sizeof serial);
    serial[0] &= 0x7f;
    time_t now = time(NULL), a = now - 86400, b = now + 7 * 86400;
    struct tm ta, tb; gmtime_r(&a, &ta); gmtime_r(&b, &tb);
    char nb[16], na[16];
    strftime(nb, sizeof nb, "%Y%m%d%H%M%S", &ta);
    strftime(na, sizeof na, "%Y%m%d%H%M%S", &tb);

    char namebuf[256];
    snprintf(namebuf, sizeof namebuf, "%s", name);
    mbedtls_x509_san_list nodes[MAX_NAMES]; const char *first;
    mbedtls_x509_san_list *sans = san_list(namebuf, nodes, &first);
    if (!sans) goto out;

    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subj) ||
        mbedtls_x509write_crt_set_issuer_name(&crt, subj) ||
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial) ||
        mbedtls_x509write_crt_set_validity(&crt, nb, na) ||
        mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans) ||
        mbedtls_x509write_crt_set_extension(&crt, OID_ACME_ID, sizeof OID_ACME_ID - 1, 1, ext, sizeof ext))
        goto out;
    unsigned char pem[4096];
    if (mbedtls_x509write_crt_pem(&crt, pem, sizeof pem, mbedtls_ctr_drbg_random, &r.drbg) != 0) goto out;
    *cert_pem = strdup((char *)pem);
    *key_pem = key_pem_of(&pk);
    rc = *cert_pem && *key_pem ? 0 : -1;
out:
    if (rc) { free(*cert_pem); free(*key_pem); *cert_pem = *key_pem = NULL; }
    mbedtls_x509write_crt_free(&crt); mbedtls_pk_free(&pk); rng_free(&r);
    return rc;
}

int oc_acme_csr(const char *names, uint8_t **der, size_t *der_len, char **key_pem) {
    *der = NULL; *key_pem = NULL; *der_len = 0;
    rng r; mbedtls_pk_context pk; mbedtls_x509write_csr csr;
    mbedtls_pk_init(&pk); mbedtls_x509write_csr_init(&csr);
    int rc = -1;
    char *store = strdup(names ? names : "");
    if (!store || rng_init(&r) != 0 || new_p256(&pk, &r) != 0) goto out;
    mbedtls_x509_san_list nodes[MAX_NAMES]; const char *first;
    mbedtls_x509_san_list *sans = san_list(store, nodes, &first);
    if (!sans) goto out;
    char subj[300];
    snprintf(subj, sizeof subj, "CN=%s", first);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_csr_set_subject_name(&csr, subj) ||
        mbedtls_x509write_csr_set_subject_alternative_name(&csr, sans)) goto out;
    unsigned char buf[4096];
    int n = mbedtls_x509write_csr_der(&csr, buf, sizeof buf, mbedtls_ctr_drbg_random, &r.drbg);
    if (n <= 0) goto out;                               /* written at the END of buf */
    *der = malloc((size_t)n);
    if (!*der) goto out;
    memcpy(*der, buf + sizeof buf - (size_t)n, (size_t)n);
    *der_len = (size_t)n;
    *key_pem = key_pem_of(&pk);
    rc = *key_pem ? 0 : -1;
out:
    if (rc) { free(*der); *der = NULL; free(*key_pem); *key_pem = NULL; }
    free(store);
    mbedtls_x509write_csr_free(&csr); mbedtls_pk_free(&pk); rng_free(&r);
    return rc;
}

static uint64_t ms_of(const mbedtls_x509_time *t) {
    struct tm tm; memset(&tm, 0, sizeof tm);
    tm.tm_year = t->year - 1900; tm.tm_mon = t->mon - 1; tm.tm_mday = t->day;
    tm.tm_hour = t->hour; tm.tm_min = t->min; tm.tm_sec = t->sec;
    return (uint64_t)timegm(&tm) * 1000u;
}

int oc_cert_validity(const char *pem, size_t len, uint64_t *not_before_ms, uint64_t *not_after_ms) {
    mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
    char *z = malloc(len + 1);
    int rc = -1;
    if (z) {
        memcpy(z, pem, len); z[len] = '\0';
        if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)z, len + 1) == 0) {
            *not_before_ms = ms_of(&crt.valid_from);
            *not_after_ms = ms_of(&crt.valid_to);
            rc = 0;
        }
    }
    free(z);
    mbedtls_x509_crt_free(&crt);
    return rc;
}

/* --- JSON (the few shapes ACME returns) ------------------------------------ */

typedef struct {
    const char *js;
    jsmntok_t  *t;
    int         n;
} jdoc;

static int jparse(jdoc *d, const char *js, size_t len) {
    jsmn_parser p;
    int cap = 256;
    d->js = js; d->t = NULL; d->n = 0;
    for (;;) {
        jsmntok_t *t = realloc(d->t, (size_t)cap * sizeof *t);
        if (!t) return -1;
        d->t = t;
        jsmn_init(&p);
        int n = jsmn_parse(&p, js, len, d->t, (unsigned)cap);
        if (n == JSMN_ERROR_NOMEM && cap < 16384) { cap *= 4; continue; }
        if (n <= 0) return -1;
        d->n = n;
        return 0;
    }
}

static void jfree(jdoc *d) { free(d->t); d->t = NULL; }

/* The index just past token `i`'s whole subtree. */
static int jskip(const jdoc *d, int i) {
    int end = i + 1;
    if (d->t[i].type == JSMN_OBJECT || d->t[i].type == JSMN_ARRAY) {
        int kids = d->t[i].size * (d->t[i].type == JSMN_OBJECT ? 2 : 1);
        for (int k = 0; k < kids && end < d->n; k++) end = jskip(d, end);
    }
    return end;
}

/* The value of `key` in the object at `obj`, or -1. */
static int jget(const jdoc *d, int obj, const char *key) {
    if (obj < 0 || obj >= d->n || d->t[obj].type != JSMN_OBJECT) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int k = 0; k < d->t[obj].size && i + 1 < d->n; k++) {
        const jsmntok_t *kt = &d->t[i];
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == kl &&
            !memcmp(d->js + kt->start, key, kl)) return i + 1;
        i = jskip(d, i + 1);
    }
    return -1;
}

/* String token `i` into `out`, with the escapes ACME values carry undone. */
static int jstr(const jdoc *d, int i, char *out, size_t cap) {
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_STRING || !cap) return -1;
    size_t o = 0;
    for (int p = d->t[i].start; p < d->t[i].end && o + 1 < cap; p++) {
        char ch = d->js[p];
        if (ch == '\\' && p + 1 < d->t[i].end) {
            char e = d->js[++p];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    return 0;
}

static int jgetstr(const jdoc *d, int obj, const char *key, char *out, size_t cap) {
    return jstr(d, jget(d, obj, key), out, cap);
}

/* --- JWS (RFC 7515 flattened JSON, ES256) --------------------------------- */

typedef struct {
    rng                 r;
    mbedtls_pk_context  key;
    char                jwk[160];
    char                thumb[OC_JWT_THUMBPRINT_LEN + 1];
    char                kid[512];            /* the account URL, once known */
    char                nonce[256];
    char                new_nonce[512], new_account[512], new_order[512];
    char               *err; size_t errcap;
    const int          *stop;
} acme;

/* The request body for `url`: `payload` NULL is POST-as-GET (an empty
 * payload). The account is named by its key until it has a URL. */
static char *jws(acme *a, const char *url, const char *payload) {
    size_t plen = payload ? strlen(payload) : 0;
    char *prot = malloc(strlen(url) + sizeof a->jwk + sizeof a->kid + sizeof a->nonce + 64);
    char *prot64 = NULL, *pay64 = NULL, *input = NULL, *out = NULL;
    if (!prot) return NULL;
    if (a->kid[0]) sprintf(prot, "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}", a->kid, a->nonce, url);
    else           sprintf(prot, "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}", a->jwk, a->nonce, url);
    size_t prl = strlen(prot);
    prot64 = malloc(prl * 4 / 3 + 4);
    pay64 = malloc(plen * 4 / 3 + 4);
    if (!prot64 || !pay64) goto out;
    oc_base64url_encode((const uint8_t *)prot, prl, prot64);
    oc_base64url_encode((const uint8_t *)(payload ? payload : ""), plen, pay64);
    size_t il = strlen(prot64) + 1 + strlen(pay64);
    input = malloc(il + 1);
    if (!input) goto out;
    sprintf(input, "%s.%s", prot64, pay64);
    char sig64[OC_JWS_SIG64_LEN + 1];
    if (oc_jws_es256_sign(&a->key, mbedtls_ctr_drbg_random, &a->r.drbg, input, il, sig64) != 0) goto out;
    out = malloc(strlen(prot64) + strlen(pay64) + strlen(sig64) + 64);
    if (out) sprintf(out, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}", prot64, pay64, sig64);
out:
    free(prot); free(prot64); free(pay64); free(input);
    return out;
}

/* A fresh nonce from newNonce (HEAD). */
static int get_nonce(acme *a) {
    oc_https_resp r;
    if (oc_https_request("HEAD", a->new_nonce, NULL, NULL, 0, NULL, HTTP_TIMEOUT_MS, &r, a->err, a->errcap) != 0)
        return -1;
    int ok = oc_https_header(&r, "Replay-Nonce", a->nonce, sizeof a->nonce);
    oc_https_resp_free(&r);
    if (!ok) fail(a->err, a->errcap, "no nonce from the CA", NULL);
    return ok ? 0 : -1;
}

/* An ACME problem document's type and detail into err. */
static void problem(acme *a, const oc_https_resp *r, const char *what) {
    jdoc d; char type[128] = "", detail[256] = "";
    if (r->body_len && jparse(&d, r->body, r->body_len) == 0) {
        jgetstr(&d, 0, "type", type, sizeof type);
        jgetstr(&d, 0, "detail", detail, sizeof detail);
        jfree(&d);
    }
    char both[400];
    snprintf(both, sizeof both, "HTTP %d %s %s", r->status, type, detail);
    fail(a->err, a->errcap, what, both);
}

/* POST `payload` (NULL for POST-as-GET) to `url` signed; the response in `r`.
 * A badNonce is retried with the fresh nonce the CA sends with it (RFC 8555
 * §6.5). 0 on a 2xx. */
static int post(acme *a, const char *url, const char *payload, oc_https_resp *r, const char *what) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!a->nonce[0] && get_nonce(a) != 0) return -1;
        char *body = jws(a, url, payload);
        a->nonce[0] = '\0';                              /* a nonce is used once */
        if (!body) { fail(a->err, a->errcap, "could not sign the request", NULL); return -1; }
        int rc = oc_https_request("POST", url, "application/jose+json", body, strlen(body),
                                  "Accept: application/json, application/pem-certificate-chain\r\n",
                                  HTTP_TIMEOUT_MS, r, a->err, a->errcap);
        free(body);
        if (rc != 0) return -1;
        oc_https_header(r, "Replay-Nonce", a->nonce, sizeof a->nonce);
        if (r->status >= 200 && r->status < 300) return 0;
        int bad_nonce = r->status == 400 && r->body && strstr(r->body, "urn:ietf:params:acme:error:badNonce");
        if (!bad_nonce) { problem(a, r, what); oc_https_resp_free(r); return -1; }
        oc_https_resp_free(r);
    }
    fail(a->err, a->errcap, what, "the CA refused every nonce");
    return -1;
}

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* POST-as-GET `url` until its "status" leaves pending/processing, or POLLS
 * tries. The final object in `d` over `r` (freed by the caller). */
static int poll_status(acme *a, const char *url, int poll_ms, oc_https_resp *r, jdoc *d,
                       char *status, size_t scap, const char *what) {
    for (int i = 0; i < POLLS; i++) {
        if (a->stop && __atomic_load_n(a->stop, __ATOMIC_ACQUIRE)) { fail(a->err, a->errcap, what, "stopped"); return -1; }
        if (post(a, url, NULL, r, what) != 0) return -1;
        if (jparse(d, r->body, r->body_len) != 0) { fail(a->err, a->errcap, what, "unreadable reply"); oc_https_resp_free(r); return -1; }
        jgetstr(d, 0, "status", status, scap);
        if (strcmp(status, "pending") && strcmp(status, "processing")) return 0;
        jfree(d); oc_https_resp_free(r);
        nap(poll_ms);
    }
    fail(a->err, a->errcap, what, "still pending");
    return -1;
}

/* --- the issuance ---------------------------------------------------------- */

/* Answer one authorization: its tls-alpn-01 challenge, through the listener. */
static int authorize(acme *a, const oc_acme_opts *o, const char *authz_url) {
    oc_https_resp r; jdoc d;
    if (post(a, authz_url, NULL, &r, "reading an authorization") != 0) return -1;
    if (jparse(&d, r.body, r.body_len) != 0) { oc_https_resp_free(&r); fail(a->err, a->errcap, "unreadable authorization", NULL); return -1; }
    char status[32] = "", name[256] = "", chal_url[512] = "", token[256] = "";
    jgetstr(&d, 0, "status", status, sizeof status);
    int id = jget(&d, 0, "identifier");
    jgetstr(&d, id, "value", name, sizeof name);
    int chs = jget(&d, 0, "challenges");
    if (chs >= 0 && d.t[chs].type == JSMN_ARRAY) {
        for (int k = 0, i = chs + 1; k < d.t[chs].size; k++, i = jskip(&d, i)) {
            char type[32] = "";
            jgetstr(&d, i, "type", type, sizeof type);
            if (!strcmp(type, "tls-alpn-01")) {
                jgetstr(&d, i, "url", chal_url, sizeof chal_url);
                jgetstr(&d, i, "token", token, sizeof token);
            }
        }
    }
    jfree(&d); oc_https_resp_free(&r);
    if (!strcmp(status, "valid")) return 0;             /* already proven */
    if (!chal_url[0] || !token[0] || !name[0]) {
        fail(a->err, a->errcap, "the CA offers no tls-alpn-01 challenge for", name); return -1;
    }

    char keyauth[400];
    snprintf(keyauth, sizeof keyauth, "%s.%s", token, a->thumb);
    char *cpem = NULL, *kpem = NULL;
    if (oc_acme_challenge_cert(name, keyauth, &cpem, &kpem) != 0 ||
        oc_tls_server_set_challenge(o->tls, name, cpem, strlen(cpem), kpem, strlen(kpem)) != 0) {
        free(cpem); free(kpem);
        fail(a->err, a->errcap, "could not set up the challenge for", name); return -1;
    }
    free(cpem); free(kpem);

    int rc = -1;
    if (post(a, chal_url, "{}", &r, "answering the challenge") == 0) {
        oc_https_resp_free(&r);
        if (poll_status(a, authz_url, o->poll_ms, &r, &d, status, sizeof status, "validating") == 0) {
            if (!strcmp(status, "valid")) rc = 0;
            else {
                /* The failed challenge says why. */
                char detail[256] = "";
                int c2 = jget(&d, 0, "challenges");
                for (int k = 0, i = c2 + 1; c2 >= 0 && k < d.t[c2].size && !detail[0]; k++, i = jskip(&d, i)) {
                    int e = jget(&d, i, "error");
                    jgetstr(&d, e, "detail", detail, sizeof detail);
                }
                char both[640];
                snprintf(both, sizeof both, "%s is %s%s%s", name, status, detail[0] ? ": " : "", detail);
                fail(a->err, a->errcap, "validation failed", both);
            }
            jfree(&d); oc_https_resp_free(&r);
        }
    }
    oc_tls_server_set_challenge(o->tls, name, NULL, 0, NULL, 0);
    return rc;
}

int oc_acme_issue(const oc_acme_opts *o, oc_cert_issued *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    acme *a = calloc(1, sizeof *a);
    if (!a) { fail(err, errcap, "out of memory", NULL); return -1; }
    a->err = err; a->errcap = errcap; a->stop = o->stop;
    mbedtls_pk_init(&a->key);
    int rc = -1;
    oc_https_resp r; jdoc d;
    char *payload = NULL;
    if (rng_init(&a->r) != 0) { fail(err, errcap, "no randomness", NULL); goto out; }
    int poll_ms = o->poll_ms > 0 ? o->poll_ms : 2000;
    oc_acme_opts oo = *o; oo.poll_ms = poll_ms;

    /* The directory. */
    if (oc_https_request("GET", o->directory, NULL, NULL, 0, NULL, HTTP_TIMEOUT_MS, &r, err, errcap) != 0) goto out;
    if (r.status != 200 || jparse(&d, r.body, r.body_len) != 0) {
        oc_https_resp_free(&r); fail(err, errcap, "the ACME directory is unreadable", o->directory); goto out;
    }
    jgetstr(&d, 0, "newNonce", a->new_nonce, sizeof a->new_nonce);
    jgetstr(&d, 0, "newAccount", a->new_account, sizeof a->new_account);
    jgetstr(&d, 0, "newOrder", a->new_order, sizeof a->new_order);
    jfree(&d); oc_https_resp_free(&r);
    if (!a->new_nonce[0] || !a->new_account[0] || !a->new_order[0]) {
        fail(err, errcap, "the ACME directory lacks newNonce/newAccount/newOrder", NULL); goto out;
    }

    /* The account: the kept one, or a new one kept from now on. */
    int fresh = 0;
    if (o->account_key_pem && *o->account_key_pem) {
        if (mbedtls_pk_parse_key(&a->key, (const unsigned char *)o->account_key_pem, strlen(o->account_key_pem) + 1,
                                 NULL, 0, mbedtls_ctr_drbg_random, &a->r.drbg) != 0) {
            fail(err, errcap, "the kept ACME account key does not parse", NULL); goto out;
        }
    } else {
        if (new_p256(&a->key, &a->r) != 0) { fail(err, errcap, "could not make an account key", NULL); goto out; }
        fresh = 1;
    }
    if (oc_jwk_p256(&a->key, a->jwk, sizeof a->jwk) < 0 || oc_jwk_thumbprint(&a->key, a->thumb) != 0) {
        fail(err, errcap, "the account key is not P-256", NULL); goto out;
    }
    if (!fresh && o->account_url && *o->account_url) {
        snprintf(a->kid, sizeof a->kid, "%s", o->account_url);
    } else {
        /* newAccount with the key (it returns the existing account for a known
         * key). Agreeing to the CA's terms is the operator's, by turning ACME
         * on (CONFIG.md, "Certificates"). */
        char acct[512];
        if (o->email && *o->email) snprintf(acct, sizeof acct, "{\"termsOfServiceAgreed\":true,\"contact\":[\"mailto:%s\"]}", o->email);
        else snprintf(acct, sizeof acct, "{\"termsOfServiceAgreed\":true}");
        if (post(a, a->new_account, acct, &r, "creating the ACME account") != 0) goto out;
        int got = oc_https_header(&r, "Location", a->kid, sizeof a->kid);
        oc_https_resp_free(&r);
        if (!got) { fail(err, errcap, "the CA gave the account no URL", NULL); goto out; }
        if (o->store_account) {
            char *kp = key_pem_of(&a->key);
            if (kp) { o->store_account(o->ctx, kp, a->kid); mbedtls_platform_zeroize(kp, strlen(kp)); free(kp); }
        }
    }

    /* The order, for every name. */
    {
        size_t cap = strlen(o->names) * 2 + 64 + (o->replaces ? strlen(o->replaces) + 32 : 0);
        payload = malloc(cap + 64 * MAX_NAMES);
        char *names = strdup(o->names);
        if (!payload || !names) { free(names); fail(err, errcap, "out of memory", NULL); goto out; }
        strcpy(payload, "{\"identifiers\":[");
        int n = 0;
        for (char *t = strtok(names, ", "); t; t = strtok(NULL, ", "))
            sprintf(payload + strlen(payload), "%s{\"type\":\"dns\",\"value\":\"%s\"}", n++ ? "," : "", t);
        strcat(payload, "]");
        /* A replacement names what it replaces (RFC 9773 §5). */
        if (o->replaces && *o->replaces) sprintf(payload + strlen(payload), ",\"replaces\":\"%s\"", o->replaces);
        strcat(payload, "}");
        free(names);
        if (!n) { fail(err, errcap, "no names to certify", NULL); goto out; }
    }
    char order_url[512] = "", finalize[512] = "";
    if (post(a, a->new_order, payload, &r, "placing the order") != 0) goto out;
    oc_https_header(&r, "Location", order_url, sizeof order_url);
    if (jparse(&d, r.body, r.body_len) != 0) { oc_https_resp_free(&r); fail(err, errcap, "unreadable order", NULL); goto out; }
    jgetstr(&d, 0, "finalize", finalize, sizeof finalize);
    char authz[MAX_NAMES][512]; int nauthz = 0;
    int az = jget(&d, 0, "authorizations");
    for (int k = 0, i = az + 1; az >= 0 && k < d.t[az].size && nauthz < MAX_NAMES; k++, i = jskip(&d, i))
        if (jstr(&d, i, authz[nauthz], sizeof authz[0]) == 0) nauthz++;
    jfree(&d); oc_https_resp_free(&r);
    if (!order_url[0] || !finalize[0] || !nauthz) { fail(err, errcap, "the order is incomplete", NULL); goto out; }

    for (int k = 0; k < nauthz; k++)
        if (authorize(a, &oo, authz[k]) != 0) goto out;

    /* Finalize with a fresh key, and collect the chain. */
    uint8_t *der = NULL; size_t dlen = 0; char *key_pem = NULL;
    if (oc_acme_csr(o->names, &der, &dlen, &key_pem) != 0) { fail(err, errcap, "could not make the CSR", NULL); goto out; }
    char *csr64 = malloc(dlen * 4 / 3 + 4);
    char *fin = csr64 ? malloc(dlen * 4 / 3 + 32) : NULL;
    if (fin) { oc_base64url_encode(der, dlen, csr64); sprintf(fin, "{\"csr\":\"%s\"}", csr64); }
    free(der); free(csr64);
    if (!fin) { free(key_pem); fail(err, errcap, "out of memory", NULL); goto out; }
    int frc = post(a, finalize, fin, &r, "finalizing");
    free(fin);
    if (frc != 0) { free(key_pem); goto out; }
    oc_https_resp_free(&r);
    char status[32] = "", cert_url[512] = "";
    if (poll_status(a, order_url, poll_ms, &r, &d, status, sizeof status, "issuing") != 0) { free(key_pem); goto out; }
    jgetstr(&d, 0, "certificate", cert_url, sizeof cert_url);
    jfree(&d); oc_https_resp_free(&r);
    if (strcmp(status, "valid") || !cert_url[0]) { free(key_pem); fail(err, errcap, "the order ended", status); goto out; }
    if (post(a, cert_url, NULL, &r, "downloading the certificate") != 0) { free(key_pem); goto out; }
    out->chain_pem = r.body; r.body = NULL;
    oc_https_resp_free(&r);
    out->key_pem = key_pem;
    out->names = strdup(o->names);
    if (!out->chain_pem || !strstr(out->chain_pem, "-----BEGIN CERTIFICATE-----") ||
        oc_cert_validity(out->chain_pem, strlen(out->chain_pem), &out->not_before_ms, &out->not_after_ms) != 0) {
        oc_cert_issued_free(out);
        fail(err, errcap, "the CA's certificate does not parse", NULL); goto out;
    }
    rc = 0;
out:
    free(payload);
    mbedtls_pk_free(&a->key);
    rng_free(&a->r);
    free(a);
    return rc;
}

/* --- renewal information (RFC 9773) ---------------------------------------- */

int oc_acme_cert_id_raw(const uint8_t *aki, size_t aki_len, const uint8_t *serial, size_t serial_len,
                        char *out, size_t cap) {
    if (!aki_len || !serial_len || 4 * ((aki_len + 2) / 3) + 1 + 4 * ((serial_len + 2) / 3) + 1 > cap) return -1;
    size_t o = oc_base64url_encode(aki, aki_len, out);
    out[o++] = '.';
    oc_base64url_encode(serial, serial_len, out + o);
    return 0;
}

int oc_acme_cert_id(const char *chain_pem, char *out, size_t cap) {
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = -1;
    if (chain_pem && mbedtls_x509_crt_parse(&crt, (const unsigned char *)chain_pem, strlen(chain_pem) + 1) >= 0 &&
        crt.version >= 3)
        rc = oc_acme_cert_id_raw(crt.authority_key_id.keyIdentifier.p, crt.authority_key_id.keyIdentifier.len,
                                 crt.serial.p, crt.serial.len, out, cap);
    mbedtls_x509_crt_free(&crt);
    return rc;
}

static int digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return -1; v = v * 10 + (s[i] - '0'); }
    *out = v;
    return 0;
}

int oc_rfc3339_ms(const char *s, uint64_t *out) {
    int Y, M, D, h, m, sec;
    if (!s || strlen(s) < 20 || digits(s, 4, &Y) || s[4] != '-' || digits(s + 5, 2, &M) || s[7] != '-' ||
        digits(s + 8, 2, &D) || (s[10] != 'T' && s[10] != 't') || digits(s + 11, 2, &h) || s[13] != ':' ||
        digits(s + 14, 2, &m) || s[16] != ':' || digits(s + 17, 2, &sec)) return -1;
    const char *p = s + 19;
    int ms = 0;
    if (*p == '.') {                               /* fractions: the first three digits count */
        int scale = 100;
        for (p++; *p >= '0' && *p <= '9'; p++) { ms += (*p - '0') * scale; scale /= 10; }
    }
    long off = 0;
    if (*p == 'Z' || *p == 'z') p++;
    else if (*p == '+' || *p == '-') {
        int oh, om;
        if (digits(p + 1, 2, &oh) || p[3] != ':' || digits(p + 4, 2, &om)) return -1;
        off = (long)(oh * 60 + om) * 60 * (*p == '-' ? -1 : 1);
        p += 6;
    } else return -1;
    if (*p || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return -1;
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = sec;
    time_t e = timegm(&t);
    if (e == (time_t)-1 || (long long)e - off < 0) return -1;
    *out = (uint64_t)((long long)e - off) * 1000u + (uint64_t)ms;
    return 0;
}

int oc_acme_renewal_info(const char *directory, const char *chain_pem, uint64_t *start_ms, uint64_t *end_ms,
                         uint64_t *retry_after_ms, char *err, size_t errcap) {
    *start_ms = *end_ms = *retry_after_ms = 0;
    char id[OC_ACME_CERT_ID_MAX], base[512], url[768];
    if (oc_acme_cert_id(chain_pem, id, sizeof id) != 0) { fail(err, errcap, "the certificate has no ARI identifier", NULL); return -1; }
    oc_https_resp r; jdoc d;
    if (oc_https_request("GET", directory, NULL, NULL, 0, NULL, HTTP_TIMEOUT_MS, &r, err, errcap) != 0) return -1;
    base[0] = '\0';
    if (r.status == 200 && jparse(&d, r.body, r.body_len) == 0) { jgetstr(&d, 0, "renewalInfo", base, sizeof base); jfree(&d); }
    oc_https_resp_free(&r);
    if (!base[0]) { fail(err, errcap, "the CA offers no renewal information", NULL); return -1; }
    snprintf(url, sizeof url, "%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", id);
    if (oc_https_request("GET", url, NULL, NULL, 0, NULL, HTTP_TIMEOUT_MS, &r, err, errcap) != 0) return -1;
    int rc = -1;
    char hv[32], st[64] = "", en[64] = "";
    if (r.status == 200 && jparse(&d, r.body, r.body_len) == 0) {
        int w = jget(&d, 0, "suggestedWindow");
        if (w > 0) { jgetstr(&d, w, "start", st, sizeof st); jgetstr(&d, w, "end", en, sizeof en); }
        jfree(&d);
        if (oc_rfc3339_ms(st, start_ms) == 0 && oc_rfc3339_ms(en, end_ms) == 0 && *end_ms > *start_ms) rc = 0;
        else fail(err, errcap, "the CA's renewal information is unreadable", NULL);
    } else {
        fail(err, errcap, "the CA gave no renewal information", NULL);
    }
    if (oc_https_header(&r, "Retry-After", hv, sizeof hv)) *retry_after_ms = (uint64_t)strtoull(hv, NULL, 10) * 1000u;
    oc_https_resp_free(&r);
    return rc;
}

