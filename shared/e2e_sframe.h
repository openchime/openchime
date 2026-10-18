/*
 * SFrame (RFC 9605), for encrypting each packet of a call end to end (ARCH-113,
 * CALLS.md §5.4). One suite: AES_128_GCM_SHA256_128 (0x0004) — AES-128-GCM with
 * a 16-byte tag, keys derived with HKDF-SHA256.
 *
 * A packet is header ‖ AEAD(key, salt ^ CTR, header ‖ metadata, plaintext). The
 * header carries the KID naming the key and the CTR making the nonce, and is
 * authenticated. The caller owns the rule that a (KID, CTR) pair is used for one
 * packet only; the call engine keeps it by giving every sender a fresh random
 * key per epoch and restarting the counter with it.
 */

#ifndef OC_E2E_SFRAME_H
#define OC_E2E_SFRAME_H

#include <stddef.h>
#include <stdint.h>

#define OC_SFRAME_SUITE    0x0004u
#define OC_SFRAME_TAG_LEN  16u
#define OC_SFRAME_HDR_MAX  17u     /* config byte, then up to 8 bytes each of KID and CTR */
#define OC_SFRAME_OVERHEAD (OC_SFRAME_HDR_MAX + OC_SFRAME_TAG_LEN)

/* Encode a header for (kid, ctr) into `out` (OC_SFRAME_HDR_MAX bytes of room);
 * returns its length. */
size_t oc_sframe_header_encode(uint64_t kid, uint64_t ctr, uint8_t *out);

/* Parse the header at the front of `in`. 0 on success, with the KID, CTR and the
 * header's length; -1 if it is truncated or not minimally encoded (RFC 9605 §4.3:
 * a value below 8 rides in the config byte, and a longer one in the fewest bytes). */
int oc_sframe_header_decode(const uint8_t *in, size_t len,
                            uint64_t *kid, uint64_t *ctr, size_t *hdr_len);

/* One KID's key and salt, derived from its base key (RFC 9605 §4.4.2). */
typedef struct {
    uint64_t kid;
    uint8_t  key[16];
    uint8_t  salt[12];
    int      ready;
} oc_sframe_key;

/* Derive `k` for `kid` from `base_key`. 0 on success. */
int oc_sframe_key_init(oc_sframe_key *k, uint64_t kid, const uint8_t *base_key, size_t base_len);
/* Forget it: zero the key and salt. */
void oc_sframe_key_wipe(oc_sframe_key *k);

/* Encrypt `pt` under `k` with counter `ctr`, authenticating `meta` (may be empty)
 * as well as the header. Writes header ‖ ciphertext ‖ tag to `out` (at most
 * pt_len + OC_SFRAME_OVERHEAD bytes) and its length to *out_len. 0 on success. */
int oc_sframe_encrypt(const oc_sframe_key *k, uint64_t ctr,
                      const uint8_t *meta, size_t meta_len,
                      const uint8_t *pt, size_t pt_len,
                      uint8_t *out, size_t cap, size_t *out_len);

/* Decrypt an SFrame ciphertext whose header's KID is `k`'s (the caller looked it
 * up with oc_sframe_header_decode). Writes the plaintext to `pt` and its length to
 * *pt_len, and the CTR to *ctr for the replay window. 0 on success; -1 on any
 * failure, which the caller drops without saying why. */
int oc_sframe_decrypt(const oc_sframe_key *k,
                      const uint8_t *meta, size_t meta_len,
                      const uint8_t *in, size_t len,
                      uint8_t *pt, size_t cap, size_t *pt_len, uint64_t *ctr);

/* A receiver's replay window over one KID's counters: the newest counter seen
 * and which of the OC_SFRAME_REPLAY_WINDOW before it have been. A counter older
 * than the window, or already seen, is refused. Zero-initialise to start. */
#define OC_SFRAME_REPLAY_WINDOW 1024u
typedef struct {
    uint64_t top;
    uint64_t seen[OC_SFRAME_REPLAY_WINDOW / 64];
    int      any;
} oc_sframe_replay;

/* 1 if `ctr` would be accepted, 0 if it is a replay or too old. Ask before
 * decrypting, mark after it authenticated: marking an unauthenticated counter
 * would let a forger burn future counters. */
int  oc_sframe_replay_ok(const oc_sframe_replay *w, uint64_t ctr);
void oc_sframe_replay_mark(oc_sframe_replay *w, uint64_t ctr);

#endif /* OC_E2E_SFRAME_H */
