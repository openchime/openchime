/* A private PKI for tests: a root, and leaves for DNS names signed by it or by
 * themselves (a daemon's own self-signed certificate). Header-only, static. */
#ifndef OC_TESTPKI_H
#define OC_TESTPKI_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

typedef struct {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    char                     pem[4096];
} testpki;

static int testpki_key(testpki *p, mbedtls_pk_context *k) {
    mbedtls_pk_init(k);
    return mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
           mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*k), mbedtls_ctr_drbg_random, &p->rng) ? -1 : 0;
}

/* Sign a certificate for `subj_key` naming `dns` (NULL: none), by `iss_key`
 * under `iss_name`; a CA if `ca`. PEM into `out`. */
static int testpki_mint(testpki *p, mbedtls_pk_context *subj_key, const char *subj, const char *dns,
                        mbedtls_pk_context *iss_key, const char *iss_name, int ca, char *out, size_t cap) {
    mbedtls_x509write_cert w; mbedtls_x509write_crt_init(&w);
    mbedtls_x509_san_list san; memset(&san, 0, sizeof san);
    static unsigned char serial = 1; serial++;
    mbedtls_x509write_crt_set_subject_key(&w, subj_key); mbedtls_x509write_crt_set_issuer_key(&w, iss_key);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3); mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    int rc = 0;
    if (dns) {
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = (unsigned char *)dns;
        san.node.san.unstructured_name.len = strlen(dns);
        rc = mbedtls_x509write_crt_set_subject_alternative_name(&w, &san);
    }
    rc = rc || mbedtls_x509write_crt_set_subject_name(&w, subj) || mbedtls_x509write_crt_set_issuer_name(&w, iss_name) ||
         mbedtls_x509write_crt_set_serial_raw(&w, &serial, 1) ||
         mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20500101000000") ||
         mbedtls_x509write_crt_set_basic_constraints(&w, ca, -1) ||
         (ca && mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_KEY_CERT_SIGN)) ||
         mbedtls_x509write_crt_pem(&w, (unsigned char *)out, cap, mbedtls_ctr_drbg_random, &p->rng);
    mbedtls_x509write_crt_free(&w);
    return rc ? -1 : 0;
}

static int testpki_init(testpki *p) {
    mbedtls_entropy_init(&p->ent); mbedtls_ctr_drbg_init(&p->rng);
    if (mbedtls_ctr_drbg_seed(&p->rng, mbedtls_entropy_func, &p->ent, NULL, 0) || testpki_key(p, &p->key)) return -1;
    return testpki_mint(p, &p->key, "CN=Test Root", NULL, &p->key, "CN=Test Root", 1, p->pem, sizeof p->pem);
}

/* A leaf for `dns`: signed by the root, or by itself (`self`). The chain (the
 * leaf, then the root if signed by it) and the key, PEM. */
static int testpki_leaf(testpki *p, const char *dns, int self, char *chain, size_t cap, char *key, size_t kcap) {
    mbedtls_pk_context k;
    if (testpki_key(p, &k)) return -1;
    char subj[300]; snprintf(subj, sizeof subj, "CN=%s", dns);
    char leaf[4096];
    int rc = testpki_mint(p, &k, subj, dns, self ? &k : &p->key, self ? subj : "CN=Test Root", 0, leaf, sizeof leaf) ||
             mbedtls_pk_write_key_pem(&k, (unsigned char *)key, kcap) ? -1 : 0;
    if (!rc) snprintf(chain, cap, "%s%s", leaf, self ? "" : p->pem);
    mbedtls_pk_free(&k);
    return rc;
}

static void testpki_free(testpki *p) {
    mbedtls_pk_free(&p->key); mbedtls_ctr_drbg_free(&p->rng); mbedtls_entropy_free(&p->ent);
}

#endif /* OC_TESTPKI_H */
