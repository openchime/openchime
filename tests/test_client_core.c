/* Headless test for the client app-core (ARCH-74). Brings the daemon's netloop
 * up in-process (TLS server + DB-writer thread, like itest_netloop.c) with one
 * registered account, then drives a real oc_client against it over the loopback:
 * connect → auth → the post-auth LIST_CHANNELS populates the model's channel
 * list, and a sent message round-trips back as a BROADCAST folded into the
 * per-channel buffer. Proves the whole core — net thread, queues, reducers, and
 * the facade — with no UI. */

#include "client.h"     /* the core facade under test */
#include "model.h"
#include "store.h"       /* to assert the persisted token/pin */
#include "resolve.h"     /* workspace resolution (REQ-010/011) */

#include "netloop.h"
#include "config.h"
#include "dbwriter.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"

#include <sqlite3.h>     /* to hand-build a pre-rename store for the upgrade test */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
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
    char cfgerr[128];
    oc_config_load(cfgerr, sizeof cfgerr);   /* daemon config (blob dir, storage, …) */
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
            const oc_model *m = oc_client_model((cl)); (void)m;                         \
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

/* The id of the first attachment on any message in channel `cid` (0 if none),
 * filling `fn_out`/`size_out` with its filename + byte size. */
static uint64_t channel_attach(const oc_model *m, uint64_t cid, char *fn_out,
                               size_t fncap, uint64_t *size_out) {
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++) {
            const oc_msg *msg = &m->channels[i].msgs[j];
            if (msg->n_attach > 0) {
                snprintf(fn_out, fncap, "%s", msg->attach[0].filename);
                if (size_out) *size_out = msg->attach[0].size;
                return msg->attach[0].id;
            }
        }
    }
    return 0;
}

/* Whole-file compare: does `path` hold exactly `data[0..len)`? */
static int file_matches(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[8192];
    size_t off = 0; int ok = 1;
    for (;;) {
        size_t got = fread(buf, 1, sizeof buf, f);
        if (got == 0) break;
        if (off + got > len || memcmp(buf, data + off, got) != 0) { ok = 0; break; }
        off += got;
    }
    fclose(f);
    return ok && off == len;
}

/* Workspace resolution (REQ-010/011). The pure pieces are tested here without
 * live DNS: normalization and the SRV-answer parser (fed a canned wire response).
 * A minimal DNS reply for `_openchime._tcp.acme.com` -> `srv.acme.com:8443`
 * (priority 10, weight 5), the answer name a compression pointer to the query. */
static const unsigned char SRV_ANSWER[] = {
    0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,   /* header */
    0x0a,'_','o','p','e','n','c','h','i','m','e', 0x04,'_','t','c','p',   /* qname */
    0x04,'a','c','m','e', 0x03,'c','o','m', 0x00,
    0x00,0x21, 0x00,0x01,                                                /* qtype SRV, qclass IN */
    0xc0,0x0c,                                                           /* answer name -> qname */
    0x00,0x21, 0x00,0x01, 0x00,0x00,0x01,0x2c, 0x00,0x14,                /* SRV, IN, ttl 300, rdlen 20 */
    0x00,0x0a, 0x00,0x05, 0x20,0xfb,                                     /* prio 10, weight 5, port 8443 */
    0x03,'s','r','v', 0x04,'a','c','m','e', 0x03,'c','o','m', 0x00       /* target srv.acme.com */
};

/* The sidebar helper (WIN-5/6): grouping, filter, sort, collapse — shared by
 * every frontend so the TUI and the GUI cannot disagree about what belongs
 * where. Built against a hand-made model, no daemon needed. */
/* Pins folded into the model (REQ-230, ARCH-90): the inline flag on a message
 * and the standalone pins overlay, which is fed by its own frames because a
 * pinned message is usually outside loaded history. */
