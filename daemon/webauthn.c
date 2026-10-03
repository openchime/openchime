/* Passkeys -- see webauthn.h. */

#include "webauthn.h"
#include "cbor.h"

#define JSMN_HEADER
#include "jsmn.h"

#include <mbedtls/bignum.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <string.h>

#define FLAG_UP 0x01
#define FLAG_AT 0x40

/* The token after the whole value at `i`. */
static int next_tok(const jsmntok_t *t, int i, int n) {
    int end = i + 1;
    if (t[i].type == JSMN_OBJECT || t[i].type == JSMN_ARRAY) {
        int kids = t[i].type == JSMN_OBJECT ? 2 * t[i].size : t[i].size;
        for (int k = 0; k < kids && end < n; k++) end = next_tok(t, end, n);
    }
    return end;
}

/* clientDataJSON says `type`, `challenge` and `origin`, each exactly, at its top. */
static int client_ok(const uint8_t *cd, size_t cdl, const char *type, const char *challenge, const char *origin) {
    jsmn_parser p;
    jsmntok_t t[64];
    jsmn_init(&p);
    int n = jsmn_parse(&p, (const char *)cd, cdl, t, 64);
    if (n < 1 || t[0].type != JSMN_OBJECT) return 0;
    int got = 0, i = 1;
    for (int k = 0; k < t[0].size && i + 1 < n; k++) {
        const char *key = (const char *)cd + t[i].start;
        int kl = t[i].end - t[i].start;
        const jsmntok_t *v = &t[i + 1];
        int bit = t[i].type != JSMN_STRING ? 0 :
                  (kl == 4 && !memcmp(key, "type", 4)) ? 1 :
                  (kl == 9 && !memcmp(key, "challenge", 9)) ? 2 :
                  (kl == 6 && !memcmp(key, "origin", 6)) ? 4 : 0;
        const char *want = bit == 1 ? type : bit == 2 ? challenge : origin;
        if (bit) {
            if ((got & bit) || v->type != JSMN_STRING || (size_t)(v->end - v->start) != strlen(want) ||
                memcmp(cd + v->start, want, strlen(want)) != 0) return 0;   /* twice is once too many */
            got |= bit;
        }
        i = next_tok(t, i + 1, n);
    }
    return got == 7;
}

/* The authenticator data's first 37 bytes: for `rp_id`, with `need` flags. */
static oc_wa_result auth_head(const uint8_t *a, size_t al, const char *rp_id, uint8_t need, uint32_t *count) {
    if (al < 37) return OC_WA_BAD;
    uint8_t h[32];
    if (mbedtls_sha256((const unsigned char *)rp_id, strlen(rp_id), h, 0) != 0) return OC_WA_BAD;
    if (memcmp(a, h, 32) != 0) return OC_WA_RP;
    if ((a[32] & need) != need) return OC_WA_FLAGS;
    *count = ((uint32_t)a[33] << 24) | ((uint32_t)a[34] << 16) | ((uint32_t)a[35] << 8) | a[36];
    return OC_WA_OK;
}

/* A COSE key this takes, as a public key: EC2 P-256 for ES256, or RSA for RS256. */
static oc_wa_result cose_pk(const uint8_t *cose, size_t cl, mbedtls_pk_context *pk) {
    oc_cbor c = { cose, cose + cl }, v;
    int64_t kty = 0, alg = 0;
    if (oc_cbor_map_get(c, NULL, 1, &v) != 1 || oc_cbor_int(&v, &kty) != 0) return OC_WA_KEY;
    if (oc_cbor_map_get(c, NULL, 3, &v) != 1 || oc_cbor_int(&v, &alg) != 0) return OC_WA_KEY;
    if (kty == 2 && alg == -7) {
        int64_t crv = 0;
        const uint8_t *x, *y; size_t xl, yl;
        oc_cbor vx, vy;
        if (oc_cbor_map_get(c, NULL, -1, &v) != 1 || oc_cbor_int(&v, &crv) != 0 || crv != 1) return OC_WA_KEY;
        if (oc_cbor_map_get(c, NULL, -2, &vx) != 1 || oc_cbor_string(&vx, OC_CBOR_BYTES, &x, &xl) != 0 || xl != 32 ||
            oc_cbor_map_get(c, NULL, -3, &vy) != 1 || oc_cbor_string(&vy, OC_CBOR_BYTES, &y, &yl) != 0 || yl != 32)
            return OC_WA_KEY;
        /* SubjectPublicKeyInfo for an uncompressed P-256 point. */
        static const uint8_t SPKI[] = { 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                                        0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04 };
        uint8_t der[sizeof SPKI + 64];
        memcpy(der, SPKI, sizeof SPKI);
        memcpy(der + sizeof SPKI, x, 32);
        memcpy(der + sizeof SPKI + 32, y, 32);
        return mbedtls_pk_parse_public_key(pk, der, sizeof der) == 0 ? OC_WA_OK : OC_WA_KEY;
    }
    if (kty == 3 && alg == -257) {
        const uint8_t *n, *e; size_t nl, el;
        oc_cbor vn, ve;
        if (oc_cbor_map_get(c, NULL, -1, &vn) != 1 || oc_cbor_string(&vn, OC_CBOR_BYTES, &n, &nl) != 0 ||
            oc_cbor_map_get(c, NULL, -2, &ve) != 1 || oc_cbor_string(&ve, OC_CBOR_BYTES, &e, &el) != 0 ||
            nl < 256 || nl > 512 || el == 0 || el > 8) return OC_WA_KEY;
        if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) return OC_WA_KEY;
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
        if (mbedtls_rsa_import_raw(rsa, n, nl, NULL, 0, NULL, 0, NULL, 0, e, el) != 0 ||
            mbedtls_rsa_complete(rsa) != 0 || mbedtls_rsa_check_pubkey(rsa) != 0) return OC_WA_KEY;
        return OC_WA_OK;
    }
    return OC_WA_KEY;
}

