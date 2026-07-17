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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Wait until the in-process daemon accepts TCP on `port` (the netloop thread is
 * spawned concurrently). oc_client's connect does not retry, so starting a
 * client before the listener is up would lose the startup race — the itest's
 * client_open handles this by retrying connect; here we gate on readiness. */
static void wait_port_ready(int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    for (int i = 0; i < 500; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) { close(fd); return; }
        if (fd >= 0) close(fd);
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
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

static int channel_unread(const oc_model *m, uint64_t cid) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].channel_id == cid) return m->channels[i].unread;
    return -1;
}

/* The message id of the message with `body` in channel `cid`, or 0. */
static uint64_t message_id_of(const oc_model *m, uint64_t cid, const char *body) {
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].body && strcmp(m->channels[i].msgs[j].body, body) == 0)
                return m->channels[i].msgs[j].message_id;
    }
    return 0;
}

/* The aggregate count for `emoji` on message `mid` (-1 if the message/emoji is
 * absent), and separately whether we reacted (`mine`). */
static int reaction_count(const oc_model *m, uint64_t cid, uint64_t mid, const char *emoji, int *mine) {
    if (mine) *mine = 0;
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++) {
            const oc_msg *msg = &m->channels[i].msgs[j];
            if (msg->message_id != mid) continue;
            for (uint8_t k = 0; k < msg->n_reactions; k++)
                if (strcmp(msg->reactions[k].emoji, emoji) == 0) {
                    if (mine) *mine = msg->reactions[k].mine;
                    return (int)msg->reactions[k].count;
                }
            return 0;
        }
    }
    return -1;
}

/* Find a message by id; return NULL if absent. */
static const oc_msg *find_msg(const oc_model *m, uint64_t cid, uint64_t mid) {
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].message_id == mid) return &m->channels[i].msgs[j];
    }
    return NULL;
}

/* Does channel `cid` hold a message whose body and author display name match? */
static int channel_has_named(const oc_model *m, uint64_t cid, const char *body, const char *name) {
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++) {
            const oc_msg *msg = &m->channels[i].msgs[j];
            if (msg->body && strcmp(msg->body, body) == 0 && strcmp(msg->author_name, name) == 0)
                return 1;
        }
    }
    return 0;
}

