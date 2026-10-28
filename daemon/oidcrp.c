/* Direct connections -- see oidcrp.h. */

#include "oidcrp.h"
#include "https_client.h"
#include "idtoken.h"
#include "json.h"

#include <mbedtls/base64.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REFRESH_MS     (24ull * 60 * 60 * 1000)   /* discovery and keys, daily */
#define RETRY_MS       (60ull * 1000)             /* a provider not reached, again */
#define JWKS_AGAIN_MS  (60ull * 1000)             /* an unknown kid refetches at most this often */
#define QUEUE_MAX      64
#define TIMEOUT_MS     10000

static const char MS_CONSUMER_TENANT[] = "9188040d-6c67-4c5b-b112-36a304b66dad";

typedef struct req {
    struct req *next;
    int         i;
    uint64_t    conn_id;
    char        code[1024], redirect[600], verifier[64], nonce[64], source[46];
} req;

typedef struct {
    oc_oidc_connect cfg;
    char            authz[512], token_ep[512], jwks_uri[512];
    char           *jwks;
    size_t          jwks_len;
    uint64_t        next_ms, jwks_ms;
    int             ok;
} conn_state;

struct oc_oidcrp {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pthread_t       th;
    int             stop, n, queued;
    req            *head, *tail;
    conn_state      c[OC_OIDC_MAX_CONNECT];
    oc_dbwriter    *dbw;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Percent-encode `in` (RFC 3986 unreserved kept) at `out`+`*o`. 0 or -1. */
static int pct(char *out, size_t cap, size_t *o, const char *in) {
    static const char H[] = "0123456789ABCDEF";
    for (; *in; in++) {
        unsigned char ch = (unsigned char)*in;
        int plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (*o + (plain ? 1 : 3) >= cap) return -1;
        if (plain) out[(*o)++] = (char)ch;
        else { out[(*o)++] = '%'; out[(*o)++] = H[ch >> 4]; out[(*o)++] = H[ch & 15]; }
    }
    out[*o] = '\0';
    return 0;
}

/* Fetch the key set for connection `c` (locked by the caller only for the swap). */
static int fetch_jwks(oc_oidcrp *rp, conn_state *c) {
    oc_https_resp r;
    char err[200], uri[512];
    pthread_mutex_lock(&rp->mu);
    snprintf(uri, sizeof uri, "%s", c->jwks_uri);
    pthread_mutex_unlock(&rp->mu);
    if (!uri[0] || oc_https_request("GET", uri, NULL, NULL, 0, "Accept: application/json\r\n", TIMEOUT_MS, &r,
                                    err, sizeof err) != 0) return -1;
    int ok = r.status == 200 && r.body && r.body_len;
    oc_json d;
    if (ok && (oc_json_parse(&d, r.body, r.body_len) != 0 || oc_json_get(&d, 0, "keys") < 0)) ok = 0;
    else if (ok) oc_json_free(&d);
    if (ok) {
        pthread_mutex_lock(&rp->mu);
        free(c->jwks);
        c->jwks = r.body; c->jwks_len = r.body_len; r.body = NULL;
        c->jwks_ms = now_ms();
        pthread_mutex_unlock(&rp->mu);
    }
    oc_https_resp_free(&r);
    return ok ? 0 : -1;
}

/* Discovery, then keys. The issuer the document names must be the one
 * configured, exactly (OpenID Connect Discovery §4.3). */
static void refresh(oc_oidcrp *rp, conn_state *c) {
    char url[600], err[200], iss[256], az[512], tk[512], jw[512];
    snprintf(url, sizeof url, "%s/.well-known/openid-configuration", c->cfg.issuer);
    size_t ul = strlen(c->cfg.issuer);
    if (ul && c->cfg.issuer[ul - 1] == '/')
        snprintf(url, sizeof url, "%s.well-known/openid-configuration", c->cfg.issuer);
    oc_https_resp r;
    int ok = 0;
    if (oc_https_request("GET", url, NULL, NULL, 0, "Accept: application/json\r\n", TIMEOUT_MS, &r, err, sizeof err) == 0) {
        oc_json d;
        if (r.status == 200 && r.body && oc_json_parse(&d, r.body, r.body_len) == 0) {
            ok = oc_json_get_str(&d, 0, "issuer", iss, sizeof iss) == 0 && strcmp(iss, c->cfg.issuer) == 0 &&
                 oc_json_get_str(&d, 0, "authorization_endpoint", az, sizeof az) == 0 &&
                 oc_json_get_str(&d, 0, "token_endpoint", tk, sizeof tk) == 0 &&
                 oc_json_get_str(&d, 0, "jwks_uri", jw, sizeof jw) == 0;
            oc_json_free(&d);
        }
        oc_https_resp_free(&r);
    }
    if (ok) {
        pthread_mutex_lock(&rp->mu);
        snprintf(c->authz, sizeof c->authz, "%s", az);
        snprintf(c->token_ep, sizeof c->token_ep, "%s", tk);
        snprintf(c->jwks_uri, sizeof c->jwks_uri, "%s", jw);
        pthread_mutex_unlock(&rp->mu);
        ok = fetch_jwks(rp, c) == 0;
    }
    if (!ok) fprintf(stderr, "openchimed: sign-in provider %s cannot be reached; its sign-in is unavailable\n",
                     c->cfg.issuer);
    pthread_mutex_lock(&rp->mu);
    c->ok = ok;
    c->next_ms = now_ms() + (ok ? REFRESH_MS : RETRY_MS);
    pthread_mutex_unlock(&rp->mu);
}

static void submit(oc_oidcrp *rp, const req *q, oc_direct_auth *d) {
    oc_job *j = oc_job_new(OC_JOB_AUTH, q->conn_id);
    if (!j) { free(d); return; }
    j->method = OC_AUTH_OIDC;
    j->direct = d;
    snprintf(j->source, sizeof j->source, "%s", q->source);
    oc_dbwriter_submit(rp->dbw, j);
}

static void fail(oc_oidcrp *rp, const req *q, uint16_t err, const char *why) {
    oc_direct_auth *d = calloc(1, sizeof *d);
    if (!d) return;
    d->err = err;
    snprintf(d->reason, sizeof d->reason, "%s", why);
    submit(rp, q, d);
}

/* The code, exchanged at the token endpoint; the ID token in the answer,
 * checked; the identity, to the writer. */
static void exchange(oc_oidcrp *rp, const req *q) {
    conn_state *c = &rp->c[q->i];
    char body[2400], tk[512], auth[1100] = "", err[200];
    size_t o = 0;
    pthread_mutex_lock(&rp->mu);
    int ready = c->ok;
    snprintf(tk, sizeof tk, "%s", c->token_ep);
    pthread_mutex_unlock(&rp->mu);
    if (!ready) { fail(rp, q, OC_ERR_AUTH_SOURCE_UNAVAILABLE, "unreachable"); return; }
    o = (size_t)snprintf(body, sizeof body, "grant_type=authorization_code&code=");
    if (pct(body, sizeof body, &o, q->code) || o + 16 >= sizeof body) goto too_long;
    memcpy(body + o, "&redirect_uri=", 15); o += 14;
    if (pct(body, sizeof body, &o, q->redirect) || o + 16 >= sizeof body) goto too_long;
    memcpy(body + o, "&code_verifier=", 16); o += 15;
    if (pct(body, sizeof body, &o, q->verifier)) goto too_long;
    if (c->cfg.secret[0]) {
        /* client_secret_basic (RFC 6749 §2.3.1): the id and secret, each encoded. */
        char pair[1600];
        size_t po = 0;
        unsigned char b64[1100]; size_t bl = 0;
        if (pct(pair, sizeof pair, &po, c->cfg.client_id) || po + 1 >= sizeof pair) goto too_long;
        pair[po++] = ':'; pair[po] = '\0';
        if (pct(pair, sizeof pair, &po, c->cfg.secret) ||
            mbedtls_base64_encode(b64, sizeof b64, &bl, (const unsigned char *)pair, po) != 0) goto too_long;
        snprintf(auth, sizeof auth, "Authorization: Basic %.*s\r\nAccept: application/json\r\n", (int)bl, b64);
        memset(pair, 0, sizeof pair);
    } else {
        if (o + 12 >= sizeof body) goto too_long;
        memcpy(body + o, "&client_id=", 12); o += 11;
        if (pct(body, sizeof body, &o, c->cfg.client_id)) goto too_long;
        snprintf(auth, sizeof auth, "Accept: application/json\r\n");
    }
    oc_https_resp r;
    int got = oc_https_request("POST", tk, "application/x-www-form-urlencoded", body, o, auth, TIMEOUT_MS, &r,
                               err, sizeof err);
    memset(auth, 0, sizeof auth);
    memset(body, 0, sizeof body);
    if (got != 0) { fail(rp, q, OC_ERR_AUTH_SOURCE_UNAVAILABLE, "token-endpoint"); return; }
    char *idt = NULL;
    if (r.status == 200 && r.body) {
        oc_json d;
        if (oc_json_parse(&d, r.body, r.body_len) == 0) {
            int t = oc_json_get(&d, 0, "id_token");
            if (t >= 0 && d.t[t].type == JSMN_STRING) idt = strndup(r.body + d.t[t].start, (size_t)(d.t[t].end - d.t[t].start));
            oc_json_free(&d);
        }
    }
    oc_https_resp_free(&r);
    if (!idt) { fail(rp, q, OC_ERR_AUTH_INVALID_TOKEN, "no-id-token"); return; }
    oc_idt_claims cl;
    oc_idt_result vr = OC_IDT_KEY;
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&rp->mu);
        char *ks = c->jwks ? strndup(c->jwks, c->jwks_len) : NULL;
        size_t kl = c->jwks_len;
        uint64_t age = now_ms() - c->jwks_ms;
        pthread_mutex_unlock(&rp->mu);
        vr = ks ? oc_idtoken_verify(idt, strlen(idt), ks, kl, c->cfg.issuer, c->cfg.client_id, q->nonce,
                                    now_ms() / 1000u, &cl) : OC_IDT_KEY;
        free(ks);
        /* A key the set lacks: the provider has rotated -- fetch it again, once. */
        if (vr != OC_IDT_KEY || attempt || age < JWKS_AGAIN_MS || fetch_jwks(rp, c) != 0) break;
    }
    memset(idt, 0, strlen(idt));
    free(idt);
    if (vr != OC_IDT_OK) {
        static const char *const WHY[] = { "ok", "format", "alg", "key", "signature", "claims", "time" };
        fail(rp, q, OC_ERR_AUTH_INVALID_TOKEN, WHY[-(int)vr < 7 ? -(int)vr : 1]);
        return;
    }
    oc_direct_auth *d = calloc(1, sizeof *d);
    if (!d) return;
    const char *iss = c->cfg.issuer;
    int ms = strncmp(iss, "https://login.microsoftonline.com/", 34) == 0;
    const char *subject = c->cfg.subject_oid && cl.oid[0] ? cl.oid : cl.sub;
    snprintf(d->iss, sizeof d->iss, "%s", iss);
    snprintf(d->sub, sizeof d->sub, "%s|%s", iss, subject);
    snprintf(d->email, sizeof d->email, "%s", cl.email);
    snprintf(d->name, sizeof d->name, "%s", cl.name);
    snprintf(d->tenant, sizeof d->tenant, "%s", ms ? (strcmp(cl.tid, MS_CONSUMER_TENANT) ? cl.tid : "") : cl.hd);
    snprintf(d->idp, sizeof d->idp, "%s", !strcmp(iss, "https://accounts.google.com") ? "google" : ms ? "microsoft" : "oidc");
    /* Microsoft says an address is verified only through xms_edov, or for a
     * personal account; the relay decides the same way. */
    d->email_verified = ms ? (strcmp(cl.tid, MS_CONSUMER_TENANT) == 0 || cl.xms_edov) : cl.email_verified;
    submit(rp, q, d);
    return;
