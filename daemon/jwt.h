/*
 * OpenChime ES256 JWT verification (AUTH.md §3.3, §8.3) — the daemon's OIDC
 * trust anchor. The daemon never speaks OIDC to providers; it verifies ONE JWT
 * minted by the maintainer's central service against a pinned SET of public
 * keys, pinning both the keys and the algorithm (alg=ES256) to close JWT's
 * classic footguns. The token's `kid` is the signing key's RFC 7638 thumbprint
 * and chooses among the pinned keys; nothing is ever fetched.
 *
 * This is a once-per-login path, so it may allocate a little and parse JSON
 * (via vendored jsmn) — unlike the JSON-free message path (ARCH-6).
 */

#ifndef OPENCHIME_JWT_H
#define OPENCHIME_JWT_H

#include <stddef.h>
#include <stdint.h>

#define OC_JWT_MAX_FIELD 256   /* per-claim bound; a longer claim is REFUSED */
#define OC_JWT_MAX_SHORT 64    /* idp / nonce / jti */
#define OC_JWT_THUMBPRINT_LEN 43   /* base64url(SHA-256), unpadded */
#define OC_JWT_MAX_LIFETIME_SECS 300u   /* exp - iat ceiling (AUTH.md §8.3) */

/* Validated identity claims (AUTH.md §8.3). Strings are NUL-terminated and
 * JSON-unescaped; an absent optional claim is the empty string. */
typedef struct {
    char     sub[OC_JWT_MAX_FIELD];    /* "<provider issuer>|<subject>" */
    char     email[OC_JWT_MAX_FIELD];
    char     name[OC_JWT_MAX_FIELD];
    char     iss[OC_JWT_MAX_FIELD];    /* the central service */
    char     aud[OC_JWT_MAX_FIELD];    /* this workspace's id */
    char     tenant[OC_JWT_MAX_FIELD]; /* the provider's organization; "" if personal */
    char     idp[OC_JWT_MAX_SHORT];    /* which provider vouched: google, microsoft */
    char     nonce[OC_JWT_MAX_SHORT];  /* base64url(SHA-256(client verifier)) */
    char     jti[OC_JWT_MAX_SHORT];    /* single-use id; the caller keeps the set */
    int      email_verified;           /* 1 only when the token says true */
    uint64_t exp;                      /* seconds since epoch */
    uint64_t iat;
    uint64_t nbf;                      /* 0 if absent */
} oc_jwt_claims;

typedef enum {
    OC_JWT_OK          = 0,
    OC_JWT_E_FORMAT    = -1,  /* not three base64url segments / unparseable JSON */
    OC_JWT_E_ALG       = -2,  /* header alg != ES256 */
    OC_JWT_E_SIGNATURE = -3,  /* signature does not verify against the pinned key */
    OC_JWT_E_CLAIMS    = -4,  /* iss/aud mismatch; a required claim missing,
                               * over-long or malformed; lifetime over the ceiling */
    OC_JWT_E_EXPIRED   = -5,  /* exp in the past, or nbf/iat in the future */
    OC_JWT_E_KEY       = -6,  /* no pinned EC P-256 key has the token's kid */
    OC_JWT_E_INTERNAL  = -7
} oc_jwt_result;

/*
 * Verify a compact JWS (`token`, length `tlen`) against the pinned ES256 public
 * keys. Pins alg=ES256, picks the key whose thumbprint is the header's `kid`
 * (an absent or unknown kid is refused), verifies the signature, then requires
 * iss==`want_iss` and aud==`want_aud` (each if non-NULL); sub, nonce, jti, iat
 * and exp present; exp in the future and iat/nbf not (small leeway); and
 * exp - iat within OC_JWT_MAX_LIFETIME_SECS. On OC_JWT_OK, `out` holds the
 * validated claims. Single use of `jti` and the nonce's verifier are the
 * caller's to check — this function holds no state.
 *
 * `pubkeys_pem` is one or more PEM SubjectPublicKeyInfo blocks, concatenated;
 * `pem_len` MUST include the trailing NUL byte.
 */
oc_jwt_result oc_jwt_verify(const char *token, size_t tlen,
                            const char *pubkeys_pem, size_t pem_len,
                            const char *want_iss, const char *want_aud,
                            uint64_t now_secs, oc_jwt_claims *out);

/* RFC 7638 thumbprint of one PEM EC P-256 public key, as unpadded base64url
 * into `out` (OC_JWT_THUMBPRINT_LEN + 1 bytes). `pem_len` includes the NUL.
 * Returns 0, or -1 if the key is not an EC P-256 public key. */
int oc_jwt_key_thumbprint(const char *pubkey_pem, size_t pem_len,
                          char out[OC_JWT_THUMBPRINT_LEN + 1]);

/* 1 if `nonce` is base64url(SHA-256(verifier)) — the proof that whoever
 * presents a token is the client that asked for it (AUTH.md §8.2). */
int oc_jwt_nonce_matches(const char *nonce, const uint8_t *verifier, size_t verifier_len);

/* base64url (RFC 4648 §5, no padding) decode. Returns the decoded byte count,
 * or -1 on an invalid character or insufficient `out_cap`. */
long oc_base64url_decode(const char *in, size_t inlen, uint8_t *out, size_t out_cap);

#endif /* OPENCHIME_JWT_H */
