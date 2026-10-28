/*
 * Passkeys as a second step (AUTH.md §8.6, W3C WebAuthn Level 2): registering
 * one from what `navigator.credentials.create` returns, and checking one from
 * what `navigator.credentials.get` returns. Attestation "none" only -- the key
 * is the person's to choose; COSE ES256 (-7) and RS256 (-257).
 *
 * Pure: no database, no clock, no allocation.
 */
#ifndef OPENCHIME_WEBAUTHN_H
#define OPENCHIME_WEBAUTHN_H

#include <stddef.h>
#include <stdint.h>

#define OC_WA_CRED_MAX 255u     /* a credential id, per the spec's bound in practice */
#define OC_WA_COSE_MAX 600u     /* a COSE key: RSA-2048's modulus and exponent fit */

typedef enum {
    OC_WA_OK = 0,
    OC_WA_BAD = -1,          /* malformed: not what a browser sends */
    OC_WA_CLIENT = -2,       /* the type, challenge or origin is not this ceremony's */
    OC_WA_RP = -3,           /* made for another relying party */
    OC_WA_FLAGS = -4,        /* no user presence, or no key where one was due */
    OC_WA_KEY = -5,          /* a key or algorithm this does not take */
    OC_WA_SIG = -6,          /* the signature does not verify */
    OC_WA_COUNTER = -7       /* the signature counter went backwards: a cloned key */
} oc_wa_result;

typedef struct {
    uint8_t  cred_id[OC_WA_CRED_MAX];
    size_t   cred_len;
    uint8_t  cose[OC_WA_COSE_MAX];
    size_t   cose_len;
    uint32_t count;
} oc_wa_cred;

/* A new passkey: `client_data` (clientDataJSON) of type webauthn.create with
 * `challenge` (base64url) and `origin`, and an attestation object made for
 * `rp_id` with user presence and attestation "none". `out` gets its id, key and
 * counter. */
oc_wa_result oc_webauthn_register(const uint8_t *client_data, size_t cdl, const uint8_t *att, size_t al,
                                  const char *challenge, const char *origin, const char *rp_id,
                                  oc_wa_cred *out);

/* A passkey's answer: webauthn.get, `challenge`, `origin`, `rp_id`, user
 * presence, and a signature over the authenticator data and the client data's
 * hash by the key `cose`. A counter that is not zero must have gone up from
 * `stored`; `*count` gets the new one. */
oc_wa_result oc_webauthn_assert(const uint8_t *client_data, size_t cdl, const uint8_t *auth, size_t adl,
                                const uint8_t *sig, size_t sl, const uint8_t *cose, size_t cosel,
                                uint32_t stored, const char *challenge, const char *origin,
                                const char *rp_id, uint32_t *count);

#endif /* OPENCHIME_WEBAUTHN_H */
