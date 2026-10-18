/* Passkeys (AUTH.md §8.6): the CBOR reader at its edges, then registration and
 * assertion driven by an in-process authenticator -- and every way they refuse. */

#include "check.h"
#include "../daemon/cbor.h"
#include "../daemon/webauthn.h"
#include "wa_fixture.h"

#include <stdio.h>
#include <string.h>

static void test_cbor(void) {
    /* {"a": 1, -2: h'0102', 3: [true, "x"]} */
    static const uint8_t M[] = { 0xa3, 0x61, 'a', 0x01, 0x21, 0x42, 0x01, 0x02, 0x03, 0x82, 0xf5, 0x61, 'x' };
    oc_cbor c = { M, M + sizeof M }, v;
    int64_t i = 0;
    const uint8_t *s; size_t l;
    CHECK(oc_cbor_map_get(c, "a", 0, &v) == 1 && oc_cbor_int(&v, &i) == 0 && i == 1);
    CHECK(oc_cbor_map_get(c, NULL, -2, &v) == 1 && oc_cbor_string(&v, OC_CBOR_BYTES, &s, &l) == 0 && l == 2 && s[1] == 2);
    CHECK(oc_cbor_map_get(c, NULL, 3, &v) == 1 && oc_cbor_skip(&v) == 0 && v.p == M + sizeof M);
    CHECK(oc_cbor_map_get(c, "b", 0, &v) == 0);
    CHECK(oc_cbor_map_get(c, NULL, -2, &v) == 1 && oc_cbor_string(&v, OC_CBOR_TEXT, &s, &l) == -1);   /* wrong type */
    /* Truncated anywhere: refused, never read past. */
    for (size_t n = 0; n < sizeof M; n++) {
        oc_cbor t = { M, M + n };
        CHECK(oc_cbor_skip(&t) == -1);
    }
    /* Indefinite length, reserved forms, and nesting past the bound. */
    static const uint8_t INDEF[] = { 0x5f, 0x41, 0x00, 0xff }, RES[] = { 0x1c };
    oc_cbor t = { INDEF, INDEF + sizeof INDEF };
    CHECK(oc_cbor_skip(&t) == -1);
    t = (oc_cbor){ RES, RES + 1 };
    CHECK(oc_cbor_skip(&t) == -1);
    uint8_t deep[OC_CBOR_MAX_DEPTH + 3];
    for (size_t k = 0; k + 1 < sizeof deep; k++) deep[k] = 0x81;
    deep[sizeof deep - 1] = 0x00;
    t = (oc_cbor){ deep, deep + sizeof deep };
    CHECK(oc_cbor_skip(&t) == -1);
    /* A length longer than what is left. */
    static const uint8_t LONG[] = { 0x5a, 0xff, 0xff, 0xff, 0xff, 0x00 };
    t = (oc_cbor){ LONG, LONG + sizeof LONG };
    CHECK(oc_cbor_string(&t, OC_CBOR_BYTES, &s, &l) == -1);
}

static void test_register_assert(void) {
    wa_key k;
    CHECK(wa_key_new(&k, 0x11) == 0);
    const char *RP = "chat.acme.example", *ORIGIN = "https://chat.acme.example", *CH = "Y2hhbGxlbmdl";
    uint8_t auth[512], att[700], sig[128];
    char cd[512];
    size_t al = wa_auth(&k, RP, 0x41, 1, auth);
    size_t atl = wa_attestation(auth, al, att);
    size_t cdl = wa_client(cd, sizeof cd, "webauthn.create", CH, ORIGIN);
    oc_wa_cred cr;
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, CH, ORIGIN, RP, &cr) == OC_WA_OK);
    CHECK(cr.cred_len == 16 && cr.cred_id[0] == 0x11 && cr.count == 0);
    /* Refusals at registration. */
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, "other", ORIGIN, RP, &cr) == OC_WA_CLIENT);
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, CH, "https://evil.example", RP, &cr) == OC_WA_CLIENT);
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, CH, ORIGIN, "evil.example", &cr) == OC_WA_RP);
    size_t gl = wa_client(cd, sizeof cd, "webauthn.get", CH, ORIGIN);
    CHECK(oc_webauthn_register((uint8_t *)cd, gl, att, atl, CH, ORIGIN, RP, &cr) == OC_WA_CLIENT);
    cdl = wa_client(cd, sizeof cd, "webauthn.create", CH, ORIGIN);
    al = wa_auth(&k, RP, 0x40, 1, auth);                         /* no user presence */
    atl = wa_attestation(auth, al, att);
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, CH, ORIGIN, RP, &cr) == OC_WA_FLAGS);
    {   /* a client data that names the challenge twice is not one */
        char twice[512];
        int n = snprintf(twice, sizeof twice, "{\"type\":\"webauthn.create\",\"challenge\":\"%s\",\"challenge\":\"%s\","
                         "\"origin\":\"%s\"}", CH, CH, ORIGIN);
        al = wa_auth(&k, RP, 0x41, 1, auth);
        atl = wa_attestation(auth, al, att);
        CHECK(oc_webauthn_register((uint8_t *)twice, (size_t)n, att, atl, CH, ORIGIN, RP, &cr) == OC_WA_CLIENT);
    }
    al = wa_auth(&k, RP, 0x41, 1, auth);
    atl = wa_attestation(auth, al, att);
    CHECK(oc_webauthn_register((uint8_t *)cd, cdl, att, atl, CH, ORIGIN, RP, &cr) == OC_WA_OK);

    /* An assertion: right, then each way it is wrong. */
    k.count = 5;
    al = wa_auth(&k, RP, 0x05, 0, auth);
    cdl = wa_client(cd, sizeof cd, "webauthn.get", CH, ORIGIN);
    size_t sl = wa_sign(&k, auth, al, cd, cdl, sig, sizeof sig);
    uint32_t n = 0;
    CHECK(sl > 0);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, RP, &n) == OC_WA_OK && n == 5);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 5, CH, ORIGIN, RP, &n) == OC_WA_COUNTER);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, "x", ORIGIN, RP, &n) == OC_WA_CLIENT);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, "evil.example", &n) == OC_WA_RP);
    sig[sl - 1] ^= 1;
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, RP, &n) == OC_WA_SIG);
    sig[sl - 1] ^= 1;
    auth[36] ^= 1;                                               /* the data, changed after signing */
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, RP, &n) == OC_WA_SIG);
    auth[36] ^= 1;
    wa_key other;
    CHECK(wa_key_new(&other, 0x22) == 0);
    uint8_t ocose[200];
    size_t ocl = wa_cose(&other, ocose);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, ocose, ocl, 0, CH, ORIGIN, RP, &n) == OC_WA_SIG);
    k.count = 0;                                                 /* no counter kept: zero is fine, twice */
    al = wa_auth(&k, RP, 0x01, 0, auth);
    sl = wa_sign(&k, auth, al, cd, cdl, sig, sizeof sig);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, RP, &n) == OC_WA_OK);
    al = wa_auth(&k, RP, 0x00, 0, auth);                         /* no user presence */
    sl = wa_sign(&k, auth, al, cd, cdl, sig, sizeof sig);
    CHECK(oc_webauthn_assert((uint8_t *)cd, cdl, auth, al, sig, sl, cr.cose, cr.cose_len, 0, CH, ORIGIN, RP, &n) == OC_WA_FLAGS);
    wa_key_free(&other);
    wa_key_free(&k);
}

int run_webauthn_tests(void) {
    printf("test_webauthn: CBOR edges, passkey registration and assertion, each refusal\n");
    test_cbor();
    test_register_assert();
    return failures;
}
