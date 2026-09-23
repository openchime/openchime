#ifndef OC_AUDIO_H
#define OC_AUDIO_H

#include <signal.h>
#include <stdint.h>

/* Audio relay sidecar (REQ-150/151, ARCH-18/28/31). A separate process forked
 * from the daemon that relays media over UDP between the participants of a
 * call. It never decodes the audio — it forwards opaque payloads (an SFU), so
 * there is no codec dependency here — and cannot: every payload is an SFrame
 * ciphertext whose keys only the participants' devices hold (ARCH-113). The daemon drives it over a Unix-domain
 * socket (authorize/revoke tokens); clients speak to it directly over UDP. */

/* Per-join bearer token that identifies (call, participant) to the sidecar:
 * OPENCHIME_AUDIO_TOKEN_PREFIX, the same for every token this daemon issues and
 * there for a front door to route by, then OC_AUDIO_TOKEN_RAND random bytes.
 * Every token one daemon issues has the one length; the sidecar takes it from
 * AUTHORIZE. A client holds up to OC_AUDIO_TOKEN_MAX. */
#define OC_AUDIO_TOKEN_RAND   16u
#define OC_AUDIO_PREFIX_MAX   16u
#define OC_AUDIO_TOKEN_MAX    (OC_AUDIO_PREFIX_MAX + OC_AUDIO_TOKEN_RAND)

/* IPC framing, both ways: u32 length (of type+payload, big-endian) then a u8
 * type then the payload. A token is the rest of the message after its fixed
 * fields.
 *   daemon -> sidecar
 *     AUTHORIZE : call_id(u64) user_id(u64) token  -- register a participant
 *     REVOKE    : token                            -- drop a participant
 *   sidecar -> daemon
 *     GONE      : token   -- the silence sweep dropped this participant, so the
 *                            daemon takes it out of the call too */
enum { OC_AUDIO_IPC_AUTHORIZE = 1, OC_AUDIO_IPC_REVOKE = 2, OC_AUDIO_IPC_GONE = 3 };

/* UDP wire format.
 *   client -> sidecar : token seq(u16 BE) payload...
 *   sidecar -> client : sender_user_id(u64 BE) seq(u16 BE) payload...
 * A client sends an initial packet (empty payload allowed) so the sidecar learns
 * its UDP source address before anyone speaks to it. The sidecar answers each
 * participant from the address that participant's packets arrived at, which on a
 * host with several is not the one the kernel would pick. */
#define OC_AUDIO_S2C_HDR   (8u + 2u)                   /* sender id + seq */
/* One UDP datagram. 1,300 bytes: what a hosting platform's UDP path is
 * documented to carry, below the 1,380 measured through one (AUDIO.md §4). */
#define OC_AUDIO_MAX_PACKET 1300u

/* Drop a participant that has sent no UDP packet in this long (REQ-152 media-side
 * mirror; the daemon also revokes on TCP disconnect), and report it GONE. A client
 * in a call sends at least every 5 s (CALLS.md §3), so only the vanished are
 * swept. */
#define OC_AUDIO_SILENCE_MS 20000u

/* Run the relay loop over an already-bound `udp_fd` and the daemon IPC `ipc_fd`,
 * until *stop is non-zero. Returns 0 on clean shutdown. Used both as the forked
 * sidecar's entry point and, in tests, driven directly on a thread. */
int oc_audio_sidecar_run(int ipc_fd, int udp_fd, volatile sig_atomic_t *stop);

/* Sweep after `ms` of silence rather than OC_AUDIO_SILENCE_MS; a test's knob, so
 * the GONE report can be seen without waiting twenty seconds. */
void oc_audio_sidecar_set_silence_ms(uint64_t ms);

#endif /* OC_AUDIO_H */
