/* Headless test for the client app-core (ARCH-74). Brings the daemon's netloop
 * up in-process (TLS server + DB-writer thread, like itest_netloop.c) with one
 * registered account, then drives a real oc_client against it over the loopback:
 * connect → auth → the post-auth LIST_CHANNELS populates the model's channel
 * list, and a sent message round-trips back as a BROADCAST folded into the
 * per-channel buffer. Proves the whole core — net thread, queues, reducers, and
 * the facade — with no UI. */

#include "client.h"     /* the core facade under test */
#include "model.h"

#include "netloop.h"
#include "dbwriter.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct core_loop_arg {
    int                   port;
    oc_tls_server        *srv;
    oc_dbwriter          *dbw;
    volatile sig_atomic_t stop;
};

static void *core_loop_thread(void *p) {
    struct core_loop_arg *a = (struct core_loop_arg *)p;
    oc_netloop_run(a->port, a->srv, a->dbw, &a->stop);
    return NULL;
}

/* Tick the client and sleep briefly, up to ~5s, until `cond(model)` holds.
 * Returns 1 if the condition was met, 0 on timeout. */
#define WAIT_FOR(cl, cond)                                                      \
    ({                                                                         \
        int _ok = 0;                                                           \
        for (int _i = 0; _i < 500; _i++) {                                     \
            oc_client_tick((cl));                                              \
            const oc_model *m = oc_client_model((cl));                         \
            if (cond) { _ok = 1; break; }                                      \
            struct timespec _ts = { 0, 10 * 1000 * 1000 };                     \
            nanosleep(&_ts, NULL);                                             \
        }                                                                      \
        _ok;                                                                   \
    })

static int channel_has_body(const oc_model *m, uint64_t cid, const char *body) {
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].body && strcmp(m->channels[i].msgs[j].body, body) == 0)
                return 1;
    }
    return 0;
}

int run_client_core_tests(void) {
    printf("test_client_core: connect+auth, channel-list populate, send round-trip\n");

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);

    unlink("build/itest_core.db");
    unlink("build/itest_core.db-wal");
    unlink("build/itest_core.db-shm");
    oc_dbwriter *dbw = oc_dbwriter_start("build/itest_core.db");
    CHECK(dbw != NULL);
    if (!dbw) { oc_tls_server_free(&srv); return failures; }

    /* The account the core authenticates as (client sends "user:pass"). */
    CHECK(oc_dbwriter_register_local(dbw, "dana", "pw-dana", OC_ROLE_OWNER, 2048) != 0);

    struct core_loop_arg arg;
    arg.port = 19000 + (int)(getpid() % 2000);
    arg.srv = &srv;
    arg.dbw = dbw;
    arg.stop = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, core_loop_thread, &arg) == 0);

    oc_client *cl = oc_client_start("127.0.0.1", arg.port, "dana:pw-dana");
    CHECK(cl != NULL);

    if (cl) {
        /* Auth completes: the model reports connected+authed with our user id. */
        CHECK(WAIT_FOR(cl, m->authed && m->user_id != 0));
        const oc_model *mm = oc_client_model(cl);
        CHECK(mm->connected);

        /* The post-auth LIST_CHANNELS populates the default channel (id 1). */
        CHECK(WAIT_FOR(cl, oc_model_channel((oc_model *)m, 1) != NULL));

        /* A sent message round-trips back as a BROADCAST folded into channel 1. */
        oc_client_send(cl, 1, "hello from the core");
        CHECK(WAIT_FOR(cl, channel_has_body(m, 1, "hello from the core")));

        oc_client_stop(cl);
    }

    arg.stop = 1;
    pthread_join(th, NULL);
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    unlink("build/itest_core.db");
    unlink("build/itest_core.db-wal");
    unlink("build/itest_core.db-shm");

    return failures;
}
