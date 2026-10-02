/* A passkey for tests (AUTH.md §8.6): an authenticator played in-process -- a
 * P-256 key, the CBOR it hands over and the signatures it makes -- so the
 * daemon's checks can be driven as a browser would drive them. */
#ifndef OPENCHIME_WA_FIXTURE_H
#define OPENCHIME_WA_FIXTURE_H

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    mbedtls_pk_context       pk;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    uint8_t                  id[16];
    uint8_t                  x[32], y[32];
    uint32_t                 count;
} wa_key;

static size_t cb_head(uint8_t *b, int major, uint64_t v) {
    if (v < 24) { b[0] = (uint8_t)(major << 5 | v); return 1; }
    if (v < 256) { b[0] = (uint8_t)(major << 5 | 24); b[1] = (uint8_t)v; return 2; }
    b[0] = (uint8_t)(major << 5 | 25); b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)v; return 3;
}
static size_t cb_int(uint8_t *b, int64_t v) { return v >= 0 ? cb_head(b, 0, (uint64_t)v) : cb_head(b, 1, (uint64_t)(-1 - v)); }
static size_t cb_bytes(uint8_t *b, const void *p, size_t n, int major) {
    size_t o = cb_head(b, major, n); memcpy(b + o, p, n); return o + n;
}

static int wa_key_new(wa_key *k, uint8_t id_byte) {
    memset(k, 0, sizeof *k);
    mbedtls_pk_init(&k->pk);
    mbedtls_entropy_init(&k->ent);
    mbedtls_ctr_drbg_init(&k->rng);
    if (mbedtls_ctr_drbg_seed(&k->rng, mbedtls_entropy_func, &k->ent, (const unsigned char *)"wa", 2) != 0 ||
        mbedtls_pk_setup(&k->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k->pk), mbedtls_ctr_drbg_random, &k->rng) != 0)
        return -1;
    uint8_t pt[65]; size_t pl = 0;
    mbedtls_ecp_keypair *kp = mbedtls_pk_ec(k->pk);
    mbedtls_ecp_group grp; mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    int rc = mbedtls_ecp_export(kp, &grp, NULL, &Q) ||
             mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &pl, pt, sizeof pt);
    mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&Q);
    if (rc || pl != 65) return -1;
    memcpy(k->x, pt + 1, 32); memcpy(k->y, pt + 33, 32);
    memset(k->id, id_byte, sizeof k->id);
    return 0;
}

static void wa_key_free(wa_key *k) {
    mbedtls_pk_free(&k->pk); mbedtls_ctr_drbg_free(&k->rng); mbedtls_entropy_free(&k->ent);
}

/* The COSE key: {1: 2, 3: -7, -1: 1, -2: x, -3: y}. */
static size_t wa_cose(const wa_key *k, uint8_t *b) {
    size_t o = cb_head(b, 5, 5);
    o += cb_int(b + o, 1);  o += cb_int(b + o, 2);
    o += cb_int(b + o, 3);  o += cb_int(b + o, -7);
    o += cb_int(b + o, -1); o += cb_int(b + o, 1);
    o += cb_int(b + o, -2); o += cb_bytes(b + o, k->x, 32, 2);
    o += cb_int(b + o, -3); o += cb_bytes(b + o, k->y, 32, 2);
    return o;
}

/* Authenticator data for `rp_id`: its hash, `flags`, the counter, and -- when
 * `attested` -- the AAGUID, the credential id and the key. */
static size_t wa_auth(const wa_key *k, const char *rp_id, uint8_t flags, int attested, uint8_t *b) {
    mbedtls_sha256((const unsigned char *)rp_id, strlen(rp_id), b, 0);
    b[32] = flags;
    b[33] = (uint8_t)(k->count >> 24); b[34] = (uint8_t)(k->count >> 16);
    b[35] = (uint8_t)(k->count >> 8);  b[36] = (uint8_t)k->count;
    size_t o = 37;
    if (attested) {
        memset(b + o, 0, 16); o += 16;
        b[o++] = 0; b[o++] = (uint8_t)sizeof k->id;
        memcpy(b + o, k->id, sizeof k->id); o += sizeof k->id;
        o += wa_cose(k, b + o);
    }
    return o;
}

/* clientDataJSON as a browser writes it. */
static size_t wa_client(char *b, size_t cap, const char *type, const char *challenge, const char *origin) {
    int n = snprintf(b, cap, "{\"type\":\"%s\",\"challenge\":\"%s\",\"origin\":\"%s\",\"crossOrigin\":false}",
                     type, challenge, origin);
    return n < 0 ? 0 : (size_t)n;
}

/* An attestation object, format "none". */
static size_t wa_attestation(const uint8_t *auth, size_t al, uint8_t *b) {
    size_t o = cb_head(b, 5, 3);
    o += cb_bytes(b + o, "fmt", 3, 3);      o += cb_bytes(b + o, "none", 4, 3);
    o += cb_bytes(b + o, "attStmt", 7, 3);  o += cb_head(b + o, 5, 0);
    o += cb_bytes(b + o, "authData", 8, 3); o += cb_bytes(b + o, auth, al, 2);
    return o;
}

/* The signature over authenticator data and the client data's hash, DER. */
static size_t wa_sign(wa_key *k, const uint8_t *auth, size_t al, const char *cd, size_t cdl, uint8_t *sig, size_t cap) {
    uint8_t cdh[32], h[32];
    mbedtls_sha256((const unsigned char *)cd, cdl, cdh, 0);
    mbedtls_sha256_context s;
    mbedtls_sha256_init(&s);
    mbedtls_sha256_starts(&s, 0); mbedtls_sha256_update(&s, auth, al); mbedtls_sha256_update(&s, cdh, 32);
    mbedtls_sha256_finish(&s, h);
    mbedtls_sha256_free(&s);
    size_t sl = 0;
    if (mbedtls_pk_sign(&k->pk, MBEDTLS_MD_SHA256, h, 32, sig, cap, &sl, mbedtls_ctr_drbg_random, &k->rng) != 0) return 0;
    return sl;
}

#endif /* OPENCHIME_WA_FIXTURE_H */
