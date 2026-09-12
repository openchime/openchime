/* Auth crypto primitives — see auth.h. mbedTLS for the hashes; /dev/urandom for
 * entropy (the daemon is Linux-only, per its epoll/eventfd core). */

#include "auth.h"

#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <errno.h>
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
                 uint32_t iterations, uint8_t out[OC_PW_HASH_LEN]) {
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256,
        (const unsigned char *)password, pwlen,
        salt, saltlen,
        iterations,
        OC_PW_HASH_LEN, out);
    return rc == 0 ? 0 : -1;
}

int oc_ct_eq(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}
