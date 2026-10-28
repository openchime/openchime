/*
 * The daemon as an OIDC issuer for its own local accounts (AUTH.md §8.10): a
 * P-256 key and an issuer name, generated once and kept in the database so they
 * survive a restore, and the short-lived ID token a browser sign-in on the
 * daemon's own pages ends with. The client presents it on the same AUTH path a
 * relay token takes, and the daemon verifies it with oc_jwt_verify against this
 * key -- one credential type for every sign-in.
 *
 * Used by the writer thread alone: one issuer, one random generator, no locks.
 */
#ifndef OPENCHIME_LOCALISSUER_H
#define OPENCHIME_LOCALISSUER_H

#include <stddef.h>
#include <stdint.h>

/* Every local token is for OpenChime's clients; the issuer names the workspace. */
#define OC_LOCAL_AUDIENCE "openchime-client"
/* How long a token lives: long enough to cross from the browser to the client,
 * which redeems it at once. */
#define OC_LOCAL_TOKEN_SECS 120u

typedef struct oc_local_issuer oc_local_issuer;

/* A new issuer identity: its private key (PEM) and its name, both malloc'd, for
 * the caller to store. Returns 0. */
int oc_local_issuer_generate(char **key_pem, char **issuer);

/* The issuer over a stored key and name. NULL if the key does not parse. */
oc_local_issuer *oc_local_issuer_open(const char *key_pem, const char *issuer);
void oc_local_issuer_close(oc_local_issuer *li);

/* Its name, and its public key (PEM, NUL-terminated) for oc_jwt_verify. */
const char *oc_local_issuer_name(const oc_local_issuer *li);
const char *oc_local_issuer_pubkey_pem(const oc_local_issuer *li);

/* A token for local user `uid`, bound to the client's PKCE challenge `nonce`:
 * sub "local|<uid>", a fresh jti, issued `now_secs` and good for
 * OC_LOCAL_TOKEN_SECS. malloc'd compact JWS, or NULL. */
char *oc_local_issuer_mint(oc_local_issuer *li, uint64_t uid, const char *nonce, uint64_t now_secs);

/* The user a verified local token names: its `sub`, "local|<uid>". 0 if it is
 * not one. */
uint64_t oc_local_subject_uid(const char *sub);

#endif /* OPENCHIME_LOCALISSUER_H */
