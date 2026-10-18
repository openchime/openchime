/*
 * Direct connections (AUTH.md §8.5): the daemon as an OpenID Connect relying
 * party of its operator's own provider. One worker thread per daemon does what
 * blocks -- the provider's discovery document and key set, fetched at start and
 * daily and again when a token names a key the set lacks (at most once a minute);
 * and each sign-in's code exchange at the token endpoint -- then checks the ID
 * token (idtoken.h) and hands the writer the identity, or why there is none, as
 * an AUTH job. Nothing here touches the database or a connection.
 *
 * HTTPS only, CA-verified against the built-in roots and OPENCHIME_EXTRA_CA
 * (https_client.h).
 */
#ifndef OPENCHIME_OIDCRP_H
#define OPENCHIME_OIDCRP_H

#include <stddef.h>
#include <stdint.h>

#include "dbwriter.h"

#include "config.h"   /* oc_oidc_connect, OC_OIDC_MAX_CONNECT */

typedef struct oc_oidcrp oc_oidcrp;

/* Start the worker for `n` connections. NULL on failure. */
oc_oidcrp *oc_oidcrp_start(const oc_oidc_connect *conns, int n, oc_dbwriter *dbw);
void       oc_oidcrp_stop(oc_oidcrp *rp);

/* Whether connection `i` can be signed in with now -- its discovery document
 * and keys are in hand -- and its authorization endpoint. 1, or 0. Any thread. */
int oc_oidcrp_ready(oc_oidcrp *rp, int i, char *authorize, size_t cap);

/* Exchange a code for connection `i`, for the connection `conn_id`: the
 * redirect it was sent to, the daemon's PKCE verifier and the nonce the sign-in
 * named, and the client's address for the limiter. The answer reaches the net
 * loop as an AUTH result through the writer. Non-blocking; 0, or -1 when the
 * queue is full. */
int oc_oidcrp_exchange(oc_oidcrp *rp, int i, uint64_t conn_id, const char *code, const char *redirect_uri,
                       const char *verifier, const char *nonce, const char *source);

#endif /* OPENCHIME_OIDCRP_H */