too_long:
    fail(rp, q, OC_ERR_AUTH_INVALID_TOKEN, "too-long");
}

static void *worker(void *arg) {
    oc_oidcrp *rp = arg;
    for (;;) {
        pthread_mutex_lock(&rp->mu);
        while (!rp->stop && !rp->head) {
            uint64_t now = now_ms(), soon = now + RETRY_MS;
            int due = 0;
            for (int i = 0; i < rp->n; i++) { if (rp->c[i].next_ms <= now) due = 1; if (rp->c[i].next_ms < soon) soon = rp->c[i].next_ms; }
            if (due) break;
            struct timespec ts = { (time_t)(soon / 1000u), (long)(soon % 1000u) * 1000000L };
            pthread_cond_timedwait(&rp->cv, &rp->mu, &ts);
        }
        if (rp->stop) { pthread_mutex_unlock(&rp->mu); break; }
        req *q = rp->head;
        if (q) { rp->head = q->next; if (!rp->head) rp->tail = NULL; rp->queued--; }
        pthread_mutex_unlock(&rp->mu);
        if (q) { exchange(rp, q); memset(q, 0, sizeof *q); free(q); continue; }
        for (int i = 0; i < rp->n; i++) {
            pthread_mutex_lock(&rp->mu);
            int due = rp->c[i].next_ms <= now_ms();
            pthread_mutex_unlock(&rp->mu);
            if (due) refresh(rp, &rp->c[i]);
        }
    }
    return NULL;
}