static void test_pins(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;

    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 5; e.author_id = 2;
    e.server_time = 1000; e.body = strdup("the runbook"); oc_model_apply(&m, &e);

    oc_channel *c = oc_model_channel(&m, 10);
    CHECK(c && c->n_msgs == 1 && c->msgs[0].pinned == 0);

    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 5; e.user_id = 7;
    e.op = 1; e.server_time = 2000; oc_model_apply(&m, &e);
    CHECK(c->msgs[0].pinned == 1 && c->msgs[0].pinned_by == 7 && c->msgs[0].pinned_at == 2000);

    /* An unpin clears the attribution too, so a frontend cannot render a stale
     * "pinned by" on an unpinned message. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 5; e.user_id = 7; e.op = 0;
    oc_model_apply(&m, &e);
    CHECK(c->msgs[0].pinned == 0 && c->msgs[0].pinned_by == 0);

    /* A pin for a message we have not loaded is dropped, not a crash: it is the
     * normal case when another member pins far up the scroll. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 9999; e.op = 1;
    oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 404; e.message_id = 5; e.op = 1;
    oc_model_apply(&m, &e);

    /* The overlay. Entries only land while it is open and for its channel — a
     * late frame from a previous open must not pollute the current one. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PINNED_MSG; e.channel_id = 10; e.message_id = 5;
    e.body = strdup("ignored"); oc_model_apply(&m, &e);
    CHECK(m.n_pins == 0);

    oc_model_pinlist_begin(&m, 10);
    CHECK(m.pinlist_open && m.pinlist_loading && m.pinlist_channel == 10);

    memset(&e, 0, sizeof e);
    e.type = OC_EV_PINNED_MSG; e.channel_id = 10; e.message_id = 5; e.author_id = 2;
    e.server_time = 1000; e.user_id = 7; e.pinned_at = 2000;
    e.body = strdup("the runbook"); oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PINNED_MSG; e.channel_id = 99; e.message_id = 6;
    e.body = strdup("other channel"); oc_model_apply(&m, &e);
    CHECK(m.n_pins == 1);
    CHECK(m.pins[0].message_id == 5 && m.pins[0].pinned_by == 7);
    CHECK(m.pins[0].pinned_at == 2000);       /* a ms stamp, not truncated to 32 bits */
    CHECK(m.pins[0].body && strcmp(m.pins[0].body, "the runbook") == 0);

    memset(&e, 0, sizeof e);
    e.type = OC_EV_PINS_END; e.channel_id = 10; oc_model_apply(&m, &e);
    CHECK(m.pinlist_open && !m.pinlist_loading);   /* open, done loading, one row */

    /* Someone else unpinning drops the row from the open list, rather than
     * leaving an entry that does nothing when clicked. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 5; e.op = 0;
    oc_model_apply(&m, &e);
    CHECK(m.n_pins == 0);

    oc_model_close_pinlist(&m);
    CHECK(!m.pinlist_open && m.n_pins == 0);

    /* A thread reply lives outside the channel's message list, so a pin on one
     * has to be applied there too — marking only the channel list left a pinned
     * reply looking unpinned, with a menu that could not unpin it. */
    m.thread_open = 1;
    m.thread_channel = 10;
    m.n_thread_msgs = 1;
    m.cap_thread_msgs = 1;
    m.thread_msgs = calloc(1, sizeof *m.thread_msgs);
    CHECK(m.thread_msgs != NULL);
    m.thread_msgs[0].message_id = 77;

    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 77; e.user_id = 3;
    e.op = 1; e.server_time = 4000; oc_model_apply(&m, &e);
    CHECK(m.thread_msgs[0].pinned == 1 && m.thread_msgs[0].pinned_by == 3);

    memset(&e, 0, sizeof e);
    e.type = OC_EV_PIN; e.channel_id = 10; e.message_id = 77; e.op = 0;
    oc_model_apply(&m, &e);
    CHECK(m.thread_msgs[0].pinned == 0);

    oc_model_free(&m);
}

