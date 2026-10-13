/*
 * OpenChime auth crypto primitives (AUTH.md §2, §4).
 *
 * Pure, stateless helpers used by the DB-writer's auth path: password hashing
 * (Argon2id, the vendored reference implementation), SHA-256 for session /
 * invite token storage, cryptographic randomness, and constant-time compare.
 * Kept out of dbwriter.c so they can be unit-tested directly (tests/test_auth.c).
 */

#ifndef OPENCHIME_AUTH_H
#define OPENCHIME_AUTH_H

#include <stddef.h>
#include <stdint.h>

#define OC_PW_SALT_LEN        16u      /* per-user random salt */
#define OC_PW_HASH_LEN        32u      /* derived key length (SHA-256 sized) */
/* Argon2id at OWASP's floor (AUTH.md §2): 19 MiB, two passes, one lane. The
 * memory is the one cost the daemon varies (a test lowers it); the passes and
 * lanes are constants, and every hash is stored beside its settings. */
#define OC_PW_MEM_KIB         19456u
#define OC_PW_PASSES          2u
#define OC_PW_LANES           1u
#define OC_PW_KDF_MAX         64       /* "argon2id$v=19$m=…,t=…,p=…" */
#define OC_SHA256_LEN         32u
#define OC_SESSION_TOKEN_BYTES 32u     /* == OC_SESSION_TOKEN_LEN on the wire */

/* Fill `buf` with `n` cryptographically-secure random bytes. 0 / -1. */
int oc_rand_bytes(void *buf, size_t n);

/* out = SHA-256(in). Used to store session / invite tokens by hash only. */
int oc_sha256(const void *in, size_t n, uint8_t out[OC_SHA256_LEN]);

/* out = Argon2id(password, salt) with `mem_kib` of memory and the constant
 * passes and lanes. 0 / -1. */
int oc_pw_derive(const char *password, size_t pwlen,
                 const uint8_t *salt, size_t saltlen,
                 uint32_t mem_kib, uint8_t out[OC_PW_HASH_LEN]);

/* The settings stored beside a hash made with `mem_kib`, in the PHC string
 * form's parameter part: "argon2id$v=19$m=19456,t=2,p=1". */
void oc_pw_kdf_format(uint32_t mem_kib, char out[OC_PW_KDF_MAX]);

/* The memory cost a stored settings string names: 0, or -1 when it is not
 * Argon2id version 19 with this daemon's passes and lanes, which no password
 * then matches. */
int oc_pw_kdf_parse(const char *kdf, uint32_t *mem_kib);

/* Constant-time equality over `n` bytes; returns 1 if equal, 0 otherwise. */
int oc_ct_eq(const void *a, const void *b, size_t n);

#endif /* OPENCHIME_AUTH_H */
