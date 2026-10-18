/* Auth crypto primitives — see auth.h. mbedTLS for the hashes; /dev/urandom for
 * entropy (the daemon is Linux-only, per its epoll/eventfd core). */

#include "auth.h"

#include "argon2.h"
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int oc_rand_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

int oc_sha256(const void *in, size_t n, uint8_t out[OC_SHA256_LEN]) {
    return mbedtls_sha256((const unsigned char *)in, n, out, 0) == 0 ? 0 : -1;
}

int oc_pw_derive(const char *password, size_t pwlen,
                 const uint8_t *salt, size_t saltlen,
                 uint32_t mem_kib, uint8_t out[OC_PW_HASH_LEN]) {
    if (mem_kib < 8 * OC_PW_LANES) return -1;
    return argon2id_hash_raw(OC_PW_PASSES, mem_kib, OC_PW_LANES, password, pwlen,
                             salt, saltlen, out, OC_PW_HASH_LEN) == ARGON2_OK ? 0 : -1;
}

void oc_pw_kdf_format(uint32_t mem_kib, char out[OC_PW_KDF_MAX]) {
    snprintf(out, OC_PW_KDF_MAX, "argon2id$v=19$m=%u,t=%u,p=%u",
             (unsigned)mem_kib, (unsigned)OC_PW_PASSES, (unsigned)OC_PW_LANES);
}

int oc_pw_kdf_parse(const char *kdf, uint32_t *mem_kib) {
    unsigned m = 0, t = 0, p = 0;
    int n = 0;
    if (!kdf || sscanf(kdf, "argon2id$v=19$m=%u,t=%u,p=%u%n", &m, &t, &p, &n) != 3 || kdf[n] != '\0')
        return -1;
    if (t != OC_PW_PASSES || p != OC_PW_LANES || m < 8 * OC_PW_LANES) return -1;
    *mem_kib = m;
    return 0;
}

int oc_ct_eq(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}