static void test_sidebar(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;

    /* Two named channels (one private), plus a DM — which has NO name on the
     * wire, the case that made DMs invisible in the Win32 sidebar. */
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 10; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 1; e.body = strdup("zulu");   e.server_time = 100; oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 11; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 0; e.body = strdup("alpha");  e.server_time = 300; oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 12; e.status = 1; e.op = OC_CHANNEL_KIND_DM;
    e.user_id = 2; e.server_time = 200; e.count = 3; oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 2; e.body = strdup("bob"); oc_model_apply(&m, &e);

    oc_sidebar_opts o; oc_sidebar_opts_defaults(&o);
    oc_sidebar_row rows[16];

    /* THREE headers now (WIN-41): Starred first — empty but present, so it can be
     * used — then Channels A-Z, then the DM titled by its peer. */
    size_t n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 6);
    CHECK(rows[0].is_header && rows[0].section == OC_SB_STARRED && rows[0].section_total == 0);
    CHECK(rows[1].is_header && rows[1].section == OC_SB_CHANNELS && rows[1].section_total == 2);
    CHECK(strcmp(rows[2].label, "alpha") == 0 && rows[2].is_private == 1);
    CHECK(strcmp(rows[3].label, "zulu") == 0  && rows[3].is_private == 0);
    CHECK(rows[4].is_header && rows[4].section == OC_SB_DMS);
    CHECK(strcmp(rows[5].label, "bob") == 0 && rows[5].unread == 3);

    /* Starring lifts a conversation OUT of its section into Starred — it must appear
     * once, not twice. */
    CHECK(oc_sidebar_toggle_star(&o, 11) == 1);
    CHECK(oc_sidebar_is_starred(&o, 11) && !oc_sidebar_is_starred(&o, 12));
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 6);
    CHECK(rows[0].is_header && rows[0].section == OC_SB_STARRED && rows[0].section_total == 1);
    CHECK(strcmp(rows[1].label, "alpha") == 0 && rows[1].section == OC_SB_STARRED);
    CHECK(rows[2].is_header && rows[2].section == OC_SB_CHANNELS);
    CHECK(strcmp(rows[3].label, "zulu") == 0);      /* alpha is gone from Channels */
    /* A DM stars too, and un-starring puts it back. */
    CHECK(oc_sidebar_toggle_star(&o, 12) == 1);
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(rows[0].section_total == 2);
    CHECK(oc_sidebar_toggle_star(&o, 11) == 1 && oc_sidebar_toggle_star(&o, 12) == 1);
    CHECK(o.n_starred == 0);
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 6 && strcmp(rows[2].label, "alpha") == 0);

    /* Recency uses the server-reported last_message_at: alpha(300) before zulu(100). */
    o.sort[OC_SB_CHANNELS] = OC_SB_SORT_RECENT;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(strcmp(rows[2].label, "alpha") == 0 && strcmp(rows[3].label, "zulu") == 0);

    /* Collapsing keeps the header (so it can be reopened) and drops the children. */
    o.collapsed[OC_SB_CHANNELS] = 1;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 4 && rows[1].is_header && rows[1].section_total == 2);
    o.collapsed[OC_SB_CHANNELS] = 0;

    /* Unread-only hides the read channels but keeps the unread DM. */
    o.filter[OC_SB_CHANNELS] = OC_SB_FILTER_UNREAD;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 4 && rows[2].is_header && strcmp(rows[3].label, "bob") == 0);
    o.filter[OC_SB_CHANNELS] = OC_SB_FILTER_ALL;

    /* Find matches the rendered LABEL, so a DM (which has no name) is findable. */
    snprintf(o.find, sizeof o.find, "bo");
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 4 && strcmp(rows[3].label, "bob") == 0);
    o.find[0] = '\0';

    /* Options round-trip through the settings bucket (ARCH-88: no local file). */
    o.sort[OC_SB_DMS] = OC_SB_SORT_UNREAD; o.collapsed[OC_SB_DMS] = 1;
    CHECK(oc_sidebar_toggle_star(&o, 11) == 1);
    CHECK(oc_sidebar_toggle_star(&o, 12) == 1);
    o.collapsed[OC_SB_STARRED] = 1;
    char enc[256]; oc_sidebar_opts_encode(&o, enc, sizeof enc);
    oc_sidebar_opts p2; oc_sidebar_opts_defaults(&p2);
    oc_sidebar_opts_parse(&p2, enc);
    CHECK(p2.sort[OC_SB_DMS] == OC_SB_SORT_UNREAD && p2.collapsed[OC_SB_DMS] == 1);
    CHECK(p2.sort[OC_SB_CHANNELS] == OC_SB_SORT_RECENT);
    /* The starred set survives the round trip, in order, with its collapse. */
    CHECK(p2.n_starred == 2 && p2.starred[0] == 11 && p2.starred[1] == 12);
    CHECK(p2.collapsed[OC_SB_STARRED] == 1);
    /* And a bucket written by an OLDER client (no ";s:" suffix) still parses, with
     * no stars — the suffix is optional by design. */
    oc_sidebar_opts p3; oc_sidebar_opts_defaults(&p3);
    oc_sidebar_opts_parse(&p3, "c:1,0,0;d:2,0,1");
    CHECK(p3.n_starred == 0 && p3.sort[OC_SB_DMS] == OC_SB_SORT_UNREAD);

    oc_model_free(&m);
}

static void test_resolve(void) {
    char d[256];
    /* Bare name gets the suffix; a dotted name passes through; no suffix = as-is. */
    CHECK(oc_resolve_domain("acme", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "acme.openchime.example") == 0);
    CHECK(oc_resolve_domain("chat.acme.com", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "chat.acme.com") == 0);
    CHECK(oc_resolve_domain("acme", NULL, d, sizeof d) == 0 && strcmp(d, "acme") == 0);
    /* A scheme and :port are stripped. */
    CHECK(oc_resolve_domain("openchime://chat.acme.com:8443", "x", d, sizeof d) == 0 &&
          strcmp(d, "chat.acme.com") == 0);
    /* Empty workspace is rejected distinctly. */
    CHECK(oc_resolve_domain("", "x", d, sizeof d) == -1);
    oc_endpoint ep;
    CHECK(oc_resolve("", NULL, &ep) == OC_RESOLVE_BAD_WORKSPACE);
    /* An explicit host:port (literal IP resolves instantly, no DNS) pins the port. */
    CHECK(oc_resolve("127.0.0.1:9000", NULL, &ep) == OC_RESOLVE_OK &&
          strcmp(ep.host, "127.0.0.1") == 0 && ep.port == 9000);

    /* The SRV parser picks the target host + port out of the wire answer. */
    char host[256]; int port = 0;
    CHECK(oc_srv_parse(SRV_ANSWER, (int)sizeof SRV_ANSWER, host, sizeof host, &port) == 0);
    CHECK(strcmp(host, "srv.acme.com") == 0 && port == 8443);
}

/* The model's sticky last_error (the login flow reads it to tell auth-fail from
 * unreachable): set by an error, preserved across the "disconnected" line, and
 * cleared on a successful auth. */
