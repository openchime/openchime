/*
 * OpenChime — Windows Credential Manager backend for the oc_secret seam
 * (ARCH-64/74). Keeps the session token in the OS credential store instead of
 * the client's SQLite file, which is the whole point of the seam: a credential
 * should not sit in a cache database.
 *
 * The mapping is the one Credential Manager was designed for — one generic
 * credential per workspace:
 *
 *   TargetName      "<service>:<workspace>"   e.g. "openchime:chat.acme.com:443"
 *   UserName        <workspace>               (informational; the store keys on target)
 *   CredentialBlob  the opaque token+expiry blob the core packs
 *   Persist         LOCAL_MACHINE             — deliberately not roaming
 *
 * Only the token blob lives here. The TOFU pin is public and the message cache
 * is far larger than a credential blob may be, so both stay in SQLite.
 *
 * Built into both Windows front-ends (the Console TUI and the Win32 GUI); on
 * non-Windows this file compiles to nothing and the libsecret backend is used.
 */

#include "secret_os.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char service[64]; } cm_ctx;

/* Build the credential target name for an account, as UTF-16. */
static int cm_target(const cm_ctx *c, const char *account, WCHAR *out, int cap) {
    char name[512];
    snprintf(name, sizeof name, "%s:%s", c->service, account ? account : "");
    return MultiByteToWideChar(CP_UTF8, 0, name, -1, out, cap) > 0;
}

static int cm_get(void *ctx, const char *account, uint8_t *out, size_t cap, size_t *len) {
    cm_ctx *c = ctx;
    WCHAR target[600];
    if (!cm_target(c, account, target, 600)) return 0;
    PCREDENTIALW cred = NULL;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &cred) || !cred) return 0;
    int ok = 0;
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0 &&
        cred->CredentialBlobSize <= cap) {
        memcpy(out, cred->CredentialBlob, cred->CredentialBlobSize);
        if (len) *len = cred->CredentialBlobSize;
        ok = 1;
    }
    CredFree(cred);
    return ok;
}

static int cm_put(void *ctx, const char *account, const uint8_t *val, size_t len) {
    cm_ctx *c = ctx;
    WCHAR target[600], user[600];
    if (!cm_target(c, account, target, 600)) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, account ? account : "", -1, user, 600) <= 0) return 0;

    CREDENTIALW cred;
    memset(&cred, 0, sizeof cred);
    cred.Type              = CRED_TYPE_GENERIC;
    cred.TargetName        = target;
    cred.UserName          = user;
    cred.CredentialBlob    = (LPBYTE)val;
    cred.CredentialBlobSize = (DWORD)len;
    /* LOCAL_MACHINE, not ENTERPRISE: a session token is bound to this daemon and
     * this device, so roaming it to other machines would be wrong. */
    cred.Persist           = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) ? 1 : 0;
}

static void cm_del(void *ctx, const char *account) {
    cm_ctx *c = ctx;
    WCHAR target[600];
    if (cm_target(c, account, target, 600)) CredDeleteW(target, CRED_TYPE_GENERIC, 0);
}

static void cm_close(void *ctx) { free(ctx); }

oc_secret *oc_secret_open_os(const char *service) {
    /* Probe the store rather than assume it: CredEnumerate failing with
     * anything other than "nothing found" means the credential store is not
     * usable in this session, and the caller must know that. */
    DWORD n = 0; PCREDENTIALW *list = NULL;
    if (!CredEnumerateW(NULL, 0, &n, &list)) {
        DWORD e = GetLastError();
        if (e != ERROR_NOT_FOUND) return NULL;
    } else if (list) {
        CredFree(list);
    }

    cm_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->service, sizeof c->service, "%s", (service && service[0]) ? service : "openchime");
    oc_secret *s = calloc(1, sizeof *s);
    if (!s) { free(c); return NULL; }
    s->get = cm_get; s->put = cm_put; s->del = cm_del; s->close = cm_close; s->ctx = c;
    return s;
}

#endif /* _WIN32 */
