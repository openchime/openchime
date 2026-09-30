#include "certs.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JSMN_HEADER
#include "jsmn.h"

#include "https_client.h"
#include "jwt.h"      /* oc_base64url_encode */
#include "push.h"     /* oc_push_sign: the machine signature (AUTH.md §8.7) */
#include "auth.h"     /* oc_rand_bytes: each certificate's moment its own */

#define RETRY_MIN_MS  60000u                 /* a failure waits a minute, ten, a hundred... */
#define RETRY_MAX_MS  (24u * 3600u * 1000u)  /* ...then a day */
#define CHECK_MS      3600000u               /* renewal is looked for hourly */
#define ARI_MIN_MS    3600000u               /* ARI is asked no more often than hourly, */
#define ARI_MAX_MS    (12u * 3600u * 1000u)  /* and at least twice a day, */
#define ARI_DEFAULT_MS (6u * 3600u * 1000u)  /* six-hourly where it names no time */

struct oc_certs {
    oc_certs_opts   o;
    char           *s_directory, *s_names, *s_email, *s_akey, *s_aurl, *s_curl, *s_aud, *s_ekey;
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             stop;
    int             obtained;
    uint64_t        issued_ms, not_after_ms;
    char            err[512];
    /* The worker's own (unlocked): the certificate held, when it is renewed,
     * the window that moment was drawn from, and when ARI is next asked. */
    char           *held;
    uint64_t        renew_ms, win_start, win_end, ari_next_ms;
};

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t oc_certs_window_pick(uint64_t start_ms, uint64_t end_ms, double rnd) {
    if (end_ms <= start_ms) return start_ms;
    if (rnd < 0) rnd = 0;
    if (rnd >= 1) rnd = 0.999999;
    return start_ms + (uint64_t)((double)(end_ms - start_ms) * rnd);
}

uint64_t oc_certs_renew_pick(uint64_t issued_ms, uint64_t not_after_ms, double rnd) {
    if (not_after_ms <= issued_ms) return issued_ms;
    uint64_t life = not_after_ms - issued_ms;
    return oc_certs_window_pick(issued_ms + life / 10 * 6, issued_ms + life / 3 * 2, rnd);
}

uint64_t oc_certs_retry_ms(int n, uint64_t base, uint64_t max) {
    uint64_t w = n <= 0 ? base : n == 1 ? base * 10 : n == 2 ? base * 100 : max;
    return w > max ? max : w;
}

/* A uniform draw in [0, 1). */
static double draw(void) {
    uint32_t v = 0;
    if (oc_rand_bytes(&v, sizeof v) != 0) return 0.5;
    return (double)v / 4294967296.0;
}

/* --- through central (AUTH.md §8.9) --------------------------------------- */

/* Strip the path off `url`, leaving scheme://authority. */
static void origin_of(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    size_t n = p ? (size_t)(p + 3 - url) + strcspn(p + 3, "/?#") : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n] = '\0';
}

static int signed_post(const char *origin, const char *path, const char *aud, const char *key,
                       const char *body, oc_https_resp *r, char *err, size_t errcap) {
    long ts = (long)time(NULL);
    char sig[256];
    if (oc_push_sign(key, aud, body, ts, sig, sizeof sig) != 0) {
        snprintf(err, errcap, "could not sign the request to central"); return -1;
    }
    char hdr[700], url[600];
    snprintf(hdr, sizeof hdr, "X-OpenChime-Audience: %s\r\nX-OpenChime-Timestamp: %ld\r\n"
                              "X-OpenChime-Signature: %s\r\n", aud, ts, sig);
    snprintf(url, sizeof url, "%s%s", origin, path);
    return oc_https_request("POST", url, "application/json", body, strlen(body), hdr, 30000, r, err, errcap);
}