static void test_last_error(void) {
    oc_model m; oc_model_init(&m);
    oc_ev *e = oc_ev_new(OC_EV_ERROR); e->body = strdup("auth failed");
    oc_model_apply(&m, e); oc_ev_free(e);
    CHECK(strcmp(m.last_error, "auth failed") == 0);
    e = oc_ev_new(OC_EV_DISCONNECTED); oc_model_apply(&m, e); oc_ev_free(e);
    CHECK(strcmp(m.last_error, "auth failed") == 0 && m.connected == false);   /* not overwritten */
    e = oc_ev_new(OC_EV_AUTH_OK); e->user_id = 7; oc_model_apply(&m, e); oc_ev_free(e);
    CHECK(m.last_error[0] == '\0');                                            /* cleared on auth */
    oc_model_free(&m);
}

/* A mock oc_secret (in-memory keyring) for the credential-cache routing test. */
static struct { char account[64]; uint8_t val[512]; size_t len; int used; } g_mock[8];
static int mock_get(void *ctx, const char *a, uint8_t *out, size_t cap, size_t *len) {
    (void)ctx;
    for (int i = 0; i < 8; i++)
        if (g_mock[i].used && strcmp(g_mock[i].account, a) == 0) {
            if (g_mock[i].len > cap) return 0;
            memcpy(out, g_mock[i].val, g_mock[i].len); *len = g_mock[i].len; return 1;
        }
    return 0;
}
static int mock_put(void *ctx, const char *a, const uint8_t *v, size_t n) {
    (void)ctx;
    if (n > sizeof g_mock[0].val) return 0;
    for (int i = 0; i < 8; i++)
        if (!g_mock[i].used || strcmp(g_mock[i].account, a) == 0) {
            g_mock[i].used = 1; snprintf(g_mock[i].account, sizeof g_mock[i].account, "%s", a);
            memcpy(g_mock[i].val, v, n); g_mock[i].len = n; return 1;
        }
    return 0;
}
static int mock_each(void *ctx, oc_secret_each_cb cb, void *ud) {
    (void)ctx;
    for (int i = 0; i < 8; i++) if (g_mock[i].used) cb(ud, g_mock[i].account);
    return 1;
}
static void mock_reset(void) { memset(g_mock, 0, sizeof g_mock); }
static void mock_del(void *ctx, const char *a) {
    (void)ctx;
    for (int i = 0; i < 8; i++)
        if (g_mock[i].used && strcmp(g_mock[i].account, a) == 0) g_mock[i].used = 0;
}

static void count_msg_cb(void *ctx, uint64_t ch, uint64_t id, uint64_t aid,
                         const char *an, uint64_t t, const char *body, int e, int d) {
    (void)ch; (void)id; (void)aid; (void)an; (void)t; (void)body; (void)e; (void)d;
    (*(int *)ctx)++;
}
static void count_outbox_cb(void *ctx, const uint8_t idem[OC_IDEM_SIZE],
                            uint64_t ch, const char *body) {
    (void)idem; (void)ch; (void)body;
    (*(int *)ctx)++;
}

/* Capturing variants, for asserting the folded content and not just the count. */
struct msg_capture { int n; uint64_t id[8]; char body[8][64]; int edited[8], deleted[8]; };
static void msg_cb(void *ctx, uint64_t ch, uint64_t id, uint64_t aid,
                   const char *an, uint64_t t, const char *body, int e, int d) {
    (void)ch; (void)aid; (void)an; (void)t;
    struct msg_capture *c = ctx;
    if (c->n >= 8) return;
    c->id[c->n] = id;
    snprintf(c->body[c->n], sizeof c->body[0], "%s", body ? body : "");
    c->edited[c->n] = e; c->deleted[c->n] = d; c->n++;
}
struct out_capture { int n; char body[8][64]; };
static void out_cb(void *ctx, const uint8_t idem[OC_IDEM_SIZE], uint64_t ch, const char *body) {
    (void)idem; (void)ch;
    struct out_capture *c = ctx;
    if (c->n >= 8) return;
    snprintf(c->body[c->n], sizeof c->body[0], "%s", body ? body : "");
    c->n++;
}
/* The store names its per-workspace files by a hash we do not export; find the
 * one .log in the state dir instead of recomputing it. */
static int find_log_file(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".log") == 0) {
            snprintf(out, cap, "%s/%s", dir, e->d_name); found = 1; break;
        }
    }
    closedir(d);
    return found;
}

/* Capture the workspace book in call order (most-recently-used first). */
struct book_capture { int n; char ws[4][64]; char label[4][64]; char user[4][64]; };
static void book_cb(void *ctx, const char *workspace, const char *label,
                    const char *username, uint64_t last_used_ms) {
    (void)last_used_ms;
    struct book_capture *b = ctx;
    if (b->n >= 4) return;
    snprintf(b->ws[b->n],    sizeof b->ws[0],    "%s", workspace ? workspace : "");
    snprintf(b->label[b->n], sizeof b->label[0], "%s", label     ? label     : "");
    snprintf(b->user[b->n],  sizeof b->user[0],  "%s", username  ? username  : "");
    b->n++;
}

