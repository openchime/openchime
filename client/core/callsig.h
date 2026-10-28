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
    /* Who in this call is sharing a screen now, 0 for nobody (REQ-161): an
     * engine sends its share only once the daemon names it, stops when someone
     * takes over, and shows only the sharer's. */
    void (*sharer)(void *ctx, uint64_t user);
    /* The video codecs it can decode, OC_CALL_CODEC_* bits, told on joining. */
    uint8_t codecs;
    /* The connection transport (PROTOCOL.md §5.17), for when UDP cannot reach
     * the relay. Before start, the core hands the engine `send` -- which queues
     * one packet ({seq, ciphertext}) to go out as CALL_MEDIA, from any thread,
     * never blocking -- or NULL when the daemon offers no such transport. The
     * core hands the engine every CALL_MEDIA that comes back through `rx_tcp`.
     * Either may be NULL in an engine that has no use for them. */
    void (*tcp)(void *ctx, int (*send)(void *sctx, uint16_t seq, const uint8_t *ct, size_t len),
                void *sctx);
    void (*rx_tcp)(void *ctx, uint64_t sender, uint16_t seq, const uint8_t *ct, size_t len);
    /* Nonzero once UDP that worked has gone quiet (AUDIO.md §4): the path has
     * most likely moved to an address the relay will not take for this token,
     * and the core rejoins for a fresh one. NULL in an engine that cannot tell. */
    int  (*lost)(void *ctx);
} oc_call_media;

/* Getting back into a call this device was put out of (PROTOCOL.md §5.17): at
 * most OC_CALLSIG_REJOINS rejoins in any OC_CALLSIG_REJOIN_MS, and given up
 * that long after the loss; after a reconnect, given up when the call is not
 * reported within OC_CALLSIG_STATE_WAIT_MS of signing in. */
#define OC_CALLSIG_REJOINS         3
#define OC_CALLSIG_REJOIN_MS       60000u
#define OC_CALLSIG_STATE_WAIT_MS   5000u

/* Packets queued for the connection, oldest first; past OC_CALLSIG_TCPQ the
 * oldest is dropped, as a UDP one would be. */
#define OC_CALLSIG_TCPQ  64
#define OC_CALLSIG_TCPMAX 1300

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

    int      tcp_ok;               /* the daemon offers calls-tcp (CAPABILITIES) */
    oc_mutex_t tq_mu;              /* guards the queue: filled by the engine's threads */
    struct { uint16_t seq, len; uint8_t ct[OC_CALLSIG_TCPMAX]; } tq[OC_CALLSIG_TCPQ];
    int      tq_head, tq_n;
    void   (*wake)(void *wctx);    /* ends the connection thread's wait: a packet is queued */
    void    *wake_ctx;

    /* A call this device is getting back into: put out of it by its connection,
     * a sweep or a moved media path, not by its own act. */
    struct {
        int      on;
        uint64_t channel_id, call_id;
        uint64_t since_ms;         /* when it was lost */
        uint64_t authed_ms;        /* a connection signed in since; 0 = none yet */
        int      sent;             /* its CALL_JOIN went on this connection */
    } rj;
    uint64_t rj_at[OC_CALLSIG_REJOINS];   /* when the last rejoins were sent */
    uint64_t rejoin_ms, state_wait_ms;    /* a test's shorter bounds; 0 = the defaults */
    uint64_t now_ms;                      /* the clock, as last told (lost, authed, tick) */
} oc_callsig;

void oc_callsig_init(oc_callsig *cs);
void oc_callsig_destroy(oc_callsig *cs);
void oc_callsig_set_media(oc_callsig *cs, const oc_call_media *media, void *ctx);
/* What the connection thread gives the queue to wake it with (NULL: nothing).
 * Once this returns with NULL, `wake` is not called again. */
void oc_callsig_set_wake(oc_callsig *cs, void (*wake)(void *wctx), void *wctx);

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

/* The connection dropped. A call this device was in is held for it by the
 * daemon for a while (PROTOCOL.md §5.17); it is rejoined once a connection
 * signs in again and the daemon still reports it. `now_ms` is the caller's
 * clock, the one oc_callsig_authed and oc_callsig_tick are given. */
void oc_callsig_lost(oc_callsig *cs, oc_queue *to_ui, uint64_t now_ms);

/* A connection signed in. */
void oc_callsig_authed(oc_callsig *cs, uint64_t now_ms);

/* Going on purpose (the client is closing): a CALL_LEAVE for the call this
 * device is in, and no rejoin. */
void oc_callsig_quit(oc_callsig *cs, oc_callsig_write write, void *wctx, oc_queue *to_ui);

/* A call request was refused: a rejoin waiting on one is given up. */
void oc_callsig_refused(oc_callsig *cs, oc_queue *to_ui);

/* Time passing: a moved media path is rejoined, and a rejoin past its bounds is
 * given up. `write` NULL while there is no connection. */
void oc_callsig_tick(oc_callsig *cs, uint64_t now_ms, oc_callsig_write write, void *wctx, oc_queue *to_ui);

/* Write the media packets the engine queued for the connection, as CALL_MEDIA.
 * The network thread calls it at the top of every turn, and is woken to (see
 * oc_callsig_set_wake) when a packet is queued. Returns 0. */
int oc_callsig_pump(oc_callsig *cs, oc_callsig_write write, void *wctx);

#endif /* OC_CALLSIG_H */
