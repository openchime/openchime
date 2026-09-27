/* The auth pool (daemon/authpool.c): every check handed over comes back once,
 * a right password matches and a wrong one does not, a change's new password is
 * derived only once the old one has matched, a held pool starts nothing and
 * counts what waits, and checks still queued at stop come back unchecked. */

#include "authpool.h"
#include "auth.h"
#include "check.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { N = 12 };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_back[N + 2];
static int g_ok[N + 2];
static int g_returned;

static void done(oc_auth_check *c, void *ctx) {
    (void)ctx;
    int i = (int)(intptr_t)c->owner;
    pthread_mutex_lock(&g_mu);
    g_back[i]++;
    g_ok[i] = c->ok;
    g_returned++;
    pthread_mutex_unlock(&g_mu);
}

static int returned(void) {
    pthread_mutex_lock(&g_mu);
    int n = g_returned;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void wait_returned(int n) {
    for (int i = 0; i < 500 && returned() < n; i++) usleep(10000);
}

/* A check of `pw` against a stored "right" at a low count. */
static oc_auth_check *mk(int i, const char *pw) {
    oc_auth_check *c = calloc(1, sizeof *c);
    memset(c->salt, 0x11, 16);
    c->slen = 16;
    c->iters = 1000;
    oc_pw_derive("right", 5, c->salt, c->slen, c->iters, c->stored);
    c->password = pw;
    c->pwlen = strlen(pw);
    c->owner = (void *)(intptr_t)i;
    return c;
}

int run_authpool_tests(void) {
    printf("test_authpool: checks come back once, right and wrong passwords, a change's derivation, hold, stop\n");
    memset(g_back, 0, sizeof g_back); memset(g_ok, 0, sizeof g_ok); g_returned = 0;
    oc_authpool *p = oc_authpool_start(2, done, NULL);
    CHECK(p != NULL);
    if (!p) return failures;
    oc_auth_check *cs[N];
    for (int i = 0; i < N; i++) { cs[i] = mk(i, i % 3 ? "right" : "wrong"); oc_authpool_submit(p, cs[i]); }
    wait_returned(N);
    int bad = 0;
    for (int i = 0; i < N; i++) bad |= g_back[i] != 1 || g_ok[i] != (i % 3 ? 1 : 0);
    CHECK(returned() == N && !bad);

    /* A change: the new password's key, derived only after the old one matched. */
    oc_auth_check *chg = mk(0, "right");
    chg->new_password = "fresh"; chg->new_pwlen = 5;
    memset(chg->new_salt, 0x22, sizeof chg->new_salt); chg->new_iters = 1000;
    oc_auth_check *nochg = mk(1, "wrong");
    nochg->new_password = "fresh"; nochg->new_pwlen = 5;
    memset(nochg->new_salt, 0x22, sizeof nochg->new_salt); nochg->new_iters = 1000;
    int before = returned();
    oc_authpool_submit(p, chg);
    oc_authpool_submit(p, nochg);
    wait_returned(before + 2);
    uint8_t want[OC_PW_HASH_LEN];
    oc_pw_derive("fresh", 5, chg->new_salt, sizeof chg->new_salt, 1000, want);
    CHECK(chg->ok == 1 && chg->derived == 1 && memcmp(chg->new_hash, want, sizeof want) == 0);
    CHECK(nochg->ok == 0 && nochg->derived == 0);

    /* Held: nothing starts, and what waits is counted. Released: all of it comes back. */
    before = returned();
    oc_authpool_hold(p, 1);
    oc_auth_check *h[3];
    for (int i = 0; i < 3; i++) { h[i] = mk(i, "right"); oc_authpool_submit(p, h[i]); }
    usleep(100000);
    CHECK(oc_authpool_waiting(p) == 3 && returned() == before);
    oc_authpool_hold(p, 0);
    wait_returned(before + 3);
    CHECK(returned() == before + 3 && oc_authpool_waiting(p) == 0);

    /* Stopped with checks queued: they come back, unchecked. */
    before = returned();
    oc_authpool_hold(p, 1);
    oc_auth_check *q[2];
    for (int i = 0; i < 2; i++) { q[i] = mk(N + i, "right"); oc_authpool_submit(p, q[i]); }
    oc_authpool_stop(p);
    CHECK(returned() == before + 2 && g_back[N] == 1 && g_back[N + 1] == 1);
    CHECK(g_ok[N] == -1 && g_ok[N + 1] == -1);

    for (int i = 0; i < N; i++) free(cs[i]);
    free(chg); free(nochg);
    for (int i = 0; i < 3; i++) free(h[i]);
    for (int i = 0; i < 2; i++) free(q[i]);
    return failures;
}
