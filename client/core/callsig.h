/*
 * OpenChime client — call signaling and media keys (REQ-150, REQ-301-305,
 * ARCH-113, docs/CALLS.md §4-§5).
 *
 * Runs on the network thread, which owns the connection and the credential
 * store: it answers the CALL_* frames, keeps this device's key pair, makes and
 * seals a fresh media key on every epoch, opens the keys the others seal to it,
 * and hands the media engine what it needs. The engine itself — capture, Opus,
 * SFrame, the relay socket, jitter buffers, the mixer — is behind the
 * oc_call_media seam, so the core links no codec (the TUI has no calls) and a
 * frontend that does calls plugs one in (client/core/call/).
 */

#ifndef OC_CALLSIG_H
#define OC_CALLSIG_H

#include <stddef.h>
#include <stdint.h>

#include "event.h"
#include "oc_thread.h"
#include "protocol.h"
#include "queue.h"
#include "store.h"

#define OC_CALL_KEY_LEN 16u   /* an SFrame base key */

/* What the core tells a media engine. Every call comes from the network thread;
 * an engine does its own locking. */
typedef struct {
    /* In a call: the relay at host:port, this device's media token and slot.
     * 0 on success. */
    int  (*start)(void *ctx, const char *host, uint16_t port, const uint8_t *token, size_t token_len,
                  uint64_t self_user, uint8_t self_slot);
    /* The participants now, and the epoch: an engine forgets senders no longer
     * here and knows which slot is whose. */
    void (*roster)(void *ctx, uint32_t epoch, const oc_call_part *parts, int n);
    /* This device's key for `epoch`: encrypt with it once the grace has passed
     * (CALLS.md §5.3). */
    void (*tx_key)(void *ctx, uint32_t epoch, uint8_t slot, const uint8_t key[OC_CALL_KEY_LEN]);
    /* A sender's key for `epoch`. */
    void (*rx_key)(void *ctx, uint64_t user, uint8_t slot, uint32_t epoch,
                   const uint8_t key[OC_CALL_KEY_LEN]);
    /* Out of the call. */
    void (*stop)(void *ctx);
} oc_call_media;

/* Sends one encoded frame on the connection; 0 on success. */
typedef int (*oc_callsig_write)(void *wctx, const uint8_t *buf, size_t len);

typedef struct {
    oc_mutex_t           mu;       /* guards media/mctx, set from the UI thread */
    const oc_call_media *media;
    void                *mctx;

    int      have_key;
    uint8_t  sk[32], pk[32];       /* this device's key pair (CALLS.md §5.2) */

    int      in_call;
    uint64_t channel_id, call_id, self_user;
    uint8_t  slot;
    uint32_t epoch;
    uint16_t n;
    oc_call_part parts[OC_MAX_CALL_PARTICIPANTS];
    char     host[256];
} oc_callsig;

void oc_callsig_init(oc_callsig *cs);
void oc_callsig_destroy(oc_callsig *cs);
void oc_callsig_set_media(oc_callsig *cs, const oc_call_media *media, void *ctx);

/* The info HPKE binds a sealed key to (CALLS.md §5.3); `out` holds
 * OC_CALLSIG_INFO_LEN bytes. Exposed for the tests. */
#define OC_CALLSIG_INFO_LEN (21u + 8u + 4u + 8u + 8u)
void oc_callsig_info(uint64_t call_id, uint32_t epoch, uint64_t sender, uint64_t recipient,
                     uint8_t out[OC_CALLSIG_INFO_LEN]);

/* A UI command: OC_CMD_CALL_*. `store`/`workspace` find or make the device key
 * a join publishes. 0 on success. */
int oc_callsig_command(oc_callsig *cs, const oc_cmd *c, oc_store *store, const char *workspace,
                       oc_callsig_write write, void *wctx, oc_queue *to_ui);

/* A server frame. Returns 1 if it was a call frame and was handled, 0 if it is
 * not one, -1 if it was malformed. Events for the model go to `to_ui`. */
int oc_callsig_frame(oc_callsig *cs, uint16_t type, oc_rbuf *p, const char *host,
                     oc_callsig_write write, void *wctx, oc_queue *to_ui);

/* The connection dropped: the daemon took this device out of any call. */
void oc_callsig_lost(oc_callsig *cs, oc_queue *to_ui);

#endif /* OC_CALLSIG_H */
