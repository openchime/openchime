/*
 * OpenChime auth crypto primitives (AUTH.md §2, §4).
 *
 * Pure, stateless helpers used by the DB-writer's auth path: password hashing
 * (PBKDF2-HMAC-SHA256 via mbedTLS — no new dependency), SHA-256 for session /
 * invite token storage, cryptographic randomness, and constant-time compare.
 * Kept out of dbwriter.c so they can be unit-tested directly (tests/test_auth.c).
 */

#ifndef OPENCHIME_AUTH_H
#define OPENCHIME_AUTH_H

#include <stddef.h>
#include <stdint.h>

#define OC_PW_SALT_LEN        16u      /* per-user random salt */
#define OC_PW_HASH_LEN        32u      /* derived key length (SHA-256 sized) */
#define OC_PW_ITERATIONS      600000u  /* PBKDF2 default, OWASP-tier (AUTH.md §2) */
#define OC_SHA256_LEN         32u
#define OC_SESSION_TOKEN_BYTES 32u     /* == OC_SESSION_TOKEN_LEN on the wire */

/* Fill `buf` with `n` cryptographically-secure random bytes. 0 / -1. */
int oc_rand_bytes(void *buf, size_t n);

/* out = SHA-256(in). Used to store session / invite tokens by hash only. */
int oc_sha256(const void *in, size_t n, uint8_t out[OC_SHA256_LEN]);

/* out = PBKDF2-HMAC-SHA256(password, salt, iterations). 0 / -1. */
int oc_pw_derive(const char *password, size_t pwlen,
                 const uint8_t *salt, size_t saltlen,
                 uint32_t iterations, uint8_t out[OC_PW_HASH_LEN]);

/* Constant-time equality over `n` bytes; returns 1 if equal, 0 otherwise. */
int oc_ct_eq(const void *a, const void *b, size_t n);

#endif /* OPENCHIME_AUTH_H */
