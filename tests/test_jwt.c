/* Unit tests for ES256 JWT verification (daemon/jwt.c + vendored jsmn).
 *
 * A test issuer stands in for the central service (AUTH.md §3.6): the test
 * generates an ECDSA P-256 keypair, mints central-style ES256 JWTs, and pins
 * the daemon to the test public key — the same faking approach the TLS/netloop
 * integration tests use. Covers the happy path plus the rejections the pin is
 * meant to guarantee: wrong key, tampering, wrong audience/issuer, expiry, and
 * a non-ES256 alg. base64url encoding + DER->raw signature conversion live here
 * (the daemon only ever decodes/verifies). */

#include "jwt.h"
#include "check.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <string.h>

static const char *B64URL =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64URL[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

/* ASN.1 DER ECDSA sig (SEQUENCE{INTEGER r, INTEGER s}) -> raw r||s, 32+32. */
static void der_to_raw(const uint8_t *der, uint8_t raw[64]) {
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

/* Mint a JWT from `hdr_json` + `payload_json`, signed with `key`. Returns the
 * token length written to `out`. */
static size_t make_jwt(mbedtls_pk_context *key, mbedtls_ctr_drbg_context *drbg,
                       const char *hdr_json, const char *payload_json, char *out) {
    size_t o = 0;
    o += b64url_encode((const uint8_t *)hdr_json, strlen(hdr_json), out + o);
    out[o++] = '.';
    o += b64url_encode((const uint8_t *)payload_json, strlen(payload_json), out + o);

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)out, o, hash, 0);
    uint8_t der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_len = 0;
    int rc = mbedtls_pk_sign(key, MBEDTLS_MD_SHA256, hash, sizeof hash,
                             der, sizeof der, &der_len,
                             mbedtls_ctr_drbg_random, drbg);
    CHECK(rc == 0);
    uint8_t raw[64];
    der_to_raw(der, raw);

    out[o++] = '.';
    o += b64url_encode(raw, sizeof raw, out + o);
    return o;
}

static const char *ISS = "https://auth.openchime.io";
static const char *AUD = "acme.example";
static const uint64_t NOW = 1700000000ull;     /* fixed "now" for exp checks */

int run_jwt_tests(void) {
    printf("test_jwt: ES256 verify happy path + wrong-key/tamper/aud/iss/expiry/alg rejections, base64url\n");

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    CHECK(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)"oc-jwt-test", 11) == 0);

    /* The test issuer's keypair, and a second (attacker) key. */
    mbedtls_pk_context key, other;
    mbedtls_pk_init(&key);
    mbedtls_pk_init(&other);
    CHECK(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    CHECK(mbedtls_pk_setup(&other, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    CHECK(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                              mbedtls_ctr_drbg_random, &drbg) == 0);
    CHECK(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(other),
                              mbedtls_ctr_drbg_random, &drbg) == 0);

    char pem[512], pem_other[512];
    CHECK(mbedtls_pk_write_pubkey_pem(&key, (unsigned char *)pem, sizeof pem) == 0);
    CHECK(mbedtls_pk_write_pubkey_pem(&other, (unsigned char *)pem_other, sizeof pem_other) == 0);
    size_t pem_len = strlen(pem) + 1;            /* mbedTLS PEM parse wants the NUL */
    size_t pem_other_len = strlen(pem_other) + 1;

    const char *HDR = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    char payload[512];
    snprintf(payload, sizeof payload,
        "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"google|1234567890\","
        "\"email\":\"alice@acme.example\",\"name\":\"Alice\",\"iat\":%llu,\"exp\":%llu}",
        ISS, AUD, (unsigned long long)(NOW - 60), (unsigned long long)(NOW + 3600));

    char token[2048];
    size_t tlen = make_jwt(&key, &drbg, HDR, payload, token);

    /* Happy path: valid signature + matching iss/aud + unexpired. */
    oc_jwt_claims c;
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_OK);
    CHECK(strcmp(c.sub, "google|1234567890") == 0);
    CHECK(strcmp(c.iss, ISS) == 0);
    CHECK(strcmp(c.aud, AUD) == 0);
    CHECK(strcmp(c.email, "alice@acme.example") == 0);
    CHECK(strcmp(c.name, "Alice") == 0);
    CHECK(c.exp == NOW + 3600);

    /* Wrong pinned key -> signature rejection. */
    CHECK(oc_jwt_verify(token, tlen, pem_other, pem_other_len, ISS, AUD, NOW, &c)
          == OC_JWT_E_SIGNATURE);

    /* Tampered payload (flip one char in the payload segment) -> rejection. */
    {
        char bad[2048];
        memcpy(bad, token, tlen + 1);
        char *dot = strchr(bad, '.');
        bad[(dot - bad) + 3] ^= 0x01;    /* mutate a byte inside the payload */
        oc_jwt_result r = oc_jwt_verify(bad, tlen, pem, pem_len, ISS, AUD, NOW, &c);
        CHECK(r == OC_JWT_E_SIGNATURE || r == OC_JWT_E_FORMAT || r == OC_JWT_E_CLAIMS);
    }

    /* Wrong audience / issuer -> claims rejection (signature still valid). */
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, ISS, "other.example", NOW, &c)
          == OC_JWT_E_CLAIMS);
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, "https://evil", AUD, NOW, &c)
          == OC_JWT_E_CLAIMS);

    /* Expired token. */
    {
        char exp_payload[512];
        snprintf(exp_payload, sizeof exp_payload,
            "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"google|x\",\"exp\":%llu}",
            ISS, AUD, (unsigned long long)(NOW - 3600));
        char t2[2048];
        size_t l2 = make_jwt(&key, &drbg, HDR, exp_payload, t2);
        CHECK(oc_jwt_verify(t2, l2, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_EXPIRED);
    }

    /* Non-ES256 alg is refused before any signature work. */
    {
        char t3[2048];
        size_t l3 = make_jwt(&key, &drbg, "{\"alg\":\"none\"}", payload, t3);
        CHECK(oc_jwt_verify(t3, l3, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_ALG);
    }

    /* Malformed (not three segments). */
    CHECK(oc_jwt_verify("abc.def", 7, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_FORMAT);

    mbedtls_pk_free(&key);
    mbedtls_pk_free(&other);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return failures;
}
