/* Unit tests for the auth crypto primitives (daemon/auth.c): PBKDF2 password
 * derivation (determinism + salt/password/iteration sensitivity), SHA-256 for
 * token storage, cryptographic randomness, and constant-time compare. Pure
 * functions, no DB or network. */

#include "auth.h"
#include "check.h"

#include <string.h>

static void test_pw_derive(void) {
    const uint8_t salt[OC_PW_SALT_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                           9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t h1[OC_PW_HASH_LEN], h2[OC_PW_HASH_LEN], h3[OC_PW_HASH_LEN];

    /* Deterministic: same inputs -> same derived key. */
    CHECK(oc_pw_derive("hunter2", 7, salt, sizeof salt, 4096, h1) == 0);
    CHECK(oc_pw_derive("hunter2", 7, salt, sizeof salt, 4096, h2) == 0);
    CHECK(memcmp(h1, h2, OC_PW_HASH_LEN) == 0);

    /* A different password yields a different key. */
    CHECK(oc_pw_derive("hunter3", 7, salt, sizeof salt, 4096, h3) == 0);
    CHECK(memcmp(h1, h3, OC_PW_HASH_LEN) != 0);

    /* A different salt yields a different key. */
    uint8_t salt2[OC_PW_SALT_LEN];
    memcpy(salt2, salt, sizeof salt2);
    salt2[0] ^= 0xFF;
    CHECK(oc_pw_derive("hunter2", 7, salt2, sizeof salt2, 4096, h3) == 0);
    CHECK(memcmp(h1, h3, OC_PW_HASH_LEN) != 0);

    /* A different iteration count yields a different key. */
    CHECK(oc_pw_derive("hunter2", 7, salt, sizeof salt, 4097, h3) == 0);
    CHECK(memcmp(h1, h3, OC_PW_HASH_LEN) != 0);
}

static void test_sha256(void) {
    /* NIST known-answer: SHA-256("abc"). */
    static const uint8_t want[OC_SHA256_LEN] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    uint8_t out[OC_SHA256_LEN];
    CHECK(oc_sha256("abc", 3, out) == 0);
    CHECK(memcmp(out, want, OC_SHA256_LEN) == 0);
}

static void test_rand_bytes(void) {
    uint8_t a[32], b[32];
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    CHECK(oc_rand_bytes(a, sizeof a) == 0);
    CHECK(oc_rand_bytes(b, sizeof b) == 0);
    /* Two draws must differ (a fixed/zero RNG would be catastrophic here). */
    CHECK(memcmp(a, b, sizeof a) != 0);
}

static void test_ct_eq(void) {
    uint8_t x[16], y[16];
    memset(x, 0x5A, sizeof x); memset(y, 0x5A, sizeof y);
    CHECK(oc_ct_eq(x, y, sizeof x) == 1);
    y[15] ^= 0x01;
    CHECK(oc_ct_eq(x, y, sizeof x) == 0);
}

int run_auth_tests(void) {
    printf("test_auth: PBKDF2 derive (determinism + sensitivity), SHA-256 KAT, randomness, constant-time compare\n");
    test_pw_derive();
    test_sha256();
    test_rand_bytes();
    test_ct_eq();
    return failures;
}
