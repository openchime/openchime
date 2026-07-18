/*
 * OpenChime client — the OS credential-store abstraction (ARCH-74). A tiny
 * get/put/del vtable the core uses to keep the session token in the platform
 * secret store (Keychain, Credential Manager, GNOME Keyring/KWallet, …) instead
 * of the plaintext SQLite file. The core defines only this interface and never
 * links any keyring library; a frontend supplies a backend (the TUI's libsecret
 * one on Linux, Windows' Credential Manager later, …). A NULL secret means "no
 * secure store" — the caller then falls back to the SQLite store, which is also
 * what happens on a headless box with no keyring. Only the session TOKEN goes
 * here; the (public) TOFU pin and message cache stay in SQLite.
 */

#ifndef OC_SECRET_H
#define OC_SECRET_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct oc_secret {
    /* Fill `out` (up to `cap`) with the secret bytes for `account`, set *len, and
     * return 1 if found; return 0 if absent or on error. */
    int  (*get)(void *ctx, const char *account, uint8_t *out, size_t cap, size_t *len);
    int  (*put)(void *ctx, const char *account, const uint8_t *val, size_t len);
    void (*del)(void *ctx, const char *account);
    void (*close)(void *ctx);
    void *ctx;
} oc_secret;

/* Null-tolerant wrappers (a NULL secret is "unavailable"). */
static inline int oc_secret_get(oc_secret *s, const char *a, uint8_t *o, size_t c, size_t *l) {
    return (s && s->get) ? s->get(s->ctx, a, o, c, l) : 0;
}
static inline int oc_secret_put(oc_secret *s, const char *a, const uint8_t *v, size_t n) {
    return (s && s->put) ? s->put(s->ctx, a, v, n) : 0;
}
static inline void oc_secret_del(oc_secret *s, const char *a) {
    if (s && s->del) s->del(s->ctx, a);
}
static inline void oc_secret_free(oc_secret *s) {
    if (s) { if (s->close) s->close(s->ctx); free(s); }
}

#endif /* OC_SECRET_H */
