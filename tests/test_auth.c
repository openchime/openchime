/* Unit tests for the auth crypto primitives (daemon/auth.c): Argon2id password
 * derivation (determinism + salt/password/memory sensitivity) and its stored
 * settings, the password policy (daemon/pwpolicy.c), SHA-256 for
 * token storage, cryptographic randomness, and constant-time compare. Pure
 * functions, no DB or network. */

#include "auth.h"
#include "pwpolicy.h"
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

    /* A different memory cost yields a different key. */
    CHECK(oc_pw_derive("hunter2", 7, salt, sizeof salt, 4097, h3) == 0);
    CHECK(memcmp(h1, h3, OC_PW_HASH_LEN) != 0);
    /* Below Argon2's floor of 8 KiB a lane: refused, not clamped. */
    CHECK(oc_pw_derive("hunter2", 7, salt, sizeof salt, 7, h3) != 0);

    /* Known answer: Argon2id v19, t=2, m=64 MiB, p=1, as an independent
     * implementation (Zig's std.crypto.pwhash.argon2) computes it. */
    static const uint8_t kat[32] = {
        0x09,0x31,0x61,0x15,0xd5,0xcf,0x24,0xed,0x5a,0x15,0xa3,0x1a,0x3b,0xa3,0x26,0xe5,
        0xcf,0x32,0xed,0xc2,0x47,0x02,0x98,0x7c,0x02,0xb6,0x56,0x6f,0x61,0x91,0x3c,0xf7 };
    CHECK(oc_pw_derive("password", 8, (const uint8_t *)"somesalt", 8, 65536, h3) == 0);
    CHECK(memcmp(h3, kat, sizeof kat) == 0);
}

/* The settings stored beside each hash: what this daemon makes, read back, and
 * nothing else read at all. */
static void test_pw_kdf(void) {
    char k[OC_PW_KDF_MAX];
    uint32_t m = 0;
    oc_pw_kdf_format(OC_PW_MEM_KIB, k);
    CHECK(strcmp(k, "argon2id$v=19$m=19456,t=2,p=1") == 0);
    CHECK(oc_pw_kdf_parse(k, &m) == 0 && m == OC_PW_MEM_KIB);
    CHECK(oc_pw_kdf_parse("argon2id$v=19$m=2048,t=2,p=1", &m) == 0 && m == 2048);
    CHECK(oc_pw_kdf_parse("", &m) != 0);                                  /* a PBKDF2 row's */
    CHECK(oc_pw_kdf_parse(NULL, &m) != 0);
    CHECK(oc_pw_kdf_parse("argon2i$v=19$m=19456,t=2,p=1", &m) != 0);
    CHECK(oc_pw_kdf_parse("argon2id$v=16$m=19456,t=2,p=1", &m) != 0);
    CHECK(oc_pw_kdf_parse("argon2id$v=19$m=19456,t=3,p=1", &m) != 0);   /* another daemon's passes */
    CHECK(oc_pw_kdf_parse("argon2id$v=19$m=19456,t=2,p=1x", &m) != 0);
    CHECK(oc_pw_kdf_parse("argon2id$v=19$m=4,t=2,p=1", &m) != 0);
}

/* What a new password must be (pwpolicy.h). */
static void test_pw_policy(void) {
    char out[OC_PW_NORM_MAX];
    const char *why = NULL;
    /* 15 characters, counted after normalization, not bytes. */
    CHECK(oc_pw_acceptable("short", "pat", "Acme", out, sizeof out, &why) != 0 && strstr(why, "15"));
    CHECK(oc_pw_acceptable("fourteen-chars", "pat", "Acme", out, sizeof out, &why) != 0);
    CHECK(oc_pw_acceptable("fifteen-chars-x", "pat", "Acme", out, sizeof out, &why) == 0);
    CHECK(oc_pw_acceptable("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                           "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9q", NULL, NULL, out, sizeof out, &why) == 0);
    /* Spaces are fine; 129 characters are not. */
    CHECK(oc_pw_acceptable("a long phrase of plain words", "pat", "Acme", out, sizeof out, &why) == 0);
    char big[200];
    memset(big, 'k', 129); big[0] = 'q'; big[129] = '\0';
    CHECK(oc_pw_acceptable(big, NULL, NULL, out, sizeof out, &why) != 0 && strstr(why, "128"));
    big[128] = '\0';
    CHECK(oc_pw_acceptable(big, NULL, NULL, out, sizeof out, &why) == 0);
    /* NFKC: a full-width or ligature form is its plain one, at sign-up and at
     * sign-in alike. */
    CHECK(oc_pw_normalize("\xEF\xAC\x81", out, sizeof out) == 0 && strcmp(out, "fi") == 0);
    CHECK(oc_pw_normalize("e\xCC\x81", out, sizeof out) == 0 && strcmp(out, "\xC3\xA9") == 0);
    CHECK(oc_pw_normalize("bad\xFF" "byte", out, sizeof out) != 0);
    CHECK(oc_pw_normalize("tab\there", out, sizeof out) != 0);
    /* Common, repeated, or built on a name. */
    CHECK(oc_pw_acceptable("000000000000000", NULL, NULL, out, sizeof out, &why) != 0 && strstr(why, "common"));
    CHECK(oc_pw_acceptable("1qaz2wsx3edc4rfv", NULL, NULL, out, sizeof out, &why) != 0);
    CHECK(oc_pw_acceptable("1QAZ2WSX3EDC4RFV", NULL, NULL, out, sizeof out, &why) != 0);  /* any case */
    CHECK(oc_pw_acceptable("zzzzzzzzzzzzzzzzzz", NULL, NULL, out, sizeof out, &why) != 0);
    CHECK(oc_pw_acceptable("patricia-in-the-garden", "Patricia", "Acme", out, sizeof out, &why) != 0 && strstr(why, "username"));
    CHECK(oc_pw_acceptable("welcome to acmecorp friends", "pat", "AcmeCorp", out, sizeof out, &why) != 0);
    CHECK(oc_pw_acceptable("my openchime password", "pat", "Acme", out, sizeof out, &why) != 0);
    CHECK(oc_pw_acceptable("a pat on the back for everyone", "pat", "Acme", out, sizeof out, &why) == 0);  /* a short name is no word */
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
    printf("test_auth: Argon2id derive (determinism + sensitivity) and settings, the password policy, SHA-256 KAT, randomness, constant-time compare\n");
    test_pw_derive();
    test_pw_kdf();
    test_pw_policy();
    test_sha256();
    test_rand_bytes();
    test_ct_eq();
    return failures;
}