oc_oidcrp *oc_oidcrp_start(const oc_oidc_connect *conns, int n, oc_dbwriter *dbw) {
    if (n < 1 || n > OC_OIDC_MAX_CONNECT) return NULL;
    oc_oidcrp *rp = calloc(1, sizeof *rp);
    if (!rp) return NULL;
    pthread_mutex_init(&rp->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_REALTIME);
    pthread_cond_init(&rp->cv, &ca);
    pthread_condattr_destroy(&ca);
    rp->n = n;
    rp->dbw = dbw;
    for (int i = 0; i < n; i++) rp->c[i].cfg = conns[i];
    if (pthread_create(&rp->th, NULL, worker, rp) != 0) {
        pthread_mutex_destroy(&rp->mu); pthread_cond_destroy(&rp->cv); free(rp);
        return NULL;
    }
    return rp;
}

void oc_oidcrp_stop(oc_oidcrp *rp) {
    if (!rp) return;
    pthread_mutex_lock(&rp->mu);
    rp->stop = 1;
    pthread_cond_signal(&rp->cv);
    pthread_mutex_unlock(&rp->mu);
    pthread_join(rp->th, NULL);
    for (req *q = rp->head, *n; q; q = n) { n = q->next; memset(q, 0, sizeof *q); free(q); }
    for (int i = 0; i < rp->n; i++) { free(rp->c[i].jwks); memset(rp->c[i].cfg.secret, 0, sizeof rp->c[i].cfg.secret); }
    pthread_mutex_destroy(&rp->mu);
    pthread_cond_destroy(&rp->cv);
    free(rp);
}