/* ARCH-88: the client writes NOTHING to disk. The store is a thin front for the
 * OS credential store, so with no keyring attached nothing persists at all —
 * which is the contract the sign-in screen's "Remember me = off" relies on. */
static void test_store_no_persistence_without_keyring(void) {
    oc_store *s = oc_store_open("build/itest_core_nostore");
    CHECK(s != NULL);
    if (!s) return;
    uint8_t tok[OC_SESSION_TOKEN_LEN], pin[OC_TLS_FINGERPRINT_LEN], got[OC_SESSION_TOKEN_LEN];
    memset(tok, 0xAB, sizeof tok); memset(pin, 0xCD, sizeof pin);
    oc_store_save_session(s, "acme:443", tok, 0);
    oc_store_save_pin(s, "acme:443", pin);
    oc_store_workspace_remember(s, "acme:443", "acme.example.com", "dana", 1000);
    CHECK(oc_store_load_session(s, "acme:443", got, NULL, 0) == 0);
    CHECK(oc_store_load_pin(s, "acme:443", got) == 0);
    struct book_capture b; memset(&b, 0, sizeof b);
    oc_store_workspace_each(s, book_cb, &b);
    CHECK(b.n == 0);
    oc_store_close(s);
}

static void test_workspace_book(void) {
    /* The book IS the credential store's contents now (one entry per workspace),
     * so it needs the keyring attached from the start. */
    mock_reset();
    oc_secret book_sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
    oc_store *s = oc_store_open("build/itest_core_book");
    CHECK(s != NULL);
    if (!s) return;
    oc_store_set_secret(s, &book_sec);

    oc_store_workspace_remember(s, "acme:443",   "acme.example.com",   "dana", 1000);
    oc_store_workspace_remember(s, "globex:443", "globex.example.com", "dana", 2000);

    /* Most-recently-used first: globex (2000) before acme (1000). */
    struct book_capture b; memset(&b, 0, sizeof b);
    oc_store_workspace_each(s, book_cb, &b);
    CHECK(b.n == 2);
    CHECK(strcmp(b.ws[0], "globex:443") == 0);
    CHECK(strcmp(b.ws[1], "acme:443") == 0);
    CHECK(strcmp(b.label[1], "acme.example.com") == 0);

    /* Re-touching acme moves it to the front, and a NULL label/username keeps
     * what was stored rather than blanking the switcher entry. */
    oc_store_workspace_remember(s, "acme:443", NULL, NULL, 3000);
    struct book_capture b2; memset(&b2, 0, sizeof b2);
    oc_store_workspace_each(s, book_cb, &b2);
    CHECK(b2.n == 2);
    CHECK(strcmp(b2.ws[0], "acme:443") == 0);
    CHECK(strcmp(b2.label[0], "acme.example.com") == 0);
    CHECK(strcmp(b2.user[0], "dana") == 0);

    /* Forgetting a workspace deletes its whole credential — book entry AND token
     * — in one go, leaving nothing behind. */
    uint8_t tok[OC_SESSION_TOKEN_LEN];
    for (unsigned i = 0; i < OC_SESSION_TOKEN_LEN; i++) tok[i] = (uint8_t)(i + 1);
    oc_store_save_session(s, "acme:443", tok, 0);
    CHECK(oc_store_load_session(s, "acme:443", tok, NULL, 0) == 1);

    oc_store_workspace_forget(s, "acme:443");
    struct book_capture b3; memset(&b3, 0, sizeof b3);
    oc_store_workspace_each(s, book_cb, &b3);
    CHECK(b3.n == 1 && strcmp(b3.ws[0], "globex:443") == 0);
    CHECK(oc_store_load_session(s, "acme:443", tok, NULL, 0) == 0);
    oc_store_close(s);
}

/* With a secret set, the session token round-trips through the keyring vtable and
 * does NOT land in the SQLite column; clearing goes through the vtable too. */
static void test_secret_routing(void) {
    memset(g_mock, 0, sizeof g_mock);
    oc_secret sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
    const char *sp = "build/itest_core_secret.db";
    unlink(sp); unlink("build/itest_core_secret.db-wal"); unlink("build/itest_core_secret.db-shm");

    uint8_t tok[OC_SESSION_TOKEN_LEN];
    for (unsigned i = 0; i < OC_SESSION_TOKEN_LEN; i++) tok[i] = (uint8_t)(i + 1);

    oc_store *s = oc_store_open(sp);
    CHECK(s != NULL);
    if (s) {
        oc_store_set_secret(s, &sec);
        oc_store_save_session(s, "host:1", tok, 0);
        uint8_t got[OC_SESSION_TOKEN_LEN];
        CHECK(oc_store_load_session(s, "host:1", got, NULL, 0) == 1 &&
              memcmp(got, tok, OC_SESSION_TOKEN_LEN) == 0);      /* via the keyring */
        oc_store_close(s);
    }
    /* Reopen WITHOUT the secret: the token is not in SQLite (it went to the mock). */
    oc_store *s2 = oc_store_open(sp);
    CHECK(s2 != NULL);
    if (s2) {
        uint8_t g2[OC_SESSION_TOKEN_LEN];
        CHECK(oc_store_load_session(s2, "host:1", g2, NULL, 0) == 0);
        oc_store_close(s2);
    }
    /* Clear via the secret. */
    oc_store *s3 = oc_store_open(sp);
    if (s3) {
        oc_store_set_secret(s3, &sec);
        oc_store_clear_session(s3, "host:1");
        uint8_t g3[OC_SESSION_TOKEN_LEN];
        CHECK(oc_store_load_session(s3, "host:1", g3, NULL, 0) == 0);
        oc_store_close(s3);
    }
    unlink(sp); unlink("build/itest_core_secret.db-wal"); unlink("build/itest_core_secret.db-shm");
}

