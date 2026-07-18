/*
 * OpenChime client — the local SQLite store (ARCH-58, CLIENT.md §5). Persists
 * across process restarts the bits a silent relaunch needs: the session token
 * (so we reconnect with OC_AUTH_SESSION instead of a password) and the per-host
 * TOFU pin (so the pinned cert survives a restart). Keyed by `workspace`
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
#include "secret.h"     /* optional OS keyring backend for the session token */

typedef struct oc_store oc_store;

/* Open (creating + migrating) the store at `path`, or NULL on any failure (the
 * caller then runs without persistence). The parent directory must exist. */
oc_store *oc_store_open(const char *path);
void      oc_store_close(oc_store *s);

/* Route the session token through an OS secret store (borrowed; NULL = keep it in
 * SQLite). When set, save/load/clear_session use the keyring; the pin + cache
 * stay in SQLite regardless. Set once, right after open. */
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

#endif /* OC_STORE_H */
