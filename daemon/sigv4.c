/* AWS SigV4 request signing — see sigv4.h. */

#include "sigv4.h"

#include <mbedtls/md.h>

#include <stdio.h>
#include <string.h>

static void hmac_sha256(const uint8_t *key, size_t klen,
                        const char *msg, size_t mlen, uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, key, klen, (const unsigned char *)msg, mlen, out);
}

static void sha256_hex(const char *msg, size_t mlen, char out[65]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t d[32];
    mbedtls_md(info, (const unsigned char *)msg, mlen, d);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

void oc_sigv4_signing_key(const char *secret_key, const char *datestamp,
                          const char *region, const char *service,
                          uint8_t out[32]) {
    char k0[256];
    int n = snprintf(k0, sizeof k0, "AWS4%s", secret_key);
    if (n < 0 || n >= (int)sizeof k0) { memset(out, 0, 32); return; }
    uint8_t k[32];
    hmac_sha256((const uint8_t *)k0, (size_t)n, datestamp, strlen(datestamp), k);
    hmac_sha256(k, 32, region, strlen(region), k);
    hmac_sha256(k, 32, service, strlen(service), k);
    hmac_sha256(k, 32, "aws4_request", 12, k);
    memcpy(out, k, 32);
}

int oc_sigv4_authorization(const char *method, const char *uri,
                           const char *host, const char *payload_sha256_hex,
                           const char *amzdate, const char *datestamp,
                           const char *region, const char *service,
                           const char *access_key, const char *secret_key,
                           char *out, size_t out_cap) {
    /* Canonical request — the fixed 3-header set, no query string. */
    char creq[2048];
    int n = snprintf(creq, sizeof creq,
        "%s\n%s\n\n"
        "host:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n\n"
        "host;x-amz-content-sha256;x-amz-date\n%s",
        method, uri, host, payload_sha256_hex, amzdate, payload_sha256_hex);
    if (n < 0 || n >= (int)sizeof creq) return -1;

    char creq_hex[65];
    sha256_hex(creq, (size_t)n, creq_hex);

    char scope[256];
    if (snprintf(scope, sizeof scope, "%s/%s/%s/aws4_request",
                 datestamp, region, service) >= (int)sizeof scope) return -1;

    char sts[512];
    int m = snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s",
                     amzdate, scope, creq_hex);
    if (m < 0 || m >= (int)sizeof sts) return -1;

    uint8_t key[32];
    oc_sigv4_signing_key(secret_key, datestamp, region, service, key);
    uint8_t sig[32];
    hmac_sha256(key, 32, sts, (size_t)m, sig);
    char sighex[65];
    hex32(sig, sighex);

    int w = snprintf(out, out_cap,
        "AWS4-HMAC-SHA256 Credential=%s/%s, "
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
        access_key, scope, sighex);
    return (w < 0 || w >= (int)out_cap) ? -1 : 0;
}
