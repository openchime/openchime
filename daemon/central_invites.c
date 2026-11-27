/* Invitations queued by central (ARCH-85). See central_invites.h. The signing is
 * the push emitter's (push.h's oc_push_sign) and the transport https_client.h's;
 * this file is the take, the report, and the waits between them. */
#include "central_invites.h"
#include "central.h"
#include "https_client.h"
#define JSMN_HEADER
#include "jsmn.h"
#include "push.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OC_CENTRAL_INVITES_MAX_PENDING 256   /* outcomes waiting to be reported */
#define OC_CENTRAL_INVITES_REASON      120   /* a refusal's reason, as reported */
#define OC_CENTRAL_INVITES_TIMEOUT_MS  30000

const oc_central_invites_timing OC_CENTRAL_INVITES_TIMING = {
    .idle_ms     = 3600u * 1000u,
    .fail_min_ms = 60u * 1000u,
    .fail_max_ms = 24ull * 3600u * 1000u,
};

typedef struct onode {
    char          id[OC_CENTRAL_INVITES_ID_LEN + 1];
    int           made;
    char          reason[OC_CENTRAL_INVITES_REASON];
    struct onode *next;
} onode;

struct oc_central_invites {
    char                      origin[512];
    char                     *audience;
    char                     *privkey;
    oc_central_invites_make_fn make;
    void                     *ctx;
    oc_central_invites_timing timing;
    pthread_t                 thread;
    pthread_mutex_t           mu;
    pthread_cond_t            cv;
    onode                    *outcomes;        /* waiting to be reported */
    int                       n_outcomes;
    uint64_t                  take_due_ms;     /* monotonic */
    uint64_t                  report_due_ms;   /* monotonic; a failed report waits */
    int                       take_failures, report_failures;
    int                       stopping;
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Strip the path off `url`, leaving scheme://authority. */
static void origin_of(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    size_t n = p ? (size_t)(p + 3 - url) + strcspn(p + 3, "/?#") : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n);
    out[n] = '\0';
}

uint64_t oc_central_invites_wait_ms(int ok, long retry_after_s, int failures,
                                    const oc_central_invites_timing *t) {
    if (ok) return retry_after_s > 0 ? (uint64_t)retry_after_s * 1000u : t->idle_ms;
    uint64_t w = t->fail_min_ms;
    for (int i = 1; i < failures && w < t->fail_max_ms; i++) w *= 2;
    return w > t->fail_max_ms ? t->fail_max_ms : w;
}