oc_wa_result oc_webauthn_register(const uint8_t *cd, size_t cdl, const uint8_t *att, size_t al,
                                  const char *challenge, const char *origin, const char *rp_id,
                                  oc_wa_cred *out) {
    if (!cd || !att || !challenge || !origin || !rp_id || !out) return OC_WA_BAD;
    if (!client_ok(cd, cdl, "webauthn.create", challenge, origin)) return OC_WA_CLIENT;
    oc_cbor c = { att, att + al }, v;
    const uint8_t *fmt, *a; size_t fl, adl;
    if (oc_cbor_map_get(c, "fmt", 0, &v) != 1 || oc_cbor_string(&v, OC_CBOR_TEXT, &fmt, &fl) != 0) return OC_WA_BAD;
    if (fl != 4 || memcmp(fmt, "none", 4) != 0) return OC_WA_KEY;
    if (oc_cbor_map_get(c, "authData", 0, &v) != 1 || oc_cbor_string(&v, OC_CBOR_BYTES, &a, &adl) != 0) return OC_WA_BAD;
    uint32_t count = 0;
    oc_wa_result r = auth_head(a, adl, rp_id, FLAG_UP | FLAG_AT, &count);
    if (r != OC_WA_OK) return r;
    /* Attested credential data: AAGUID, the id's length and the id, the key. */
    if (adl < 37 + 16 + 2) return OC_WA_BAD;
    size_t idl = ((size_t)a[53] << 8) | a[54];
    if (idl == 0 || idl > OC_WA_CRED_MAX || adl < 55 + idl) return OC_WA_BAD;
    oc_cbor k = { a + 55 + idl, a + adl };
    const uint8_t *ks = k.p;
    if (oc_cbor_skip(&k) != 0) return OC_WA_BAD;
    size_t kl = (size_t)(k.p - ks);
    if (kl > OC_WA_COSE_MAX) return OC_WA_KEY;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    r = cose_pk(ks, kl, &pk);
    mbedtls_pk_free(&pk);
    if (r != OC_WA_OK) return r;
    memcpy(out->cred_id, a + 55, idl);
    out->cred_len = idl;
    memcpy(out->cose, ks, kl);
    out->cose_len = kl;
    out->count = count;
    return OC_WA_OK;
}

oc_wa_result oc_webauthn_assert(const uint8_t *cd, size_t cdl, const uint8_t *auth, size_t adl,
                                const uint8_t *sig, size_t sl, const uint8_t *cose, size_t cosel,
                                uint32_t stored, const char *challenge, const char *origin,
                                const char *rp_id, uint32_t *count) {
    if (!cd || !auth || !sig || !cose || !challenge || !origin || !rp_id) return OC_WA_BAD;
    if (!client_ok(cd, cdl, "webauthn.get", challenge, origin)) return OC_WA_CLIENT;
    uint32_t n = 0;
    oc_wa_result r = auth_head(auth, adl, rp_id, FLAG_UP, &n);
    if (r != OC_WA_OK) return r;
    /* What was signed: the authenticator data, then the client data's hash. */
    uint8_t cdh[32], h[32];
    if (mbedtls_sha256(cd, cdl, cdh, 0) != 0) return OC_WA_BAD;
    mbedtls_sha256_context s;
    mbedtls_sha256_init(&s);
    int bad = mbedtls_sha256_starts(&s, 0) || mbedtls_sha256_update(&s, auth, adl) ||
              mbedtls_sha256_update(&s, cdh, sizeof cdh) || mbedtls_sha256_finish(&s, h);
    mbedtls_sha256_free(&s);
    if (bad) return OC_WA_BAD;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    r = cose_pk(cose, cosel, &pk);
    if (r == OC_WA_OK && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof h, sig, sl) != 0) r = OC_WA_SIG;
    mbedtls_pk_free(&pk);
    if (r != OC_WA_OK) return r;
    /* Zero means the authenticator keeps no counter; otherwise it only rises. */
    if ((n || stored) && n <= stored) return OC_WA_COUNTER;
    if (count) *count = n;
    return OC_WA_OK;
}
