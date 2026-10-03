/* The relay's published keys -- see relaykeys.h. */

#include "relaykeys.h"
#include "https_client.h"
#include "json.h"
#include "jwt.h"

#include <mbedtls/pk.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REFRESH_MS (24ull * 60 * 60 * 1000)   /* daily */
#define RETRY_MS   (60ull * 1000)             /* until the relay first answers, and after a failure */
#define TIMEOUT_MS 10000

struct oc_relaykeys {
    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             stop, fetched;
    char            url[512];
    oc_dbwriter    *dbw;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int oc_relaykeys_pem(const char *jwks, size_t len, char *out, size_t cap) {
    static const unsigned char SPKI[] = { 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
                                          0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
                                          0x42, 0x00, 0x04 };
    if (!jwks || !out || cap == 0) return -1;
    out[0] = '\0';
    oc_json d;
    if (oc_json_parse(&d, jwks, len) != 0) return -1;
    int keys = oc_json_get(&d, 0, "keys");
    if (keys < 0 || d.t[keys].type != JSMN_ARRAY) { oc_json_free(&d); return -1; }
    int n = 0, rc = 0;
    size_t used = 0;
    for (int k = 0, i = keys + 1; k < d.t[keys].size && i < d.n && n < OC_RELAYKEYS_MAX; k++, i = oc_json_skip(&d, i)) {
        char kty[8], crv[16], use[8], alg[16], x[64], y[64];
        if (d.t[i].type != JSMN_OBJECT || oc_json_get_str(&d, i, "kty", kty, sizeof kty) != 0 || strcmp(kty, "EC") != 0 ||
            oc_json_get_str(&d, i, "crv", crv, sizeof crv) != 0 || strcmp(crv, "P-256") != 0) continue;
        /* a key marked for anything but signing, or for another algorithm, is not the relay's token key */
        if (oc_json_get_str(&d, i, "use", use, sizeof use) == 0 && strcmp(use, "sig") != 0) continue;
        if (oc_json_get_str(&d, i, "alg", alg, sizeof alg) == 0 && strcmp(alg, "ES256") != 0) continue;
        unsigned char der[sizeof SPKI + 64];
        memcpy(der, SPKI, sizeof SPKI);
        if (oc_json_get_str(&d, i, "x", x, sizeof x) != 0 || oc_json_get_str(&d, i, "y", y, sizeof y) != 0 ||
            oc_base64url_decode(x, strlen(x), der + sizeof SPKI, 32) != 32 ||
            oc_base64url_decode(y, strlen(y), der + sizeof SPKI + 32, 32) != 32) continue;
        /* parsed, so a point that is not on the curve goes no further */
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        if (mbedtls_pk_parse_public_key(&pk, der, sizeof der) == 0) {
            if (mbedtls_pk_write_pubkey_pem(&pk, (unsigned char *)out + used, cap - used) == 0) {
                used += strlen(out + used);
                n++;
            } else {
                rc = -1;   /* it does not fit */
            }
        }
        mbedtls_pk_free(&pk);
        if (rc) break;
    }
    oc_json_free(&d);
    if (rc) { out[0] = '\0'; return -1; }
    return n;
}

/* One fetch. 0 if the writer was given a key set. */
static int fetch(oc_relaykeys *rk) {
    oc_https_resp r;
    char err[200];
    if (oc_https_request("GET", rk->url, NULL, NULL, 0, "Accept: application/json\r\n", TIMEOUT_MS, &r,
                         err, sizeof err) != 0) {
        fprintf(stderr, "openchimed: the relay's keys at %s: %s\n", rk->url, err);
        return -1;
    }
    int rc = -1;
    char *pem = malloc(OC_RELAYKEYS_MAX * 256);
    if (pem && r.status == 200 && r.body) {
        int n = oc_relaykeys_pem(r.body, r.body_len, pem, OC_RELAYKEYS_MAX * 256);
        if (n > 0) {
            oc_dbwriter_set_relay_keys(rk->dbw, pem);
            rc = 0;
        } else {
            fprintf(stderr, "openchimed: the relay's keys at %s: no ES256 key in the answer\n", rk->url);
        }
    } else if (pem) {
        fprintf(stderr, "openchimed: the relay's keys at %s: HTTP %d\n", rk->url, r.status);
    }
    free(pem);
    oc_https_resp_free(&r);
    return rc;
}

static void *worker(void *arg) {
    oc_relaykeys *rk = arg;
    for (;;) {
        int ok = fetch(rk) == 0;
        pthread_mutex_lock(&rk->mu);
        if (ok) rk->fetched = 1;
        uint64_t due = now_ms() + (ok ? REFRESH_MS : RETRY_MS);
        struct timespec ts = { (time_t)(due / 1000u), (long)(due % 1000u) * 1000000L };
        while (!rk->stop && now_ms() < due)
            if (pthread_cond_timedwait(&rk->cv, &rk->mu, &ts) != 0) break;
        int stop = rk->stop;
        pthread_mutex_unlock(&rk->mu);
        if (stop) break;
    }
    return NULL;
}

oc_relaykeys *oc_relaykeys_start(const char *jwks_url, oc_dbwriter *dbw) {
    if (!jwks_url || !dbw || strlen(jwks_url) >= sizeof ((oc_relaykeys *)0)->url) return NULL;
    oc_relaykeys *rk = calloc(1, sizeof *rk);
    if (!rk) return NULL;
    snprintf(rk->url, sizeof rk->url, "%s", jwks_url);
    rk->dbw = dbw;
    pthread_mutex_init(&rk->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_REALTIME);
    pthread_cond_init(&rk->cv, &ca);
    pthread_condattr_destroy(&ca);
    if (pthread_create(&rk->th, NULL, worker, rk) != 0) {
        pthread_mutex_destroy(&rk->mu); pthread_cond_destroy(&rk->cv); free(rk);
        return NULL;
    }
    return rk;
}

int oc_relaykeys_fetched(oc_relaykeys *rk) {
    if (!rk) return 0;
    pthread_mutex_lock(&rk->mu);
    int f = rk->fetched;
    pthread_mutex_unlock(&rk->mu);
    return f;
}

void oc_relaykeys_stop(oc_relaykeys *rk) {
    if (!rk) return;
    pthread_mutex_lock(&rk->mu);
    rk->stop = 1;
    pthread_cond_signal(&rk->cv);
    pthread_mutex_unlock(&rk->mu);
    pthread_join(rk->th, NULL);
    pthread_mutex_destroy(&rk->mu);
    pthread_cond_destroy(&rk->cv);
    free(rk);
}