static int is_id(const char *s, int len) {
    if (len != OC_CENTRAL_INVITES_ID_LEN) return 0;
    for (int i = 0; i < len; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int tok_is(const char *js, const jsmntok_t *t, const char *s) {
    size_t n = strlen(s);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == n && !memcmp(js + t->start, s, n);
}

/* The index just past the value starting at t[i], its children included (a key's
 * one child is its value, so an object's keys carry their values). */
static int skip(const jsmntok_t *t, int i, int n) {
    int end = i + 1;
    for (int k = 0; k < t[i].size && end < n; k++) end = skip(t, end, n);
    return end;
}

int oc_central_invites_parse(const char *body, size_t len,
                             char ids[][OC_CENTRAL_INVITES_ID_LEN + 1],
                             char addresses[][OC_CENTRAL_INVITES_ADDR], int max) {
    if (!body) return -1;
    jsmn_parser p;
    jsmntok_t t[512];
    jsmn_init(&p);
    int n = jsmn_parse(&p, body, len, t, (unsigned)(sizeof t / sizeof t[0]));
    if (n <= 0 || t[0].type != JSMN_OBJECT) return -1;
    /* Find "invitations", an array of objects. */
    int arr = -1;
    for (int i = 1, k = 0; i < n && k < t[0].size; k++) {
        if (tok_is(body, &t[i], "invitations") && i + 1 < n && t[i + 1].type == JSMN_ARRAY) { arr = i + 1; break; }
        i = skip(t, i + 1, n);
    }
    if (arr < 0) return -1;
    int found = 0;
    for (int e = 0, i = arr + 1; e < t[arr].size && i < n; e++) {
        int next = skip(t, i, n);
        if (t[i].type == JSMN_OBJECT && found < max) {
            const jsmntok_t *id = NULL, *addr = NULL;
            for (int j = i + 1, k = 0; j + 1 < next && k < t[i].size; k++) {
                if (tok_is(body, &t[j], "id") && t[j + 1].type == JSMN_STRING) id = &t[j + 1];
                else if (tok_is(body, &t[j], "address") && t[j + 1].type == JSMN_STRING) addr = &t[j + 1];
                j = skip(t, j + 1, n);
            }
            int al = addr ? addr->end - addr->start : 0;
            if (id && is_id(body + id->start, id->end - id->start) && al > 0 && al < OC_CENTRAL_INVITES_ADDR) {
                memcpy(ids[found], body + id->start, OC_CENTRAL_INVITES_ID_LEN);
                ids[found][OC_CENTRAL_INVITES_ID_LEN] = '\0';
                memcpy(addresses[found], body + addr->start, (size_t)al);
                addresses[found][al] = '\0';
                found++;
            }
        }
        i = next;
    }
    return found;
}

/* `s` as a JSON string's contents, escaped, appended at out[*off]. */
static int put_escaped(char *out, size_t cap, size_t *off, const char *s) {
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        char esc[8];
        const char *add = esc;
        if (*q == '"')       add = "\\\"";
        else if (*q == '\\') add = "\\\\";
        else if (*q < 0x20)  snprintf(esc, sizeof esc, "\\u%04x", *q);
        else { esc[0] = (char)*q; esc[1] = '\0'; }
        size_t al = strlen(add);
        if (*off + al >= cap) return -1;
        memcpy(out + *off, add, al);
        *off += al;
    }
    return 0;
}

int oc_central_invites_report_body(const char *const *ids, const int *made,
                                   const char *const *reasons, int n, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    size_t off = 0;
    int w = snprintf(out, cap, "{\"outcomes\":[");
    if (w < 0 || (size_t)w >= cap) return -1;
    off = (size_t)w;
    for (int i = 0; i < n; i++) {
        w = snprintf(out + off, cap - off, "%s{\"id\":\"%s\",\"made\":%s,\"reason\":",
                     i ? "," : "", ids[i], made[i] ? "true" : "false");
        if (w < 0 || (size_t)w >= cap - off) return -1;
        off += (size_t)w;
        if (made[i] || !reasons[i]) {
            if (off + 4 >= cap) return -1;
            memcpy(out + off, "null", 4);
            off += 4;
        } else {
            if (off + 1 >= cap) return -1;
            out[off++] = '"';
            if (put_escaped(out, cap, &off, reasons[i]) != 0 || off + 1 >= cap) return -1;
            out[off++] = '"';
        }
        if (off + 1 >= cap) return -1;
        out[off++] = '}';
    }
    if (off + 3 > cap) return -1;
    memcpy(out + off, "]}", 3);
    return 0;
}

/* A signed POST of `body` to `path` at central (AUTH.md §8.7's machine signature). */
static int signed_post(oc_central_invites *ci, const char *path, const char *body, oc_https_resp *r) {
    long ts = (long)time(NULL);
    char sig[256], hdr[700], url[700], err[160];
    if (oc_push_sign(ci->privkey, "POST", path, ci->audience, body, ts, sig, sizeof sig) != 0) return -1;
    snprintf(hdr, sizeof hdr, "X-OpenChime-Audience: %s\r\nX-OpenChime-Timestamp: %ld\r\n"
                              "X-OpenChime-Signature: %s\r\n" OC_MACHINE_SIG_VERSION_HEADER,
             ci->audience, ts, sig);
    snprintf(url, sizeof url, "%s%s", ci->origin, path);
    return oc_https_request("POST", url, "application/json", body, strlen(body), hdr,
                            OC_CENTRAL_INVITES_TIMEOUT_MS, r, err, sizeof err);
}

/* Take what central has pending, and hand each to `make`. Returns the wait before
 * the next take. Called without the lock. */
static uint64_t take(oc_central_invites *ci) {
    oc_https_resp r;
    int ok = 0;
    long retry_after = 0;
    static char ids[OC_CENTRAL_INVITES_MAX][OC_CENTRAL_INVITES_ID_LEN + 1];
    static char addrs[OC_CENTRAL_INVITES_MAX][OC_CENTRAL_INVITES_ADDR];
    int n = 0;
    if (signed_post(ci, OC_CENTRAL_INVITATIONS_TAKE, "{}", &r) == 0) {
        if (r.status == 200) {
            n = oc_central_invites_parse(r.body, r.body_len, ids, addrs, OC_CENTRAL_INVITES_MAX);
            ok = n >= 0;
            char ra[32];
            if (oc_https_header(&r, "Retry-After", ra, sizeof ra)) retry_after = strtol(ra, NULL, 10);
        }
        oc_https_resp_free(&r);
    }
    ci->take_failures = ok ? 0 : ci->take_failures + 1;
    for (int i = 0; i < n; i++) ci->make(ci->ctx, ids[i], addrs[i]);
    return oc_central_invites_wait_ms(ok, retry_after, ci->take_failures, &ci->timing);
}

/* Report the outcomes waiting, in one request. Called with the lock held; drops
 * it while the request is under way. */
static void report(oc_central_invites *ci) {
    onode *batch = ci->outcomes;
    int n = ci->n_outcomes;
    ci->outcomes = NULL;
    ci->n_outcomes = 0;
    pthread_mutex_unlock(&ci->mu);

    const char *ids[OC_CENTRAL_INVITES_MAX_PENDING], *reasons[OC_CENTRAL_INVITES_MAX_PENDING];
    int made[OC_CENTRAL_INVITES_MAX_PENDING], i = 0;
    for (onode *o = batch; o && i < n; o = o->next, i++) {
        ids[i] = o->id; made[i] = o->made; reasons[i] = o->reason;
    }
    size_t cap = 64 + (size_t)i * (OC_CENTRAL_INVITES_ID_LEN + OC_CENTRAL_INVITES_REASON * 6 + 64);
    char *body = malloc(cap);
    int delivered = 0, final = 0;
    if (body && oc_central_invites_report_body(ids, made, reasons, i, body, cap) == 0) {
        oc_https_resp r;
        if (signed_post(ci, OC_CENTRAL_INVITATIONS_REPORT, body, &r) == 0) {
            delivered = r.status >= 200 && r.status < 300;
            /* A 4xx is central saying no: asking again gets the same answer. */
            final = r.status >= 400 && r.status < 500;
            if (final)
                fprintf(stderr, "openchimed: central refused a report of %d invitations (HTTP %d); "
                                "it offers them again once their lease lapses\n", i, r.status);
            oc_https_resp_free(&r);
        }
    }
    free(body);

    pthread_mutex_lock(&ci->mu);
    if (delivered || final || ci->stopping) {
        ci->report_failures = 0;
        while (batch) { onode *x = batch; batch = batch->next; free(x); }
        return;
    }
    /* Not delivered: the same outcomes, the same ids, after a wait. */
    ci->report_failures++;
    ci->report_due_ms = mono_ms() + oc_central_invites_wait_ms(0, 0, ci->report_failures, &ci->timing);
    onode *tail = batch;
    while (tail && tail->next) tail = tail->next;
    if (tail) { tail->next = ci->outcomes; ci->outcomes = batch; ci->n_outcomes += n; }
}

static void wait_until(oc_central_invites *ci, uint64_t due_ms) {
    uint64_t now = mono_ms();
    if (due_ms <= now) return;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    uint64_t wait = due_ms - now;
    until.tv_sec  += (time_t)(wait / 1000);
    until.tv_nsec += (long)(wait % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&ci->cv, &ci->mu, &until);
}

static void *worker(void *arg) {
    oc_central_invites *ci = arg;
    pthread_mutex_lock(&ci->mu);
    while (!ci->stopping) {
        uint64_t now = mono_ms();
        if (ci->outcomes && ci->report_due_ms <= now) {
            report(ci);
            continue;
        }
        if (ci->take_due_ms <= now) {
            pthread_mutex_unlock(&ci->mu);
            uint64_t wait = take(ci);
            pthread_mutex_lock(&ci->mu);
            ci->take_due_ms = mono_ms() + wait;
            continue;
        }
        uint64_t due = ci->take_due_ms;
        if (ci->outcomes && ci->report_due_ms < due) due = ci->report_due_ms;
        wait_until(ci, due);
    }
    pthread_mutex_unlock(&ci->mu);
    return NULL;
}

oc_central_invites *oc_central_invites_start_timed(const char *enroll_url, const char *audience,
                                                   const char *privkey_pem,
                                                   oc_central_invites_make_fn make, void *ctx,
                                                   const oc_central_invites_timing *timing) {
    if (!enroll_url || !*enroll_url || !audience || !privkey_pem || !make || !timing) return NULL;
    oc_central_invites *ci = calloc(1, sizeof *ci);
    if (!ci) return NULL;
    origin_of(enroll_url, ci->origin, sizeof ci->origin);
    ci->audience = strdup(audience);
    ci->privkey  = strdup(privkey_pem);
    ci->make = make;
    ci->ctx = ctx;
    ci->timing = *timing;
    ci->take_due_ms = mono_ms();       /* the first take is at once */
    if (!ci->audience || !ci->privkey) goto fail;
    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) goto fail;
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int cv_ok = pthread_cond_init(&ci->cv, &ca) == 0;
    pthread_condattr_destroy(&ca);
    if (!cv_ok) goto fail;
    if (pthread_mutex_init(&ci->mu, NULL) != 0) { pthread_cond_destroy(&ci->cv); goto fail; }
    if (pthread_create(&ci->thread, NULL, worker, ci) != 0) {
        pthread_mutex_destroy(&ci->mu); pthread_cond_destroy(&ci->cv); goto fail;
    }
    return ci;
fail:
    free(ci->audience);
    free(ci->privkey);
    free(ci);
    return NULL;
}

