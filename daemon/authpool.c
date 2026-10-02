/* The auth pool. See authpool.h. */

#include "authpool.h"
#include "e2e_hpke.h"

#include <pthread.h>
#include <stdlib.h>

#define OC_AUTHPOOL_MAX 8

struct oc_authpool {
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    oc_auth_check   *head, *tail;
    int              stop;
    int              hold;
    size_t           waiting;
    pthread_t        th[OC_AUTHPOOL_MAX];
    int              n;
    oc_authpool_done done;
    void            *ctx;
};

static void *pool_loop(void *arg) {
    oc_authpool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->stop && (!p->head || p->hold)) pthread_cond_wait(&p->cv, &p->mu);
        if (p->stop) { pthread_mutex_unlock(&p->mu); break; }   /* the rest go back in stop */
        oc_auth_check *c = p->head;
        p->head = c->next;
        if (!p->head) p->tail = NULL;
        p->waiting--;
        pthread_mutex_unlock(&p->mu);

        uint8_t derived[OC_PW_HASH_LEN];
        if (c->password)
            c->ok = oc_pw_derive(c->password, c->pwlen, c->salt, c->slen, c->iters, derived) == 0 &&
                    oc_ct_eq(derived, c->stored, OC_PW_HASH_LEN);
        else
            c->ok = c->new_password != NULL;   /* derive-only */
        oc_e2e_wipe(derived, sizeof derived);
        c->derived = c->ok == 1 && c->new_password &&
                     oc_pw_derive(c->new_password, c->new_pwlen, c->new_salt, sizeof c->new_salt,
                                  c->new_iters, c->new_hash) == 0;
        p->done(c, p->ctx);
    }
    return NULL;
}

oc_authpool *oc_authpool_start(int nthreads, oc_authpool_done done, void *ctx) {
    if (nthreads < 1) nthreads = 1;
    if (nthreads > OC_AUTHPOOL_MAX) nthreads = OC_AUTHPOOL_MAX;
    oc_authpool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->done = done;
    p->ctx = ctx;
    for (int i = 0; i < nthreads; i++)
        if (pthread_create(&p->th[p->n], NULL, pool_loop, p) == 0) p->n++;
    if (p->n == 0) {
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
    return p;
}

void oc_authpool_submit(oc_authpool *p, oc_auth_check *chk) {
    chk->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->stop) {
        pthread_mutex_unlock(&p->mu);
        chk->ok = -1;
        p->done(chk, p->ctx);
        return;
    }
    if (p->tail) p->tail->next = chk; else p->head = chk;
    p->tail = chk;
    p->waiting++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

void oc_authpool_hold(oc_authpool *p, int on) {
    pthread_mutex_lock(&p->mu);
    p->hold = on;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

size_t oc_authpool_waiting(oc_authpool *p) {
    pthread_mutex_lock(&p->mu);
    size_t n = p->waiting;
    pthread_mutex_unlock(&p->mu);
    return n;
}

void oc_authpool_stop(oc_authpool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    oc_auth_check *rest = p->head;
    p->head = p->tail = NULL;
    p->waiting = 0;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (oc_auth_check *c = rest, *n; c; c = n) {
        n = c->next;
        c->ok = -1;
        p->done(c, p->ctx);
    }
    for (int i = 0; i < p->n; i++) pthread_join(p->th[i], NULL);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}
