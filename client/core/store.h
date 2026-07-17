/*
 * OpenChime client — the local SQLite store (ARCH-58, CLIENT.md §5). Persists
 * across process restarts the bits a silent relaunch needs: the session token
 * (so we reconnect with OC_AUTH_SESSION instead of a password) and the per-host
 * TOFU pin (so the pinned cert survives a restart). Keyed by `instance`
 * ("host:port"), so one store file can hold several servers' state.
 *
 * The store reuses the daemon's migration runner (oc_migrate) with its own
 * client migration set. It is owned and used by the net thread only — a single
 * sqlite connection on one thread, no cross-thread access. When the store cannot
 * be opened the client still runs, just without persistence (in-memory only).
 */

#ifndef OC_STORE_H
#define OC_STORE_H

#include <stdint.h>

#include "tls.h"        /* OC_TLS_FINGERPRINT_LEN */
#include "protocol.h"   /* OC_SESSION_TOKEN_LEN */

typedef struct oc_store oc_store;

/* Open (creating + migrating) the store at `path`, or NULL on any failure (the
 * caller then runs without persistence). The parent directory must exist. */
oc_store *oc_store_open(const char *path);
void      oc_store_close(oc_store *s);

/* Session token (ARCH-58). load returns 1 and fills `token`/`expiry` iff a
 * non-expired token is stored for `instance` (`now_ms` = current time in ms; use
 * 0 to skip the expiry check). save upserts; clear drops it (logout / expiry). */
int  oc_store_load_session(oc_store *s, const char *instance,
                           uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t *expiry,
                           uint64_t now_ms);
void oc_store_save_session(oc_store *s, const char *instance,
                           const uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t expiry);
void oc_store_clear_session(oc_store *s, const char *instance);

/* TOFU pin (ARCH-10): the server cert's SHA-256, remembered on first connect and
 * enforced thereafter. load returns 1 and fills `pin` iff one is stored. */
int  oc_store_load_pin(oc_store *s, const char *instance,
                       uint8_t pin[OC_TLS_FINGERPRINT_LEN]);
void oc_store_save_pin(oc_store *s, const char *instance,
                       const uint8_t pin[OC_TLS_FINGERPRINT_LEN]);

#endif /* OC_STORE_H */
