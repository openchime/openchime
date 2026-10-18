/* A provider's ID token (AUTH.md §8.5): RS256, PS256 and ES256 tokens signed
 * here against a key set published here, and each way one is refused. */

#include "check.h"
#include "../daemon/idtoken.h"
#include "../daemon/jwt.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <string.h>

static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_rng;
static mbedtls_pk_context g_rsa, g_ec, g_other;
static char g_jwks[4096];

static void b64u(const uint8_t *in, size_t n, char *out) { oc_base64url_encode(in, n, out); }

static void make_keys(void) {
    mbedtls_entropy_init(&g_ent); mbedtls_ctr_drbg_init(&g_rng);
    mbedtls_ctr_drbg_seed(&g_rng, mbedtls_entropy_func, &g_ent, (const unsigned char *)"idt", 3);
    mbedtls_pk_init(&g_rsa); mbedtls_pk_init(&g_ec); mbedtls_pk_init(&g_other);
    mbedtls_pk_setup(&g_rsa, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_rsa), mbedtls_ctr_drbg_random, &g_rng, 2048, 65537);
    mbedtls_pk_setup(&g_other, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_other), mbedtls_ctr_drbg_random, &g_rng, 2048, 65537);
    mbedtls_pk_setup(&g_ec, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g_ec), mbedtls_ctr_drbg_random, &g_rng);
    uint8_t n[256], e[3] = { 1, 0, 1 }, pt[65]; size_t pl = 0;
    mbedtls_rsa_export_raw(mbedtls_pk_rsa(g_rsa), n, sizeof n, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    mbedtls_ecp_group grp; mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_export(mbedtls_pk_ec(g_ec), &grp, NULL, &Q);
    mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &pl, pt, sizeof pt);
    mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&Q);
    char nb[400], eb[8], xb[64], yb[64];
    b64u(n, sizeof n, nb); b64u(e, 3, eb); b64u(pt + 1, 32, xb); b64u(pt + 33, 32, yb);
    snprintf(g_jwks, sizeof g_jwks,
             "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"r1\",\"use\":\"sig\",\"n\":\"%s\",\"e\":\"%s\"},"
             "{\"kty\":\"EC\",\"kid\":\"e1\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}]}", nb, eb, xb, yb);
}

