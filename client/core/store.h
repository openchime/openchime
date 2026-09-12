/*
 * OpenChime client — the local store (ARCH-58/88, CLIENT.md §5). Persists across
 * process restarts the bits a silent relaunch needs: the session token (so we
 * reconnect with OC_AUTH_SESSION instead of a password), the per-host TOFU pin,
 * the cached history, the offline outbox, and the workspace book. Keyed by
 * `workspace` ("host:port"), so one store holds several servers' state.
 *
 * **No local storage at all (ARCH-88).** Everything durable is one credential per
 * workspace in the OS credential store: session token, TOFU pin, and the book
 * fields. There is no database and no file. Cached history is gone — the daemon
 * is the source of truth and remembers each user's read position server-side
 * (REQ-090) — and the offline outbox lives in RAM on the net thread. With no OS
 * credential store nothing is persisted and the client runs in-memory only.
 */

#ifndef OC_STORE_H
#define OC_STORE_H

#include <stdint.h>

#include "tls.h"        /* OC_TLS_FINGERPRINT_LEN */
#include "protocol.h"   /* OC_SESSION_TOKEN_LEN */
#include "secret.h"     /* optional OS keyring backend for the session token */

typedef struct oc_store oc_store;

/* Create a store handle. `path` is vestigial — nothing is written to disk — and
 * only distinguishes "persistence on" from NULL ("off", which the sign-in
 * screen's Remember-me uses). NULL on failure. */
oc_store *oc_store_open(const char *path);
void      oc_store_close(oc_store *s);

/* Attach the OS credential store (borrowed). The session token AND the TOFU pin
 * live there and NOWHERE else, so with NULL neither is persisted — the user signs
 * in again next launch and the next connect re-TOFUs. Set once, right after
 * open, and before any load_session/load_pin probe. */
void      oc_store_set_secret(oc_store *s, oc_secret *secret);

/* Session token (ARCH-58). load returns 1 and fills `token`/`expiry` iff a
 * non-expired token is stored for `workspace` (`now_ms` = current time in ms; use
 * 0 to skip the expiry check). save upserts; clear drops it (logout / expiry). */
int  oc_store_load_session(oc_store *s, const char *workspace,
                           uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t *expiry,
                           uint64_t now_ms);
void oc_store_save_session(oc_store *s, const char *workspace,
                           const uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t expiry);
void oc_store_clear_session(oc_store *s, const char *workspace);

/* TOFU pin (ARCH-10): the server cert's SHA-256, remembered on first connect and
 * enforced thereafter. load returns 1 and fills `pin` iff one is stored. */
int  oc_store_load_pin(oc_store *s, const char *workspace,
                       uint8_t pin[OC_TLS_FINGERPRINT_LEN]);
void oc_store_save_pin(oc_store *s, const char *workspace,
                       const uint8_t pin[OC_TLS_FINGERPRINT_LEN]);

/* The workspace book (REQ-012): the list of workspaces this machine knows about,
 * so a frontend can offer a switcher without the user retyping an address. One
 * row per workspace, holding the `label` the user typed (`acme.example.com` —
 * friendlier than the resolved "host:port" key) and the `username` they signed
 * in as, ordered most-recently-used first.
 *
 * The book is not stored separately: there is one credential per workspace, so
 * enumerating the credential store IS the book (oc_secret_each). These three are
 * safe from a SECOND oc_store handle outside the net thread — the OS credential
 * store serializes its own access — which is how the switcher lists workspaces
 * that have no running client.
 *
 * remember() upserts and stamps last-used; a NULL label/username preserves the
 * stored one, so re-login never blanks the switcher. forget() deletes the whole
 * credential — token, pin and book entry in one go, leaving nothing behind. */
void oc_store_workspace_remember(oc_store *s, const char *workspace,
                                 const char *label, const char *username,
                                 uint64_t now_ms);
void oc_store_workspace_forget(oc_store *s, const char *workspace);
typedef void (*oc_store_workspace_cb)(void *ctx, const char *workspace,
                                      const char *label, const char *username,
                                      uint64_t last_used_ms);
void oc_store_workspace_each(oc_store *s, oc_store_workspace_cb cb, void *ctx);

#endif /* OC_STORE_H */
