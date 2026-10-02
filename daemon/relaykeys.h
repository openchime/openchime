/*
 * The relay's published keys (AUTH.md §3.3). A daemon enrolled with central
 * reads `<relay origin>/oidc/jwks` at boot and daily, over HTTPS verified with
 * the built-in roots, and trusts the ES256 keys there beside the ones pinned in
 * OPENCHIME_OIDC_PUBKEY -- which stay the floor whatever the fetch says. So a
 * rotation that publishes the next key a day before signing with it reaches
 * every enrolled daemon by itself.
 */
#ifndef OPENCHIME_RELAYKEYS_H
#define OPENCHIME_RELAYKEYS_H

#include <stddef.h>

#include "dbwriter.h"

#define OC_RELAYKEYS_MAX 8   /* keys taken from one fetch */

typedef struct oc_relaykeys oc_relaykeys;

/* Every EC P-256 signing key in a JWKS document, as concatenated PEM
 * SubjectPublicKeyInfo blocks into `out`. The number of keys (at most
 * OC_RELAYKEYS_MAX), 0 for a set with none, -1 for a document that is not one or
 * keys that do not fit. Pure. */
int oc_relaykeys_pem(const char *jwks, size_t len, char *out, size_t cap);

/* Fetch `jwks_url` now, then daily -- every minute while it gets no good answer --
 * and give the writer each key set that parses. NULL on failure to start. */
oc_relaykeys *oc_relaykeys_start(const char *jwks_url, oc_dbwriter *dbw);
/* 1 once a fetch has given the writer a key set. */
int  oc_relaykeys_fetched(oc_relaykeys *rk);
void oc_relaykeys_stop(oc_relaykeys *rk);

#endif /* OPENCHIME_RELAYKEYS_H */
