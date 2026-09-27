/*
 * The call media relay (REQ-150/151, ARCH-18/73), run by the net loop.
 *
 * It forwards opaque payloads between the participants of a call -- an SFU that
 * never decodes, and cannot: every payload is an SFrame ciphertext whose keys
 * only the participants' devices hold (ARCH-113). The net loop owns the UDP
 * socket, watches it with the rest of its descriptors, and calls
 * oc_relay_on_readable when it is readable; it authorizes and revokes
 * participants as calls change, and calls oc_relay_sweep on its tick. Nothing
 * here blocks, allocates per packet, or takes a lock: it is the loop's.
 *
 * A participant is found by its token, which leads every packet it sends, and a
 * call's participants are a list, so neither the lookup nor the fan-out walks
 * the table (ARCH-22). Each forwarded packet goes to the call's other
 * participants in one sendmmsg.
 *
 * A participant whose network passes no UDP sends and receives over its
 * connection instead (CALL_MEDIA, PROTOCOL.md §5.17): the loop hands such a
 * packet to oc_relay_from_tcp, and the relay hands packets for such a
 * participant back through hooks.tcp_send. A participant is on whichever
 * transport its latest packet came by, so one can move either way mid-call.
 */

#ifndef OPENCHIME_RELAY_H
#define OPENCHIME_RELAY_H

#include <stddef.h>
#include <stdint.h>

#include "audio.h"

typedef struct oc_relay oc_relay;

typedef struct {
    /* The silence sweep dropped this participant (no packet for the silence
     * interval): the loop takes them out of their call. Called after the sweep
     * has finished, so it may authorize and revoke. */
    void (*gone)(void *ctx, const uint8_t *token, size_t len);
    /* A packet for a participant on the connection transport: `sender`'s
     * `seq` and ciphertext, for the loop to write as CALL_MEDIA to connection
     * `conn_id`, or to drop if that connection is backed up. */
    void (*tcp_send)(void *ctx, uint64_t conn_id, uint64_t sender, uint16_t seq,
                     const uint8_t *ct, size_t len);
} oc_relay_hooks;

/* Relay over an already-bound UDP socket, which the caller keeps and closes.
 * Sets it non-blocking, asks for large buffers (a screen share's keyframe is a
 * burst of a hundred packets or more), and turns on the reporting of each
 * packet's destination address, so each participant is answered from the
 * address it wrote to. NULL on allocation failure. */
oc_relay *oc_relay_open(int udp_fd, const oc_relay_hooks *hooks, void *ctx);
void      oc_relay_close(oc_relay *r);

/* Register a participant: `token` (OC_AUDIO_TOKEN_RAND..OC_AUDIO_TOKEN_MAX bytes)
 * becomes theirs, in call `call_id`, speaking as `user_id` from connection
 * `conn_id`. Re-authorizing a known token moves it. 0, or -1 when the table is
 * full or the token is not a usable length. */
int  oc_relay_authorize(oc_relay *r, uint64_t call_id, uint64_t user_id, uint64_t conn_id,
                        const uint8_t *token, size_t len);

/* A packet connection `conn_id` sent as CALL_MEDIA: relayed as a UDP one from
 * that participant would be, and the participant is now on the connection
 * transport. 0, or -1 if the connection is no participant. */
int  oc_relay_from_tcp(oc_relay *r, uint64_t conn_id, uint16_t seq, const uint8_t *ct, size_t len);
void oc_relay_revoke(oc_relay *r, const uint8_t *token, size_t len);

/* Relay up to `max_packets` waiting datagrams. 1 if it stopped at that bound
 * (more may be waiting: call again next turn), 0 once the socket is drained. */
int  oc_relay_on_readable(oc_relay *r, int max_packets);

/* Drop every participant silent for longer than the silence interval, then
 * report each through hooks.gone. */
void oc_relay_sweep(oc_relay *r);

/* The silence interval: OC_AUDIO_SILENCE_MS unless a test shortens it (0 restores). */
void oc_relay_set_silence_ms(oc_relay *r, uint64_t ms);

/* How many participants are registered; for tests. */
size_t oc_relay_count(const oc_relay *r);

#endif /* OPENCHIME_RELAY_H */
