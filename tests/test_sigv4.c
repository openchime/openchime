/* Known-answer tests for AWS SigV4 signing (daemon/sigv4.c), the security core of
 * the S3 blob backend (ARCH-70). Reference values were computed independently
 * (Python hmac/hashlib) so a signing bug here can't slip through. The full
 * request round-trip is additionally exercised against a live MinIO by the
 * integration path. */

#include "sigv4.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* AWS's documented signing-key derivation example. */
static void test_signing_key(void) {
    uint8_t key[32];
    oc_sigv4_signing_key("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
                         "20120215", "us-east-1", "iam", key);
    char h[65]; hex32(key, h);
    CHECK(strcmp(h, "004aa806e13dae88b9032d9261bcb04c67d023afadd221e6b0d206e1760e0b5e") == 0);
}

/* A full Authorization for the daemon's exact PUT-to-S3 request shape. */
static void test_authorization(void) {
    char out[512];
    CHECK(oc_sigv4_authorization(
        "PUT", "/openchime-dev/00000000000000ab", "minio:9000",
        "UNSIGNED-PAYLOAD", "20250101T000000Z", "20250101",
        "us-east-1", "s3", "minioadmin", "minioadmin", out, sizeof out) == 0);
    CHECK(strcmp(out,
        "AWS4-HMAC-SHA256 Credential=minioadmin/20250101/us-east-1/s3/aws4_request, "
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
        "Signature=da40886b041d92ee72438f5aeb73e13638207122387ffb5d0397a836362af4e4") == 0);
}

/* A too-small output buffer is reported, not overrun. */
static void test_small_buffer(void) {
    char out[16];
    CHECK(oc_sigv4_authorization("PUT", "/b/k", "h", "UNSIGNED-PAYLOAD",
        "20250101T000000Z", "20250101", "us-east-1", "s3", "ak", "sk",
        out, sizeof out) == -1);
}

int run_sigv4_tests(void) {
    printf("test_sigv4: signing-key derivation, full authorization header, buffer bound\n");
    test_signing_key();
    test_authorization();
    test_small_buffer();
    return failures;
}
