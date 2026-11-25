/* Release notes for the clients. See release_notes.h. The transport and the
 * signing are the push emitter's (push.h's oc_machine_http); this file is the
 * cache, the one-fetch-per-build rule and the hand-back to the net loop. */

#include "release_notes.h"
#include "central.h"
#include "protocol.h"   /* OC_RELEASE_NOTES_MAX */
#include "push.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct waiter { uint64_t conn_id; struct waiter *next; } waiter;

typedef struct {
    char     build_id[65];
    int      used;          /* the slot holds an entry */
    int      fetching;      /* a fetch is queued or under way */
    int      have;          /* an answer (or a failure) is held */
    int      ok, status;
    char    *body;
    size_t   body_len;
    uint64_t at_ms;         /* when the held answer came */
    uint64_t used_ms;       /* when it was last asked for */
    waiter  *waiting;
} entry;

typedef struct done { oc_rn_answer a; struct done *next; } done;

struct oc_release_notes_worker {
    char            *url, *audience, *privkey;
    oc_rn_clock      clock;
    void            *clock_ctx;
    int              efd;
    pthread_t        thread;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    int              stopping;
    entry            cache[OC_RELEASE_NOTES_CACHE_MAX];
    done            *head, *tail;
    unsigned         fetches;
};

static uint64_t wall_ms(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int fresh(const entry *e, uint64_t now) {
    if (!e->have) return 0;
    uint64_t keep = e->ok ? OC_RELEASE_NOTES_CACHE_MS : OC_RELEASE_NOTES_FAIL_MS;
    return now - e->at_ms < keep;
}

static void fill(oc_rn_answer *a, const entry *e, uint64_t conn_id) {
    memset(a, 0, sizeof *a);
    a->conn_id = conn_id;
    snprintf(a->build_id, sizeof a->build_id, "%s", e->build_id);
    a->ok = e->ok;
    a->status = e->status;
    if (e->ok && e->body_len) {
        a->body = malloc(e->body_len + 1);
        if (a->body) { memcpy(a->body, e->body, e->body_len); a->body[e->body_len] = '\0'; a->body_len = e->body_len; }
        else a->ok = 0;
    }
}

static void clear(entry *e) {
    free(e->body);
    while (e->waiting) { waiter *n = e->waiting->next; free(e->waiting); e->waiting = n; }
    memset(e, 0, sizeof *e);
}

/* The slot for `id`: its own, a free one, or the least recently used that is not
 * being fetched. NULL when every slot is busy fetching. Called locked. */
static entry *slot_for(oc_release_notes_worker *w, const char *id) {
    entry *free_slot = NULL, *lru = NULL;
    for (int i = 0; i < OC_RELEASE_NOTES_CACHE_MAX; i++) {
        entry *e = &w->cache[i];
        if (e->used && !strcmp(e->build_id, id)) return e;
        if (!e->used) { if (!free_slot) free_slot = e; continue; }
        if (!e->fetching && (!lru || e->used_ms < lru->used_ms)) lru = e;
    }
    entry *e = free_slot ? free_slot : lru;
    if (!e) return NULL;
    clear(e);
    e->used = 1;
    snprintf(e->build_id, sizeof e->build_id, "%s", id);
    return e;
}

static void push_done(oc_release_notes_worker *w, const oc_rn_answer *a) {
    done *d = malloc(sizeof *d);
    if (!d) { free(a->body); return; }
    d->a = *a;
    d->next = NULL;
    if (w->tail) w->tail->next = d; else w->head = d;
    w->tail = d;
}

int oc_release_notes_ask(oc_release_notes_worker *w, const char *build_id, uint64_t conn_id, oc_rn_answer *now) {
    if (!w || !build_id || !now) return -1;
    pthread_mutex_lock(&w->mu);
    uint64_t t = w->clock(w->clock_ctx);
    entry *e = slot_for(w, build_id);
    if (!e) {   /* every slot is mid-fetch: unavailable for now */
        memset(now, 0, sizeof *now);
        now->conn_id = conn_id;
        snprintf(now->build_id, sizeof now->build_id, "%s", build_id);
        pthread_mutex_unlock(&w->mu);
        return 1;
    }
    e->used_ms = t;
    if (fresh(e, t)) {
        fill(now, e, conn_id);
        pthread_mutex_unlock(&w->mu);
        return 1;
    }
    waiter *wt = malloc(sizeof *wt);
    if (!wt) {
        memset(now, 0, sizeof *now);
        now->conn_id = conn_id;
        snprintf(now->build_id, sizeof now->build_id, "%s", build_id);
        pthread_mutex_unlock(&w->mu);
        return 1;
    }
    wt->conn_id = conn_id;
    wt->next = e->waiting;
    e->waiting = wt;
    if (!e->fetching) { e->fetching = 1; pthread_cond_signal(&w->cv); }
    pthread_mutex_unlock(&w->mu);
    return 0;
}

/* One fetch: central's answer for `id`. */
static void fetch(oc_release_notes_worker *w, const char *id, int *ok, int *status, char **body, size_t *len) {
    *ok = 0; *status = 0; *body = NULL; *len = 0;
    char path[160];
    if (snprintf(path, sizeof path, OC_CENTRAL_RELEASE_BUILD "%s", id) >= (int)sizeof path) return;
    oc_machine_http *h = oc_machine_http_open(w->url);
    if (!h) return;
    size_t cap = OC_RELEASE_NOTES_MAX + 2;   /* one byte over the limit shows a body too large */
    char *resp = malloc(cap);
    size_t rlen = 0;
    int st = 0;
    if (resp && oc_machine_http_get_read(h, path, w->audience, w->privkey, &st, resp, cap, &rlen) == 0 &&
        (st == 200 || st == 404) && rlen <= OC_RELEASE_NOTES_MAX) {
        *ok = 1; *status = st;
        if (st == 200 && rlen) { *body = resp; *len = rlen; resp = NULL; }
    }
    free(resp);
    oc_machine_http_close(h);
}

static void *run(void *arg) {
    oc_release_notes_worker *w = arg;
    pthread_mutex_lock(&w->mu);
    for (;;) {
        entry *e = NULL;
        for (int i = 0; i < OC_RELEASE_NOTES_CACHE_MAX && !e; i++)
            if (w->cache[i].used && w->cache[i].fetching) e = &w->cache[i];
        if (w->stopping) break;
        if (!e) { pthread_cond_wait(&w->cv, &w->mu); continue; }
        char id[65];
        snprintf(id, sizeof id, "%s", e->build_id);
        w->fetches++;
        pthread_mutex_unlock(&w->mu);
        int ok, status; char *body; size_t len;
        fetch(w, id, &ok, &status, &body, &len);
        pthread_mutex_lock(&w->mu);
        /* The slot is still this build's: only a fetching slot is never reused. */
        free(e->body);
        e->fetching = 0;
        e->have = 1;
        e->ok = ok; e->status = status; e->body = body; e->body_len = len;
        e->at_ms = w->clock(w->clock_ctx);
        int any = e->waiting != NULL;
        while (e->waiting) {
            waiter *n = e->waiting->next;
            oc_rn_answer a;
            fill(&a, e, e->waiting->conn_id);
            push_done(w, &a);
            free(e->waiting);
            e->waiting = n;
        }
        if (any) { uint64_t one = 1; ssize_t wr = write(w->efd, &one, sizeof one); (void)wr; }
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

oc_release_notes_worker *oc_release_notes_start(const char *enroll_url, const char *audience,
                                                const char *privkey_pem, oc_rn_clock clock, void *clock_ctx) {
    if (!enroll_url || !*enroll_url || !audience || !privkey_pem) return NULL;
    oc_release_notes_worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->url = strdup(enroll_url);
    w->audience = strdup(audience);
    w->privkey = strdup(privkey_pem);
    w->clock = clock ? clock : wall_ms;
    w->clock_ctx = clock_ctx;
    w->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (!w->url || !w->audience || !w->privkey || w->efd < 0 || pthread_create(&w->thread, NULL, run, w) != 0) {
        if (w->efd >= 0) close(w->efd);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        free(w->url); free(w->audience); free(w->privkey); free(w);
        return NULL;
    }
    return w;
}

int oc_release_notes_eventfd(oc_release_notes_worker *w) { return w ? w->efd : -1; }

int oc_release_notes_next(oc_release_notes_worker *w, oc_rn_answer *a) {
    if (!w || !a) return 0;
    pthread_mutex_lock(&w->mu);
    done *d = w->head;
    if (d) { w->head = d->next; if (!w->head) w->tail = NULL; }
    pthread_mutex_unlock(&w->mu);
    if (!d) return 0;
    *a = d->a;
    free(d);
    return 1;
}

void oc_rn_answer_free(oc_rn_answer *a) {
    if (!a) return;
    free(a->body);
    a->body = NULL;
    a->body_len = 0;
}

unsigned oc_release_notes_fetches(oc_release_notes_worker *w) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    unsigned n = w->fetches;
    pthread_mutex_unlock(&w->mu);
    return n;
}

void oc_release_notes_stop(oc_release_notes_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (int i = 0; i < OC_RELEASE_NOTES_CACHE_MAX; i++) clear(&w->cache[i]);
    oc_rn_answer a;
    while (oc_release_notes_next(w, &a)) oc_rn_answer_free(&a);
    close(w->efd);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w->url); free(w->audience); free(w->privkey);
    free(w);
}
