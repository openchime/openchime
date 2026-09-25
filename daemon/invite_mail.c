/* Invitation mail report (ARCH-85). See invite_mail.h. The transport and the
 * signing are the push emitter's (push.h's oc_machine_http); this file is the
 * queue, the body and the retry policy. */

#include "invite_mail.h"
#include "push.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OC_INVITE_MAIL_MAX_QUEUE 1024
#define OC_INVITE_MAIL_PATH      "/api/machine/invite/notify"
#define OC_INVITE_MAIL_EMAIL_MAX 256   /* an address is at most 254 bytes */

typedef struct inode {
    char          id[33];
    char          email[OC_INVITE_MAIL_EMAIL_MAX];
    uint64_t      expires_at;
    int           attempts;     /* made so far */
    uint64_t      due_ms;       /* monotonic; when it may be sent */
    struct inode *next;
} inode;

struct oc_invite_mail {
    oc_machine_http *http;
    char            *audience;
    char            *privkey;
    unsigned         backoff_ms;

    pthread_t        thread;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    inode           *head;
    int              qlen;
    int              stopping;
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int oc_invite_mail_build_body(const char *invite_id, const char *email, uint64_t expires_at,
                              char *out, size_t cap) {
    if (!invite_id || !email || !out || cap == 0) return -1;
    size_t off = 0;
    int w = snprintf(out, cap, "{\"inviteId\":\"%s\",\"email\":\"", invite_id);
    if (w < 0 || (size_t)w >= cap) return -1;
    off = (size_t)w;
    for (const unsigned char *p = (const unsigned char *)email; *p; p++) {
        char esc[8];
        const char *add = esc;
        if (*p == '"')       add = "\\\"";
        else if (*p == '\\') add = "\\\\";
        else if (*p < 0x20)  snprintf(esc, sizeof esc, "\\u%04x", *p);
        else { esc[0] = (char)*p; esc[1] = '\0'; }
        size_t al = strlen(add);
        if (off + al >= cap) return -1;
        memcpy(out + off, add, al);
        off += al;
    }
    w = snprintf(out + off, cap - off, "\",\"expiresAt\":%llu}", (unsigned long long)expires_at);
    if (w < 0 || (size_t)w >= cap - off) return -1;
    return 0;
}

oc_invite_mail_next oc_invite_mail_disposition(int sent, int status) {
    if (!sent || status >= 500) return OC_INVITE_MAIL_RETRY;   /* central is having trouble */
    if (status >= 200 && status < 300) return OC_INVITE_MAIL_DONE;
    /* 4xx and anything else central chose to say: asking again gets the same answer. */
    return OC_INVITE_MAIL_FINAL;
}

/* The node due soonest, or NULL. Called with the lock held. */
static inode *soonest(oc_invite_mail *m) {
    inode *best = NULL;
    for (inode *n = m->head; n; n = n->next)
        if (!best || n->due_ms < best->due_ms) best = n;
    return best;
}

static void unlink_node(oc_invite_mail *m, inode *x) {
    for (inode **pp = &m->head; *pp; pp = &(*pp)->next)
        if (*pp == x) { *pp = x->next; m->qlen--; return; }
}

static void *worker(void *arg) {
    oc_invite_mail *m = arg;
    pthread_mutex_lock(&m->mu);
    for (;;) {
        if (m->stopping) break;
        inode *n = soonest(m);
        if (!n) { pthread_cond_wait(&m->cv, &m->mu); continue; }
        uint64_t now = mono_ms();
        if (n->due_ms > now) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            uint64_t wait = n->due_ms - now;
            until.tv_sec  += (time_t)(wait / 1000);
            until.tv_nsec += (long)(wait % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&m->cv, &m->mu, &until);
            continue;
        }
        unlink_node(m, n);
        pthread_mutex_unlock(&m->mu);

        char body[512];
        int status = 0, sent = 0;
        if (oc_invite_mail_build_body(n->id, n->email, n->expires_at, body, sizeof body) == 0)
            sent = oc_machine_http_post(m->http, OC_INVITE_MAIL_PATH, m->audience, m->privkey,
                                        body, &status) == 0;
        n->attempts++;
        oc_invite_mail_next next = oc_invite_mail_disposition(sent, status);

        pthread_mutex_lock(&m->mu);
        if (next == OC_INVITE_MAIL_RETRY && n->attempts < OC_INVITE_MAIL_ATTEMPTS && !m->stopping) {
            n->due_ms = mono_ms() + ((uint64_t)m->backoff_ms << (n->attempts - 1));
            n->next = m->head; m->head = n; m->qlen++;
            continue;
        }
        /* The address stays out of the log: the report is about a person. */
        if (next == OC_INVITE_MAIL_FINAL)
            fprintf(stderr, "openchimed: invitation mail report %s refused (HTTP %d)\n", n->id, status);
        else if (next == OC_INVITE_MAIL_RETRY)
            fprintf(stderr, "openchimed: invitation mail report %s not delivered after %d attempts; "
                            "the copyable invitation still stands\n", n->id, n->attempts);
        free(n);
    }
    pthread_mutex_unlock(&m->mu);
    return NULL;
}

