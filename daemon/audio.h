#ifndef OC_AUDIO_H
#define OC_AUDIO_H

#include <stdint.h>

/* Call media on the wire (REQ-150/151, ARCH-18/73). The relay that carries it
 * runs in the daemon's net loop (relay.h). It never decodes the audio -- it
 * forwards opaque payloads, so there is no codec dependency here -- and cannot:
 * every payload is an SFrame ciphertext whose keys only the participants'
 * devices hold (ARCH-113). Clients speak to it directly over UDP. */

/* Per-join bearer token that identifies (call, participant) to the relay:
 * OPENCHIME_AUDIO_TOKEN_PREFIX, the same for every token this daemon issues and
 * there for a front door to route by, then OC_AUDIO_TOKEN_RAND random bytes.
 * Every token one daemon issues has the one length; the relay takes it from
 * its first participant. A client holds up to OC_AUDIO_TOKEN_MAX. */
#define OC_AUDIO_TOKEN_RAND   16u
#define OC_AUDIO_PREFIX_MAX   16u
#define OC_AUDIO_TOKEN_MAX    (OC_AUDIO_PREFIX_MAX + OC_AUDIO_TOKEN_RAND)

/* UDP wire format.
 *   client -> relay : token seq(u16 BE) payload...
 *   relay -> client : sender_user_id(u64 BE) seq(u16 BE) payload...
 * A client sends an initial packet (empty payload allowed) so the relay learns
 * its UDP source address before anyone speaks to it. An empty payload -- a
 * keepalive -- is answered to its sender alone, which is how a client learns the
 * relay can hear it. The relay answers each participant from the address that
 * participant's packets arrived at, which on a host with several is not the one
 * the kernel would pick. */
#define OC_AUDIO_S2C_HDR   (8u + 2u)                   /* sender id + seq */
/* One UDP datagram. 1,300 bytes: what a hosting platform's UDP path is
 * documented to carry, below the 1,380 measured through one (AUDIO.md §4). */
#define OC_AUDIO_MAX_PACKET 1300u

/* Drop a participant that has sent no UDP packet in this long (REQ-152 media-side
 * mirror; the daemon also revokes on TCP disconnect), and report it gone. A client
 * in a call sends at least every 5 s (CALLS.md §3), so only the vanished are
 * swept. */
#define OC_AUDIO_SILENCE_MS 20000u

/* How long a call holds the seat of a participant whose connection went, for
 * the client to reconnect and rejoin (REQ-152): off the relay meanwhile, still
 * in the roster. Past it the seat is dropped as a leave would be. */
#define OC_CALL_REJOIN_GRACE_MS 15000u

#endif /* OC_AUDIO_H */