/* A JSON string member's value, from a flat object. */
static int json_str(const char *js, size_t len, const char *key, char *out, size_t cap) {
    jsmn_parser p; jsmntok_t t[64];
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, t, 64);
    if (n <= 0 || t[0].type != JSMN_OBJECT) return -1;
    size_t kl = strlen(key);
    for (int i = 1; i + 1 < n; i++) {
        if (t[i].type == JSMN_STRING && (size_t)(t[i].end - t[i].start) == kl && !memcmp(js + t[i].start, key, kl) &&
            t[i + 1].type == JSMN_STRING) {
            size_t o = 0;
            for (int k = t[i + 1].start; k < t[i + 1].end && o + 1 < cap; k++) {
                char c = js[k];
                if (c == '\\' && k + 1 < t[i + 1].end) { char e = js[++k]; c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e; }
                out[o++] = c;
            }
            out[o] = '\0';
            return 0;
        }
    }
    return -1;
}

int oc_central_issue(const char *central_url, const char *audience, const char *enroll_key_pem,
                     oc_cert_issued *out, int *retry_ms, int *final, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    *retry_ms = 0; *final = 0;
    char origin[512];
    origin_of(central_url, origin, sizeof origin);
    oc_https_resp r;

    /* The names this workspace may have: central's to say. */
    if (signed_post(origin, "/api/machine/tls/names", audience, enroll_key_pem, "{}", &r, err, errcap) != 0) return -1;
    char names[1024] = "";
    if (r.status != 200 || json_str(r.body, r.body_len, "names", names, sizeof names) != 0 || !names[0]) {
        snprintf(err, errcap, "central gave no names (HTTP %d)", r.status);
        *final = r.status >= 400 && r.status < 500;
        oc_https_resp_free(&r); return -1;
    }
    oc_https_resp_free(&r);

    uint8_t *der = NULL; size_t dlen = 0; char *key_pem = NULL;
    if (oc_acme_csr(names, &der, &dlen, &key_pem) != 0) { snprintf(err, errcap, "could not make the CSR"); return -1; }
    char *b64 = malloc(dlen * 4 / 3 + 4), *body = b64 ? malloc(dlen * 4 / 3 + 32) : NULL;
    if (body) { oc_base64url_encode(der, dlen, b64); sprintf(body, "{\"csr\":\"%s\"}", b64); }
    free(der); free(b64);
    if (!body) { free(key_pem); snprintf(err, errcap, "out of memory"); return -1; }
    int rc = signed_post(origin, "/api/machine/tls/certificate", audience, enroll_key_pem, body, &r, err, errcap);
    free(body);
    if (rc != 0) { free(key_pem); return -1; }
    if (r.status == 202) {
        char ra[32] = "";
        *retry_ms = oc_https_header(&r, "Retry-After", ra, sizeof ra) ? atoi(ra) * 1000 : 30000;
        if (*retry_ms <= 0) *retry_ms = 30000;
        oc_https_resp_free(&r); free(key_pem);
        return 1;
    }
    if (r.status != 200) {
        snprintf(err, errcap, "central refused the certificate (HTTP %d)", r.status);
        *final = r.status >= 400 && r.status < 500;
        oc_https_resp_free(&r); free(key_pem); return -1;
    }
    char *chain = malloc(r.body_len + 1);
    if (!chain || json_str(r.body, r.body_len, "chain_pem", chain, r.body_len + 1) != 0 ||
        !strstr(chain, "-----BEGIN CERTIFICATE-----")) {
        free(chain); oc_https_resp_free(&r); free(key_pem);
        snprintf(err, errcap, "central's certificate is unreadable"); return -1;
    }
    oc_https_resp_free(&r);
    out->chain_pem = chain; out->key_pem = key_pem; out->names = strdup(names);
    if (oc_cert_validity(chain, strlen(chain), &out->not_before_ms, &out->not_after_ms) != 0) {
        oc_cert_issued_free(out); snprintf(err, errcap, "central's certificate does not parse"); return -1;
    }
    return 0;
}

/* --- the worker ------------------------------------------------------------ */

