#include "devicecodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"     /* oc_rand_bytes, oc_sha256 */
#include "jwt.h"      /* oc_base64url_encode */

/* RFC 8628 §6.1's advice: consonants only, no vowels to spell words with and
 * none of the letters people confuse. */
static const char ALPHA[] = "BCDFGHJKLMNPQRSTVWXZ";

typedef enum { ST_FREE = 0, ST_PENDING, ST_APPROVED, ST_DENIED } dstate;

typedef struct {
    dstate   st;
    uint8_t  secret_hash[32];              /* SHA-256 of the device code */
    char     user_code[OC_USER_CODE_LEN + 1];
    char     challenge[48];
    char     source[46], addr[46];
    uint64_t created_ms, expires_ms, last_poll_ms;
    unsigned interval_s;
    char    *token;
} dentry;

struct oc_devcodes {
    dentry   e[OC_DEVICE_MAX];
    uint64_t ttl_ms;
    unsigned interval_s;
};

static void clear(dentry *x) {
    if (x->token) { memset(x->token, 0, strlen(x->token)); free(x->token); }
    memset(x, 0, sizeof *x);
}

oc_devcodes *oc_devcodes_new(void) {
    oc_devcodes *d = calloc(1, sizeof *d);
    if (d) { d->ttl_ms = OC_DEVICE_TTL_MS; d->interval_s = OC_DEVICE_INTERVAL_S; }
    return d;
}

void oc_devcodes_free(oc_devcodes *d) {
    if (!d) return;
    for (int i = 0; i < OC_DEVICE_MAX; i++) clear(&d->e[i]);
    free(d);
}

void oc_devcodes_set_ttl(oc_devcodes *d, uint64_t ms) { if (d) d->ttl_ms = ms ? ms : OC_DEVICE_TTL_MS; }
void oc_devcodes_set_interval(oc_devcodes *d, unsigned s) { if (d) d->interval_s = s ? s : OC_DEVICE_INTERVAL_S; }
unsigned oc_devcodes_interval(const oc_devcodes *d) { return d ? d->interval_s : OC_DEVICE_INTERVAL_S; }

/* Expired requests are gone, whatever their state. */
static void sweep(oc_devcodes *d, uint64_t now) {
    for (int i = 0; i < OC_DEVICE_MAX; i++)
        if (d->e[i].st != ST_FREE && now >= d->e[i].expires_ms) clear(&d->e[i]);
}

int oc_user_code_normalise(const char *typed, char out[OC_USER_CODE_LEN + 1]) {
    char k[8]; int n = 0;
    for (const char *p = typed; p && *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!strchr(ALPHA, c) || c == '\0' || n == 8) return -1;
        k[n++] = c;
    }
    if (n != 8) return -1;
    snprintf(out, OC_USER_CODE_LEN + 1, "%.4s-%.4s", k, k + 4);
    return 0;
}

static dentry *by_user(oc_devcodes *d, const char *code) {
    for (int i = 0; i < OC_DEVICE_MAX; i++)
        if (d->e[i].st != ST_FREE && strcmp(d->e[i].user_code, code) == 0) return &d->e[i];
    return NULL;
}

