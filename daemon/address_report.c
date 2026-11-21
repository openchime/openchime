/* The address report (AUTH.md §3.6). See address_report.h. The transport and the
 * signing are the push emitter's (push.h's oc_machine_http), and each request
 * opens its own connection, so nothing is held across the wait between rounds;
 * this file is the rounds, the retries and what the answers mean. */

#include "address_report.h"
#include "enroll.h"
#include "push.h"

#define JSMN_HEADER   /* declarations only; the impl lives in daemon/jwt.c's TU */
#include "jsmn.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct oc_address_report {
    oc_machine_http          *http;
    char                     *audience, *privkey, *body;
    oc_address_report_timing  t;

    pthread_t                 thread;
    pthread_mutex_t           mu;
    pthread_cond_t            cv;
    int                       stopping;
};

static const oc_address_report_timing PRODUCTION = {
    OC_ADDRESS_FIRST_RETRY_SECS * 1000u, OC_ADDRESS_RETRY_MAX_SECS * 1000u,
    OC_ADDRESS_ROUND_SECS * 1000u, OC_ADDRESS_REPEAT_SECS * 1000u,
};

static unsigned long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}

oc_address_next oc_address_report_disposition(int sent, int status) {
    if (!sent || status == 422 || status == 429 || status >= 500) return OC_ADDRESS_RETRY;
    if (status >= 200 && status < 300) return OC_ADDRESS_RECORDED;
    return OC_ADDRESS_REFUSED;   /* any other 4xx: asking again gets the same answer */
}

int oc_address_report_recorded(const char *resp, char *out, size_t cap) {
    if (!resp || !out || cap == 0) return -1;
    jsmn_parser p;
    jsmntok_t t[32];
    jsmn_init(&p);
    int n = jsmn_parse(&p, resp, strlen(resp), t, 32);
    if (n <= 0 || t[0].type != JSMN_OBJECT) return -1;
    for (int i = 1; i + 1 < n; i++) {
        if (t[i].type == JSMN_STRING && t[i].end - t[i].start == 7 && !memcmp(resp + t[i].start, "address", 7) &&
            t[i + 1].type == JSMN_STRING) {
            size_t len = (size_t)(t[i + 1].end - t[i + 1].start);
            if (len == 0 || len >= cap) return -1;
            /* Logged: printable ASCII only, so a reply cannot write control bytes into the log. */
            for (size_t k = 0; k < len; k++) {
                unsigned char c = (unsigned char)resp[t[i + 1].start + k];
                if (c < 0x21 || c > 0x7e || c == '\\') return -1;
            }
            memcpy(out, resp + t[i + 1].start, len);
            out[len] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Wait up to `ms`, returning non-zero once stopping. Called with the lock held. */
static int wait_ms(oc_address_report *r, unsigned long long ms) {
    unsigned long long until_ms = mono_ms() + ms;
    struct timespec until = { (time_t)(until_ms / 1000), (long)(until_ms % 1000) * 1000000L };
    while (!r->stopping && mono_ms() < until_ms)
        pthread_cond_timedwait(&r->cv, &r->mu, &until);
    return r->stopping;
}

/* One round: 1 when it ended in a refusal (or a stop), 0 otherwise. */
static int round_once(oc_address_report *r) {
    unsigned long long end = mono_ms() + r->t.round_ms;
    unsigned long long pause = r->t.first_retry_ms;
    for (;;) {
        pthread_mutex_unlock(&r->mu);
        char resp[1024];
        int status = 0;
        int sent = oc_machine_http_post_read(r->http, OC_ADDRESS_REPORT_PATH, r->audience, r->privkey,
                                             r->body, &status, resp, sizeof resp) == 0;
        oc_address_next next = oc_address_report_disposition(sent, status);
        pthread_mutex_lock(&r->mu);

        if (next == OC_ADDRESS_RECORDED) {
            char addr[300];
            if (oc_address_report_recorded(resp, addr, sizeof addr) == 0)
                fprintf(stderr, "openchimed: central recorded this workspace's address (%s)\n", addr);
            else
                fprintf(stderr, "openchimed: central recorded the address this server is reached at\n");
            return 0;
        }
        if (next == OC_ADDRESS_REFUSED) {
            fprintf(stderr, "openchimed: central refused this workspace's address report (HTTP %d); "
                            "reporting again on next start\n", status);
            return 1;
        }
        if (mono_ms() >= end) {
            fprintf(stderr, "openchimed: central could not record this workspace's address; "
                            "trying again in %u minutes\n", r->t.repeat_ms / 60000u);
            return 0;
        }
        if (wait_ms(r, pause)) return 1;
        pause = pause * 2 > r->t.retry_max_ms ? r->t.retry_max_ms : pause * 2;
    }
}

static void *worker(void *arg) {
    oc_address_report *r = arg;
    pthread_mutex_lock(&r->mu);
    while (!r->stopping) {
        if (round_once(r)) break;
        if (wait_ms(r, r->t.repeat_ms)) break;
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

oc_address_report *oc_address_report_start(const char *enroll_url, const char *audience,
                                           const char *privkey_pem, const char *body,
                                           const oc_address_report_timing *timing) {
    if (!enroll_url || !*enroll_url || !audience || !privkey_pem || !body) return NULL;
    oc_address_report *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->t = timing ? *timing : PRODUCTION;
    if (r->t.first_retry_ms == 0) r->t.first_retry_ms = 1;
    if (r->t.retry_max_ms < r->t.first_retry_ms) r->t.retry_max_ms = r->t.first_retry_ms;
    r->audience = strdup(audience);
    r->privkey  = strdup(privkey_pem);
    r->body     = strdup(body);
    r->http     = oc_machine_http_open(enroll_url);
    if (!r->audience || !r->privkey || !r->body || !r->http) goto fail;

    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) goto fail;
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int cv_ok = pthread_cond_init(&r->cv, &ca) == 0;
    pthread_condattr_destroy(&ca);
    if (!cv_ok) goto fail;
    if (pthread_mutex_init(&r->mu, NULL) != 0) { pthread_cond_destroy(&r->cv); goto fail; }
    if (pthread_create(&r->thread, NULL, worker, r) != 0) {
        pthread_mutex_destroy(&r->mu); pthread_cond_destroy(&r->cv); goto fail;
    }
    return r;

fail:
    oc_machine_http_close(r->http);
    free(r->audience);
    free(r->privkey);
    free(r->body);
    free(r);
    return NULL;
}

void oc_address_report_stop(oc_address_report *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->stopping = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    pthread_join(r->thread, NULL);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    oc_machine_http_close(r->http);
    free(r->audience);
    free(r->privkey);
    free(r->body);
    free(r);
}
