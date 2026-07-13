/* Shared test issuer (AUTH.md §3.6): an ECDSA P-256 keypair that mints
 * central-style ES256 JWTs and exposes its public key as PEM. Used by the JWT
 * unit test (crypto) and the dbwriter OIDC test (wiring) — the same faking
 * approach as the TLS/netloop integration fakes. Header-only, static inline. */

#ifndef OC_TEST_ISSUER_H
#define OC_TEST_ISSUER_H

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <string.h>

static const char OC_ISSUER_B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static inline size_t oc_b64url_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = OC_ISSUER_B64URL[(v >> 18) & 63];
        out[o++] = OC_ISSUER_B64URL[(v >> 12) & 63];
        if (rem > 1) out[o++] = OC_ISSUER_B64URL[(v >> 6) & 63];
        if (rem > 2) out[o++] = OC_ISSUER_B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

/* ASN.1 DER ECDSA sig (SEQUENCE{INTEGER r, INTEGER s}) -> raw r||s (32+32). */
static inline void oc_der_to_raw(const uint8_t *der, uint8_t raw[64]) {
    memset(raw, 0, 64);
    size_t i = 2;                       /* skip SEQUENCE tag + (single-byte) len */
    size_t rlen = der[i + 1];
    const uint8_t *r = der + i + 2;
    size_t rs = 0; while (rs < rlen && r[rs] == 0) rs++;
    memcpy(raw + (32 - (rlen - rs)), r + rs, rlen - rs);
    i = i + 2 + rlen;
    size_t slen = der[i + 1];
    const uint8_t *s = der + i + 2;
    size_t ss = 0; while (ss < slen && s[ss] == 0) ss++;
    memcpy(raw + 32 + (32 - (slen - ss)), s + ss, slen - ss);
}

typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       key;
    char                     pem[512];   /* public key, NUL-terminated */
} oc_issuer;

/* Returns 0 on success. `seed` personalizes the DRBG (any short string). */
static inline int oc_issuer_init(oc_issuer *is, const char *seed) {
    mbedtls_entropy_init(&is->entropy);
    mbedtls_ctr_drbg_init(&is->drbg);
    mbedtls_pk_init(&is->key);
    if (mbedtls_ctr_drbg_seed(&is->drbg, mbedtls_entropy_func, &is->entropy,
                              (const unsigned char *)seed, strlen(seed)) != 0) return -1;
    if (mbedtls_pk_setup(&is->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return -1;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(is->key),
                            mbedtls_ctr_drbg_random, &is->drbg) != 0) return -1;
    if (mbedtls_pk_write_pubkey_pem(&is->key, (unsigned char *)is->pem, sizeof is->pem) != 0)
        return -1;
    return 0;
}

static inline void oc_issuer_free(oc_issuer *is) {
    mbedtls_pk_free(&is->key);
    mbedtls_ctr_drbg_free(&is->drbg);
    mbedtls_entropy_free(&is->entropy);
}

/* Mint a JWT from `hdr_json` + `payload_json` signed by the issuer; writes the
 * compact token to `out` (NUL-terminated) and returns its length. */
static inline size_t oc_issuer_mint(oc_issuer *is, const char *hdr_json,
                                    const char *payload_json, char *out) {
    size_t o = 0;
    o += oc_b64url_encode((const uint8_t *)hdr_json, strlen(hdr_json), out + o);
    out[o++] = '.';
    o += oc_b64url_encode((const uint8_t *)payload_json, strlen(payload_json), out + o);

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)out, o, hash, 0);
    uint8_t der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_len = 0;
    if (mbedtls_pk_sign(&is->key, MBEDTLS_MD_SHA256, hash, sizeof hash,
                        der, sizeof der, &der_len,
                        mbedtls_ctr_drbg_random, &is->drbg) != 0) {
        out[o] = '\0';
        return o;   /* leaves a token with no valid signature; caller's CHECK fails */
    }
    uint8_t raw[64];
    oc_der_to_raw(der, raw);
    out[o++] = '.';
    o += oc_b64url_encode(raw, sizeof raw, out + o);
    return o;
}

#endif /* OC_TEST_ISSUER_H */
