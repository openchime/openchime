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

/* Enumeration callback: one `account` per stored entry for this service. */
typedef void (*oc_secret_each_cb)(void *ud, const char *account);

typedef struct oc_secret {
    /* Fill `out` (up to `cap`) with the secret bytes for `account`, set *len, and
     * return 1 if found; return 0 if absent or on error. */
    int  (*get)(void *ctx, const char *account, uint8_t *out, size_t cap, size_t *len);
    int  (*put)(void *ctx, const char *account, const uint8_t *val, size_t len);
    void (*del)(void *ctx, const char *account);
    /* List every account this service has stored. Optional (NULL = the backend
     * cannot enumerate); returns 1 if it did. This is what lets the workspace
     * book live in the credential store rather than a file — the list of
     * workspaces IS the list of credentials. */
    int  (*each)(void *ctx, oc_secret_each_cb cb, void *ud);
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
static inline int oc_secret_each(oc_secret *s, oc_secret_each_cb cb, void *ud) {
    return (s && s->each) ? s->each(s->ctx, cb, ud) : 0;
}
static inline void oc_secret_free(oc_secret *s) {
    if (s) { if (s->close) s->close(s->ctx); free(s); }
}

#endif /* OC_SECRET_H */
