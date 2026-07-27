/*
 * OpenChime — libsecret credential backend (Linux), behind the shared
 * oc_secret_open_os() entry point (secret_os.h). Stores the session token in the
 * Secret Service (GNOME Keyring / KWallet) as a hex-encoded password entry keyed
 * by (service, account). When libsecret isn't compiled in, or no Secret Service
 * is reachable (headless / no D-Bus), it returns NULL — and the core then
 * persists no credential at all, rather than falling back to plaintext.
 */

#include "secret_os.h"

#ifndef _WIN32   /* Windows uses secret_win.c */

#ifdef OC_HAVE_LIBSECRET

#include <libsecret/secret.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libsecret's SecretSchema has trailing reserved fields we intentionally leave
 * zero; silence the -Wextra note for this one initializer. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const SecretSchema OC_TOKEN_SCHEMA = {
    "com.openchime.SessionToken", SECRET_SCHEMA_NONE,
    {
        { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, 0 },
    }
};
#pragma GCC diagnostic pop

typedef struct { char service[64]; } ls_ctx;

static const char HEX[] = "0123456789abcdef";
static void hex_enc(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) { out[2 * i] = HEX[b[i] >> 4]; out[2 * i + 1] = HEX[b[i] & 0xf]; }
    out[2 * n] = '\0';
}
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_dec(const char *s, uint8_t *out, size_t cap, size_t *len) {
    size_t n = strlen(s);
    if (n % 2) return 0;
    n /= 2;
    if (n > cap) return 0;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(s[2 * i]), lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *len = n;
    return 1;
}

static int ls_get(void *ctx, const char *account, uint8_t *out, size_t cap, size_t *len) {
    ls_ctx *c = (ls_ctx *)ctx;
    GError *err = NULL;
    gchar *v = secret_password_lookup_sync(&OC_TOKEN_SCHEMA, NULL, &err,
                                           "service", c->service, "account", account, NULL);
    if (err) { g_error_free(err); return 0; }
    if (!v) return 0;
    int ok = hex_dec(v, out, cap, len);
    secret_password_free(v);
    return ok;
}

static int ls_put(void *ctx, const char *account, const uint8_t *val, size_t n) {
    ls_ctx *c = (ls_ctx *)ctx;
    char hex[256];
    if (2 * n + 1 > sizeof hex) return 0;
    hex_enc(val, n, hex);
    char label[160];
    snprintf(label, sizeof label, "OpenChime session token (%s)", account);
    GError *err = NULL;
    gboolean ok = secret_password_store_sync(&OC_TOKEN_SCHEMA, SECRET_COLLECTION_DEFAULT, label,
                                             hex, NULL, &err,
                                             "service", c->service, "account", account, NULL);
    if (err) g_error_free(err);
    return ok ? 1 : 0;
}

static void ls_del(void *ctx, const char *account) {
    ls_ctx *c = (ls_ctx *)ctx;
    GError *err = NULL;
    secret_password_clear_sync(&OC_TOKEN_SCHEMA, NULL, &err,
                               "service", c->service, "account", account, NULL);
    if (err) g_error_free(err);
}

static void ls_close(void *ctx) { free(ctx); }

oc_secret *oc_secret_open_os(const char *service) {
    /* Probe for a running Secret Service; without one (headless / no D-Bus) fall
     * back to SQLite by returning NULL. */
    GError *err = NULL;
    SecretService *svc = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, &err);
    if (!svc) { if (err) g_error_free(err); return NULL; }
    g_object_unref(svc);

    ls_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->service, sizeof c->service, "%s", (service && service[0]) ? service : "openchime");
    oc_secret *s = calloc(1, sizeof *s);
    if (!s) { free(c); return NULL; }
    s->get = ls_get; s->put = ls_put; s->del = ls_del; s->close = ls_close; s->ctx = c;
    return s;
}

#else /* no libsecret: no OS keyring on this build */

oc_secret *oc_secret_open_os(const char *service) { (void)service; return NULL; }

#endif

#endif /* !_WIN32 */
