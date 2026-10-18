/* Second-step tickets -- see webstep.h. */

#include "webstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"     /* oc_rand_bytes, oc_sha256, oc_ct_eq */
#include "jwt.h"      /* oc_base64url_encode */

typedef struct {
    int            used;
    uint8_t        hash[32];          /* SHA-256 of the ticket */
    char           source[46];
    uint64_t       expires_ms;
    int            tries;
    oc_step_ticket t;
} sentry;

struct oc_websteps {
    sentry   e[OC_STEP_MAX];
    uint64_t ttl_ms;
};

static void clear(sentry *x) { memset(x, 0, sizeof *x); }

oc_websteps *oc_websteps_new(void) {
    oc_websteps *s = calloc(1, sizeof *s);
    if (s) s->ttl_ms = OC_STEP_TTL_MS;
    return s;
}

void oc_websteps_free(oc_websteps *s) {
    if (!s) return;
    for (int i = 0; i < OC_STEP_MAX; i++) clear(&s->e[i]);
    free(s);
}

void oc_websteps_set_ttl(oc_websteps *s, uint64_t ms) { if (s) s->ttl_ms = ms ? ms : OC_STEP_TTL_MS; }

static void sweep(oc_websteps *s, uint64_t now) {
    for (int i = 0; i < OC_STEP_MAX; i++)
        if (s->e[i].used && now >= s->e[i].expires_ms) clear(&s->e[i]);
}

static sentry *find(oc_websteps *s, const char *ticket, uint64_t now) {
    if (!s || !ticket || strlen(ticket) != OC_STEP_TICKET_LEN) return NULL;
    sweep(s, now);
    uint8_t h[32];
    if (oc_sha256(ticket, OC_STEP_TICKET_LEN, h) != 0) return NULL;
    sentry *hit = NULL;
    for (int i = 0; i < OC_STEP_MAX; i++)
        if (s->e[i].used && oc_ct_eq(s->e[i].hash, h, sizeof h)) hit = &s->e[i];
    return hit;
}

int oc_websteps_put(oc_websteps *s, const char *source, const oc_step_ticket *t, uint64_t now_ms,
                    char out[OC_STEP_TICKET_LEN + 1]) {
    sweep(s, now_ms);
    int mine = 0;
    sentry *slot = NULL;
    for (int i = 0; i < OC_STEP_MAX; i++) {
        if (!s->e[i].used) { if (!slot) slot = &s->e[i]; continue; }
        if (strcmp(s->e[i].source, source ? source : "") == 0) mine++;
    }
    if (mine >= OC_STEP_PER_SOURCE) return -2;
    if (!slot) return -1;
    uint8_t raw[32];
    if (oc_rand_bytes(raw, sizeof raw) != 0) return -1;
    char enc[4 * ((sizeof raw + 2) / 3) + 1];   /* the encoder's own bound */
    size_t n = oc_base64url_encode(raw, sizeof raw, enc);
    memset(raw, 0, sizeof raw);
    if (n != OC_STEP_TICKET_LEN) { memset(enc, 0, sizeof enc); return -1; }
    memcpy(out, enc, OC_STEP_TICKET_LEN + 1);
    memset(enc, 0, sizeof enc);
    if (oc_sha256(out, OC_STEP_TICKET_LEN, slot->hash) != 0) return -1;
    slot->used = 1;
    snprintf(slot->source, sizeof slot->source, "%s", source ? source : "");
    slot->expires_ms = now_ms + s->ttl_ms;
    slot->tries = OC_STEP_TRIES;
    slot->t = *t;
    return 0;
}

int oc_websteps_get(oc_websteps *s, const char *ticket, uint64_t now_ms, oc_step_ticket *out) {
    sentry *x = find(s, ticket, now_ms);
    if (!x) return 0;
    *out = x->t;
    return 1;
}

int oc_websteps_fail(oc_websteps *s, const char *ticket, uint64_t now_ms) {
    sentry *x = find(s, ticket, now_ms);
    if (!x) return 0;
    if (--x->tries <= 0) { clear(x); return 0; }
    return x->tries;
}

void oc_websteps_drop(oc_websteps *s, const char *ticket) {
    sentry *x = find(s, ticket, 0);
    if (x) clear(x);
}
