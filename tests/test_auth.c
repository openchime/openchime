/* Unit tests for the auth crypto primitives (daemon/auth.c): Argon2id password
 * derivation (determinism + salt/password/memory sensitivity) and its stored
 * settings, the password policy (daemon/pwpolicy.c), SHA-256 for
 * token storage, cryptographic randomness, and constant-time compare. Pure
 * functions, no DB or network. */

#include "auth.h"
#include "pwpolicy.h"
#include "webpages.h"
#include "check.h"

#include <stdlib.h>
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
    /* 15 characters, counted after normalization, not bytes. */
    CHECK(oc_pw_check("short", "pat", "Acme", out, sizeof out) == OC_PW_TOO_SHORT);
    CHECK(oc_pw_check("fourteen-chars", "pat", "Acme", out, sizeof out) == OC_PW_TOO_SHORT);
    CHECK(oc_pw_check("fifteen-chars-x", "pat", "Acme", out, sizeof out) == OC_PW_OK);
    CHECK(oc_pw_check("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                      "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9q", NULL, NULL, out, sizeof out) == OC_PW_OK);
    /* Spaces are fine; 129 characters are not. */
    CHECK(oc_pw_check("a long phrase of plain words", "pat", "Acme", out, sizeof out) == OC_PW_OK);
    char big[200];
    memset(big, 'k', 129); big[0] = 'q'; big[129] = '\0';
    CHECK(oc_pw_check(big, NULL, NULL, out, sizeof out) == OC_PW_TOO_LONG);
    big[128] = '\0';
    CHECK(oc_pw_check(big, NULL, NULL, out, sizeof out) == OC_PW_OK);
    /* NFKC: a full-width or ligature form is its plain one, at sign-up and at
     * sign-in alike. */
    CHECK(oc_pw_normalize("\xEF\xAC\x81", out, sizeof out) == 0 && strcmp(out, "fi") == 0);
    CHECK(oc_pw_normalize("e\xCC\x81", out, sizeof out) == 0 && strcmp(out, "\xC3\xA9") == 0);
    CHECK(oc_pw_normalize("bad\xFF" "byte", out, sizeof out) != 0);
    CHECK(oc_pw_normalize("tab\there", out, sizeof out) != 0);
    CHECK(oc_pw_check("tab\there-and-more-words", NULL, NULL, out, sizeof out) == OC_PW_BAD_CHARS);
    /* Common, repeated, or built on a name. */
    CHECK(oc_pw_check("000000000000000", NULL, NULL, out, sizeof out) == OC_PW_COMMON);
    CHECK(oc_pw_check("1qaz2wsx3edc4rfv", NULL, NULL, out, sizeof out) == OC_PW_COMMON);
    CHECK(oc_pw_check("1QAZ2WSX3EDC4RFV", NULL, NULL, out, sizeof out) == OC_PW_COMMON);  /* any case */
    CHECK(oc_pw_check("zzzzzzzzzzzzzzzzzz", NULL, NULL, out, sizeof out) == OC_PW_COMMON);
    CHECK(oc_pw_check("patricia-in-the-garden", "Patricia", "Acme", out, sizeof out) == OC_PW_PERSONAL);
    CHECK(oc_pw_check("welcome to acmecorp friends", "pat", "AcmeCorp", out, sizeof out) == OC_PW_PERSONAL);
    CHECK(oc_pw_check("my openchime password", "pat", "Acme", out, sizeof out) == OC_PW_PERSONAL);
    CHECK(oc_pw_check("a pat on the back for everyone", "pat", "Acme", out, sizeof out) == OC_PW_OK);  /* a short name is no word */
    /* Every refusal has a sentence for the form; acceptance has none. */
    CHECK(oc_pw_verdict_text(OC_PW_OK)[0] == '\0');
    for (oc_pw_verdict v = OC_PW_BAD_CHARS; v <= OC_PW_PERSONAL; v++) CHECK(oc_pw_verdict_text(v)[0] != '\0');
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

/* The pages' machine-readable state (webpages.h): every error kind has a code
 * of its own and words to show; every page names its screen; a refusal and an
 * outcome are marked on the page, so nothing need read the words. */
static void test_page_markers(void) {
    for (int e = 1; e < OC_PERR_COUNT; e++) {
        const char *code = oc_page_err_code((oc_page_err)e);
        CHECK(code[0] != '\0' && oc_page_err_text((oc_page_err)e)[0] != '\0');
        CHECK(strspn(code, "abcdefghijklmnopqrstuvwxyz-") == strlen(code));   /* fits an attribute bare */
        for (int f = 1; f < e; f++) CHECK(strcmp(code, oc_page_err_code((oc_page_err)f)) != 0);
    }
    CHECK(oc_page_err_code(OC_PERR_NONE)[0] == '\0' && oc_page_err_code(OC_PERR_COUNT)[0] == '\0');
    /* Each password refusal is its own kind, with pwpolicy's words. */
    for (oc_pw_verdict v = OC_PW_BAD_CHARS; v <= OC_PW_PERSONAL; v++)
        CHECK(oc_page_err_pw(v) != OC_PERR_NONE &&
              strcmp(oc_page_err_text(oc_page_err_pw(v)), oc_pw_verdict_text(v)) == 0);
    CHECK(oc_page_err_pw(OC_PW_OK) == OC_PERR_NONE);

    /* A form sent back says why on its message, and only then. */
    oc_page pg = { .kind = OC_PAGE_PASSWORD, .redirect_uri = "", .nonce = "", .username = "pat",
                   .invite = "", .user_code = "" };
    size_t len = 0;
    char *html = oc_page_render(&pg, &len);
    CHECK(html && strstr(html, "data-oc-page=\"password\"") && !strstr(html, "data-oc-error=") &&
          !strstr(html, "data-oc-result="));
    free(html);
    pg.err = OC_PERR_PW_MISMATCH;
    html = oc_page_render(&pg, &len);
    CHECK(html && strstr(html, "data-oc-error=\"pw-mismatch\"") && strstr(html, "data-oc-page=\"password\""));
    free(html);
    pg.err = OC_PERR_NONE;
    pg.done = 1;
    html = oc_page_render(&pg, &len);
    CHECK(html && strstr(html, "data-oc-page=\"password-done\"") && strstr(html, "data-oc-result=\"password-changed\""));
    free(html);

    /* Every security screen is named apart. */
    const char *seen[8] = { 0 };
    for (int sc = OC_SEC_SIGNIN; sc <= OC_SEC_PASSKEY_DONE; sc++) {
        oc_page sp = { .kind = OC_PAGE_SECURITY, .redirect_uri = "", .nonce = "", .username = "pat",
                       .invite = "", .user_code = "", .sec = (oc_sec_screen)sc, .ticket = "t", .secret = "K",
                       .otpauth = "otpauth://totp/x", .codes = "a\nb", .pk_challenge = "c", .pk_rp = "rp",
                       .pk_creds = "", .pk_user = "u", .pk_name = "n" };
        seen[sc] = oc_page_screen(&sp);
        char want[64];
        snprintf(want, sizeof want, "data-oc-page=\"%s\"", seen[sc]);
        html = oc_page_render(&sp, &len);
        CHECK(html && strstr(html, want));
        free(html);
        for (int k = 0; k < sc; k++) CHECK(strcmp(seen[k], seen[sc]) != 0);
    }
    /* The device page's outcomes. */
    oc_page dp = { .kind = OC_PAGE_DEVICE, .redirect_uri = "", .nonce = "", .username = "",
                   .invite = "", .user_code = "" };
    CHECK(strcmp(oc_page_screen(&dp), "device-code") == 0 && oc_page_result(&dp)[0] == '\0');
    dp.user_code = "ABCD-EFGH";
    CHECK(strcmp(oc_page_screen(&dp), "device-confirm") == 0);
    dp.done = 1;
    CHECK(strcmp(oc_page_result(&dp), "approved") == 0);
    dp.done = 0; dp.denied = 1;
    CHECK(strcmp(oc_page_result(&dp), "denied") == 0);
    /* The static pages name themselves too. */
    CHECK(strstr(oc_page_invalid(&len), "data-oc-page=\"invalid\"") &&
          strstr(oc_page_unavailable(&len), "data-oc-page=\"unavailable\""));
}

int run_auth_tests(void) {
    printf("test_auth: Argon2id derive (determinism + sensitivity) and settings, the password policy, SHA-256 KAT, randomness, constant-time compare\n");
    test_pw_derive();
    test_pw_kdf();
    test_pw_policy();
    test_page_markers();
    test_sha256();
    test_rand_bytes();
    test_ct_eq();
    return failures;
}