/* A token: header {alg, kid}, `payload`, signed by `pk` the way `alg` says. */
static void sign_token(const char *alg, const char *kid, const char *payload, mbedtls_pk_context *pk,
                       char *out, size_t cap) {
    char hdr[128], hb[200], pb[2048], in[2400];
    snprintf(hdr, sizeof hdr, "{\"alg\":\"%s\"%s%s%s,\"typ\":\"JWT\"}", alg, kid ? ",\"kid\":\"" : "", kid ? kid : "", kid ? "\"" : "");
    b64u((const uint8_t *)hdr, strlen(hdr), hb);
    b64u((const uint8_t *)payload, strlen(payload), pb);
    snprintf(in, sizeof in, "%s.%s", hb, pb);
    uint8_t h[32], sig[512]; size_t sl = 0;
    mbedtls_sha256((const unsigned char *)in, strlen(in), h, 0);
    if (!strcmp(alg, "ES256")) {
        char s64[OC_JWS_SIG64_LEN + 1];
        oc_jws_es256_sign(pk, mbedtls_ctr_drbg_random, &g_rng, in, strlen(in), s64);
        snprintf(out, cap, "%s.%s", in, s64);
        return;
    }
    if (!strcmp(alg, "PS256")) mbedtls_rsa_set_padding(mbedtls_pk_rsa(*pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, h, 32, sig, sizeof sig, &sl, mbedtls_ctr_drbg_random, &g_rng);
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(*pk), MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    char sb[700];
    b64u(sig, sl, sb);
    snprintf(out, cap, "%s.%s", in, sb);
}

#define ISS "https://idp.example"
#define CID "client-1"
#define NOW 1900000000ull

static oc_idt_result check(const char *alg, const char *kid, const char *payload, mbedtls_pk_context *pk) {
    char tok[3000];
    sign_token(alg, kid, payload, pk, tok, sizeof tok);
    oc_idt_claims c;
    return oc_idtoken_verify(tok, strlen(tok), g_jwks, strlen(g_jwks), ISS, CID, "n-1", NOW, &c);
}

int run_idtoken_tests(void) {
    printf("test_idtoken: RS256/PS256/ES256 from a key set; aud, azp, nonce, issuer, time, keys, algs\n");
    make_keys();
    const char *GOOD = "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s-1\",\"nonce\":\"n-1\","
                       "\"exp\":1900000300,\"iat\":1899999990,\"email\":\"kim@acme.example\",\"email_verified\":true,"
                       "\"hd\":\"acme.example\"}";
    char tok[3000];
    oc_idt_claims c;
    sign_token("RS256", "r1", GOOD, &g_rsa, tok, sizeof tok);
    CHECK(oc_idtoken_verify(tok, strlen(tok), g_jwks, strlen(g_jwks), ISS, CID, "n-1", NOW, &c) == OC_IDT_OK);
    CHECK(!strcmp(c.sub, "s-1") && c.email_verified && !strcmp(c.hd, "acme.example"));
    CHECK(check("PS256", "r1", GOOD, &g_rsa) == OC_IDT_OK);
    CHECK(check("ES256", "e1", GOOD, &g_ec) == OC_IDT_OK);
    /* Keys: the wrong one, an unknown kid, no kid where two keys could be meant. */
    CHECK(check("RS256", "r1", GOOD, &g_other) == OC_IDT_SIGNATURE);
    CHECK(check("RS256", "nope", GOOD, &g_rsa) == OC_IDT_KEY);
    CHECK(check("RS256", NULL, GOOD, &g_rsa) == OC_IDT_OK);          /* the set's one RSA key */
    CHECK(check("ES256", "r1", GOOD, &g_ec) == OC_IDT_KEY);          /* an RSA key for an EC alg */
    /* Algorithms off the list. */
    CHECK(check("none", "r1", GOOD, &g_rsa) == OC_IDT_ALG);
    CHECK(check("HS256", "r1", GOOD, &g_rsa) == OC_IDT_ALG);
    CHECK(check("RS512", "r1", GOOD, &g_rsa) == OC_IDT_ALG);
    /* Claims. */
    static const struct { const char *p; oc_idt_result want; } T[] = {
        { "{\"iss\":\"https://evil.example\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "/\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"other\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":[\"" CID "\"],\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_OK },
        { "{\"iss\":\"" ISS "\",\"aud\":[\"" CID "\",\"x\"],\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":[\"" CID "\",\"x\"],\"azp\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_OK },
        { "{\"iss\":\"" ISS "\",\"aud\":[\"" CID "\",\"x\"],\"azp\":\"x\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-2\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"nonce\":\"n-1\",\"exp\":1900000300}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\"}", OC_IDT_CLAIMS },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1899999939}", OC_IDT_TIME },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1899999941}", OC_IDT_OK },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300,\"nbf\":1900000061}", OC_IDT_TIME },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300,\"iat\":1900000061}", OC_IDT_TIME },
        { "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300,\"email\":7}", OC_IDT_CLAIMS },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        oc_idt_result got = check("RS256", "r1", T[i].p, &g_rsa);
        if (got != T[i].want) printf("  claims row %zu: got %d want %d\n", i, (int)got, (int)T[i].want);
        CHECK(got == T[i].want);
    }
    /* Not a token at all. */
    CHECK(oc_idtoken_verify("a.b", 3, g_jwks, strlen(g_jwks), ISS, CID, "n-1", NOW, &c) == OC_IDT_FORMAT);
    CHECK(oc_idtoken_verify("a.b.c.d", 7, g_jwks, strlen(g_jwks), ISS, CID, "n-1", NOW, &c) == OC_IDT_FORMAT);
    /* Microsoft's own claims come through. */
    sign_token("RS256", "r1", "{\"iss\":\"" ISS "\",\"aud\":\"" CID "\",\"sub\":\"s\",\"nonce\":\"n-1\",\"exp\":1900000300,"
               "\"oid\":\"o-1\",\"tid\":\"t-1\",\"xms_edov\":true}", &g_rsa, tok, sizeof tok);
    CHECK(oc_idtoken_verify(tok, strlen(tok), g_jwks, strlen(g_jwks), ISS, CID, "n-1", NOW, &c) == OC_IDT_OK &&
          !strcmp(c.oid, "o-1") && !strcmp(c.tid, "t-1") && c.xms_edov && !c.email_verified);
    mbedtls_pk_free(&g_rsa); mbedtls_pk_free(&g_ec); mbedtls_pk_free(&g_other);
    mbedtls_ctr_drbg_free(&g_rng); mbedtls_entropy_free(&g_ent);
    return failures;
}
