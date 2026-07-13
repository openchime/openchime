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
#include "issuer.h"
#include "check.h"

#include <string.h>

static const char *ISS = "https://auth.openchime.io";
static const char *AUD = "acme.example";
static const uint64_t NOW = 1700000000ull;     /* fixed "now" for exp checks */

int run_jwt_tests(void) {
    printf("test_jwt: ES256 verify happy path + wrong-key/tamper/aud/iss/expiry/alg rejections, base64url\n");

    /* The test issuer's keypair, and a second (attacker) issuer. */
    oc_issuer iss, other;
    CHECK(oc_issuer_init(&iss, "oc-jwt-test") == 0);
    CHECK(oc_issuer_init(&other, "oc-jwt-attacker") == 0);
    const char *pem = iss.pem, *pem_other = other.pem;
    size_t pem_len = strlen(pem) + 1;            /* mbedTLS PEM parse wants the NUL */
    size_t pem_other_len = strlen(pem_other) + 1;

    const char *HDR = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    char payload[512];
    snprintf(payload, sizeof payload,
        "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"google|1234567890\","
        "\"email\":\"alice@acme.example\",\"name\":\"Alice\",\"iat\":%llu,\"exp\":%llu}",
        ISS, AUD, (unsigned long long)(NOW - 60), (unsigned long long)(NOW + 3600));

    char token[2048];
    size_t tlen = oc_issuer_mint(&iss, HDR, payload, token);

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
        size_t l2 = oc_issuer_mint(&iss, HDR, exp_payload, t2);
        CHECK(oc_jwt_verify(t2, l2, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_EXPIRED);
    }

    /* Non-ES256 alg is refused before any signature work. */
    {
        char t3[2048];
        size_t l3 = oc_issuer_mint(&iss, "{\"alg\":\"none\"}", payload, t3);
        CHECK(oc_jwt_verify(t3, l3, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_ALG);
    }

    /* Malformed (not three segments). */
    CHECK(oc_jwt_verify("abc.def", 7, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_FORMAT);

    oc_issuer_free(&iss);
    oc_issuer_free(&other);
    return failures;
}
