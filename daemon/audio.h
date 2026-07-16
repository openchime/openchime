#ifndef OC_AUDIO_H
#define OC_AUDIO_H

#include <signal.h>
#include <stdint.h>

/* Audio relay sidecar (REQ-150/151, ARCH-18/28/31). A separate process forked
 * from the daemon that relays Opus media over UDP between the participants of a
 * call. It never decodes the audio — it forwards opaque payloads (an SFU), so
 * there is no codec dependency here. The daemon drives it over a Unix-domain
 * socket (authorize/revoke tokens); clients speak to it directly over UDP. */

/* Per-join bearer token that identifies (call, participant) to the sidecar. */
#define OC_AUDIO_TOKEN_LEN   16u

/* IPC (daemon -> sidecar) framing: u32 length (of type+payload, big-endian) then
 * a u8 type then the payload.
 *   AUTHORIZE : call_id(u64) user_id(u64) token(16)  -- register a participant
 *   REVOKE    : token(16)                            -- drop a participant */
enum { OC_AUDIO_IPC_AUTHORIZE = 1, OC_AUDIO_IPC_REVOKE = 2 };

/* UDP wire format.
 *   client -> sidecar : token(16) seq(u16 BE) opus-payload...
 *   sidecar -> client : sender_user_id(u64 BE) seq(u16 BE) opus-payload...
 * A client sends an initial packet (empty payload allowed) so the sidecar learns
 * its UDP source address before anyone speaks to it. */
#define OC_AUDIO_C2S_HDR   (OC_AUDIO_TOKEN_LEN + 2u)   /* token + seq */
#define OC_AUDIO_S2C_HDR   (8u + 2u)                   /* sender id + seq */
#define OC_AUDIO_MAX_PACKET 1400u                      /* one UDP datagram, sub-MTU */

/* Drop a participant that has sent no UDP packet in this long (REQ-152 media-side
 * mirror; the daemon also revokes on TCP disconnect). */
#define OC_AUDIO_SILENCE_MS 20000u

/* Run the relay loop over an already-bound `udp_fd` and the daemon IPC `ipc_fd`,
 * until *stop is non-zero. Returns 0 on clean shutdown. Used both as the forked
 * sidecar's entry point and, in tests, driven directly on a thread. */
int oc_audio_sidecar_run(int ipc_fd, int udp_fd, volatile sig_atomic_t *stop);

#endif /* OC_AUDIO_H */
