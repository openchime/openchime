#include "queue.h"

#include <stdlib.h>

void oc_queue_init(oc_queue *q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->head = q->tail = NULL;
}

void oc_queue_destroy(oc_queue *q) {
    for (oc_qnode *n = q->head; n; ) { oc_qnode *nx = n->next; free(n); n = nx; }
    q->head = q->tail = NULL;
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
}

void oc_queue_push(oc_queue *q, void *item) {
    oc_qnode *n = malloc(sizeof *n);
    if (!n) return;
    n->item = item;
    n->next = NULL;
    pthread_mutex_lock(&q->mu);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static void *pop_locked(oc_queue *q) {
    oc_qnode *n = q->head;
    if (!n) return NULL;
    q->head = n->next;
    if (!q->head) q->tail = NULL;
    void *item = n->item;
    free(n);
    return item;
}

void *oc_queue_pop(oc_queue *q) {
    pthread_mutex_lock(&q->mu);
    while (!q->head) pthread_cond_wait(&q->cv, &q->mu);
    void *item = pop_locked(q);
    pthread_mutex_unlock(&q->mu);
    return item;
}

void *oc_queue_try_pop(oc_queue *q) {
    pthread_mutex_lock(&q->mu);
    void *item = pop_locked(q);
    pthread_mutex_unlock(&q->mu);
    return item;
}
