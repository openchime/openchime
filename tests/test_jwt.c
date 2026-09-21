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

/* A key and its RFC 7638 thumbprint as ANOTHER implementation computed it (the
 * central service's .NET signer), so the two sides are checked against each
 * other rather than this one against itself. */
static const char VECTOR_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEDqHKx+q7OAQmDqj5y9NaFRRyMFoH\n"
    "tdCjHSfxAGUIA9+4UU024HTVfwxlAyEJ/AZuJbyJG5DrtbEcEXSVdX1U1A==\n"
    "-----END PUBLIC KEY-----\n";
static const char VECTOR_KID[] = "lQcjs5PMWtfx7_V-dHADA3euhG-Jiopb92C5hPrTmYM";

static oc_jwt_result verify_payload(oc_issuer *is, const char *pems, const char *payload,
                                    oc_jwt_claims *c) {
    char hdr[160], token[4096];
    oc_issuer_header(is, hdr, sizeof hdr);
    size_t tlen = oc_issuer_mint(is, hdr, payload, token);
    return oc_jwt_verify(token, tlen, pems, strlen(pems) + 1, ISS, AUD, NOW, c);
}

int run_jwt_tests(void) {
    printf("test_jwt: ES256 verify to the contract — key set by kid, required claims, "
           "lifetime, unescape, over-long refused, nonce, base64url\n");

    /* The test issuer's keypair, and a second (attacker) issuer. */
    oc_issuer iss, other;
    CHECK(oc_issuer_init(&iss, "oc-jwt-test") == 0);
    CHECK(oc_issuer_init(&other, "oc-jwt-attacker") == 0);
    const char *pem = iss.pem, *pem_other = other.pem;
    size_t pem_len = strlen(pem) + 1;            /* mbedTLS PEM parse wants the NUL */
    size_t pem_other_len = strlen(pem_other) + 1;

    char HDR[160];
    oc_issuer_header(&iss, HDR, sizeof HDR);
    char payload[1024];
    oc_issuer_payload(payload, sizeof payload, ISS, AUD, "https://accounts.google.com|1234567890",
                      "jti-1", NOW - 10, NOW + 290,
                      "\"email\":\"alice@acme.example\",\"email_verified\":true,"
                      "\"name\":\"Alice\",\"idp\":\"google\",\"tenant\":\"acme.example\"");

    char token[4096];
    size_t tlen = oc_issuer_mint(&iss, HDR, payload, token);

    /* Happy path: every claim of the contract comes back. */
    oc_jwt_claims c;
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_OK);
    CHECK(strcmp(c.sub, "https://accounts.google.com|1234567890") == 0);
    CHECK(strcmp(c.iss, ISS) == 0);
    CHECK(strcmp(c.aud, AUD) == 0);
    CHECK(strcmp(c.email, "alice@acme.example") == 0);
    CHECK(c.email_verified == 1);
    CHECK(strcmp(c.name, "Alice") == 0);
    CHECK(strcmp(c.idp, "google") == 0);
    CHECK(strcmp(c.tenant, "acme.example") == 0);
    CHECK(strcmp(c.jti, "jti-1") == 0);
    CHECK(strlen(c.nonce) == 43);
    CHECK(c.exp == NOW + 290 && c.iat == NOW - 10 && c.nbf == NOW - 10);

    /* The thumbprint agrees with another implementation's. */
    {
        char tp[OC_JWT_THUMBPRINT_LEN + 1];
        CHECK(oc_jwt_key_thumbprint(VECTOR_PEM, sizeof VECTOR_PEM, tp) == 0);
        CHECK(strcmp(tp, VECTOR_KID) == 0);
        CHECK(oc_jwt_key_thumbprint("not a key", 10, tp) == -1);
    }

    /* A key SET: the kid picks the right key wherever it sits, and a block that
     * is not a key at all does not lock out the good one beside it. */
    {
        char set[2048];
        snprintf(set, sizeof set, "%s%s-----BEGIN PUBLIC KEY-----\nAAAA\n-----END PUBLIC KEY-----\n%s",
                 pem_other, VECTOR_PEM, pem);
        CHECK(oc_jwt_verify(token, tlen, set, strlen(set) + 1, ISS, AUD, NOW, &c) == OC_JWT_OK);
        snprintf(set, sizeof set, "%s%s", pem, pem_other);
        CHECK(oc_jwt_verify(token, tlen, set, strlen(set) + 1, ISS, AUD, NOW, &c) == OC_JWT_OK);
    }

    /* Signed by a key that is not pinned: its kid names nothing here. */
    CHECK(oc_jwt_verify(token, tlen, pem_other, pem_other_len, ISS, AUD, NOW, &c) == OC_JWT_E_KEY);

    /* A pinned key's kid on another key's signature: the signature decides. */
    {
        char t2[4096];
        size_t l2 = oc_issuer_mint(&other, HDR, payload, t2);
        CHECK(oc_jwt_verify(t2, l2, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_SIGNATURE);
    }

    /* No kid at all is refused, even with one key pinned. */
    {
        char t2[4096];
        size_t l2 = oc_issuer_mint(&iss, "{\"alg\":\"ES256\",\"typ\":\"JWT\"}", payload, t2);
        CHECK(oc_jwt_verify(t2, l2, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_KEY);
    }

    /* Tamper with the payload (flip one char): the signature no longer verifies. */
    {
        char bad[4096];
        memcpy(bad, token, tlen + 1);
        const char *d1 = strchr(bad, '.');
        bad[(d1 - bad) + 5] = (bad[(d1 - bad) + 5] == 'A') ? 'B' : 'A';
        oc_jwt_result r = oc_jwt_verify(bad, tlen, pem, pem_len, ISS, AUD, NOW, &c);
        CHECK(r == OC_JWT_E_SIGNATURE || r == OC_JWT_E_FORMAT);
    }

    /* Wrong audience / issuer. */
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, ISS, "other.example", NOW, &c)
          == OC_JWT_E_CLAIMS);
    CHECK(oc_jwt_verify(token, tlen, pem, pem_len, "https://evil", AUD, NOW, &c)
          == OC_JWT_E_CLAIMS);

    /* Each required claim, missing, refuses the token. */
    {
        static const char *const MISSING[] = {
            /* no exp */  "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"jti\":\"j\",\"nonce\":\"n\",\"iat\":1699999990}",
            /* no iat */  "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"jti\":\"j\",\"nonce\":\"n\",\"exp\":1700000290}",
            /* no sub */  "{\"iss\":\"%s\",\"aud\":\"%s\",\"jti\":\"j\",\"nonce\":\"n\",\"iat\":1699999990,\"exp\":1700000290}",
            /* no jti */  "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"nonce\":\"n\",\"iat\":1699999990,\"exp\":1700000290}",
            /* no nonce */"{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"jti\":\"j\",\"iat\":1699999990,\"exp\":1700000290}",
            /* empty sub */"{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"\",\"jti\":\"j\",\"nonce\":\"n\",\"iat\":1699999990,\"exp\":1700000290}",
            /* exp a string */"{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"s\",\"jti\":\"j\",\"nonce\":\"n\",\"iat\":1699999990,\"exp\":\"1700000290\"}",
        };
        for (size_t i = 0; i < sizeof MISSING / sizeof MISSING[0]; i++) {
            char pl[512];
            snprintf(pl, sizeof pl, MISSING[i], ISS, AUD);
            CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
        }
    }

    /* Expired; not yet valid; issued in the future; a life over the ceiling. */
    {
        char pl[1024];
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 3600, NOW - 3400, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_EXPIRED);
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW + 600, NOW + 800, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_EXPIRED);
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 3600, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
    }

    /* Claims are JSON-unescaped: an escaped and a literal spelling of one subject
     * are one subject, and a name keeps its quotes and its accents. */
    {
        char pl[1024];
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "https:\\/\\/idp.example|\\u0061bc", "j", NOW - 10,
                          NOW + 290, "\"name\":\"Zo\\u00eb \\\"Z\\\" \\ud83d\\ude00\"");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
        CHECK(strcmp(c.sub, "https://idp.example|abc") == 0);
        CHECK(strcmp(c.name, "Zo\xc3\xab \"Z\" \xf0\x9f\x98\x80") == 0);
        /* A lone surrogate, an unknown escape and an embedded NUL are refused. */
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "a\\ud83dz", "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "a\\qz", "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) != OC_JWT_OK);   /* the parser's or ours */
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "a\\u0000z", "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
    }

    /* An over-long claim is refused, never truncated: 255 bytes fit, 256 do not. */
    {
        char sub[300], pl[1024];
        memset(sub, 'a', 255); sub[255] = '\0';
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, sub, "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
        CHECK(strlen(c.sub) == 255);
        memset(sub, 'a', 256); sub[256] = '\0';
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, sub, "j", NOW - 10, NOW + 290, "");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
        /* An optional claim too: a long name does not quietly become a short one. */
        char extra[400] = "\"name\":\"";
        memset(extra + 8, 'n', 300); strcpy(extra + 308, "\"");
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 290, extra);
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_E_CLAIMS);
    }

    /* email_verified is true only for the JSON literal. */
    {
        char pl[1024];
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 290,
                          "\"email\":\"a@b.example\",\"email_verified\":\"true\"");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
        CHECK(c.email_verified == 0);
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 290,
                          "\"email\":\"a@b.example\"");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
        CHECK(c.email_verified == 0 && c.tenant[0] == '\0' && c.idp[0] == '\0');
    }

    /* A provider's extra claims, nested ones included, fit and are skipped. */
    {
        char pl[2048];
        oc_issuer_payload(pl, sizeof pl, ISS, AUD, "s", "j", NOW - 10, NOW + 290,
            "\"amr\":[\"pwd\",\"mfa\",\"hwk\"],\"x\":{\"sub\":\"inner\",\"a\":[1,2,3,4,5,6,7,8]},"
            "\"c1\":1,\"c2\":2,\"c3\":3,\"c4\":4,\"c5\":5,\"c6\":6,\"c7\":7,\"c8\":8,\"c9\":9,"
            "\"d1\":\"v\",\"d2\":\"v\",\"d3\":\"v\",\"d4\":\"v\",\"d5\":\"v\",\"d6\":\"v\",\"d7\":\"v\"");
        CHECK(verify_payload(&iss, pem, pl, &c) == OC_JWT_OK);
        CHECK(strcmp(c.sub, "s") == 0);   /* the nested "sub" is not the subject */
    }

    /* The nonce is the hash of the verifier the client kept (RFC 7636's shape). */
    {
        const char *v = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";   /* RFC 7636 appendix B */
        CHECK(oc_jwt_nonce_matches("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
                                   (const uint8_t *)v, strlen(v)) == 1);
        CHECK(oc_jwt_nonce_matches("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cN",
                                   (const uint8_t *)v, strlen(v)) == 0);
        CHECK(oc_jwt_nonce_matches("short", (const uint8_t *)v, strlen(v)) == 0);
        CHECK(oc_jwt_nonce_matches(NULL, (const uint8_t *)v, strlen(v)) == 0);
    }

    /* Non-ES256 alg is refused before any signature work. */
    {
        char t3[4096];
        size_t l3 = oc_issuer_mint(&iss, "{\"alg\":\"none\"}", payload, t3);
        CHECK(oc_jwt_verify(t3, l3, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_ALG);
    }

    /* Malformed (not three segments). */
    CHECK(oc_jwt_verify("abc.def", 7, pem, pem_len, ISS, AUD, NOW, &c) == OC_JWT_E_FORMAT);

    oc_issuer_free(&iss);
    oc_issuer_free(&other);
    return failures;
}
