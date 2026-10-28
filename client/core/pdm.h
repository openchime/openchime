/*
 * Encrypted DMs on the client (PRIVATE-DM.md): the handshake and the ratchet of
 * each conversation this device is in, on the network thread, which is the only
 * place their keys exist. The conversation itself is an ordinary DM channel
 * marked e2e: its messages go out and come back as ordinary frames, whose
 * bodies are sealed here on the way out (oc_pdm_seal) and opened here on the
 * way in (oc_pdm_unseal). The UI is told what happens to the session
 * (OC_EV_PDM) and asks for what it wants (OC_CMD_PDM_*); it never sees a key,
 * and nothing here is written anywhere but the remembered fingerprint of each
 * peer's device key (store.h).
 */

#ifndef OC_PDM_H
#define OC_PDM_H

#include <stddef.h>
#include <stdint.h>

#include "event.h"
#include "noise_xx.h"
#include "protocol.h"
#include "queue.h"
#include "ratchet.h"
#include "store.h"

#define OC_PDM_CLIENT_MAX   16
#define OC_PDM_MAX_TEXT     16384u            /* the plaintext of one message */
#define OC_PDM_LIMIT_MSGS   10000u            /* a session ends after this many messages... */
#define OC_PDM_LIMIT_MS     (24ull * 60 * 60 * 1000)   /* ...or this long */
#define OC_PDM_SAFETY_LEN   72                /* 12 groups of 5 digits, spaced, and the NUL */
#define OC_PDM_E2E_MAX      256               /* encrypted DM channels remembered as such */

/* What an encrypted DM's message reads as on a device that cannot open it: one
 * that was not in the conversation, or after its keys are gone. */
#define OC_PDM_UNREADABLE   "\xF0\x9F\x94\x92 Encrypted message: only the device that took part in this conversation can read it."

typedef int (*oc_pdm_write)(void *wctx, const uint8_t *buf, size_t len);

/* A body seen in the conversation and what it says, found by its digest: the
 * ratchet opens each message once, and a device cannot open its own, so every
 * body this device sealed or opened is kept, in memory, for when it comes again
 * (its own message coming back, a catch-up after a reconnect, an edit). */
typedef struct { uint8_t digest[16]; char *text; } oc_pdm_plain;

typedef struct {
    int           used;
    uint64_t      id;              /* the channel; 0 until the daemon names it (PDM_OPENED) */
    uint64_t      peer;
    int           initiator;
    int           state;           /* see pdm.c */
    oc_noise_hs   hs;
    oc_ratchet   *r;               /* once open; heap, wiped on the way out */
    uint8_t       hs1[OC_PDM_MAX_HS];
    size_t        hs1_len;
    uint8_t       peer_pk[OC_NOISE_DHLEN];
    uint32_t      msgs;
    uint64_t      opened_ms;
    oc_pdm_plain *plain;
    size_t        n_plain, cap_plain;
} oc_pdm_sess;

typedef struct {
    int         enabled;         /* the daemon offers it (CAPABILITIES: pdm) */
    uint64_t    self;
    oc_store   *store;
    char        workspace[288];
    int         have_key;
    uint8_t     sk[OC_NOISE_DHLEN], pk[OC_NOISE_DHLEN];
    oc_pdm_sess s[OC_PDM_CLIENT_MAX];
    uint64_t    e2e[OC_PDM_E2E_MAX]; /* channels known to be encrypted DMs */
    size_t      n_e2e;
} oc_pdm;

/* Signed in: who this is, and where the device key and the peers are kept. */
void oc_pdm_authed(oc_pdm *d, uint64_t self, oc_store *store, const char *workspace);

/* A frame from the daemon. 1 when it was a private DM's (handled), 0 when not,
 * -1 when the connection should be dropped. */
int oc_pdm_frame(oc_pdm *d, uint16_t type, oc_rbuf *p, oc_pdm_write write, void *wctx, oc_queue *to_ui);

/* OC_CMD_PDM_OPEN (channel_id: the person), _ACCEPT and _DECLINE (channel_id: the
 * DM), _CLOSE, _TRUST. */
void oc_pdm_command(oc_pdm *d, const oc_cmd *c, oc_pdm_write write, void *wctx, oc_queue *to_ui);

/* A channel the daemon described (CHANNEL_INFO, CHANNEL_LIST): remembered when
 * it is an encrypted DM, so its bodies are sealed and opened. */
void oc_pdm_channel(oc_pdm *d, uint64_t channel_id, int e2e);
int  oc_pdm_is_e2e(const oc_pdm *d, uint64_t channel_id);
/* 1 when this device has the session of encrypted DM `channel_id` (asked for
 * or accepted here); an encrypted DM it has not is another device's, and is
 * not shown here. */
int  oc_pdm_holds(const oc_pdm *d, uint64_t channel_id);

/* `text` sealed for the channel's session: the body to send instead (heap), or
 * NULL when this device cannot send in it now (no session here, not open yet,
 * the other device's key not trusted, too long). */
char *oc_pdm_seal(oc_pdm *d, uint64_t channel_id, const char *text);

/* A body that came in an encrypted DM, opened: its text (heap), or
 * OC_PDM_UNREADABLE when this device cannot read it. An empty body (a deleted
 * message) stays empty. */
char *oc_pdm_unseal(oc_pdm *d, uint64_t channel_id, const uint8_t *body, size_t len);

/* The session limits (OC_PDM_LIMIT_*). */
void oc_pdm_tick(oc_pdm *d, uint64_t now_ms, oc_pdm_write write, void *wctx, oc_queue *to_ui);

/* Leaving on purpose: every private DM is closed, so the other side is told now
 * rather than in 24 hours. */
void oc_pdm_quit(oc_pdm *d, oc_pdm_write write, void *wctx);

/* Forget everything, wiping every key and every text kept. */
void oc_pdm_free(oc_pdm *d);

/* The safety number of two devices (PRIVATE-DM.md §5): the same on both sides. */
void oc_pdm_safety_number(uint64_t user_a, const uint8_t pk_a[32], uint64_t user_b, const uint8_t pk_b[32],
                          char out[OC_PDM_SAFETY_LEN]);

#endif /* OC_PDM_H */
