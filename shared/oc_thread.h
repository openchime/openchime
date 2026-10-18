/*
 * Thread/mutex/condvar shim (ARCH-81). The client core owns a network thread and
 * two mutex+condvar queues, written against pthreads. This is the one seam that
 * needs a real abstraction to build on Windows (sockets are already shimmed in
 * shared/sock.h; DNS and RNG are backend swaps). POSIX passes straight through
 * to pthreads; Windows maps to CRITICAL_SECTION + CONDITION_VARIABLE + a thread
 * handle. The daemon is untouched — it keeps using pthreads directly.
 *
 * The thread entry keeps the pthread signature (void *(*)(void *)) so callers
 * are identical on both platforms; Windows trampolines through _beginthreadex.
 */

#ifndef OC_THREAD_H
#define OC_THREAD_H

typedef void *(*oc_thread_fn)(void *);

#ifndef _WIN32
/* ---- POSIX ---- */
#include <pthread.h>

typedef pthread_mutex_t oc_mutex_t;
typedef pthread_cond_t  oc_cond_t;
typedef pthread_t       oc_thread_t;

static inline void oc_mutex_init(oc_mutex_t *m)    { pthread_mutex_init(m, NULL); }
static inline void oc_mutex_destroy(oc_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void oc_mutex_lock(oc_mutex_t *m)    { pthread_mutex_lock(m); }
static inline void oc_mutex_unlock(oc_mutex_t *m)  { pthread_mutex_unlock(m); }

static inline void oc_cond_init(oc_cond_t *c)      { pthread_cond_init(c, NULL); }
static inline void oc_cond_destroy(oc_cond_t *c)   { pthread_cond_destroy(c); }
static inline void oc_cond_wait(oc_cond_t *c, oc_mutex_t *m) { pthread_cond_wait(c, m); }
static inline void oc_cond_signal(oc_cond_t *c)    { pthread_cond_signal(c); }

static inline int  oc_thread_create(oc_thread_t *t, oc_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void oc_thread_join(oc_thread_t t)   { pthread_join(t, NULL); }

#else
/* ---- Windows ---- */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <stdint.h>

typedef CRITICAL_SECTION   oc_mutex_t;
typedef CONDITION_VARIABLE oc_cond_t;
typedef HANDLE             oc_thread_t;

static inline void oc_mutex_init(oc_mutex_t *m)    { InitializeCriticalSection(m); }
static inline void oc_mutex_destroy(oc_mutex_t *m) { DeleteCriticalSection(m); }
static inline void oc_mutex_lock(oc_mutex_t *m)    { EnterCriticalSection(m); }
static inline void oc_mutex_unlock(oc_mutex_t *m)  { LeaveCriticalSection(m); }

static inline void oc_cond_init(oc_cond_t *c)      { InitializeConditionVariable(c); }
static inline void oc_cond_destroy(oc_cond_t *c)   { (void)c; /* no destroy on Win32 */ }
static inline void oc_cond_wait(oc_cond_t *c, oc_mutex_t *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
static inline void oc_cond_signal(oc_cond_t *c)    { WakeConditionVariable(c); }

/* Trampoline: _beginthreadex wants unsigned __stdcall(void*); we carry the
 * pthread-shaped fn+arg through and discard the void* return. */
struct oc__thunk { oc_thread_fn fn; void *arg; };
static unsigned __stdcall oc__thread_entry(void *p) {
    struct oc__thunk t = *(struct oc__thunk *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
static inline int oc_thread_create(oc_thread_t *t, oc_thread_fn fn, void *arg) {
    struct oc__thunk *th = (struct oc__thunk *)malloc(sizeof *th);
    if (!th) return -1;
    th->fn = fn; th->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, oc__thread_entry, th, 0, NULL);
    if (!h) { free(th); return -1; }
    *t = (HANDLE)h;
    return 0;
}
static inline void oc_thread_join(oc_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#endif /* _WIN32 */

#endif /* OC_THREAD_H */