oc_invite_mail *oc_invite_mail_start_backoff(const char *enroll_url, const char *ca_bundle,
                                             const char *audience, const char *privkey_pem,
                                             unsigned backoff_ms) {
    if (!enroll_url || !*enroll_url || !audience || !privkey_pem) return NULL;
    oc_invite_mail *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->backoff_ms = backoff_ms ? backoff_ms : 1;
    m->audience = strdup(audience);
    m->privkey  = strdup(privkey_pem);
    m->http     = oc_machine_http_open(enroll_url, ca_bundle);
    if (!m->audience || !m->privkey || !m->http) goto fail;

    pthread_condattr_t ca;
    if (pthread_condattr_init(&ca) != 0) goto fail;
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int cv_ok = pthread_cond_init(&m->cv, &ca) == 0;
    pthread_condattr_destroy(&ca);
    if (!cv_ok) goto fail;
    if (pthread_mutex_init(&m->mu, NULL) != 0) { pthread_cond_destroy(&m->cv); goto fail; }
    if (pthread_create(&m->thread, NULL, worker, m) != 0) {
        pthread_mutex_destroy(&m->mu); pthread_cond_destroy(&m->cv); goto fail;
    }
    return m;

fail:
    oc_machine_http_close(m->http);
    free(m->audience);
    free(m->privkey);
    free(m);
    return NULL;
}

oc_invite_mail *oc_invite_mail_start(const char *enroll_url, const char *ca_bundle,
                                     const char *audience, const char *privkey_pem) {
    return oc_invite_mail_start_backoff(enroll_url, ca_bundle, audience, privkey_pem, 2000);
}

void oc_invite_mail_report(oc_invite_mail *m, const char *invite_id, const char *email,
                           uint64_t expires_at) {
    if (!m || !invite_id || strlen(invite_id) != 32 || !email || !*email) return;
    if (strlen(email) >= OC_INVITE_MAIL_EMAIL_MAX) return;
    inode *n = calloc(1, sizeof *n);
    if (!n) return;
    memcpy(n->id, invite_id, 33);
    snprintf(n->email, sizeof n->email, "%s", email);
    n->expires_at = expires_at;
    n->due_ms = mono_ms();
    pthread_mutex_lock(&m->mu);
    if (m->stopping || m->qlen >= OC_INVITE_MAIL_MAX_QUEUE) {
        pthread_mutex_unlock(&m->mu);
        free(n);
        return;
    }
    n->next = m->head; m->head = n; m->qlen++;
    pthread_cond_signal(&m->cv);
    pthread_mutex_unlock(&m->mu);
}

void oc_invite_mail_stop(oc_invite_mail *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    m->stopping = 1;
    pthread_cond_broadcast(&m->cv);
    pthread_mutex_unlock(&m->mu);
    pthread_join(m->thread, NULL);
    for (inode *n = m->head; n; ) { inode *nx = n->next; free(n); n = nx; }
    pthread_cond_destroy(&m->cv);
    pthread_mutex_destroy(&m->mu);
    oc_machine_http_close(m->http);
    free(m->audience);
    free(m->privkey);
    free(m);
}