int run_client_core_tests(void) {
    printf("test_client_core: connect+auth, channel-list, send round-trip, unread, backfill\n");

    /* The daemon opens its blob store at netloop startup; point it at a build-local
     * dir (the /data/blobs default isn't writable in the test sandbox), matching
     * how itest_netloop provisions it. */
    setenv("OPENCHIME_BLOB_DIR", "build/itest_core_blobs", 1);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);

    unlink("build/itest_core.db");
    unlink("build/itest_core.db-wal");
    unlink("build/itest_core.db-shm");
    oc_dbwriter *dbw = oc_dbwriter_start("build/itest_core.db");
    CHECK(dbw != NULL);
    if (!dbw) { oc_tls_server_free(&srv); return failures; }

    /* Accounts the cores authenticate as (client sends "user:pass"). */
    CHECK(oc_dbwriter_register_local(dbw, "dana", "pw-dana", OC_ROLE_OWNER,  2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "erik", "pw-erik", OC_ROLE_MEMBER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "faye", "pw-faye", OC_ROLE_MEMBER, 2048) != 0);

    struct core_loop_arg arg;
    arg.port = 19000 + (int)(getpid() % 2000);
    arg.srv = &srv;
    arg.dbw = dbw;
    arg.stop = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, core_loop_thread, &arg) == 0);
    wait_port_ready(arg.port);

    oc_client *a = oc_client_start("127.0.0.1", arg.port, "dana:pw-dana");
    oc_client *b = oc_client_start("127.0.0.1", arg.port, "erik:pw-erik");
    CHECK(a != NULL);
    CHECK(b != NULL);

    if (a && b) {
        /* Auth completes: each model reports connected+authed with its user id,
         * and the post-auth LIST_CHANNELS populates the default channel (id 1). */
        CHECK(WAIT_FOR(a, m->authed && m->user_id != 0));
        CHECK(WAIT_FOR(b, m->authed && m->user_id != 0));
        CHECK(oc_client_model(a)->connected);
        CHECK(WAIT_FOR(a, oc_model_channel((oc_model *)m, 1) != NULL));
        CHECK(WAIT_FOR(b, oc_model_channel((oc_model *)m, 1) != NULL));

        /* dana sends: it round-trips to her own model as a BROADCAST (channel 1),
         * and reaches erik live. erik counts it unread (author != self). */
        oc_client_send(a, 1, "hello from the core");
        CHECK(WAIT_FOR(a, channel_has_body(m, 1, "hello from the core")));
        CHECK(WAIT_FOR(b, channel_has_body(m, 1, "hello from the core") && channel_unread(m, 1) == 1));

        /* The message carries dana's display name (= her login name), live. */
        CHECK(channel_has_named(oc_client_model(b), 1, "hello from the core", "dana"));

        /* erik reacts to dana's message: both see the aggregate (count 1), erik
         * sees it as his own; a second react toggles it off (count 0). */
        uint64_t mid = message_id_of(oc_client_model(b), 1, "hello from the core");
        CHECK(mid != 0);
        oc_client_react(b, 1, mid, ":+1:", 1);
        int mine = 0;
        CHECK(WAIT_FOR(b, reaction_count(m, 1, mid, ":+1:", NULL) == 1));
        CHECK(reaction_count(oc_client_model(b), 1, mid, ":+1:", &mine) == 1 && mine == 1);
        CHECK(WAIT_FOR(a, reaction_count(m, 1, mid, ":+1:", NULL) == 1));
        oc_client_react(b, 1, mid, ":+1:", 0);
        CHECK(WAIT_FOR(a, reaction_count(m, 1, mid, ":+1:", NULL) == 0));

        /* Marking channel 1 read clears erik's unread. */
        oc_client_mark_read(b, 1);
        oc_client_tick(b);
        CHECK(channel_unread(oc_client_model(b), 1) == 0);

        /* dana's own send never counts as unread for her. */
        CHECK(channel_unread(oc_client_model(a), 1) == 0);

        /* A second message, then a fresh client (faye) that was not connected for
         * either: it backfills channel 1 and sees the full history replayed. */
        oc_client_send(a, 1, "second line for history");
        CHECK(WAIT_FOR(b, channel_has_body(m, 1, "second line for history")));

        oc_client *c = oc_client_start("127.0.0.1", arg.port, "faye:pw-faye");
        CHECK(c != NULL);
        if (c) {
            CHECK(WAIT_FOR(c, m->authed && oc_model_channel((oc_model *)m, 1) != NULL));
            oc_client_backfill(c, 1);
            CHECK(WAIT_FOR(c, channel_has_body(m, 1, "hello from the core") &&
                              channel_has_body(m, 1, "second line for history")));
            /* Backfilled history carries the author's display name too (the JOIN
             * fallback in the replay query). */
            CHECK(channel_has_named(oc_client_model(c), 1, "second line for history", "dana"));
            oc_client_stop(c);
        }

        /* dana edits her first message; erik sees the new body + edited flag,
         * then dana deletes it and erik sees the tombstone. (After faye's
         * backfill, which asserted the pre-edit body.) */
        oc_client_edit(a, 1, mid, "edited body");
        CHECK(WAIT_FOR(b, find_msg(m, 1, mid) && find_msg(m, 1, mid)->edited &&
                          find_msg(m, 1, mid)->body &&
                          strcmp(find_msg(m, 1, mid)->body, "edited body") == 0));
        oc_client_delete(a, 1, mid);
        CHECK(WAIT_FOR(b, find_msg(m, 1, mid) && find_msg(m, 1, mid)->deleted));

        oc_client_stop(a);
        oc_client_stop(b);
    } else {
        if (a) oc_client_stop(a);
        if (b) oc_client_stop(b);
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
