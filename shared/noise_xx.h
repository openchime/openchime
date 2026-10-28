/*
 * Noise_XX_25519_AESGCM_SHA256: the handshake that starts a private DM
 * (PRIVATE-DM.md). Three messages, relayed by the daemon, which can read none of
 * them usefully:
 *
 *     -> e
 *     <- e, ee, s, es
 *     -> s, se
 *
 * Each side proves its static key (the device key it already holds for calls,
 * ARCH-113) and both mix fresh ephemerals, so the result is authenticated in both
 * directions and forward secret without a signature scheme. The names follow the
 * Noise specification (revision 34) so the code reads against it section by
 * section. Over mbedTLS; secrets are wiped as they stop being needed.
 */

#ifndef OC_NOISE_XX_H
#define OC_NOISE_XX_H

#include <stddef.h>
#include <stdint.h>

#define OC_NOISE_DHLEN   32u
#define OC_NOISE_HASHLEN 32u
#define OC_NOISE_TAGLEN  16u
#define OC_NOISE_MAXMSG  65535u   /* the specification's message limit */

/* One direction of a transport after the handshake (§5.1): a key and a counter. */
typedef struct {
    uint8_t  k[32];
    uint64_t n;
    int      has_k;
} oc_noise_cipher;

typedef struct {
    /* SymmetricState (§5.2) */
    uint8_t ck[OC_NOISE_HASHLEN];
    uint8_t h[OC_NOISE_HASHLEN];
    oc_noise_cipher cs;
    /* HandshakeState (§5.3) */
    int     initiator;
    int     step;                      /* messages written or read so far, 0..3 */
    uint8_t s_sk[OC_NOISE_DHLEN], s_pk[OC_NOISE_DHLEN];
    uint8_t e_sk[OC_NOISE_DHLEN], e_pk[OC_NOISE_DHLEN];
    uint8_t rs[OC_NOISE_DHLEN], re[OC_NOISE_DHLEN];
    int     has_e_fixed;               /* tests only: the ephemeral was given */
} oc_noise_hs;

/* Start a handshake as initiator or responder, with the static private key
 * `s_sk` and a prologue both sides must give identically (§6). 0 on success. */
int oc_noise_xx_init(oc_noise_hs *hs, int initiator,
                     const uint8_t *prologue, size_t prologue_len,
                     const uint8_t s_sk[OC_NOISE_DHLEN]);

/* oc_noise_xx_init with the ephemeral key given rather than made: the published
 * test vectors fix it. Nothing but a test has a reason to call this. */
int oc_noise_xx_init_with(oc_noise_hs *hs, int initiator,
                          const uint8_t *prologue, size_t prologue_len,
                          const uint8_t s_sk[OC_NOISE_DHLEN], const uint8_t e_sk[OC_NOISE_DHLEN]);

/* Write this side's next handshake message carrying `payload` (encrypted once a
 * key exists). Writes at most `cap` bytes to `out` and its length to `out_len`.
 * 0 on success; -1 if it is not this side's turn or anything fails. */
int oc_noise_xx_write(oc_noise_hs *hs, const uint8_t *payload, size_t payload_len,
                      uint8_t *out, size_t cap, size_t *out_len);

/* Read the other side's next handshake message; its payload goes to `payload`
 * (at most `cap` bytes). 0 on success; -1 on a wrong turn, a short or altered
 * message, or a small-order key. */
int oc_noise_xx_read(oc_noise_hs *hs, const uint8_t *msg, size_t msg_len,
                     uint8_t *payload, size_t cap, size_t *payload_len);

/* What a finished handshake hands on: the two transport ciphers of §5.2 Split
 * (`send` is this side's), the peer's static public key, the handshake hash
 * (which binds everything said, and both safety numbers derive from it), and what
 * the private DM's Double Ratchet starts from: a root key, a third HKDF output of
 * the final chaining key, and the responder's handshake ephemeral as Bob's first
 * ratchet key (its private half on the responder, its public half on the
 * initiator). */
typedef struct {
    oc_noise_cipher send, recv;
    uint8_t rs[OC_NOISE_DHLEN];
    uint8_t hh[OC_NOISE_HASHLEN];
    uint8_t rk[OC_NOISE_HASHLEN];
    uint8_t bob_sk[OC_NOISE_DHLEN];   /* responder only */
    uint8_t bob_pk[OC_NOISE_DHLEN];
} oc_noise_result;

/* After the third message. Wipes the handshake. 0 on success; -1 before the
 * handshake is done. */
int oc_noise_xx_split(oc_noise_hs *hs, oc_noise_result *out);

/* The transport messages of §5.1: AES-256-GCM with the counter as nonce. `out`
 * holds pt_len + OC_NOISE_TAGLEN; decrypt writes ct_len - OC_NOISE_TAGLEN. A
 * failed decryption leaves the counter where it was and `out` zeroed. */
int oc_noise_encrypt(oc_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                     const uint8_t *pt, size_t pt_len, uint8_t *out);
int oc_noise_decrypt(oc_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t ct_len, uint8_t *out);

/* Zero everything a handshake holds. */
void oc_noise_xx_wipe(oc_noise_hs *hs);

#endif /* OC_NOISE_XX_H */
