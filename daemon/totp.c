/* TOTP, base32 and sealed secrets -- see totp.h. */

#include "totp.h"
#include "auth.h"   /* oc_rand_bytes */

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint32_t oc_totp_code(const uint8_t *secret, size_t len, uint64_t step) {
    uint8_t msg[8], mac[20];
    for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)step; step >>= 8; }
    const mbedtls_md_info_t *sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!sha1 || mbedtls_md_hmac(sha1, secret, len, msg, sizeof msg, mac) != 0) return 1000000u;
    /* RFC 4226 §5.3: dynamic truncation. */
    unsigned off = mac[19] & 0x0f;
    uint32_t bin = ((uint32_t)(mac[off] & 0x7f) << 24) | ((uint32_t)mac[off + 1] << 16) |
                   ((uint32_t)mac[off + 2] << 8) | mac[off + 3];
    memset(mac, 0, sizeof mac);
    return bin % 1000000u;
}

int oc_totp_verify(const uint8_t *secret, size_t len, const char *code, uint64_t now_s,
                   uint64_t last_step, uint64_t *matched) {
    if (!code || strlen(code) != OC_TOTP_DIGITS) return 0;
    uint32_t want = 0;
    for (unsigned i = 0; i < OC_TOTP_DIGITS; i++) {
        if (code[i] < '0' || code[i] > '9') return 0;
        want = want * 10u + (uint32_t)(code[i] - '0');
    }
    uint64_t step = now_s / OC_TOTP_STEP_S;
    /* Every candidate is computed, so how long this takes says nothing of which
     * one matched. */
    int ok = 0;
    uint64_t hit = 0;
    for (int d = -1; d <= 1; d++) {
        uint64_t s = step + (uint64_t)(int64_t)d;
        if (d < 0 && step == 0) continue;
        uint32_t got = oc_totp_code(secret, len, s);
        if (got == want && s > last_step && !ok) { ok = 1; hit = s; }
    }
    if (ok && matched) *matched = hit;
    return ok;
}

int oc_base32_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    size_t need = (n * 8 + 4) / 5;
    if (cap <= need) return -1;
    size_t o = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) { out[o++] = A[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) out[o++] = A[(buf << (5 - bits)) & 31];
    out[o] = '\0';
    return (int)o;
}

static void uid_bytes(uint64_t uid, uint8_t ad[8]) {
    for (int i = 7; i >= 0; i--) { ad[i] = (uint8_t)uid; uid >>= 8; }
}

int oc_factor_seal(const uint8_t key[OC_FACTOR_KEY_LEN], uint64_t uid,
                   const uint8_t secret[OC_TOTP_SECRET_LEN], uint8_t out[OC_TOTP_SEALED_LEN]) {
    uint8_t ad[8];
    uid_bytes(uid, ad);
    if (oc_rand_bytes(out, 12) != 0) return -1;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, OC_FACTOR_KEY_LEN * 8) ||
             mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, OC_TOTP_SECRET_LEN, out, 12, ad, sizeof ad,
                                       secret, out + 12, 16, out + 12 + OC_TOTP_SECRET_LEN);
    mbedtls_gcm_free(&g);
    return rc ? -1 : 0;
}

int oc_factor_open(const uint8_t key[OC_FACTOR_KEY_LEN], uint64_t uid,
                   const uint8_t *sealed, size_t len, uint8_t secret[OC_TOTP_SECRET_LEN]) {
    if (!sealed || len != OC_TOTP_SEALED_LEN) { memset(secret, 0, OC_TOTP_SECRET_LEN); return -1; }
    uint8_t ad[8];
    uid_bytes(uid, ad);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, OC_FACTOR_KEY_LEN * 8) ||
             mbedtls_gcm_auth_decrypt(&g, OC_TOTP_SECRET_LEN, sealed, 12, ad, sizeof ad,
                                      sealed + 12 + OC_TOTP_SECRET_LEN, 16, sealed + 12, secret);
    mbedtls_gcm_free(&g);
    if (rc) { memset(secret, 0, OC_TOTP_SECRET_LEN); return -1; }
    return 0;
}

int oc_factor_key_load(const char *path, uint8_t key[OC_FACTOR_KEY_LEN], char *err, size_t errcap) {
    if (!path || !path[0]) { snprintf(err, errcap, "no path"); return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { snprintf(err, errcap, "cannot create %s: %s", path, strerror(errno)); return -1; }
        int ok = oc_rand_bytes(key, OC_FACTOR_KEY_LEN) == 0 &&
                 write(fd, key, OC_FACTOR_KEY_LEN) == (ssize_t)OC_FACTOR_KEY_LEN && fsync(fd) == 0;
        close(fd);
        if (!ok) {
            memset(key, 0, OC_FACTOR_KEY_LEN);
            unlink(path);
            snprintf(err, errcap, "cannot write %s", path);
            return -1;
        }
        return 1;
    }
    if (fd < 0) { snprintf(err, errcap, "cannot read %s: %s", path, strerror(errno)); return -1; }
    struct stat sb;
    ssize_t n = -1;
    uint8_t extra;
    if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size == (off_t)OC_FACTOR_KEY_LEN)
        n = read(fd, key, OC_FACTOR_KEY_LEN);
    int more = n == (ssize_t)OC_FACTOR_KEY_LEN && read(fd, &extra, 1) != 0;
    close(fd);
    if (n != (ssize_t)OC_FACTOR_KEY_LEN || more) {
        memset(key, 0, OC_FACTOR_KEY_LEN);
        snprintf(err, errcap, "%s is not a %u-byte key", path, OC_FACTOR_KEY_LEN);
        return -1;
    }
    if (sb.st_mode & 077)
        fprintf(stderr, "openchimed: %s is readable by others than its owner; it should be mode 0600\n", path);
    return 0;
}