oc_central_invites *oc_central_invites_start(const char *enroll_url, const char *audience,
                                             const char *privkey_pem,
                                             oc_central_invites_make_fn make, void *ctx) {
    return oc_central_invites_start_timed(enroll_url, audience, privkey_pem, make, ctx,
                                          &OC_CENTRAL_INVITES_TIMING);
}

void oc_central_invites_outcome(oc_central_invites *ci, const char *id, int made, const char *reason) {
    if (!ci || !id || strlen(id) != OC_CENTRAL_INVITES_ID_LEN) return;
    onode *o = calloc(1, sizeof *o);
    if (!o) return;
    memcpy(o->id, id, OC_CENTRAL_INVITES_ID_LEN + 1);
    o->made = made != 0;
    snprintf(o->reason, sizeof o->reason, "%s", (!made && reason && *reason) ? reason : (made ? "" : "refused"));
    pthread_mutex_lock(&ci->mu);
    if (ci->stopping || ci->n_outcomes >= OC_CENTRAL_INVITES_MAX_PENDING) {
        pthread_mutex_unlock(&ci->mu);
        free(o);
        return;
    }
    o->next = ci->outcomes;
    ci->outcomes = o;
    ci->n_outcomes++;
    pthread_cond_signal(&ci->cv);
    pthread_mutex_unlock(&ci->mu);
}

void oc_central_invites_stop(oc_central_invites *ci) {
    if (!ci) return;
    pthread_mutex_lock(&ci->mu);
    ci->stopping = 1;
    pthread_cond_signal(&ci->cv);
    pthread_mutex_unlock(&ci->mu);
    pthread_join(ci->thread, NULL);
    while (ci->outcomes) { onode *x = ci->outcomes; ci->outcomes = x->next; free(x); }
    pthread_mutex_destroy(&ci->mu);
    pthread_cond_destroy(&ci->cv);
    free(ci->audience);
    free(ci->privkey);
    free(ci);
}
