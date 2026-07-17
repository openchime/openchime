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

/* A roster member's role / disabled flag (-1 if the user isn't in the roster). */
static int member_role(const oc_model *m, uint64_t uid) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == uid) return (int)m->users[i].role;
    return -1;
}
static int member_disabled(const oc_model *m, uint64_t uid) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == uid) return (int)m->users[i].disabled;
    return -1;
}

/* The id of a DM channel whose peer is `peer`, or 0. */
static uint64_t dm_with_peer(const oc_model *m, uint64_t peer) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].kind == OC_CHANNEL_KIND_DM && m->channels[i].peer_id == peer)
            return m->channels[i].channel_id;
    return 0;
}

/* The id of a channel with the given name, or 0. */
static uint64_t channel_named(const oc_model *m, const char *name) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].name && strcmp(m->channels[i].name, name) == 0)
            return m->channels[i].channel_id;
    return 0;
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

        /* The roster loads after auth: dana resolves erik (and faye) by name. */
        CHECK(WAIT_FOR(a, oc_model_user_id(m, "erik") != 0 && oc_model_user_id(m, "faye") != 0));
        {
            const oc_model *am = oc_client_model(a);
            uint64_t eid = oc_model_user_id(am, "erik");
            CHECK(eid != 0 && strcmp(oc_model_user_name(am, eid), "erik") == 0);
        }

        /* dana sends: it round-trips to her own model as a BROADCAST (channel 1),
         * and reaches erik live. erik counts it unread (author != self). */
        oc_client_send(a, 1, "hello from the core");
        CHECK(WAIT_FOR(a, channel_has_body(m, 1, "hello from the core")));
        CHECK(WAIT_FOR(b, channel_has_body(m, 1, "hello from the core") && channel_unread(m, 1) == 1));

        /* The message carries dana's display name (= her login name), live. */
        CHECK(channel_has_named(oc_client_model(b), 1, "hello from the core", "dana"));

        /* erik signals typing in channel 1; dana sees erik (and only erik) typing
         * (the server relays to other members, so dana's own view excludes her). */
        uint64_t erik_id = oc_client_model(b)->user_id;
        oc_client_typing(b, 1);
        uint64_t typers[4];
        CHECK(WAIT_FOR(a, oc_model_typing(m, 1, m->user_id, typers, 4) == 1));
        CHECK(typers[0] == erik_id);

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

        /* who-reacted (REQ-071): erik reacts :+1:, dana reacts :tada:; dana
         * inspects the message and the reactor list carries both — each reactor
         * paired with the emoji they used. */
        oc_client_react(b, 1, mid, ":+1:", 1);
        oc_client_react(a, 1, mid, ":tada:", 1);
        CHECK(WAIT_FOR(a, reaction_count(m, 1, mid, ":+1:", NULL) == 1 &&
                          reaction_count(m, 1, mid, ":tada:", NULL) == 1));
        oc_client_list_reactions(a, 1, mid);
        CHECK(WAIT_FOR(a, m->reactlist_open && m->n_reactors >= 2));
        {
            const oc_model *am = oc_client_model(a);
            int erik_thumb = 0, dana_tada = 0;
            for (size_t j = 0; j < am->n_reactors; j++) {
                const char *nm = oc_model_user_name(am, am->reactors[j].user_id);
                if (nm && strcmp(nm, "erik") == 0 && strcmp(am->reactors[j].emoji, ":+1:") == 0) erik_thumb = 1;
                if (nm && strcmp(nm, "dana") == 0 && strcmp(am->reactors[j].emoji, ":tada:") == 0) dana_tada = 1;
            }
            CHECK(erik_thumb && dana_tada);
        }
        oc_client_close_reactions(a);
        CHECK(!oc_client_model(a)->reactlist_open && oc_client_model(a)->n_reactors == 0);

        /* notification prefs (REQ-130/131): dana mutes channel 1 to mentions-only
         * and sets a DND window; each SET returns a fresh NOTIFY_PREFS sync that
         * folds into the channel's notify_level + the model DND fields. */
        oc_client_set_notify_pref(a, 1, OC_NOTIFY_MENTIONS);
        CHECK(WAIT_FOR(a, oc_model_channel((oc_model *)m, 1) &&
                          oc_model_channel((oc_model *)m, 1)->notify_level == OC_NOTIFY_MENTIONS));
        oc_client_set_dnd(a, 1, 1320, 420);        /* 22:00 -> 07:00 */
        CHECK(WAIT_FOR(a, m->dnd_enabled && m->dnd_start_min == 1320 && m->dnd_end_min == 420));
        /* Opening the prefs overlay re-syncs and the channel level survives. */
        oc_client_toggle_prefs(a, 1);
        CHECK(WAIT_FOR(a, m->prefs_open &&
                          oc_model_channel((oc_model *)m, 1)->notify_level == OC_NOTIFY_MENTIONS));
        /* Clearing DND turns it off. */
        oc_client_set_dnd(a, 0, 0, 0);
        CHECK(WAIT_FOR(a, !m->dnd_enabled));
        oc_client_toggle_prefs(a, 0);
        CHECK(!oc_client_model(a)->prefs_open);

        /* erik replies to dana's message in a thread: both see the parent's reply
         * count rise, and opening the thread streams the reply into the buffer. */
        oc_client_reply(b, 1, mid, "a threaded reply");
        CHECK(WAIT_FOR(a, find_msg(m, 1, mid) && find_msg(m, 1, mid)->reply_count == 1));
        oc_client_open_thread(b, 1, mid);
        CHECK(WAIT_FOR(b, m->thread_open && m->n_thread_msgs >= 1));
        {
            const oc_model *bm = oc_client_model(b);
            int found = 0;
            for (size_t j = 0; j < bm->n_thread_msgs; j++)
                if (bm->thread_msgs[j].body && strcmp(bm->thread_msgs[j].body, "a threaded reply") == 0)
                    found = 1;
            CHECK(found);
        }
        oc_client_close_thread(b);
        CHECK(!oc_client_model(b)->thread_open && oc_client_model(b)->n_thread_msgs == 0);

        /* dana searches for a word in her message; the hit streams into the
         * search buffer and includes that message. */
        oc_client_search(a, "core");
        CHECK(WAIT_FOR(a, m->search_open && m->n_search >= 1));
        {
            const oc_model *am = oc_client_model(a);
            int found = 0;
            for (size_t j = 0; j < am->n_search; j++)
                if (am->search_results[j].message_id == mid) found = 1;
            CHECK(found);
        }
        oc_client_close_search(a);
        CHECK(!oc_client_model(a)->search_open && oc_client_model(a)->n_search == 0);

        /* dana creates a public channel — it shows up joined in her list — and
         * erik joins it by id, seeing it joined too. */
        oc_client_create_channel(a, "war-room");
        CHECK(WAIT_FOR(a, channel_named(m, "war-room") != 0));
        uint64_t warroom = channel_named(oc_client_model(a), "war-room");
        CHECK(warroom != 0);
        oc_client_join_channel(b, warroom);
        CHECK(WAIT_FOR(b, oc_model_channel((oc_model *)m, warroom) &&
                          oc_model_channel((oc_model *)m, warroom)->joined));

        /* dana opens a DM with erik: a DM channel appears carrying erik as the
         * peer (the daemon now reports the DM peer in CHANNEL_INFO). */
        uint64_t erikid = oc_model_user_id(oc_client_model(a), "erik");
        CHECK(erikid != 0);
        oc_client_open_dm(a, erikid);
        CHECK(WAIT_FOR(a, dm_with_peer(m, erikid) != 0));

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
            /* faye logs out: the server revokes her session and closes the
             * connection, so the model reports disconnected. */
            oc_client_logout(c, OC_LOGOUT_THIS);
            CHECK(WAIT_FOR(c, !m->connected));
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

        /* admin / user management (REQ-030/033): dana (owner) promotes erik to
         * admin — the USER_UPDATED folds his new role into her roster — mints a
         * tenant invite token (shown once in the model), then removes him, which
         * marks him disabled in the roster and drops his connection. This runs
         * last, since removal closes erik's client (b). */
        oc_client_set_role(a, erikid, OC_ROLE_ADMIN);
        CHECK(WAIT_FOR(a, member_role(m, erikid) == OC_ROLE_ADMIN));
        oc_client_invite_user(a, OC_ROLE_MEMBER);
        CHECK(WAIT_FOR(a, m->invite_token[0] != '\0' && m->invite_role == OC_ROLE_MEMBER));
        oc_client_remove_user(a, erikid);
        CHECK(WAIT_FOR(a, member_disabled(m, erikid) == 1));

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