int oc_oidcrp_ready(oc_oidcrp *rp, int i, char *authorize, size_t cap) {
    if (!rp || i < 0 || i >= rp->n) return 0;
    pthread_mutex_lock(&rp->mu);
    int ok = rp->c[i].ok;
    if (ok && authorize) snprintf(authorize, cap, "%s", rp->c[i].authz);
    pthread_mutex_unlock(&rp->mu);
    return ok;
}

int oc_oidcrp_exchange(oc_oidcrp *rp, int i, uint64_t conn_id, const char *code, const char *redirect_uri,
                       const char *verifier, const char *nonce, const char *source) {
    if (!rp || i < 0 || i >= rp->n) return -1;
    req *q = calloc(1, sizeof *q);
    if (!q) return -1;
    q->i = i;
    q->conn_id = conn_id;
    snprintf(q->code, sizeof q->code, "%s", code);
    snprintf(q->redirect, sizeof q->redirect, "%s", redirect_uri);
    snprintf(q->verifier, sizeof q->verifier, "%s", verifier);
    snprintf(q->nonce, sizeof q->nonce, "%s", nonce);
    snprintf(q->source, sizeof q->source, "%s", source ? source : "");
    pthread_mutex_lock(&rp->mu);
    if (rp->queued >= QUEUE_MAX) { pthread_mutex_unlock(&rp->mu); free(q); return -1; }
    if (rp->tail) rp->tail->next = q; else rp->head = q;
    rp->tail = q;
    rp->queued++;
    pthread_cond_signal(&rp->cv);
    pthread_mutex_unlock(&rp->mu);
    return 0;
}
