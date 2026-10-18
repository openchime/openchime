/*
 * A local account's second step (AUTH.md §8.6, REQ-184): time-based one-time
 * passwords (RFC 6238 over RFC 4226, HMAC-SHA1, six digits, thirty-second
 * steps), the base32 a setup key is shown in, and the sealing that keeps each
 * secret encrypted under a key held outside the database.
 *
 * No database, no clock of its own, no allocation; only the factor key's
 * loading touches a file.
 */
#ifndef OPENCHIME_TOTP_H
#define OPENCHIME_TOTP_H

#include <stddef.h>
#include <stdint.h>

#define OC_TOTP_SECRET_LEN 20u   /* 160 bits, RFC 4226's recommendation */
#define OC_TOTP_STEP_S     30u
#define OC_TOTP_DIGITS     6u
#define OC_FACTOR_KEY_LEN  32u   /* AES-256 */
/* A sealed secret: a 12-byte nonce, the ciphertext, a 16-byte tag. */
#define OC_TOTP_SEALED_LEN (12u + OC_TOTP_SECRET_LEN + 16u)

/* The code for `step` (RFC 4226's HOTP of the step count), 0..999999. */
uint32_t oc_totp_code(const uint8_t *secret, size_t len, uint64_t step);

/* Whether `code` (six ASCII digits) is the code for the step `now_s` falls in,
 * or the one before or after -- a clock a little off is allowed -- and for a
 * step after `last_step`, so a code is never good twice. 1 with the step it
 * matched in *matched, else 0. */
int oc_totp_verify(const uint8_t *secret, size_t len, const char *code, uint64_t now_s,
                   uint64_t last_step, uint64_t *matched);

/* RFC 4648 base32, no padding: what a person types into an authenticator. The
 * text and its length; -1 if `cap` is too small. */
int oc_base32_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* Seal a secret under the factor key, bound to the account it is for (`uid` is
 * the associated data, so a sealed secret moved to another account does not
 * open). 0, or -1. */
int oc_factor_seal(const uint8_t key[OC_FACTOR_KEY_LEN], uint64_t uid,
                   const uint8_t secret[OC_TOTP_SECRET_LEN], uint8_t out[OC_TOTP_SEALED_LEN]);
/* Open one: 0 and the secret, or -1 -- the wrong key, the wrong account, or a
 * value changed -- with `secret` wiped. */
int oc_factor_open(const uint8_t key[OC_FACTOR_KEY_LEN], uint64_t uid,
                   const uint8_t *sealed, size_t len, uint8_t secret[OC_TOTP_SECRET_LEN]);

/* The factor key at `path`: read, or made -- 32 random bytes, mode 0600 -- when
 * there is none yet. 0 read, 1 made, -1 (and why in `err`) when the file is
 * there but not a key, or cannot be read or made: the caller goes on without
 * one, and no second step can be passed until it is fixed. */
int oc_factor_key_load(const char *path, uint8_t key[OC_FACTOR_KEY_LEN], char *err, size_t errcap);

#endif /* OPENCHIME_TOTP_H */