int oc_devcodes_begin(oc_devcodes *d, const char *source, const char *addr, const char *challenge,
                      uint64_t now_ms, char device_code[OC_DEVICE_CODE_LEN + 1],
                      char user_code[OC_USER_CODE_LEN + 1]) {
    sweep(d, now_ms);
    dentry *slot = NULL;
    int from_source = 0;
    for (int i = 0; i < OC_DEVICE_MAX; i++) {
        if (d->e[i].st == ST_FREE) { if (!slot) slot = &d->e[i]; continue; }
        if (strcmp(d->e[i].source, source ? source : "") == 0) from_source++;
    }
    if (from_source >= OC_DEVICE_PER_SOURCE) return -2;
    if (!slot || !challenge || strlen(challenge) >= sizeof slot->challenge) return -1;
    uint8_t secret[32], r[8];
    /* A user code no pending request has: 20^8 codes, so a clash is rare. */
    for (int tries = 0; ; tries++) {
        if (tries == 8 || oc_rand_bytes(r, sizeof r) != 0) return -1;
        char k[9];
        for (int i = 0; i < 8; i++) k[i] = ALPHA[r[i] % 20];   /* 256 % 20: a slight lean, no matter */
        k[8] = '\0';
        snprintf(user_code, OC_USER_CODE_LEN + 1, "%.4s-%.4s", k, k + 4);
        if (!by_user(d, user_code)) break;
    }
    if (oc_rand_bytes(secret, sizeof secret) != 0) return -1;
    oc_base64url_encode(secret, sizeof secret, device_code);
    memset(slot, 0, sizeof *slot);
    if (oc_sha256(device_code, strlen(device_code), slot->secret_hash) != 0) { memset(secret, 0, sizeof secret); return -1; }
    memset(secret, 0, sizeof secret);
    slot->st = ST_PENDING;
    snprintf(slot->user_code, sizeof slot->user_code, "%s", user_code);
    snprintf(slot->challenge, sizeof slot->challenge, "%s", challenge);
    snprintf(slot->source, sizeof slot->source, "%s", source ? source : "");
    snprintf(slot->addr, sizeof slot->addr, "%s", addr ? addr : "");
    slot->created_ms = now_ms;
    slot->expires_ms = now_ms + d->ttl_ms;
    slot->interval_s = d->interval_s;
    return 0;
}

oc_dev_poll oc_devcodes_poll(oc_devcodes *d, const char *device_code, uint64_t now_ms,
                             char **token, unsigned *interval_s) {
    *token = NULL;
    sweep(d, now_ms);
    uint8_t h[32];
    if (!device_code || oc_sha256(device_code, strlen(device_code), h) != 0) return OC_DEV_GONE;
    dentry *x = NULL;
    for (int i = 0; i < OC_DEVICE_MAX && !x; i++)
        if (d->e[i].st != ST_FREE && memcmp(d->e[i].secret_hash, h, sizeof h) == 0) x = &d->e[i];
    if (!x) return OC_DEV_GONE;
    if (x->st == ST_DENIED) { clear(x); return OC_DEV_DENIED; }
    if (x->st == ST_APPROVED) { *token = x->token; x->token = NULL; clear(x); return OC_DEV_TOKEN; }
    /* RFC 8628 §3.5: a poll sooner than the interval makes it five seconds
     * longer, for this request, from now on. */
    int slow = x->last_poll_ms && now_ms < x->last_poll_ms + (uint64_t)x->interval_s * 1000u;
    x->last_poll_ms = now_ms;
    if (slow) x->interval_s += 5;
    *interval_s = x->interval_s;
    return slow ? OC_DEV_SLOW : OC_DEV_PENDING;
}

int oc_devcodes_find(oc_devcodes *d, const char *typed, uint64_t now_ms, oc_dev_info *out) {
    sweep(d, now_ms);
    char code[OC_USER_CODE_LEN + 1];
    if (oc_user_code_normalise(typed, code) != 0) return 0;
    dentry *x = by_user(d, code);
    if (!x || x->st != ST_PENDING) return 0;
    snprintf(out->user_code, sizeof out->user_code, "%s", x->user_code);
    snprintf(out->challenge, sizeof out->challenge, "%s", x->challenge);
    snprintf(out->addr, sizeof out->addr, "%s", x->addr);
    out->created_ms = x->created_ms;
    return 1;
}

int oc_devcodes_approve(oc_devcodes *d, const char *user_code, const char *token, uint64_t now_ms) {
    sweep(d, now_ms);
    dentry *x = user_code ? by_user(d, user_code) : NULL;
    if (!x || x->st != ST_PENDING || !token || !(x->token = strdup(token))) return -1;
    x->st = ST_APPROVED;
    return 0;
}

int oc_devcodes_deny(oc_devcodes *d, const char *user_code, uint64_t now_ms) {
    sweep(d, now_ms);
    dentry *x = user_code ? by_user(d, user_code) : NULL;
    if (!x || x->st != ST_PENDING) return -1;
    x->st = ST_DENIED;
    return 0;
}
