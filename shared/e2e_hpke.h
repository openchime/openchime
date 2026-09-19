/*
 * HPKE (RFC 9180) in Auth mode, for carrying a call's media keys from one device
 * to another (ARCH-113, CALLS.md §5). One suite only:
 * DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, AES-128-GCM — kem 0x0020, kdf 0x0001,
 * aead 0x0001 — and the single-shot form: one message per sealing, sequence 0.
 *
 * Auth mode authenticates the sender's static key as well as encrypting to the
 * receiver's, so opening succeeds only with the key of the device the roster says
 * sent it. That is what lets a call do without a signature scheme.
 *
 * Over mbedTLS, which both builds already vendor. Secrets are wiped before return.
 */

#ifndef OC_E2E_HPKE_H
#define OC_E2E_HPKE_H

#include <stddef.h>
#include <stdint.h>

#define OC_X25519_LEN    32u   /* a private key, a public key, an `enc` */
#define OC_HPKE_TAG_LEN  16u   /* AES-128-GCM's tag, appended to the ciphertext */

/* Fill `buf` from the operating system's entropy through mbedTLS's CTR-DRBG.
 * 0 on success. There is no fallback: a key made from anything weaker is worse
 * than no call. */
int oc_e2e_random(void *buf, size_t n);

/* A fresh X25519 key pair; 0 on success. */
int oc_x25519_keypair(uint8_t sk[OC_X25519_LEN], uint8_t pk[OC_X25519_LEN]);
/* The public key of `sk`; 0 on success. */
int oc_x25519_public(const uint8_t sk[OC_X25519_LEN], uint8_t pk[OC_X25519_LEN]);

/* Seal `pt` to the receiver `pk_r`, authenticated as the holder of `sk_s`.
 * Writes the 32-byte encapsulated key to `enc` and pt_len + OC_HPKE_TAG_LEN bytes
 * to `ct`. 0 on success. */
int oc_hpke_seal_auth(const uint8_t pk_r[OC_X25519_LEN], const uint8_t sk_s[OC_X25519_LEN],
                      const uint8_t *info, size_t info_len,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, size_t pt_len,
                      uint8_t enc[OC_X25519_LEN], uint8_t *ct);

/* Open what oc_hpke_seal_auth sealed: the receiver's `sk_r`, the sender's public
 * `pk_s`. Writes ct_len - OC_HPKE_TAG_LEN bytes to `pt`. 0 on success; anything
 * else — a wrong key, another sender, altered bytes, a different info or aad —
 * fails, and `pt` is zeroed. */
int oc_hpke_open_auth(const uint8_t enc[OC_X25519_LEN], const uint8_t sk_r[OC_X25519_LEN],
                      const uint8_t pk_s[OC_X25519_LEN],
                      const uint8_t *info, size_t info_len,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *ct, size_t ct_len, uint8_t *pt);

/* oc_hpke_seal_auth with the ephemeral key given rather than made: the RFC's test
 * vectors fix it. Nothing but a test has a reason to call this. */
int oc_hpke_seal_auth_with(const uint8_t sk_e[OC_X25519_LEN],
                           const uint8_t pk_r[OC_X25519_LEN], const uint8_t sk_s[OC_X25519_LEN],
                           const uint8_t *info, size_t info_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t enc[OC_X25519_LEN], uint8_t *ct);

/* Zero `n` bytes at `p` in a way the compiler may not remove. */
void oc_e2e_wipe(void *p, size_t n);

#endif /* OC_E2E_HPKE_H */