int run_client_core_tests(void) {
    printf("test_client_core: sidebar, resolve, last-error, secret-routing, connect+auth, channel-list, send round-trip, unread, backfill, attachments, webhooks, client-settings, profile, seen-by, persisted store, v3 workspace upgrade, workspace book, cached history, session reconnect, offline outbox\n");

    test_sidebar();
    test_pins();
    test_resolve();
    test_last_error();
    test_secret_routing();
    test_store_no_persistence_without_keyring();
    test_workspace_book();

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
    CHECK(oc_dbwriter_register_local(dbw, "gil",  "pw-gil",  OC_ROLE_MEMBER, 2048) != 0);

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

        /* read receipts (REQ-090 seen-by): erik marks channel 1 read; the server
         * fans his advanced read cursor to dana, whose seen-by (excluding herself)
         * then names erik as having read up to dana's message. */
        oc_client_mark_read(b, 1);
        {
            uint64_t seen[8];
            CHECK(WAIT_FOR(a, oc_model_seen_by(m, 1, mid, m->user_id, seen, 8) == 1 && seen[0] == erik_id));
        }

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

        /* synced client-settings bucket: dana sets keys on device `a`; each SET
         * returns a fresh snapshot that folds into her model (read via
         * oc_model_setting) and — because the daemon fans the sync to all of a
         * user's connections — reaches a second dana device `d`. Deleting a key
         * (empty value) drops it everywhere. */
        oc_client *d = oc_client_start("127.0.0.1", arg.port, "dana:pw-dana");
        CHECK(d);
        CHECK(WAIT_FOR(d, m->authed && m->user_id != 0));
        oc_client_list_settings(d);                 /* subscribe to the (empty) bucket */
        CHECK(WAIT_FOR(d, m->settings_synced));
        oc_client_set_setting(a, "mouse", "1");
        CHECK(WAIT_FOR(a, oc_model_setting(m, "mouse") && strcmp(oc_model_setting(m, "mouse"), "1") == 0));
        CHECK(WAIT_FOR(d, oc_model_setting(m, "mouse") && strcmp(oc_model_setting(m, "mouse"), "1") == 0));
        oc_client_set_setting(a, "time_24h", "1");   /* a second key: both persist */
        CHECK(WAIT_FOR(d, oc_model_setting(m, "time_24h") &&
                          strcmp(oc_model_setting(m, "time_24h"), "1") == 0 &&
                          oc_model_setting(m, "mouse") != NULL));
        oc_client_set_setting(a, "mouse", "");       /* empty value deletes the key */
        CHECK(WAIT_FOR(d, oc_model_setting(m, "mouse") == NULL &&
                          oc_model_setting(m, "time_24h") != NULL));
        oc_client_stop(d);

        /* self-service profile (REQ-020): dana renames herself; the change fans to
         * every roster — her own (a) and erik's view (b). A password rotation
         * succeeds with the right old password and is rejected with a wrong one.
         * Restore both so the later "dana" author/roster assertions still hold. */
        uint64_t danaid = oc_client_model(a)->user_id;
        oc_client_set_display_name(a, "Dana Q");
        CHECK(WAIT_FOR(a, strcmp(oc_model_user_name(m, danaid), "Dana Q") == 0));
        CHECK(WAIT_FOR(b, strcmp(oc_model_user_name(m, danaid), "Dana Q") == 0));
        oc_client_change_password(a, "pw-dana", "pw-dana-2");
        CHECK(WAIT_FOR(a, strstr(m->status, "profile updated") != NULL));
        /* Assert on error_seq, not on last_error's text. `last_error` is CLEARED
         * on OC_EV_CONNECTED/AUTH_OK, so any reconnect between the rejection
         * arriving and this check wipes the evidence — a race that made this the
         * one intermittently-failing assertion in the suite. `error_seq` only
         * ever increments, so it cannot be un-observed. */
        uint32_t errs_before = oc_client_model(a)->error_seq;
        oc_client_change_password(a, "wrong-old", "irrelevant");   /* rejected */
        CHECK(WAIT_FOR(a, m->error_seq > errs_before));
        oc_client_change_password(a, "pw-dana-2", "pw-dana");       /* restore password */
        oc_client_set_display_name(a, "dana");                      /* restore name */
        CHECK(WAIT_FOR(a, strcmp(oc_model_user_name(m, danaid), "dana") == 0));
        CHECK(WAIT_FOR(b, strcmp(oc_model_user_name(m, danaid), "dana") == 0));

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

        /* attachments (REQ-140/141): dana uploads a multi-chunk file to channel 1;
         * the core streams it (UPLOAD_BEGIN→CHUNK×N→END→OK) and links it into a
         * message. erik sees the message carry the attachment metadata, downloads
         * it by id, and the reassembled bytes match the original. */
        {
            const size_t N = 150000;   /* > 2× the 65024-byte chunk: exercises windowing */
            unsigned char *blob = malloc(N);
            CHECK(blob != NULL);
            for (size_t k = 0; k < N; k++) blob[k] = (unsigned char)((k * 7 + 3) % 251);
            const char *src = "build/itest_core_upload.bin";
            const char *dst = "build/itest_core_download.bin";
            unlink(src); unlink(dst);
            FILE *sf = fopen(src, "wb");
            CHECK(sf != NULL);
            if (sf) { CHECK(fwrite(blob, 1, N, sf) == N); fclose(sf); }

            oc_client_upload(a, 1, src);
            /* erik receives a message carrying the attachment (empty body). */
            char fn[128] = {0}; uint64_t asz = 0, aid = 0;
            CHECK(WAIT_FOR(b, (aid = channel_attach(m, 1, fn, sizeof fn, &asz)) != 0));
            CHECK(asz == N);
            CHECK(strcmp(fn, "itest_core_upload.bin") == 0);
            /* erik downloads it by id; the bytes round-trip intact. */
            uint64_t aid_b = channel_attach(oc_client_model(b), 1, fn, sizeof fn, &asz);
            oc_client_download(b, aid_b, dst);
            CHECK(WAIT_FOR(b, file_matches(dst, blob, N)));

            free(blob);
            unlink(src); unlink(dst);
        }

        /* incoming webhooks (REQ-170): dana mints a webhook on channel 1 — the
         * server answers with a WEBHOOK_INFO whose token is shown once in her
         * model. She opens the webhook overlay (a list refresh), sees the labeled
         * entry, then deletes it and the row drops from the list. */
        oc_client_create_webhook(a, 1, "ci-bot");
        CHECK(WAIT_FOR(a, m->webhook_token[0] != '\0' && m->webhook_new_id != 0));
        uint64_t wid = oc_client_model(a)->webhook_new_id;
        oc_client_webhooks(a, 1);
        CHECK(WAIT_FOR(a, m->weblist_open && m->n_webhooks >= 1));
        {
            const oc_model *am = oc_client_model(a);
            int found = 0;
            for (size_t j = 0; j < am->n_webhooks; j++)
                if (am->webhooks[j].webhook_id == wid && strcmp(am->webhooks[j].label, "ci-bot") == 0) found = 1;
            CHECK(found);
        }
        oc_client_delete_webhook(a, wid);
        CHECK(WAIT_FOR(a, m->n_webhooks == 0));
        oc_client_close_webhooks(a);

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

        /* local store (ARCH-58): the session token + TOFU pin persist to a client
         * SQLite file. A client authenticates with a password, its token lands in
         * the store, and a second client pointed at the same store — given a
         * *wrong* password — still authenticates, because it rides in on the
         * stored session token (OC_AUTH_SESSION), never using the password. */
        {
            const char *sp = "build/itest_core_store.db";
            unlink(sp); unlink("build/itest_core_store.db-wal"); unlink("build/itest_core_store.db-shm");
            char inst[64]; snprintf(inst, sizeof inst, "127.0.0.1:%d", arg.port);

            /* A session token persists only into a credential store, so this
             * round-trip needs one (the in-memory mock stands in for the OS). */
            oc_secret store_sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
            oc_client *s1 = oc_client_start_secure("127.0.0.1", arg.port, "faye:pw-faye",
                                                   sp, &store_sec);
            CHECK(s1 != NULL);
            if (s1) {
                CHECK(WAIT_FOR(s1, m->authed && m->user_id != 0));
                oc_client_stop(s1);   /* QUIT (not logout): the session stays live */
            }
            /* The token + pin were persisted for this workspace. */
            oc_store *chk = oc_store_open(sp);
            CHECK(chk != NULL);
            if (chk) {
                uint8_t tok[OC_SESSION_TOKEN_LEN], pin[OC_TLS_FINGERPRINT_LEN];
                oc_store_set_secret(chk, &store_sec);
                CHECK(oc_store_load_session(chk, inst, tok, NULL, 0) == 1);
                CHECK(oc_store_load_pin(chk, inst, pin) == 1);
                oc_store_close(chk);
            }
            /* Wrong password, but the stored token authenticates it anyway. */
            oc_client *s2 = oc_client_start_secure("127.0.0.1", arg.port, "faye:WRONG-pw",
                                                   sp, &store_sec);
            CHECK(s2 != NULL);
            if (s2) {
                CHECK(WAIT_FOR(s2, m->authed && m->user_id != 0));
                oc_client_stop(s2);
            }
            unlink(sp); unlink("build/itest_core_store.db-wal"); unlink("build/itest_core_store.db-shm");
        }

        /* auto-reconnect with the session token (REQ-100/101). A fresh client
         * authenticates (capturing a session token), sends a message, then the
         * daemon's netloop is torn down and restarted on the same port (same DB,
         * so the session survives). The client silently re-authenticates with the
         * stored token — no password — its in-memory history is preserved, and it
         * can send again on the recovered session. */
        oc_client *rc = oc_client_start("127.0.0.1", arg.port, "faye:pw-faye");
        CHECK(rc != NULL);
        if (rc) {
            CHECK(WAIT_FOR(rc, m->authed && oc_model_channel((oc_model *)m, 1) != NULL));
            oc_client_send(rc, 1, "before the restart");
            CHECK(WAIT_FOR(rc, channel_has_body(m, 1, "before the restart")));

            /* Bounce the daemon: the client's connection drops and it begins
             * reconnecting with backoff while the listener is down. */
            arg.stop = 1;
            pthread_join(th, NULL);
            arg.stop = 0;
            CHECK(pthread_create(&th, NULL, core_loop_thread, &arg) == 0);
            wait_port_ready(arg.port);
            oc_client_reconnect(rc);   /* cut the backoff so the retry is prompt */

            /* It comes back authenticated (session-token reconnect), with its
             * pre-restart history intact, and a new send round-trips. */
            CHECK(WAIT_FOR(rc, m->connected && m->authed));
            CHECK(channel_has_body(oc_client_model(rc), 1, "before the restart"));
            oc_client_send(rc, 1, "after the restart");
            CHECK(WAIT_FOR(rc, channel_has_body(m, 1, "after the restart")));

            oc_client_stop(rc);
        }

        /* ARCH-88: a client keeps NO local history, so a cold one must still land
         * on what it missed. It sends a cursorless BACKFILL_REQUEST and the daemon
         * resumes from that user's server-side read position (REQ-090) — the thing
         * that makes a stateless client possible. Proven by posting while faye is
         * away and asserting a brand-new client (no store at all) receives it. */
        {
            oc_client *c1 = oc_client_start("127.0.0.1", arg.port, "faye:pw-faye");
            CHECK(c1 != NULL);
            if (c1) {
                CHECK(WAIT_FOR(c1, m->authed && oc_model_channel((oc_model *)m, 1) != NULL));
                oc_client_backfill(c1, 1);
                oc_client_send(c1, 1, "read before going away");
                CHECK(WAIT_FOR(c1, channel_has_body(m, 1, "read before going away")));
                oc_client_mark_read(c1, 1);      /* advances the server-side cursor */
                for (int k = 0; k < 40; k++) oc_client_tick(c1);
                oc_client_stop(c1);
            }
            /* Posted while faye has no client running at all. */
            oc_client *other = oc_client_start("127.0.0.1", arg.port, "gil:pw-gil");
            if (other) {
                CHECK(WAIT_FOR(other, m->authed));
                oc_client_send(other, 1, "arrived while away");
                CHECK(WAIT_FOR(other, channel_has_body(m, 1, "arrived while away")));
                oc_client_stop(other);
            }
            /* A cold client: no store, no cursor, nothing remembered. */
            oc_client *c2 = oc_client_start("127.0.0.1", arg.port, "faye:pw-faye");
            CHECK(c2 != NULL);
            if (c2) {
                CHECK(WAIT_FOR(c2, m->authed));
                CHECK(WAIT_FOR(c2, channel_has_body(m, 1, "arrived while away")));
                oc_client_stop(c2);
            }
        }

        /* offline outbox (REQ-102), now in memory: a message composed while the
         * daemon is down is held by the net thread and resent when the connection
         * comes back — within the life of the process, which is what REQ-102 asks
         * for ("queued locally, sent automatically on reconnect"). */
        {
            oc_client *o1 = oc_client_start("127.0.0.1", arg.port, "faye:pw-faye");
            CHECK(o1 != NULL);
            if (o1) {
                CHECK(WAIT_FOR(o1, m->authed && oc_model_channel((oc_model *)m, 1) != NULL));
                arg.stop = 1;
                pthread_join(th, NULL);
                CHECK(WAIT_FOR(o1, !m->connected));
                oc_client_send(o1, 1, "queued while offline");   /* -> in-memory outbox */

                /* Bring the daemon back; the same client reconnects and flushes. */
                arg.stop = 0;
                CHECK(pthread_create(&th, NULL, core_loop_thread, &arg) == 0);
                wait_port_ready(arg.port);
                oc_client_reconnect(o1);
                CHECK(WAIT_FOR(o1, m->authed));
                CHECK(WAIT_FOR(o1, channel_has_body(m, 1, "queued while offline")));
                oc_client_stop(o1);
            }
        }
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
