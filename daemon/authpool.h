/*
 * The auth pool (AUTH.md §2, ARCH-66): a few threads that check a password
 * against a stored credential, so the key derivation -- deliberately the
 * slowest thing a sign-in does -- runs on neither the database writer nor a
 * reader, and a burst of sign-ins holds up nobody's sends or reads.
 *
 * It knows nothing of the database. The caller fetches the credential, hands
 * the pool a check, and gets the same check back through `done`, called on a
 * pool thread, with `ok` filled in. A check that carries a new password -- a
 * password change -- also has the new one's key derived, once the old one has
 * matched. A check handed over is the pool's until it comes back; every check
 * handed over comes back exactly once.
 */
#ifndef OPENCHIME_AUTHPOOL_H
#define OPENCHIME_AUTHPOOL_H

#include <stddef.h>
#include <stdint.h>

#include "auth.h"

typedef struct oc_authpool oc_authpool;

typedef struct oc_auth_check {
    const char   *password;               /* borrowed: the owner keeps it alive */
    size_t        pwlen;
    uint8_t       salt[64];
    size_t        slen;
    uint32_t      iters;
    uint8_t       stored[OC_PW_HASH_LEN];
    /* A password change: the new password (borrowed), and the salt and count to
     * derive it with. NULL for a sign-in. A check with no `password` derives
     * the new one only: a reset, whose one-time link stood in for the old. */
    const char   *new_password;
    size_t        new_pwlen;
    uint8_t       new_salt[OC_PW_SALT_LEN];
    uint32_t      new_iters;
    int           ok;                     /* out: 1 matched, 0 did not, -1 never checked (stopping) */
    int           derived;                /* out: the new password's key is in new_hash */
    uint8_t       new_hash[OC_PW_HASH_LEN];
    void         *owner;                  /* the caller's */
    struct oc_auth_check *next;           /* queue linkage; not for callers */
} oc_auth_check;

typedef void (*oc_authpool_done)(oc_auth_check *chk, void *ctx);

/* Start `nthreads` threads (at least 1). NULL if none would start. */
oc_authpool *oc_authpool_start(int nthreads, oc_authpool_done done, void *ctx);

/* Queue a check. Once the pool is stopping, the check comes straight back with
 * ok = -1, on the calling thread. */
void oc_authpool_submit(oc_authpool *p, oc_auth_check *chk);

/* Hold (1) or release (0) the queue: while held, checks wait and none is
 * started. A test's knob, for putting something between a credential's fetch
 * and its check. How many are waiting, for the same. */
void   oc_authpool_hold(oc_authpool *p, int on);
size_t oc_authpool_waiting(oc_authpool *p);

/* Stop: checks under way finish and come back; checks still queued come back
 * with ok = -1, on the calling thread. Then the threads are joined. */
void oc_authpool_stop(oc_authpool *p);

#endif /* OPENCHIME_AUTHPOOL_H */
