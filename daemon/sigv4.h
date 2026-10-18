#ifndef OC_SIGV4_H
#define OC_SIGV4_H

#include <stddef.h>
#include <stdint.h>

/* AWS Signature Version 4 signing for the S3 blob backend (ARCH-70). Scoped to
 * the exact request shape the daemon issues to S3/MinIO — the fixed signed-header
 * set { host, x-amz-content-sha256, x-amz-date } and no query string — so it
 * needs no general header canonicalizer. Uses HMAC-SHA256 / SHA-256 (mbedTLS). */

/* Derive the SigV4 signing key: HMAC chain over ("AWS4"+secret) -> datestamp ->
 * region -> service -> "aws4_request". Exposed for known-answer testing. */
void oc_sigv4_signing_key(const char *secret_key, const char *datestamp,
                          const char *region, const char *service,
                          uint8_t out[32]);

/* Build the `Authorization` header value for a request. `payload_sha256_hex` is
 * the lowercase hex SHA-256 of the body, or the literal "UNSIGNED-PAYLOAD" for a
 * streamed body (permitted by S3/MinIO). `amzdate` is "YYYYMMDDTHHMMSSZ",
 * `datestamp` "YYYYMMDD". Writes the full header value to `out`; returns 0 on
 * success, -1 if a buffer was too small. */
int oc_sigv4_authorization(const char *method, const char *uri,
                           const char *host, const char *payload_sha256_hex,
                           const char *amzdate, const char *datestamp,
                           const char *region, const char *service,
                           const char *access_key, const char *secret_key,
                           char *out, size_t out_cap);

#endif /* OC_SIGV4_H */
