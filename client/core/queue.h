/*
 * OpenChime client — thread-safe pointer queue (ARCH-62).
 *
 * A mutex+condvar FIFO of opaque pointers, one each way between the UI thread
 * and the network thread (the same shape as the daemon's net↔dbwriter queue).
 * The UI thread uses oc_queue_try_pop (non-blocking, drained each frame); the
 * network thread can block with oc_queue_pop or poll with try_pop.
 */

#ifndef OC_QUEUE_H
#define OC_QUEUE_H

#include "oc_thread.h"

typedef struct oc_qnode { void *item; struct oc_qnode *next; } oc_qnode;

typedef struct {
    oc_mutex_t mu;
    oc_cond_t  cv;
    oc_qnode       *head, *tail;
} oc_queue;

void  oc_queue_init(oc_queue *q);
void  oc_queue_destroy(oc_queue *q);      /* frees remaining nodes (not items) */
void  oc_queue_push(oc_queue *q, void *item);
void *oc_queue_pop(oc_queue *q);          /* blocks until an item is available */
void *oc_queue_try_pop(oc_queue *q);      /* NULL if empty */

#endif /* OC_QUEUE_H */
