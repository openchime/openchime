/*
 * A provider's ID token (OpenID Connect Core §3.1.3.7), for a direct
 * connection (AUTH.md §8.5): the daemon is the relying party, so it verifies
 * the provider's token itself -- against the key set the provider publishes,
 * not a pinned key as for the relay's (jwt.h).
 *
 * The algorithms are an allow-list: RS256, PS256 and ES256 -- never `none`,
 * never a MAC. The key is the one the header's `kid` names (or the set's only
 * key of the algorithm's type). Then `iss` exactly; `aud` the client id, alone
 * or in an array -- and with several, `azp` the client id too; `exp`, `nbf` and
 * `iat` with a minute's leeway; and the nonce the sign-in sent.
 *
 * Pure: no clock of its own, no network. The key set is the caller's.
 */
#ifndef OPENCHIME_IDTOKEN_H
#define OPENCHIME_IDTOKEN_H

#include <stddef.h>
#include <stdint.h>

#define OC_IDT_FIELD 256
#define OC_IDT_LEEWAY_S 60u

typedef struct {
    char     sub[OC_IDT_FIELD];
    char     oid[OC_IDT_FIELD];      /* Microsoft: the person's id in the tenant */
    char     email[OC_IDT_FIELD];
    char     name[OC_IDT_FIELD];
    char     hd[OC_IDT_FIELD];       /* Google: the hosted domain */
    char     tid[OC_IDT_FIELD];      /* Microsoft: the tenant */
    int      email_verified;         /* the token said true */
    int      xms_edov;               /* Microsoft: the tenant verified the address's domain */
    uint64_t exp;
} oc_idt_claims;

typedef enum {
    OC_IDT_OK = 0,
    OC_IDT_FORMAT = -1,     /* not a compact JWS, or its JSON is not what a token is */
    OC_IDT_ALG = -2,        /* an algorithm not on the list */
    OC_IDT_KEY = -3,        /* no key in the set for it */
    OC_IDT_SIGNATURE = -4,
    OC_IDT_CLAIMS = -5,     /* issuer, audience, azp, nonce or subject not this sign-in's */
    OC_IDT_TIME = -6        /* expired, or not yet good */
} oc_idt_result;

oc_idt_result oc_idtoken_verify(const char *token, size_t tlen, const char *jwks, size_t jwks_len,
                                const char *issuer, const char *client_id, const char *nonce,
                                uint64_t now_s, oc_idt_claims *out);

#endif /* OPENCHIME_IDTOKEN_H */
