/* A local account's second step (AUTH.md §8.6): RFC 6238's own vectors, the
 * window and replay rules, base32, and the sealing that keeps a secret useless
 * without the factor key and to any other account. */

#include "check.h"
#include "../daemon/totp.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int run_totp_tests(void) {
    printf("test_totp: RFC 6238 SHA1 vectors, +-1 step, no replay, base32, sealed secrets\n");
    /* RFC 6238 appendix B: the SHA1 secret, eight-digit codes; six digits are
     * the same number mod 10^6. */
    static const uint8_t K[] = "12345678901234567890";
    static const struct { uint64_t t; uint32_t code8; } V[] = {
        { 59, 94287082u }, { 1111111109, 7081804u }, { 1111111111, 14050471u },
        { 1234567890, 89005924u }, { 2000000000, 69279037u }, { 20000000000ull, 65353130u },
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        uint32_t got = oc_totp_code(K, 20, V[i].t / OC_TOTP_STEP_S);
        if (got != V[i].code8 % 1000000u) printf("  t=%llu got %06u\n", (unsigned long long)V[i].t, got);
        CHECK(got == V[i].code8 % 1000000u);
    }

    /* The step a code is for, or one either side; never twice; only digits. */
    uint64_t now = 1111111111, step = now / OC_TOTP_STEP_S, hit = 0;
    char c[8];
    snprintf(c, sizeof c, "%06u", oc_totp_code(K, 20, step));
    CHECK(oc_totp_verify(K, 20, c, now, 0, &hit) == 1 && hit == step);
    CHECK(oc_totp_verify(K, 20, c, now, step, &hit) == 0);              /* used: no replay */
    snprintf(c, sizeof c, "%06u", oc_totp_code(K, 20, step - 1));
    CHECK(oc_totp_verify(K, 20, c, now, 0, &hit) == 1 && hit == step - 1);
    snprintf(c, sizeof c, "%06u", oc_totp_code(K, 20, step + 1));
    CHECK(oc_totp_verify(K, 20, c, now, 0, &hit) == 1 && hit == step + 1);
    snprintf(c, sizeof c, "%06u", oc_totp_code(K, 20, step + 2));
    CHECK(oc_totp_verify(K, 20, c, now, 0, &hit) == 0);                 /* too far */
    snprintf(c, sizeof c, "%06u", oc_totp_code(K, 20, step - 2));
    CHECK(oc_totp_verify(K, 20, c, now, 0, &hit) == 0);
    CHECK(oc_totp_verify(K, 20, "12345", now, 0, &hit) == 0);
    CHECK(oc_totp_verify(K, 20, "12345a", now, 0, &hit) == 0);
    CHECK(oc_totp_verify(K, 20, NULL, now, 0, &hit) == 0);

    /* Base32 (RFC 4648 §10), unpadded. */
    char b[64];
    CHECK(oc_base32_encode((const uint8_t *)"foobar", 6, b, sizeof b) == 10 && strcmp(b, "MZXW6YTBOI") == 0);
    CHECK(oc_base32_encode((const uint8_t *)"f", 1, b, sizeof b) == 2 && strcmp(b, "MY") == 0);
    CHECK(oc_base32_encode(K, 20, b, 33) == 32 && strcmp(b, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") == 0);
    CHECK(oc_base32_encode(K, 20, b, 32) == -1);                        /* no room for the NUL */

    /* Sealed: opens with its key and its account only, and not once changed. */
    uint8_t key[OC_FACTOR_KEY_LEN], other[OC_FACTOR_KEY_LEN], sealed[OC_TOTP_SEALED_LEN], out[OC_TOTP_SECRET_LEN];
    memset(key, 0x11, sizeof key);
    memset(other, 0x22, sizeof other);
    CHECK(oc_factor_seal(key, 7, K, sealed) == 0);
    CHECK(memcmp(sealed + 12, K, 20) != 0);
    CHECK(oc_factor_open(key, 7, sealed, sizeof sealed, out) == 0 && memcmp(out, K, 20) == 0);
    CHECK(oc_factor_open(other, 7, sealed, sizeof sealed, out) == -1);
    CHECK(oc_factor_open(key, 8, sealed, sizeof sealed, out) == -1);    /* moved to another account */
    sealed[20] ^= 1;
    CHECK(oc_factor_open(key, 7, sealed, sizeof sealed, out) == -1);
    static const uint8_t zero[OC_TOTP_SECRET_LEN] = { 0 };
    CHECK(memcmp(out, zero, sizeof zero) == 0);                         /* wiped on refusal */
    CHECK(oc_factor_open(key, 7, sealed, sizeof sealed - 1, out) == -1);

    /* The factor key file: made once, mode 0600, then read back the same; a file
     * that is not a key is refused rather than used. */
    {
        const char *path = "build/test_totp_factor.key";
        unlink(path);
        char why[256];
        uint8_t k1[OC_FACTOR_KEY_LEN], k2[OC_FACTOR_KEY_LEN];
        CHECK(oc_factor_key_load(path, k1, why, sizeof why) == 1);
        struct stat sb;
        CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600 && sb.st_size == OC_FACTOR_KEY_LEN);
        CHECK(oc_factor_key_load(path, k2, why, sizeof why) == 0 && memcmp(k1, k2, sizeof k1) == 0);
        FILE *f = fopen(path, "ab");
        if (f) { fputc('x', f); fclose(f); }
        CHECK(oc_factor_key_load(path, k2, why, sizeof why) == -1 && why[0]);
        unlink(path);
        CHECK(oc_factor_key_load("build/no-such-dir/factor.key", k2, why, sizeof why) == -1);
    }
    return failures;
}
