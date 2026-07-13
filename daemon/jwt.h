/*
 * OpenChime ES256 JWT verification (AUTH.md §3.3) — the daemon's OIDC trust
 * anchor. The daemon never speaks OIDC to providers; it verifies ONE JWT minted
 * by the maintainer's central service against ONE pinned public key, pinning
 * both the key and the algorithm (alg=ES256) to close JWT's classic footguns.
 *
 * This is a once-per-login path, so it may allocate a little and parse JSON
 * (via vendored jsmn) — unlike the JSON-free message path (ARCH-6).
 */

#ifndef OPENCHIME_JWT_H
#define OPENCHIME_JWT_H

#include <stddef.h>
#include <stdint.h>

#define OC_JWT_MAX_FIELD 256   /* per-claim copy bound (sub/iss/aud/email/name) */

/* Validated identity claims (AUTH.md §3.3). Strings are NUL-terminated. */
typedef struct {
    char     sub[OC_JWT_MAX_FIELD];    /* "<provider issuer>|<subject>" */
    char     email[OC_JWT_MAX_FIELD];
    char     name[OC_JWT_MAX_FIELD];
    char     iss[OC_JWT_MAX_FIELD];    /* the central service */
    char     aud[OC_JWT_MAX_FIELD];    /* this instance's id */
    uint64_t exp;                      /* seconds since epoch */
    uint64_t iat;
} oc_jwt_claims;

typedef enum {
    OC_JWT_OK          = 0,
    OC_JWT_E_FORMAT    = -1,  /* not three base64url segments / unparseable JSON */
    OC_JWT_E_ALG       = -2,  /* header alg != ES256 */
    OC_JWT_E_SIGNATURE = -3,  /* signature does not verify against the pinned key */
    OC_JWT_E_CLAIMS    = -4,  /* iss or aud mismatch */
    OC_JWT_E_EXPIRED   = -5,  /* exp in the past */
    OC_JWT_E_KEY       = -6,  /* pinned key unparseable or not an EC P-256 key */
    OC_JWT_E_INTERNAL  = -7
} oc_jwt_result;

/*
 * Verify a compact JWS (`token`, length `tlen`) against a pinned ES256 public
 * key. Pins alg=ES256, verifies the signature, then requires iss==`want_iss`
 * (if non-NULL), aud==`want_aud` (if non-NULL), and exp in the future (small
 * leeway). On OC_JWT_OK, `out` holds the validated claims.
 *
 * `pubkey_pem` is a PEM SubjectPublicKeyInfo; `pem_len` MUST include the
 * trailing NUL byte (mbedTLS PEM parsing requirement).
 */
oc_jwt_result oc_jwt_verify(const char *token, size_t tlen,
                            const char *pubkey_pem, size_t pem_len,
                            const char *want_iss, const char *want_aud,
                            uint64_t now_secs, oc_jwt_claims *out);

/* base64url (RFC 4648 §5, no padding) decode. Returns the decoded byte count,
 * or -1 on an invalid character or insufficient `out_cap`. */
long oc_base64url_decode(const char *in, size_t inlen, uint8_t *out, size_t out_cap);

#endif /* OPENCHIME_JWT_H */
