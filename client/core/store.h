/*
 * OpenChime client — the local store (ARCH-58/88, CLIENT.md §5). Persists across
 * process restarts the bits a silent relaunch needs: the session token (so we
 * reconnect with OC_AUTH_SESSION instead of a password), the per-host TOFU pin,
 * the cached history, the offline outbox, and the workspace book. Keyed by
 * `workspace` ("host:port"), so one store holds several servers' state.
 *
 * **No database engine (ARCH-88).** The token and pin live in the OS credential
 * store via the oc_secret seam; the cache, outbox and book are plain files under
 * `path`, which is a DIRECTORY. It is owned by the net thread (the three book
 * calls excepted, below); when the store cannot be opened the client still runs,
 * just without persistence.
 */

#ifndef OC_STORE_H
#define OC_STORE_H

#include <stdint.h>

#include "tls.h"        /* OC_TLS_FINGERPRINT_LEN */
#include "protocol.h"   /* OC_SESSION_TOKEN_LEN */
#include "secret.h"     /* optional OS keyring backend for the session token */

typedef struct oc_store oc_store;

/* Open the store rooted at directory `path`, creating it if needed; NULL on any
 * failure (the caller then runs without persistence). */
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

/* Cached message history (ARCH-45/46), so a relaunch shows history instantly and
 * backfills only from the last cached id. save upserts one message; edit/delete
 * update an existing row's body/flags. All keyed by (workspace, message_id). */
void oc_store_save_message(oc_store *s, const char *workspace, uint64_t channel_id,
                           uint64_t message_id, uint64_t author_id,
                           const char *author_name, uint64_t server_time,
                           const char *body, int edited, int deleted);
void oc_store_edit_message(oc_store *s, const char *workspace, uint64_t message_id,
                           const char *body);
void oc_store_delete_message(oc_store *s, const char *workspace, uint64_t message_id);

/* Replay cached messages for `workspace` in ascending message-id order, invoking
 * `cb` for each. `body` may be NULL (deleted). The store owns the strings for the
 * duration of the callback only. */
typedef void (*oc_store_msg_cb)(void *ctx, uint64_t channel_id, uint64_t message_id,
                                uint64_t author_id, const char *author_name,
                                uint64_t server_time, const char *body,
                                int edited, int deleted);
void oc_store_each_message(oc_store *s, const char *workspace,
                           oc_store_msg_cb cb, void *ctx);

/* Offline outbox (REQ-102). A message is added here (with its idempotency token)
 * before it is sent, removed when the server acks it, and re-sent from here on
 * reconnect — so a send survives a drop or an app restart, and the daemon's
 * idempotency dedups any partial delivery. Keyed by (workspace, idem). */
void oc_store_outbox_add(oc_store *s, const char *workspace,
                         const uint8_t idem[OC_IDEM_SIZE], uint64_t channel_id,
                         const char *body);
void oc_store_outbox_remove(oc_store *s, const char *workspace,
                            const uint8_t idem[OC_IDEM_SIZE]);
/* Replay pending outbox messages in insertion order (oldest first). */
typedef void (*oc_store_outbox_cb)(void *ctx, const uint8_t idem[OC_IDEM_SIZE],
                                   uint64_t channel_id, const char *body);
void oc_store_outbox_each(oc_store *s, const char *workspace,
                          oc_store_outbox_cb cb, void *ctx);

/* The workspace book (REQ-012): the list of workspaces this machine knows about,
 * so a frontend can offer a switcher without the user retyping an address. One
 * row per workspace, holding the `label` the user typed (`acme.example.com` —
 * friendlier than the resolved "host:port" key) and the `username` they signed
 * in as, ordered most-recently-used first.
 *
 * Unlike the rest of this header these three are safe to call from a SECOND
 * oc_store handle on the same directory, opened by the frontend outside the net
 * thread — the book is written at login/logout, not on the message path, and it
 * is rewritten whole under an atomic rename, so a concurrent reader sees either
 * the old file or the new one. That is how the switcher lists workspaces that
 * have no running client.
 *
 * remember() upserts and stamps last-used; a NULL label/username preserves the
 * stored one, so re-login never blanks the switcher. forget() removes the entry
 * AND that workspace's session token, TOFU pin, cached history, and outbox. */
void oc_store_workspace_remember(oc_store *s, const char *workspace,
                                 const char *label, const char *username,
                                 uint64_t now_ms);
void oc_store_workspace_forget(oc_store *s, const char *workspace);
typedef void (*oc_store_workspace_cb)(void *ctx, const char *workspace,
                                      const char *label, const char *username,
                                      uint64_t last_used_ms);
void oc_store_workspace_each(oc_store *s, oc_store_workspace_cb cb, void *ctx);

#endif /* OC_STORE_H */
