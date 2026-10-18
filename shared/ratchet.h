/*
 * The Double Ratchet that carries a private DM's messages (PRIVATE-DM.md), after
 * the Signal specification ("The Double Ratchet Algorithm", revision 4) with
 * X25519, HKDF-SHA256 and AES-256-GCM:
 *
 * - every message key is derived once from a sending or receiving chain, used for
 *   one message and wiped;
 * - every change of speaker brings a new X25519 ratchet key, so a key taken from
 *   a device stops working after the next reply in each direction;
 * - messages may arrive out of order: the keys of skipped messages are kept, at
 *   most OC_RATCHET_MAX_SKIP of them, and each is wiped when used.
 *
 * It starts from the Noise handshake (noise_xx.h): the root key, and the
 * responder's handshake ephemeral as Bob's first ratchet key. The initiator
 * (Alice) can send at once; the responder (Bob) sends after Alice's first.
 * The state lives in memory only and is wiped by oc_ratchet_wipe.
 */

#ifndef OC_RATCHET_H
#define OC_RATCHET_H

#include <stddef.h>
#include <stdint.h>

#define OC_RATCHET_KEYLEN    32u
#define OC_RATCHET_HEADERLEN 40u   /* ratchet public key, previous chain length, number */
#define OC_RATCHET_TAGLEN    16u
#define OC_RATCHET_MAX_SKIP  256u  /* skipped message keys kept, across all chains */

typedef struct {
    uint8_t  dh[OC_RATCHET_KEYLEN];
    uint32_t n;
    uint8_t  mk[OC_RATCHET_KEYLEN];
    int      used;
} oc_ratchet_skipped;

typedef struct {
    uint8_t  dhs_sk[OC_RATCHET_KEYLEN], dhs_pk[OC_RATCHET_KEYLEN];   /* our ratchet key */
    uint8_t  dhr[OC_RATCHET_KEYLEN];                                 /* theirs */
    int      has_dhr;
    uint8_t  rk[OC_RATCHET_KEYLEN];
    uint8_t  cks[OC_RATCHET_KEYLEN], ckr[OC_RATCHET_KEYLEN];
    int      has_cks, has_ckr;
    uint32_t ns, nr, pn;
    uint8_t  ad[OC_RATCHET_KEYLEN];      /* the handshake hash: binds every message to the session */
    oc_ratchet_skipped skipped[OC_RATCHET_MAX_SKIP];
} oc_ratchet;

/* Alice: the root key and Bob's ratchet public key from the handshake. */
int oc_ratchet_init_alice(oc_ratchet *r, const uint8_t rk[OC_RATCHET_KEYLEN],
                          const uint8_t bob_pk[OC_RATCHET_KEYLEN], const uint8_t ad[OC_RATCHET_KEYLEN]);
/* Bob: the root key and his own ratchet key pair (his handshake ephemeral). */
int oc_ratchet_init_bob(oc_ratchet *r, const uint8_t rk[OC_RATCHET_KEYLEN],
                        const uint8_t bob_sk[OC_RATCHET_KEYLEN], const uint8_t bob_pk[OC_RATCHET_KEYLEN],
                        const uint8_t ad[OC_RATCHET_KEYLEN]);

/* Encrypt one message: writes the header to `header` and pt_len + OC_RATCHET_TAGLEN
 * bytes to `ct`. 0 on success; -1 when this side cannot send yet (Bob before his
 * first message from Alice) or anything fails. */
int oc_ratchet_encrypt(oc_ratchet *r, const uint8_t *pt, size_t pt_len,
                       uint8_t header[OC_RATCHET_HEADERLEN], uint8_t *ct);

/* Decrypt one message: writes ct_len - OC_RATCHET_TAGLEN bytes to `pt`. 0 on
 * success. On any failure (altered bytes, a replay, a key already used, more
 * than OC_RATCHET_MAX_SKIP skipped) the state is left exactly as it was and `pt`
 * zeroed. */
int oc_ratchet_decrypt(oc_ratchet *r, const uint8_t header[OC_RATCHET_HEADERLEN],
                       const uint8_t *ct, size_t ct_len, uint8_t *pt);

/* Zero the whole state. */
void oc_ratchet_wipe(oc_ratchet *r);

#endif /* OC_RATCHET_H */