/* Wait `ms`, or until stopped. 1 if stopped. */
static int wait_ms(oc_certs *c, uint64_t ms) {
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += (time_t)(ms / 1000); dl.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&c->mu);
    while (!c->stop && pthread_cond_timedwait(&c->cv, &c->mu, &dl) == 0) {}
    int s = c->stop;
    pthread_mutex_unlock(&c->mu);
    return s;
}

/* A certificate now held: its renewal drawn from the daemon's own window, and
 * ARI asked about it at once (ACME). */
static void hold(oc_certs *c, const char *chain, uint64_t issued_ms, uint64_t not_after_ms) {
    if (chain != c->held) { free(c->held); c->held = chain ? strdup(chain) : NULL; }
    c->win_start = issued_ms + (not_after_ms - issued_ms) / 10 * 6;
    c->win_end = issued_ms + (not_after_ms - issued_ms) / 3 * 2;
    c->renew_ms = not_after_ms > issued_ms ? oc_certs_renew_pick(issued_ms, not_after_ms, draw()) : 0;
    c->ari_next_ms = 0;
}

/* Ask the CA when to renew (RFC 9773): a window it has not said before replaces
 * the moment, drawn anew from it -- one already open means now, which is how a
 * revocation reaches the daemon. When to ask next: as the CA says, within an
 * hour and twelve. */
static void ari_check(oc_certs *c, uint64_t now) {
    uint64_t st = 0, en = 0, after = 0;
    char err[256] = "";
    if (oc_acme_renewal_info(c->s_directory, c->held, &st, &en, &after, err, sizeof err) == 0 &&
        (st != c->win_start || en != c->win_end)) {
        c->win_start = st; c->win_end = en;
        /* A window already open is the CA asking now -- an incident, a
         * revocation -- and is not waited out; one still to come gets its
         * moment drawn from it. */
        c->renew_ms = st <= now ? now : oc_certs_window_pick(st, en, draw());
        if (st <= now)
            fprintf(stderr, "openchimed: the CA asks for the TLS certificate to be renewed now (its window opened "
                            "%llu s ago)\n", (unsigned long long)((now - st) / 1000));
    }
    uint64_t next = after ? after : ARI_DEFAULT_MS;
    if (next < ARI_MIN_MS) next = ARI_MIN_MS;
    if (next > ARI_MAX_MS) next = ARI_MAX_MS;
    if (c->o.ari_check_ms > 0) next = (uint64_t)c->o.ari_check_ms;
    c->ari_next_ms = now + next;
}

static void *worker(void *p) {
    oc_certs *c = p;
    uint64_t base = c->o.retry_ms > 0 ? (uint64_t)c->o.retry_ms : RETRY_MIN_MS;
    uint64_t max = c->o.retry_max_ms > 0 ? (uint64_t)c->o.retry_max_ms : RETRY_MAX_MS;
    uint64_t check = c->o.check_ms > 0 ? (uint64_t)c->o.check_ms : CHECK_MS;
    int fails = 0;
    if (c->not_after_ms) hold(c, c->o.chain_pem, c->issued_ms, c->not_after_ms);
    for (;;) {
        if (__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE)) break;
        uint64_t now = now_ms();
        /* The CA's word first, where there is a CA to ask and something to ask about. */
        if (c->o.source == OC_CERTS_ACME && c->held && now >= c->ari_next_ms) ari_check(c, now);
        if (c->renew_ms && now < c->renew_ms) {         /* nothing due: look again later */
            uint64_t w = c->renew_ms - now;
            if (w > check) w = check;
            if (c->o.source == OC_CERTS_ACME && c->held && c->ari_next_ms > now && c->ari_next_ms - now < w)
                w = c->ari_next_ms - now;
            if (wait_ms(c, w)) break;
            continue;
        }
        oc_cert_issued got;
        char err[512] = "", replaces[OC_ACME_CERT_ID_MAX] = "";
        int rc, final = 0, later_ms = 0;
        if (c->o.source == OC_CERTS_ACME) {
            if (c->held && oc_acme_cert_id(c->held, replaces, sizeof replaces) != 0) replaces[0] = '\0';
            oc_acme_opts ao = { .directory = c->s_directory, .names = c->s_names, .email = c->s_email,
                                .tls = c->o.tls, .account_key_pem = c->s_akey, .account_url = c->s_aurl,
                                .store_account = c->o.store_account, .ctx = c->o.ctx, .poll_ms = c->o.poll_ms,
                                .stop = &c->stop, .replaces = replaces[0] ? replaces : NULL };
            rc = oc_acme_issue(&ao, &got, err, sizeof err);
        } else {
            rc = oc_central_issue(c->s_curl, c->s_aud, c->s_ekey, &got, &later_ms, &final, err, sizeof err);
        }
        if (rc == 0 && oc_tls_server_use(c->o.tls, got.chain_pem, strlen(got.chain_pem),
                                         got.key_pem, strlen(got.key_pem)) != 0) {
            rc = -1; snprintf(err, sizeof err, "the issued certificate would not load");
        }
        if (rc == 0) {
            fprintf(stderr, "openchimed: TLS certificate for %s, valid until %llu\n", got.names,
                    (unsigned long long)(got.not_after_ms / 1000));
            if (c->o.store_cert) c->o.store_cert(c->o.ctx, &got);
            hold(c, got.chain_pem, got.not_before_ms, got.not_after_ms);
            pthread_mutex_lock(&c->mu);
            c->obtained++;
            c->issued_ms = got.not_before_ms; c->not_after_ms = got.not_after_ms;
            c->err[0] = '\0';
            pthread_mutex_unlock(&c->mu);
            oc_cert_issued_free(&got);
            fails = 0;
            continue;
        }
        if (rc == 1) { if (wait_ms(c, (uint64_t)later_ms)) break; continue; }   /* central: not yet */
        fprintf(stderr, "openchimed: TLS certificate not obtained: %s%s\n", err,
                final ? " (not retried)" : "");
        pthread_mutex_lock(&c->mu);
        snprintf(c->err, sizeof c->err, "%s", err);
        pthread_mutex_unlock(&c->mu);
        if (final) break;
        if (wait_ms(c, oc_certs_retry_ms(fails, base, max))) break;
        fails++;
    }
    return NULL;
}

static char *dup0(const char *s) { return s ? strdup(s) : NULL; }

oc_certs *oc_certs_start(const oc_certs_opts *o) {
    oc_certs *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->o = *o;
    c->s_directory = dup0(o->directory); c->s_names = dup0(o->names); c->s_email = dup0(o->email);
    c->s_akey = dup0(o->account_key_pem); c->s_aurl = dup0(o->account_url);
    c->s_curl = dup0(o->central_url); c->s_aud = dup0(o->audience); c->s_ekey = dup0(o->enroll_key_pem);
    c->issued_ms = o->issued_ms; c->not_after_ms = o->not_after_ms;
    pthread_mutex_init(&c->mu, NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&c->cv, &ca);
    pthread_condattr_destroy(&ca);
    if (pthread_create(&c->th, NULL, worker, c) != 0) { oc_certs_stop(c); return NULL; }
    return c;
}

void oc_certs_stop(oc_certs *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    __atomic_store_n(&c->stop, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    if (c->th) pthread_join(c->th, NULL);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c->s_directory); free(c->s_names); free(c->s_email); free(c->s_akey); free(c->s_aurl);
    free(c->s_curl); free(c->s_aud); free(c->s_ekey);
    free(c->held);
    free(c);
}

void oc_certs_status(oc_certs *c, int *obtained, uint64_t *not_after_ms, char *err, size_t cap) {
    pthread_mutex_lock(&c->mu);
    if (obtained) *obtained = c->obtained;
    if (not_after_ms) *not_after_ms = c->not_after_ms;
    if (err && cap) snprintf(err, cap, "%s", c->err);
    pthread_mutex_unlock(&c->mu);
}
