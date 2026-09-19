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
#include "complete.h"    /* who the New message pane may address (REQ-229) */

#include "netloop.h"
#include "config.h"
#include "dbwriter.h"
#include "protocol.h"
#include "tls.h"
#include "tts_render.h"
#include "stt_render.h"
#include "audio_dev.h"
#include "oc_dictate.h"
#include "oc_mp4.h"
#include "oc_call_engine.h"
#include "audio.h"        /* the relay, run on a thread for the call test */
#include "e2e_hpke.h"
#include "e2e_sframe.h"
#include "check.h"

#include <math.h>
#include <sqlite3.h>     /* to hand-build a pre-rename store for the upgrade test */
#include "oc_port.h"      /* oc_utc_offset_min: the value the core sends on connect */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
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

/* Read-aloud with a stub voice (ARCH-111): the core's half is the queue, the
 * fetch and the bytes, none of which needs a real model. */
static void *core_tts_open(void *ctx, char *err, size_t cap) { (void)ctx; (void)err; (void)cap; static int t; return &t; }
static void core_tts_close(void *e) { (void)e; }
static const char *core_tts_voice_id(int v) { return v == 0 ? "core-voice-m" : "core-voice-f"; }
static const char *core_tts_voice_label(int v) { return v == 0 ? "Core Low" : "Core High"; }

static int core_tts_say(void *e, const char *segment, int voice, float **pcm, size_t *n,
                        char *err, size_t cap) {
    (void)e; (void)voice; (void)err; (void)cap;
    size_t samples = strlen(segment) * 240;          /* 10 ms a character at 24 kHz */
    *pcm = calloc(samples, sizeof **pcm);
    if (!*pcm) return -1;
    for (size_t i = 0; i < samples; i++) (*pcm)[i] = (float)(0.2 * sin(2 * M_PI * 220.0 * (double)i / 24000.0));
    *n = samples;
    return 0;
}

static const oc_tts_engine CORE_TTS = {
    .version = "core-stub-1", .lang = "en-US", .rate = 24000, .voices = 2, .ctx = NULL,
    .voice_id = core_tts_voice_id, .voice_label = core_tts_voice_label,
    .preview = "This is a test voice.",
    .open = core_tts_open, .close = core_tts_close, .say = core_tts_say,
};

/* Voice input with a stub recognizer (ARCH-112): the first sample is a code, the
 * second a marker -- 1 says "spoken <marker> at erik", 3 fails. */
static void *core_stt_open(void *ctx, char *err, size_t cap) { (void)ctx; (void)err; (void)cap; static int t; return &t; }
static void core_stt_close(void *e) { (void)e; }
static int core_stt_hear(void *e, const int16_t *pcm, size_t n, char **text, char *err, size_t cap) {
    (void)e;
    int code = n > 0 ? pcm[0] : 0, marker = n > 1 ? pcm[1] : 0;
    if (code == 3) { snprintf(err, cap, "stub failure"); return -1; }
    char buf[64] = "";
    if (code == 1) snprintf(buf, sizeof buf, "spoken %d at erik", marker);
    *text = strdup(buf);
    return *text ? 0 : -1;
}
static const oc_stt_engine CORE_STT = {
    .version = "core-stt-1", .lang = "en-US", .ctx = NULL,
    .open = core_stt_open, .close = core_stt_close, .hear = core_stt_hear,
};

static void stt_say(oc_client *c, uint8_t mode, uint64_t channel, int16_t code, int16_t marker) {
    int16_t pcm[8000];
    memset(pcm, 0, sizeof pcm);
    pcm[0] = code;
    pcm[1] = marker;
    CHECK(oc_client_stt_send(c, mode, channel, 0, pcm, 8000) != 0);
}

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
/* The newest video message in channel `cid` (REQ-165), or NULL. */
static const oc_msg *channel_video(const oc_model *m, uint64_t cid) {
    const oc_msg *found = NULL;
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++) {
            const oc_msg *msg = &m->channels[i].msgs[j];
            if (msg->n_attach && msg->attach[0].media_kind == OC_MEDIA_VIDEO_MESSAGE) found = msg;
        }
    }
    return found;
}

static int channel_video_count(const oc_model *m, uint64_t cid) {
    int n = 0;
    for (size_t i = 0; i < m->n_channels; i++) {
        if (m->channels[i].channel_id != cid) continue;
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].n_attach &&
                m->channels[i].msgs[j].attach[0].media_kind == OC_MEDIA_VIDEO_MESSAGE) n++;
    }
    return n;
}

/* Take fetched bytes for `aid` if they have arrived; others are dropped. */
static uint8_t *take_fetched(oc_client *cl, uint64_t aid, size_t *len) {
    uint64_t id; size_t n; uint8_t *d;
    while ((d = oc_model_take_attachment((oc_model *)oc_client_model(cl), &id, &n)) != NULL) {
        if (id == aid) { *len = n; return d; }
        free(d);
    }
    return NULL;
}

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

/* The sidebar helper (6): grouping, filter, sort, collapse — shared by
 * every frontend so the TUI and the GUI cannot disagree about what belongs
 * where. Built against a hand-made model, no daemon needed. */
/* What the daemon offers, folded into the model (REQ-295). Read-aloud is shown
 * only where CAPABILITIES names "tts", and the answer is replaced on every
 * connection rather than added to -- a client that reconnects to a daemon whose
 * voice data has gone must stop offering the feature, not keep the old yes. */
static int caps_fold(oc_model *m, const char *names) {
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CAPABILITIES;
    e.body = names ? strdup(names) : NULL;
    oc_model_apply(m, &e);
    free(e.body);
    return oc_model_tts_available(m);
}

static void test_capabilities(void) {
    oc_model m; oc_model_init(&m);
    CHECK(oc_model_tts_available(&m) == 0);                 /* nothing said yet: nothing shown */
    CHECK(caps_fold(&m, "tts") == 1);
    CHECK(caps_fold(&m, "stt") == 0);                       /* replaced, not accumulated */
    CHECK(oc_model_has_capability(&m, OC_CAP_STT) == 1);
    CHECK(caps_fold(&m, "stt,tts") == 1);                   /* order is not meaning */
    CHECK(caps_fold(&m, "ttsx,xtts,tt") == 0);              /* whole names only */
    CHECK(caps_fold(&m, "") == 0);                          /* the daemon offers nothing */
    CHECK(caps_fold(&m, "tts") == 1);
    CHECK(caps_fold(&m, NULL) == 0);
    CHECK(oc_model_has_capability(&m, "") == 0);
    oc_model_free(&m);
}

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

/* A GROUP DM is titled by everyone in it, the reader included (REQ-056). This
 * went untested, and the title quietly disagreed with both the member pane and
 * the participant count beside it: a three-person group read "bob, carol". */
static void test_group_dm_title(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;                                  /* signed in as alice */

    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 1; e.body = strdup("alice"); oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 2; e.body = strdup("bob");   oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 3; e.body = strdup("carol"); oc_model_apply(&m, &e);

    /* The daemon sends every member of the DM, self included (dbwriter.c selects
     * the whole channel_members row set), so n_peers is 3 here, not 2. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 20; e.status = 1; e.op = OC_CHANNEL_KIND_DM;
    e.user_id = 2; e.server_time = 500;
    e.n_peers = 3; e.peers[0] = 1; e.peers[1] = 2; e.peers[2] = 3;
    oc_model_apply(&m, &e);

    const oc_channel *c = oc_model_channel(&m, 20);
    CHECK(c != NULL);
    CHECK(c->n_peers == 3);                          /* the count the GUI badge draws */

    char title[96];
    oc_model_dm_title(&m, c, title, sizeof title);
    CHECK(strcmp(title, "alice, bob, carol") == 0);  /* alphabetical, self included */

    /* A 1:1 DM is still titled by the other person, not by both of them. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 21; e.status = 1; e.op = OC_CHANNEL_KIND_DM;
    e.user_id = 2; e.server_time = 600; oc_model_apply(&m, &e);
    const oc_channel *d = oc_model_channel(&m, 21);
    CHECK(d != NULL);
    oc_model_dm_title(&m, d, title, sizeof title);
    CHECK(strcmp(title, "bob") == 0);

    oc_model_free(&m);
}

/* A channel that appears AFTER the settings sync starts at the user's global
 * default (REQ-134), not at whatever zeroed memory means.
 *
 * NOTIFY_PREFS is a full sync: its header sets the default and re-seeds every
 * channel the model already holds, then the per-channel entries set the
 * exceptions. A channel created later never passed through that, so it was born
 * with notify_level 0 — which is OC_NOTIFY_ALL, the one level a user who set
 * "only when mentioned" has explicitly said they do not want. The setting held
 * until somebody made a channel and then quietly stopped holding there. */
static void test_new_channel_takes_the_default_level(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;

    oc_ev e;
    /* The sync: "mentions only, everywhere". */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_DND; e.op = OC_NOTIFY_MENTIONS; oc_model_apply(&m, &e);
    CHECK(m.notify_default == OC_NOTIFY_MENTIONS);

    /* A channel that arrives afterwards. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 42; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 1; e.body = strdup("late"); e.server_time = 500;
    oc_model_apply(&m, &e);

    const oc_channel *c = oc_model_channel(&m, 42);
    CHECK(c != NULL);
    if (c) CHECK(c->notify_level == OC_NOTIFY_MENTIONS);

    /* An explicit per-channel preference still outranks the default, so this
     * is a floor and not a new way to ignore what the user set. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_NOTIFY_PREF; e.channel_id = 42; e.op = OC_NOTIFY_ALL; e.status = 0;
    oc_model_apply(&m, &e);
    c = oc_model_channel(&m, 42);
    if (c) CHECK(c->notify_level == OC_NOTIFY_ALL);

    /* And a default of ALL still means ALL: the seeding must not have been
     * written as "anything but zero". */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_DND; e.op = OC_NOTIFY_ALL; oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 43; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 1; e.body = strdup("later"); e.server_time = 600;
    oc_model_apply(&m, &e);
    c = oc_model_channel(&m, 43);
    CHECK(c != NULL);
    if (c) CHECK(c->notify_level == OC_NOTIFY_ALL);

    oc_model_free(&m);
}

/* The client's half of the one notify decision (ARCH-89, ARCH-103).
 *
 * oc_model_notify_scan gathers the inputs and asks oc_notify_decide, the same
 * function the daemon's push query asks — so what is worth testing here is not
 * the precedence order (test_push sweeps that against the daemon over all 192
 * states) but the SCAN: which messages it considers at all. That is where the
 * client's copy was wrong in ways the shared rule could not see, because it was
 * never asked about the messages that were skipped. */
static void test_notify_scan(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;

    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 1; e.body = strdup("alice"); oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 2; e.body = strdup("bob");   oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 10; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 1; e.body = strdup("general"); e.server_time = 10;
    oc_model_apply(&m, &e);

    oc_channel *c = oc_model_channel(&m, 10);
    CHECK(c != NULL);
    if (!c) { oc_model_free(&m); return; }
    c->notify_level = OC_NOTIFY_MENTIONS;

    /* A BATCH: a message that names me, then two that do not. The tick sees all
     * three at once. Sampling only the newest — which is what the toast gate
     * used to do — asks the rule about "afternoon all" and gets silence, so the
     * mention two rows above it is never announced at all. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 1; e.author_id = 2;
    e.server_time = 100; e.body = strdup("@alice can you look"); oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 2; e.author_id = 2;
    e.server_time = 200; e.body = strdup("never mind"); oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 3; e.author_id = 2;
    e.server_time = 300; e.body = strdup("afternoon all"); oc_model_apply(&m, &e);

    int men = 0, kw = 0, vip = 0;
    const oc_msg *pick = oc_model_notify_scan(&m, c, 0, 0, 0, &men, &kw, &vip);
    CHECK(pick != NULL);
    if (pick) CHECK(pick->message_id == 1);   /* the only one that notifies */
    CHECK(men == 1);
    CHECK(kw == 0 && vip == 0);

    /* `since_id` is honoured: nothing above message 3 is new. */
    pick = oc_model_notify_scan(&m, c, 3, 0, 0, &men, &kw, &vip);
    CHECK(pick == NULL);
    CHECK(men == 0);   /* the out-params are cleared, not left from last time */

    /* The NEWEST notifying message is the subject when several qualify, and the
     * flags are the OR across all of them — the sound and the taskbar flash ask
     * "did any of this name me?", not "did the last one?". */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 4; e.author_id = 2;
    e.server_time = 400; e.body = strdup("@alice again"); oc_model_apply(&m, &e);
    pick = oc_model_notify_scan(&m, c, 0, 0, 0, &men, &kw, &vip);
    if (pick) CHECK(pick->message_id == 4);
    CHECK(men == 1);

    /* A DELETED message is not news. The tombstone stays for the reader, but it
     * has no body to preview and nothing left to interrupt for — this used to
     * raise a toast with an empty body. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_DELETE; e.channel_id = 10; e.message_id = 4; oc_model_apply(&m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_DELETE; e.channel_id = 10; e.message_id = 1; oc_model_apply(&m, &e);
    pick = oc_model_notify_scan(&m, c, 0, 0, 0, &men, &kw, &vip);
    CHECK(pick == NULL);        /* both mentions are tombstones now */
    CHECK(men == 0);

    /* My OWN message never notifies me, whatever it says. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 5; e.author_id = 1;
    e.server_time = 500; e.body = strdup("@alice talking to myself");
    oc_model_apply(&m, &e);
    pick = oc_model_notify_scan(&m, c, 0, 0, 0, &men, &kw, &vip);
    CHECK(pick == NULL);

    /* The shared rule still governs: mute silences a mention, and so does the
     * schedule and the pause. Precedence itself is test_push's sweep; this only
     * proves the scan actually routes through it rather than deciding on its
     * own. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 6; e.author_id = 2;
    e.server_time = 600; e.body = strdup("@alice one more"); oc_model_apply(&m, &e);
    CHECK(oc_model_notify_scan(&m, c, 5, 0, 0, NULL, NULL, NULL) != NULL);
    CHECK(oc_model_notify_scan(&m, c, 5, 1, 0, NULL, NULL, NULL) == NULL);   /* quiet */
    CHECK(oc_model_notify_scan(&m, c, 5, 0, 1, NULL, NULL, NULL) == NULL);   /* paused */
    c->muted = 1;
    CHECK(oc_model_notify_scan(&m, c, 5, 0, 0, NULL, NULL, NULL) == NULL);   /* muted */
    c->muted = 0;

    /* A KEYWORD hit passes the MENTIONS level as an @-mention does (REQ-135),
     * through the same scanner the daemon resolved with (ARCH-103) — the client
     * used to ignore keywords entirely, so the phone rang and the desktop did
     * not. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 7; e.author_id = 2;
    e.server_time = 700; e.body = strdup("the deploy is done"); oc_model_apply(&m, &e);
    CHECK(oc_model_notify_scan(&m, c, 6, 0, 0, NULL, NULL, NULL) == NULL);
    snprintf(m.kw_terms[0], sizeof m.kw_terms[0], "deploy");
    m.n_kw_terms = 1;
    men = kw = vip = 0;
    pick = oc_model_notify_scan(&m, c, 6, 0, 0, &men, &kw, &vip);
    CHECK(pick != NULL);
    if (pick) CHECK(pick->message_id == 7);
    CHECK(kw == 1 && men == 0);
    /* Still inside the level, not around it: mute and the schedule silence a
     * keyword exactly as they silence a mention. */
    CHECK(oc_model_notify_scan(&m, c, 6, 1, 0, NULL, NULL, NULL) == NULL);
    m.n_kw_terms = 0;

    /* A PRIORITY PERSON pierces the level, the schedule and the pause alike —
     * the one input that says WHO where the rest say WHEN. Mute is the
     * deliberate limit and still wins. */
    memset(&e, 0, sizeof e);
    e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 8; e.author_id = 2;
    e.server_time = 800; e.body = strdup("morning"); oc_model_apply(&m, &e);
    CHECK(oc_model_notify_scan(&m, c, 7, 1, 1, NULL, NULL, NULL) == NULL);
    m.pri_people[0] = 2; m.n_pri_people = 1;
    men = kw = vip = 0;
    pick = oc_model_notify_scan(&m, c, 7, 1, 1, &men, &kw, &vip);
    CHECK(pick != NULL);            /* through both the schedule and the pause */
    CHECK(vip == 1);
    c->muted = 1;
    CHECK(oc_model_notify_scan(&m, c, 7, 0, 0, NULL, NULL, NULL) == NULL);
    c->muted = 0;

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

    /* With nothing starred there is NO Starred section at all: it is purely
     * derived, so an empty one is a row that says nothing and opens onto
     * nothing. Two headers then — Channels A-Z, and the DM titled by its peer. */
    size_t n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 5);
    CHECK(rows[0].is_header && rows[0].section == OC_SB_CHANNELS && rows[0].section_total == 2);
    CHECK(strcmp(rows[1].label, "alpha") == 0 && rows[1].is_private == 1);
    CHECK(strcmp(rows[2].label, "zulu") == 0  && rows[2].is_private == 0);
    CHECK(rows[3].is_header && rows[3].section == OC_SB_DMS);
    CHECK(strcmp(rows[4].label, "bob") == 0 && rows[4].unread == 3);

    /* Starring lifts a conversation OUT of its section into Starred — it must appear
     * once, not twice. */
    CHECK(oc_sidebar_toggle_star(&o, 11) == 1);
    CHECK(oc_sidebar_is_starred(&o, 11) && !oc_sidebar_is_starred(&o, 12));
    n = oc_model_sidebar(&m, &o, rows, 16);
    /* ...and starring one brings the section INTO existence. */
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
    /* Un-starring the last one takes the section away again, and the
     * conversations go back where they came from. */
    CHECK(n == 5 && rows[0].section == OC_SB_CHANNELS);
    CHECK(strcmp(rows[1].label, "alpha") == 0);

    /* Unreads only (REQ-234's sidebar): every section keeps just what has something
     * unread — here only the DM (3 unread) — and the open conversation stays even
     * though it is read, so reading it cannot pull it out from under you. */
    o.unreads_only = 1;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 3);
    CHECK(rows[0].is_header && rows[0].section == OC_SB_CHANNELS && rows[0].section_total == 2);
    CHECK(rows[1].is_header && rows[1].section == OC_SB_DMS);
    CHECK(strcmp(rows[2].label, "bob") == 0);
    o.keep_id = 10;                                  /* zulu is open */
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 4 && strcmp(rows[1].label, "zulu") == 0);
    {   /* It persists, and an older setting without it reads as off. */
        char enc[512]; oc_sidebar_opts_encode(&o, enc, sizeof enc);
        CHECK(strstr(enc, ";ro:1") != NULL);
        oc_sidebar_opts back; oc_sidebar_opts_defaults(&back);
        oc_sidebar_opts_parse(&back, enc);
        CHECK(back.unreads_only == 1 && back.keep_id == 0);
        oc_sidebar_opts old; oc_sidebar_opts_defaults(&old);
        old.unreads_only = 1;
        oc_sidebar_opts_parse(&old, "c:0,0,0;d:0,0,0");
        CHECK(old.unreads_only == 0);
        /* A custom section NAMED with the key must not switch it on. */
        oc_sidebar_opts named; oc_sidebar_opts_defaults(&named);
        oc_sidebar_opts_parse(&named, "c:0,0,0;d:0,0,0;sc:0;u:x;ro:1|0,0,0|");
        CHECK(named.unreads_only == 0);
    }
    o.unreads_only = 0; o.keep_id = 0;

    /* Recency uses the server-reported last_message_at: alpha(300) before zulu(100). */
    o.sort[OC_SB_CHANNELS] = OC_SB_SORT_RECENT;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(strcmp(rows[1].label, "alpha") == 0 && strcmp(rows[2].label, "zulu") == 0);

    /* Collapsing keeps the header (so it can be reopened) and drops the children. */
    o.collapsed[OC_SB_CHANNELS] = 1;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 3 && rows[0].is_header && rows[0].section_total == 2);
    o.collapsed[OC_SB_CHANNELS] = 0;

    /* Unread-only hides the read channels but keeps the unread DM. */
    o.filter[OC_SB_CHANNELS] = OC_SB_FILTER_UNREAD;
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 3 && rows[1].is_header && strcmp(rows[2].label, "bob") == 0);
    o.filter[OC_SB_CHANNELS] = OC_SB_FILTER_ALL;

    /* Find matches the rendered LABEL, so a DM (which has no name) is findable. */
    snprintf(o.find, sizeof o.find, "bo");
    n = oc_model_sidebar(&m, &o, rows, 16);
    CHECK(n == 3 && strcmp(rows[2].label, "bob") == 0);
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
    CHECK(p3.n_custom == 0);

    /* ---- user-defined sections (the other half of REQ-234) ---------- */
    oc_sidebar_opts u; oc_sidebar_opts_defaults(&u);
    int work = oc_sidebar_section_add(&u, "Work");
    CHECK(work == 0 && u.n_custom == 1);

    /* A named section sits where Starred would be (nothing is starred here, so
     * that section is absent) and above Channels, and a conversation in it is
     * REMOVED from Channels — the same appear-once rule Starred follows. A custom
     * section, unlike Starred, stays visible when empty: somebody made it, and
     * hiding it would leave no way to manage or remove it. */
    CHECK(oc_sidebar_assign(&u, 10, work) == 1);         /* zulu */
    CHECK(oc_sidebar_section_of(&u, 10) == work);
    n = oc_model_sidebar(&m, &u, rows, 16);
    CHECK(n == 6);
    CHECK(rows[0].is_header && rows[0].section == OC_SB_CUSTOM_BASE);
    CHECK(strcmp(rows[0].label, "Work") == 0 && rows[0].section_total == 1);
    CHECK(strcmp(rows[1].label, "zulu") == 0 && rows[1].section == OC_SB_CUSTOM_BASE);
    CHECK(rows[2].is_header && rows[2].section == OC_SB_CHANNELS && rows[2].section_total == 1);
    CHECK(strcmp(rows[3].label, "alpha") == 0);           /* zulu is gone from here */

    /* At most one section: assigning again MOVES rather than duplicating. */
    int later = oc_sidebar_section_add(&u, "Later");
    CHECK(later == 1);
    CHECK(oc_sidebar_assign(&u, 10, later) == 1);
    CHECK(oc_sidebar_section_of(&u, 10) == later);
    CHECK(u.custom[work].n_ids == 0 && u.custom[later].n_ids == 1);

    /* Starred WINS over a custom section: two lift-it-out rules need a precedence,
     * and a conversation in both must still appear exactly once. */
    CHECK(oc_sidebar_toggle_star(&u, 10) == 1);
    n = oc_model_sidebar(&m, &u, rows, 16);
    CHECK(rows[0].section_total == 1);                    /* in Starred */
    {
        int seen = 0;
        for (size_t i = 0; i < n; i++)
            if (!rows[i].is_header && rows[i].channel_id == 10) seen++;
        CHECK(seen == 1);
    }
    CHECK(oc_sidebar_toggle_star(&u, 10) == 1);           /* back to Later */

    /* Per-section sort/filter/collapse work through the accessors, which is what a
     * frontend must use: a custom section's number is past the end of the built-in
     * arrays, and indexing them directly would read off the end. */
    CHECK(oc_sb_custom_index(OC_SB_CUSTOM_BASE + 1) == 1);
    CHECK(oc_sb_custom_index(OC_SB_CHANNELS) == -1);
    oc_sb_set_collapsed(&u, OC_SB_CUSTOM_BASE + later, 1);
    CHECK(oc_sb_collapsed_of(&u, OC_SB_CUSTOM_BASE + later) == 1);
    n = oc_model_sidebar(&m, &u, rows, 16);
    CHECK(rows[2].is_header);                             /* its child is hidden */
    oc_sb_set_collapsed(&u, OC_SB_CUSTOM_BASE + later, 0);
    oc_sb_set_sort(&u, OC_SB_CUSTOM_BASE + later, OC_SB_SORT_RECENT);
    CHECK(oc_sb_sort_of(&u, OC_SB_CUSTOM_BASE + later) == OC_SB_SORT_RECENT);
    CHECK(u.sort[OC_SB_CHANNELS] == OC_SB_SORT_AZ);       /* did not touch a built-in */

    /* The name is sanitised, because it round-trips through ONE flat setting string
     * and the separators cannot survive inside it. Stripped, not rejected: the user
     * asked for a section and should get one. */
    int odd = oc_sidebar_section_add(&u, "A;b:c,d|e ");
    CHECK(odd == 2 && strcmp(u.custom[odd].name, "Abcde") == 0);
    CHECK(oc_sidebar_section_add(&u, ";;;") == -1);       /* nothing left = no section */
    CHECK(oc_sidebar_section_add(&u, "") == -1);

    /* Round-trip, with the ids and each section's own sort/filter/collapse. */
    char e2[512]; oc_sidebar_opts_encode(&u, e2, sizeof e2);
    oc_sidebar_opts p4; oc_sidebar_opts_defaults(&p4);
    oc_sidebar_opts_parse(&p4, e2);
    CHECK(p4.n_custom == 3);
    CHECK(strcmp(p4.custom[0].name, "Work") == 0);
    CHECK(strcmp(p4.custom[1].name, "Later") == 0);
    CHECK(strcmp(p4.custom[2].name, "Abcde") == 0);
    CHECK(p4.custom[1].n_ids == 1 && p4.custom[1].ids[0] == 10);
    CHECK(oc_sb_sort_of(&p4, OC_SB_CUSTOM_BASE + 1) == OC_SB_SORT_RECENT);
    CHECK(oc_sidebar_section_of(&p4, 10) == 1);

    /* Removing a section returns its conversations to Channels rather than losing
     * them: a section is a view, not a container. */
    oc_sidebar_section_remove(&p4, 1);
    CHECK(p4.n_custom == 2 && oc_sidebar_section_of(&p4, 10) == -1);
    CHECK(strcmp(p4.custom[1].name, "Abcde") == 0);       /* the list closed up */
    n = oc_model_sidebar(&m, &p4, rows, 16);
    {
        int in_channels = 0;
        for (size_t i = 0; i < n; i++)
            if (!rows[i].is_header && rows[i].channel_id == 10 &&
                rows[i].section == OC_SB_CHANNELS) in_channels = 1;
        CHECK(in_channels);
    }

    /* Rename keeps the members; an empty rename is refused rather than leaving an
     * unclickable header. */
    oc_sidebar_section_rename(&p4, 0, "Projects");
    CHECK(strcmp(p4.custom[0].name, "Projects") == 0);
    oc_sidebar_section_rename(&p4, 0, ";");
    CHECK(strcmp(p4.custom[0].name, "Projects") == 0);

    /* The cap refuses rather than evicting, like the starred list. */
    oc_sidebar_opts f2; oc_sidebar_opts_defaults(&f2);
    for (unsigned i = 0; i < OC_SB_CUSTOM_MAX; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "s%u", i);
        CHECK(oc_sidebar_section_add(&f2, nm) == (int)i);
    }
    CHECK(oc_sidebar_section_add(&f2, "one more") == -1);
    CHECK(f2.n_custom == (int)OC_SB_CUSTOM_MAX);

    /* "Active only" and a GROUP DM (REQ-056). A group has no single peer, so the 1:1
     * test — "is peer_id online" — read peer 0 as offline and hid every group. The
     * rule for a group is "is ANYONE in it around", which is what the filter means. */
    {
        oc_ev ge; memset(&ge, 0, sizeof ge);
        ge.type = OC_EV_CHANNEL; ge.channel_id = 13; ge.status = 1; ge.op = OC_CHANNEL_KIND_DM;
        ge.n_peers = 3; ge.peers[0] = 1; ge.peers[1] = 2; ge.peers[2] = 3;
        oc_model_apply(&m, &ge);
        memset(&ge, 0, sizeof ge);
        ge.type = OC_EV_USER; ge.user_id = 3; ge.body = strdup("carol"); oc_model_apply(&m, &ge);

        oc_sidebar_opts a2; oc_sidebar_opts_defaults(&a2);
        a2.filter[OC_SB_DMS] = OC_SB_FILTER_ACTIVE;
        oc_sidebar_row ar[16];
        size_t an = oc_model_sidebar(&m, &a2, ar, 16);
        int saw_group = 0;
        for (size_t i = 0; i < an; i++)
            if (!ar[i].is_header && ar[i].channel_id == 13) saw_group = 1;
        CHECK(!saw_group);                       /* nobody in it is online yet */

        memset(&ge, 0, sizeof ge);
        ge.type = OC_EV_PRESENCE; ge.user_id = 3; ge.status = OC_PRESENCE_ONLINE;
        oc_model_apply(&m, &ge);
        an = oc_model_sidebar(&m, &a2, ar, 16);
        saw_group = 0;
        for (size_t i = 0; i < an; i++)
            if (!ar[i].is_header && ar[i].channel_id == 13) saw_group = 1;
        CHECK(saw_group);                        /* carol is around, so the group is */
    }


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
    /* `localhost` is a host, not an org shorthand: suffixing it gave
     * "localhost.openchime.example", which resolves nowhere — so the address a
     * developer reaches for first was the one address that could not work. */
    CHECK(oc_resolve_domain("localhost", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "localhost") == 0);
    CHECK(oc_resolve_domain("localhost:8443", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "localhost") == 0);
    CHECK(oc_resolve_domain("LocalHost.", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "localhost") == 0);
    /* A name that merely starts that way is still an org name. */
    CHECK(oc_resolve_domain("localhost-eu", "openchime.example", d, sizeof d) == 0 &&
          strcmp(d, "localhost-eu.openchime.example") == 0);
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
/* `val` is sized well past the credential blob for the same reason the libsecret
 * backend's hex buffer is: a mock that starts refusing writes when the blob grows
 * would fail these tests everywhere except at the line that actually changed. */
static struct { char account[64]; uint8_t val[1024]; size_t len; int used; } g_mock[8];
static int mock_get(void *ctx, const char *a, uint8_t *out, size_t cap, size_t *len) {
    (void)ctx;
    for (int i = 0; i < 8; i++)
        if (g_mock[i].used && strcmp(g_mock[i].account, a) == 0) {
            if (g_mock[i].len > cap) return 0;
            memcpy(out, g_mock[i].val, g_mock[i].len); *len = g_mock[i].len; return 1;
        }
    return 0;
}
/* What the backend actually holds, so the migration can be asserted on bytes
 * rather than on the API agreeing with itself. */
static size_t mock_len_of(const char *a) {
    for (int i = 0; i < 8; i++)
        if (g_mock[i].used && strcmp(g_mock[i].account, a) == 0) return g_mock[i].len;
    return 0;
}
static int mock_ver_of(const char *a) {
    for (int i = 0; i < 8; i++)
        if (g_mock[i].used && strcmp(g_mock[i].account, a) == 0) return g_mock[i].val[0];
    return -1;
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

/* The message/outbox replay callbacks and the .log finder that used to live
 * here went with the client's SQLite store (ARCH-88): the API they fed no
 * longer exists, so they were five definitions nothing could call. Kept as a
 * note rather than as code, because dead scaffolding reads like coverage. */

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
    oc_store_save_session(s, "acme:443", tok, 0, "dana");
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
    oc_store_save_session(s, "acme:443", tok, 0, "dana");
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
/* Whose token is it? This is the assertion the first attempt at the fix could
 * not have passed: it compared against the BOOK's username, which the Win32
 * client writes with the account it is about to TRY, immediately after starting
 * the net thread (connect_start). So the book said "bob" while the token was
 * alice's, the comparison agreed, and the second client came up as the first
 * one's user. The token's account is written only with the token, by the code
 * that obtained it, and nothing a frontend does can move it. */
static void test_session_owner(void) {
    mock_reset();
    oc_secret sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
    oc_store *s = oc_store_open("ignored");
    CHECK(s != NULL);
    if (!s) return;
    oc_store_set_secret(s, &sec);

    uint8_t tok[OC_SESSION_TOKEN_LEN];
    for (unsigned i = 0; i < OC_SESSION_TOKEN_LEN; i++) tok[i] = (uint8_t)(0xA0 + i);
    char who[128];

    /* No token, no owner. */
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 0);

    oc_store_save_session(s, "acme:443", tok, 0, "alice");
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 1 && strcmp(who, "alice") == 0);

    /* Exactly what the GUI does at connect time, for an account that has not
     * authenticated and may never. The token is still alice's. */
    oc_store_workspace_remember(s, "acme:443", "acme:443", "bob", 1000);
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 1 && strcmp(who, "alice") == 0);

    /* A silent reconnect names nobody and must not blank the record. */
    oc_store_save_session(s, "acme:443", tok, 0, "");
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 1 && strcmp(who, "alice") == 0);

    /* Signing in as somebody else replaces both together. */
    oc_store_save_session(s, "acme:443", tok, 0, "bob");
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 1 && strcmp(who, "bob") == 0);

    /* The owner goes out with the token, and the pin stays. */
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    for (unsigned i = 0; i < OC_TLS_FINGERPRINT_LEN; i++) pin[i] = (uint8_t)(i + 7);
    oc_store_save_pin(s, "acme:443", pin);
    oc_store_clear_session(s, "acme:443");
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 0);
    CHECK(oc_store_load_session(s, "acme:443", tok, NULL, 0) == 0);
    uint8_t back[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_store_load_pin(s, "acme:443", back) == 1 && memcmp(back, pin, sizeof pin) == 0);
    oc_store_close(s);
}

/* An entry written by a client from before tokens recorded their account is read,
 * not discarded. Discarding it would take the TOFU pin with it, and a dropped pin
 * is a silent re-pin on the next connect (ARCH-10) — a downgrade in what the
 * client promises, paid by everyone who upgrades. */
static void test_store_legacy_entry(void) {
    mock_reset();
    oc_secret sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
    oc_store *s = oc_store_open("ignored");
    CHECK(s != NULL);
    if (!s) return;
    oc_store_set_secret(s, &sec);

    /* Version 1: [ver][flags][expiry][token 32][pin 32][last_used][label 128][user 128]. */
    enum { V1_LABEL = 128, V1_USER = 128,
           V1_BLOB = 2 + 8 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN + 8 + V1_LABEL + V1_USER };
    uint8_t v1[V1_BLOB];
    memset(v1, 0, sizeof v1);
    v1[0] = 1;
    v1[1] = 1 | 2 | 4;                                  /* token + pin + book */
    for (unsigned i = 0; i < OC_SESSION_TOKEN_LEN; i++) v1[10 + i] = (uint8_t)(i + 1);
    for (unsigned i = 0; i < OC_TLS_FINGERPRINT_LEN; i++)
        v1[10 + OC_SESSION_TOKEN_LEN + i] = (uint8_t)(0x40 + i);
    char *lbl = (char *)(v1 + 18 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN);
    snprintf(lbl, V1_LABEL, "%s", "acme.example.com");
    snprintf(lbl + V1_LABEL, V1_USER, "%s", "dana");
    CHECK(mock_put(NULL, "acme:443", v1, sizeof v1) == 1);

    /* Everything it held survives being read by this version... */
    uint8_t tok[OC_SESSION_TOKEN_LEN], pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_store_load_session(s, "acme:443", tok, NULL, 0) == 1 && tok[0] == 1);
    CHECK(oc_store_load_pin(s, "acme:443", pin) == 1 && pin[0] == 0x40);
    struct book_capture b; memset(&b, 0, sizeof b);
    oc_store_workspace_each(s, book_cb, &b);
    CHECK(b.n == 1 && strcmp(b.label[0], "acme.example.com") == 0 && strcmp(b.user[0], "dana") == 0);
    /* ...and the one thing it cannot hold reads as unknown, not as nobody. */
    char who[128];
    CHECK(oc_store_session_user(s, "acme:443", who, sizeof who) == 0);

    /* The next write of anything upgrades the entry in place, keeping the rest. */
    oc_store_save_pin(s, "acme:443", pin);
    CHECK(mock_len_of("acme:443") > (size_t)V1_BLOB && mock_ver_of("acme:443") == 3);
    CHECK(oc_store_load_session(s, "acme:443", tok, NULL, 0) == 1 && tok[0] == 1);
    memset(&b, 0, sizeof b);
    oc_store_workspace_each(s, book_cb, &b);
    CHECK(b.n == 1 && strcmp(b.user[0], "dana") == 0);
    oc_store_close(s);
}

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
        oc_store_save_session(s, "host:1", tok, 0, "dana");
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

/* Build a model with one channel and one other user, ready to be fed replies. */
static void notice_fixture(oc_model *m) {
    oc_model_init(m);
    m->user_id = 1;
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 1; e.body = strdup("alice"); oc_model_apply(m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER; e.user_id = 2; e.body = strdup("bob"); oc_model_apply(m, &e);
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL; e.channel_id = 10; e.status = 1; e.op = OC_CHANNEL_KIND;
    e.is_public = 1; e.body = strdup("general"); e.server_time = 10;
    oc_model_apply(m, &e);
}

static void feed_reply(oc_model *m, uint64_t message_id, uint64_t author,
                       int participant, const char *body) {
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_THREAD_REPLY;
    e.channel_id = 10; e.parent_id = 500; e.message_id = message_id;
    e.author_id = author; e.server_time = 100 + message_id; e.count = 1;
    e.participant = (uint8_t)participant;
    e.body = strdup(body);
    oc_model_apply(m, &e);
    free(e.body);
}

/* WHICH thread replies the client considers at all (REQ-061).
 *
 * The precedence order is test_push's, swept against the daemon over all 384
 * states. What is left — and what the client alone can get wrong — is the
 * gathering: a reply is not in any scroll the client keeps, so everything about
 * whether it is even a candidate lives here. */
static void test_thread_notices(void) {
    oc_thread_notice out[OC_MAX_THREAD_NOTICES];
    {   /* The daemon says it is not mine: nothing to consider. */
        oc_model m; notice_fixture(&m);
        feed_reply(&m, 1, 2, 0, "not your thread");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        oc_model_free(&m);
    }
    {   /* Mine, from someone else: a toast, and the body travels with it. */
        oc_model m; notice_fixture(&m);
        feed_reply(&m, 1, 2, 1, "here is the answer");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 1);
        CHECK(out[0].message_id == 1 && out[0].author_id == 2);
        CHECK(strcmp(out[0].body, "here is the answer") == 0);
        oc_model_free(&m);
    }
    {   /* My own reply, in my own thread. Your own words are not news, and the
         * model must not queue one at all — a later take cannot tell it apart. */
        oc_model m; notice_fixture(&m);
        feed_reply(&m, 1, 1, 1, "my own reply");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        oc_model_free(&m);
    }
    {   /* A LIST_THREAD replay. The frames are identical to live ones and carry
         * the same true participation byte, so only the in-flight mark tells
         * history from news — and the terminator ends it. */
        oc_model m; notice_fixture(&m);
        oc_model_open_thread(&m, 10, 500);
        feed_reply(&m, 1, 2, 1, "an old reply");
        feed_reply(&m, 2, 2, 1, "another old one");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        oc_ev e;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_THREAD_END; e.parent_id = 500; e.count = 2;
        oc_model_apply(&m, &e);
        feed_reply(&m, 3, 2, 1, "a live one");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 1);
        CHECK(out[0].message_id == 3);
        oc_model_free(&m);
    }
    {   /* Closing a thread clears the mark too. A replay that never arrives —
         * the request failed, or the user closed the pane first — would
         * otherwise silence every reply for the rest of the session. */
        oc_model m; notice_fixture(&m);
        oc_model_open_thread(&m, 10, 500);
        oc_model_close_thread(&m);
        feed_reply(&m, 1, 2, 1, "live after a closed thread");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 1);
        oc_model_free(&m);
    }
    {   /* Taking DRAINS: a reply is considered once, whatever the verdict, or a
         * timer that runs every tick would toast it every tick. */
        oc_model m; notice_fixture(&m);
        feed_reply(&m, 1, 2, 1, "once");
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 1);
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        /* Silenced by quiet hours, and still drained — it was considered. */
        feed_reply(&m, 2, 2, 1, "during quiet hours");
        CHECK(oc_model_thread_notify_take(&m, 1, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        CHECK(oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES) == 0);
        oc_model_free(&m);
    }
    {   /* The queue is bounded and drops the OLDEST: a burst costs the stalest
         * notice, never memory, and what survives is what just happened. */
        oc_model m; notice_fixture(&m);
        for (uint64_t i = 1; i <= OC_MAX_THREAD_NOTICES + 5; i++)
            feed_reply(&m, i, 2, 1, "burst");
        size_t n = oc_model_thread_notify_take(&m, 0, 0, out, OC_MAX_THREAD_NOTICES);
        CHECK(n == OC_MAX_THREAD_NOTICES);
        CHECK(out[n - 1].message_id == OC_MAX_THREAD_NOTICES + 5);
        CHECK(out[0].message_id == 6);
        oc_model_free(&m);
    }
    {   /* The Threads badge moves with the toast, rather than waiting for the
         * next LIST_THREADS to say so (REQ-062). */
        oc_model m; notice_fixture(&m);
        oc_ev e;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_THREAD_SUMMARY; e.message_id = 500; e.channel_id = 10;
        e.author_id = 2; e.server_time = 10; e.reply_count = 1; e.unread_count = 0;
        e.following = 1; e.body = strdup("the root");
        oc_model_apply(&m, &e);
        CHECK(oc_model_thread_unread(&m) == 0);
        feed_reply(&m, 1, 2, 1, "a reply");
        CHECK(oc_model_thread_unread(&m) == 1);
        oc_model_free(&m);
    }
}

/* What a channel's badge counts (REQ-284): the messages that would have
 * NOTIFIED, with the schedule and the pause left out. */
static void test_unread_counts_what_notifies(void) {
    {   /* On MENTIONS, only the one that names me. Before REQ-284 this counted
         * every message and the daemon counted something else again. */
        oc_model m; notice_fixture(&m);
        oc_channel *c = oc_model_channel(&m, 10);
        CHECK(c != NULL);
        if (c) c->notify_level = OC_NOTIFY_MENTIONS;
        oc_ev e;
        for (int i = 1; i <= 3; i++) {
            memset(&e, 0, sizeof e);
            e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = (uint64_t)i;
            e.author_id = 2; e.server_time = 100 + i;
            e.body = strdup(i == 2 ? "@alice look" : "chatter");
            oc_model_apply(&m, &e);
        }
        CHECK(channel_unread(&m, 10) == 1);
        oc_model_free(&m);
    }
    {   /* Muted counts nothing, whatever the level says. */
        oc_model m; notice_fixture(&m);
        oc_channel *c = oc_model_channel(&m, 10);
        if (c) { c->notify_level = OC_NOTIFY_ALL; c->muted = 1; }
        oc_ev e;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 1; e.author_id = 2;
        e.server_time = 100; e.body = strdup("@alice look"); oc_model_apply(&m, &e);
        CHECK(channel_unread(&m, 10) == 0);
        oc_model_free(&m);
    }
    {   /* A priority person pierces the level, exactly as they pierce a toast. */
        oc_model m; notice_fixture(&m);
        oc_channel *c = oc_model_channel(&m, 10);
        if (c) c->notify_level = OC_NOTIFY_MENTIONS;
        m.n_pri_people = 1; m.pri_people[0] = 2;
        oc_ev e;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 1; e.author_id = 2;
        e.server_time = 100; e.body = strdup("nothing special"); oc_model_apply(&m, &e);
        CHECK(channel_unread(&m, 10) == 1);
        oc_model_free(&m);
    }
    {   /* My own messages never badge, and NONE passes nothing. */
        oc_model m; notice_fixture(&m);
        oc_channel *c = oc_model_channel(&m, 10);
        if (c) c->notify_level = OC_NOTIFY_ALL;
        oc_ev e;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 1; e.author_id = 1;
        e.server_time = 100; e.body = strdup("mine"); oc_model_apply(&m, &e);
        CHECK(channel_unread(&m, 10) == 0);
        if (c) c->notify_level = OC_NOTIFY_NONE;
        memset(&e, 0, sizeof e);
        e.type = OC_EV_MESSAGE; e.channel_id = 10; e.message_id = 2; e.author_id = 2;
        e.server_time = 101; e.body = strdup("@alice look"); oc_model_apply(&m, &e);
        CHECK(channel_unread(&m, 10) == 0);
        oc_model_free(&m);
    }
    {   /* A thread reply badges the THREAD, never the channel: it is not in the
         * scroll, so a channel badge would point at something you cannot find
         * by opening it. */
        oc_model m; notice_fixture(&m);
        oc_channel *c = oc_model_channel(&m, 10);
        if (c) c->notify_level = OC_NOTIFY_ALL;
        feed_reply(&m, 1, 2, 1, "a reply");
        CHECK(channel_unread(&m, 10) == 0);
        oc_model_free(&m);
    }
}


/* Who the To: field in New message may address (REQ-229). The rules are not
 * decoration: each one here is a message that would otherwise be refused after
 * the composer had already been emptied, or a channel joined by picking a name
 * out of an address list. */
static void tgt_user(oc_model *m, uint64_t id, const char *name, int disabled,
                     const char *full_name, const char *title) {
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_USER;
    e.user_id = id;
    e.body = strdup(name);
    e.status = OC_ROLE_MEMBER;
    e.op = disabled;
    if (full_name) snprintf(e.pf_full_name, sizeof e.pf_full_name, "%s", full_name);
    if (title)     snprintf(e.pf_title, sizeof e.pf_title, "%s", title);
    oc_model_apply(m, &e);
    free(e.body);
}

static void tgt_channel(oc_model *m, uint64_t id, const char *name, int archived, int joined) {
    oc_ev e;
    memset(&e, 0, sizeof e);
    e.type = OC_EV_CHANNEL;
    e.channel_id = id;
    e.body = strdup(name);
    e.status = joined;
    e.op = 0;               /* an ordinary channel, not a DM */
    e.archived = (uint8_t)archived;
    oc_model_apply(m, &e);
    free(e.body);
}

static int tgt_has(const oc_model *m, const char *q, const char *name) {
    oc_target out[12];
    size_t n = oc_complete_targets(m, q, out, 12);
    for (size_t i = 0; i < n; i++)
        if (strcmp(out[i].name, name) == 0) return 1;
    return 0;
}

static void test_addressable_targets(void) {
    oc_model m; oc_model_init(&m);
    m.user_id = 1;
    tgt_user(&m, 1, "me", 0, NULL, NULL);
    tgt_user(&m, 2, "alice", 0, "Alice Aardvark", "Platform");
    tgt_user(&m, 3, "gene", 0, NULL, NULL);
    tgt_user(&m, 4, "ghost", 1, NULL, NULL);            /* removed by an admin */
    tgt_channel(&m, 10, "general", 0, 1);
    tgt_channel(&m, 11, "attic", 1, 1);                 /* archived: read-only */
    tgt_channel(&m, 12, "strangers", 0, 0);             /* public, never joined */

    CHECK(tgt_has(&m, "ali", "alice"));
    CHECK(!tgt_has(&m, "me", "me"));                    /* never yourself */
    CHECK(!tgt_has(&m, "gho", "ghost"));                /* a removed account */
    CHECK(tgt_has(&m, "gen", "general"));
    CHECK(!tgt_has(&m, "att", "attic"));                /* archived is read-only */
    CHECK(!tgt_has(&m, "str", "strangers"));            /* picking is not joining */

    /* The sigil SELECTS. "#gen" means the channel, not the person called gene,
     * which is what it means in the composer and everywhere else. */
    CHECK(tgt_has(&m, "#gen", "general"));
    CHECK(!tgt_has(&m, "#gen", "gene"));
    CHECK(tgt_has(&m, "@gen", "gene"));
    CHECK(!tgt_has(&m, "@gen", "general"));

    /* A colleague is findable by the name they are known by, and by the title the
     * row already prints beside it -- not only by the handle they log in with. */
    CHECK(tgt_has(&m, "Aardvark", "alice"));
    CHECK(tgt_has(&m, "Platform", "alice"));

    oc_model_free(&m);
}

/* ---- calls end to end (REQ-150, REQ-301-305, ARCH-113) -------------------------
 * A relay on a thread, as itest_netloop runs one, and a TAP in front of it: a UDP
 * forwarder the daemon advertises as the relay's port, with an upstream socket per
 * client so the relay still tells the clients apart. What passes through it is
 * what anyone on the network -- or the relay itself -- would see. */

static struct { int ipc, udp; volatile sig_atomic_t stop; } g_relay;
static pthread_t g_relay_th;
static void *relay_thread(void *p) {
    (void)p;
    oc_audio_sidecar_run(g_relay.ipc, g_relay.udp, &g_relay.stop);
    return NULL;
}

#define TAP_CLIENTS 8
#define TAP_KEEP    4000
typedef struct { uint64_t kid; uint8_t b0; size_t len; uint64_t t_ms; } tap_pkt;
static struct {
    int      front;                          /* where the clients send */
    uint16_t relay_port;
    struct { struct sockaddr_in from; int up; } cl[TAP_CLIENTS];
    int      n_cl;
    pthread_mutex_t mu;
    tap_pkt  seen[TAP_KEEP];                 /* client -> relay payloads that carried audio */
    int      n_seen, bad_header;
    volatile int stop;
} g_tap;
static pthread_t g_tap_th;

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *tap_thread(void *p) {
    (void)p;
    struct sockaddr_in relay;
    memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET;
    relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(g_tap.relay_port);
    uint8_t buf[2048];
    while (!g_tap.stop) {
        /* Only the sockets polled are looked at afterwards: a client first seen
         * in this pass has no revents yet, and reading one anyway is a blocking
         * recv on an empty socket. Every recv is non-blocking all the same. */
        struct pollfd pf[1 + TAP_CLIENTS];
        memset(pf, 0, sizeof pf);
        int polled = g_tap.n_cl;
        pf[0].fd = g_tap.front; pf[0].events = POLLIN;
        for (int i = 0; i < polled; i++) { pf[1 + i].fd = g_tap.cl[i].up; pf[1 + i].events = POLLIN; }
        if (poll(pf, (nfds_t)(1 + polled), 50) <= 0) continue;
        if (pf[0].revents & POLLIN) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(g_tap.front, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                int k = 0;
                while (k < g_tap.n_cl && (g_tap.cl[k].from.sin_port != from.sin_port ||
                                          g_tap.cl[k].from.sin_addr.s_addr != from.sin_addr.s_addr)) k++;
                if (k == g_tap.n_cl && k < TAP_CLIENTS) {
                    g_tap.cl[k].from = from;
                    g_tap.cl[k].up = socket(AF_INET, SOCK_DGRAM, 0);
                    g_tap.n_cl++;
                }
                if (k < g_tap.n_cl) {
                    /* token(16) seq(2) payload: record what the payload looks like. */
                    if (n > 18) {
                        uint64_t kid, ctr; size_t hl;
                        pthread_mutex_lock(&g_tap.mu);
                        if (oc_sframe_header_decode(buf + 18, (size_t)n - 18, &kid, &ctr, &hl) != 0) g_tap.bad_header++;
                        else if (g_tap.n_seen < TAP_KEEP && (size_t)n > 18 + hl)
                            g_tap.seen[g_tap.n_seen++] = (tap_pkt){ kid, buf[18 + hl], (size_t)n - 18, mono_ms() };
                        pthread_mutex_unlock(&g_tap.mu);
                    }
                    sendto(g_tap.cl[k].up, buf, (size_t)n, 0, (struct sockaddr *)&relay, sizeof relay);
                }
            }
        }
        for (int i = 0; i < polled; i++)
            if (pf[1 + i].revents & POLLIN) {
                ssize_t n = recv(g_tap.cl[i].up, buf, sizeof buf, MSG_DONTWAIT);
                if (n > 0) sendto(g_tap.front, buf, (size_t)n, 0, (struct sockaddr *)&g_tap.cl[i].from, sizeof g_tap.cl[i].from);
            }
    }
    return NULL;
}

/* A test's microphone and ears: a tone out, and how much of each tone came in. */
typedef struct {
    double freq, phase;
    pthread_mutex_t mu;
    double hear[3];              /* amplitude of each listened-for tone, last 0.5 s */
    double acc[3][25];
    int    at, frames;
} tone_io;
static const double LISTEN[3] = { 440.0, 660.0, 880.0 };

static void tone_source(void *ctx, int16_t *pcm, int n) {
    tone_io *t = ctx;
    for (int i = 0; i < n; i++) {
        pcm[i] = (int16_t)(6000.0 * sin(t->phase));
        t->phase += 2 * M_PI * t->freq / 16000.0;
        if (t->phase > 2 * M_PI) t->phase -= 2 * M_PI;
    }
}

/* Goertzel: the amplitude of one frequency in a frame, through a Hann window so
 * a loud tone 220 Hz away does not leak into the one being measured. */
static double goertzel(const int16_t *x, int n, double f) {
    double w = 2 * M_PI * f / 16000.0, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double h = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        double s = x[i] * h + c * s1 - s2; s2 = s1; s1 = s;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 4 * sqrt(p > 0 ? p : 0) / n;    /* x2 for the window's loss of gain */
}

static void tone_sink(void *ctx, const int16_t *pcm, int n) {
    tone_io *t = ctx;
    pthread_mutex_lock(&t->mu);
    for (int k = 0; k < 3; k++) {
        t->acc[k][t->at] = goertzel(pcm, n, LISTEN[k]);
        double sum = 0;
        for (int i = 0; i < 25; i++) sum += t->acc[k][i];
        t->hear[k] = sum / 25;
    }
    t->at = (t->at + 1) % 25;
    t->frames++;
    pthread_mutex_unlock(&t->mu);
}

static double heard(tone_io *t, int k) {
    pthread_mutex_lock(&t->mu);
    double v = t->hear[k];
    pthread_mutex_unlock(&t->mu);
    return v;
}

/* Wait up to `ms` for `cond`, ticking the three clients meanwhile. */
#define CALL_WAIT(ms, cond) ({ int _ok = 0; for (int _i = 0; _i < (ms) / 20; _i++) { \
        oc_client_tick(a); oc_client_tick(b); if (c) oc_client_tick(c); \
        if (cond) { _ok = 1; break; } usleep(20000); } _ok; })

static int calls_in(const oc_model *m, uint64_t ch) { return oc_model_call_in(m, ch) != NULL; }

static void test_calls_e2e(oc_client *a, oc_client *b, int port) {
    tone_io ta = { 440, 0, PTHREAD_MUTEX_INITIALIZER, {0}, {{0}}, 0, 0 };
    tone_io tb = { 660, 0, PTHREAD_MUTEX_INITIALIZER, {0}, {{0}}, 0, 0 };
    tone_io tc = { 880, 0, PTHREAD_MUTEX_INITIALIZER, {0}, {{0}}, 0, 0 };
    oc_call_engine_opts oa = { NULL, NULL, 0, tone_source, tone_sink, &ta };
    oc_call_engine_opts ob = { NULL, NULL, 0, tone_source, tone_sink, &tb };
    oc_call_engine_opts occ = { NULL, NULL, 0, tone_source, tone_sink, &tc };
    oc_call_engine *ea = oc_call_engine_new(&oa), *eb = oc_call_engine_new(&ob), *ec = oc_call_engine_new(&occ);
    oc_client_set_call_media(a, oc_call_engine_media(), ea);
    oc_client_set_call_media(b, oc_call_engine_media(), eb);
    oc_client *c = oc_client_start("127.0.0.1", port, "faye:pw-faye");
    CHECK(c != NULL);
    if (!c) { oc_call_engine_free(ea); oc_call_engine_free(eb); oc_call_engine_free(ec); return; }
    oc_client_set_call_media(c, oc_call_engine_media(), ec);
    CHECK(WAIT_FOR(c, m->authed && oc_model_channel((oc_model *)m, 1) != NULL));
    const oc_model *ma = oc_client_model(a), *mb = oc_client_model(b), *mc = oc_client_model(c);
    uint64_t ua = ma->user_id, ub = mb->user_id, uc = mc->user_id;
    oc_call_stats st;

    /* dana starts a call in channel 1, inviting erik: she is in it; erik sees it
     * in his Calls list as an invitation, and it is one worth a toast. */
    uint64_t inv[1] = { ub };
    oc_client_call_start(a, 1, inv, 1);
    CHECK(CALL_WAIT(3000, ma->in_call && ma->call.channel_id == 1 && ma->call.starter == ua));
    CHECK(CALL_WAIT(3000, calls_in(mb, 1) && oc_model_call_invited(oc_model_call_in(mb, 1), ub)));
    oc_call_notice nt[4];
    CHECK(oc_client_call_notify_take(b, 0, 0, nt, 4) == 1 && nt[0].channel_id == 1 && nt[0].starter == ua);
    CHECK(oc_client_call_notify_take(b, 0, 0, nt, 4) == 0);        /* considered once */

    /* erik joins: each hears the other's tone and not their own. */
    oc_client_call_join(b, 1);
    CHECK(CALL_WAIT(3000, mb->in_call && mb->call.n_parts == 2 && ma->call.n_parts == 2));
    CHECK(CALL_WAIT(6000, heard(&ta, 1) > 1500 && heard(&tb, 0) > 1500));
    /* Once settled, each at the level it was sent (6000), less what Opus and
     * the limiter take -- not a fraction of it, which would be frames lost. */
    CHECK(CALL_WAIT(4000, heard(&ta, 1) > 4800 && heard(&tb, 0) > 4800));
    printf("  two in the call: dana hears 660 Hz at %.0f, erik hears 440 Hz at %.0f\n", heard(&ta, 1), heard(&tb, 0));
    CHECK(heard(&ta, 0) < 300 && heard(&tb, 1) < 300);
    oc_call_engine_stats(ea, &st);
    CHECK(st.active && st.n_peers == 1 && st.peers[0].user_id == ub && st.peers[0].packets > 20);
    CHECK(st.peers[0].keyed);
    /* A joiner misses at most the grace (CALLS.md §5.3): the others go on with
     * the previous key for a second, which a joiner never gets. After that
     * nothing fails to decrypt. */
    {
        CHECK(CALL_WAIT(3000, 0) == 0);
        oc_call_engine_stats(eb, &st);
        uint32_t u1 = st.n_peers ? st.peers[0].undecryptable : 0, p1 = st.n_peers ? st.peers[0].packets : 0;
        CHECK(CALL_WAIT(1000, 0) == 0);
        oc_call_engine_stats(eb, &st);
        CHECK(st.n_peers == 1 && st.peers[0].undecryptable == u1 && st.peers[0].packets > p1 + 20);
        printf("  erik's first second: %u packets he could not decrypt, none since\n", u1);
    }

    /* faye joins: a new epoch, new keys all round, and everyone hears everyone. */
    uint32_t epoch2 = ma->call.epoch;
    oc_client_call_join(c, 1);
    CHECK(CALL_WAIT(3000, mc->in_call && ma->call.n_parts == 3 && mb->call.n_parts == 3));
    CHECK(ma->call.epoch > epoch2);
    CHECK(CALL_WAIT(8000, heard(&ta, 1) > 1000 && heard(&ta, 2) > 1000 && heard(&tb, 0) > 1000 &&
                           heard(&tb, 2) > 1000 && heard(&tc, 0) > 1000 && heard(&tc, 1) > 1000));
    CHECK(CALL_WAIT(4000, heard(&tc, 0) > 4800 && heard(&tc, 1) > 4800 && heard(&tb, 2) > 4800));
    printf("  three: dana hears 660 %.0f / 880 %.0f, faye hears 440 %.0f / 660 %.0f\n",
           heard(&ta, 1), heard(&ta, 2), heard(&tc, 0), heard(&tc, 1));

    /* faye leaves. Everyone after her epoch sends under keys she was never given:
     * the relay may forward to her or not, it makes no difference. */
    uint32_t her_last = mc->call.epoch;
    oc_client_call_leave(c, 1);
    CHECK(CALL_WAIT(3000, !mc->in_call && ma->call.n_parts == 2));
    CHECK(ma->call.epoch > her_last);
    uint64_t after_leave = mono_ms() + 1500;          /* past the 1 s grace */
    CHECK(CALL_WAIT(3000, mono_ms() > after_leave + 800));
    {
        int later = 0, newer = 0;
        pthread_mutex_lock(&g_tap.mu);
        for (int i = 0; i < g_tap.n_seen; i++)
            if (g_tap.seen[i].t_ms > after_leave) { later++; if ((g_tap.seen[i].kid >> 8) > her_last) newer++; }
        pthread_mutex_unlock(&g_tap.mu);
        CHECK(later > 20 && newer == later);
    }
    CHECK(CALL_WAIT(4000, heard(&ta, 2) < 300 && heard(&ta, 1) > 1000));    /* 880 Hz is gone */

    /* What the network carried was ciphertext: every audio payload an SFrame
     * header and then bytes that do not repeat. In the clear the first byte
     * after it would be the high byte of the frame number -- the same for every
     * packet of a call this short. */
    {
        int distinct = 0, seen_b[256] = {0};
        pthread_mutex_lock(&g_tap.mu);
        for (int i = 0; i < g_tap.n_seen; i++) if (!seen_b[g_tap.seen[i].b0]++) distinct++;
        int n = g_tap.n_seen, bad = g_tap.bad_header;
        pthread_mutex_unlock(&g_tap.mu);
        printf("  the relay saw %d audio packets: %d distinct leading bytes, %d without an SFrame header\n",
               n, distinct, bad);
        CHECK(n > 200 && distinct > 200 && bad == 0);
    }

    /* erik mutes: dana stops hearing him. Push to talk, held, lets him through. */
    oc_call_engine_set_mute(eb, 1);
    CHECK(CALL_WAIT(4000, heard(&ta, 1) < 300));
    /* ...and she can see that he is muted, which only the call's keys say. */
    CHECK(CALL_WAIT(3000, ({ oc_call_engine_stats(ea, &st); st.n_peers == 1 && st.peers[0].muted; })));
    oc_call_engine_set_ptt(eb, 1);
    CHECK(CALL_WAIT(4000, heard(&ta, 1) > 1000));
    oc_call_engine_set_ptt(eb, 0);
    oc_call_engine_set_mute(eb, 0);

    /* dana turns erik down to nothing: she hears nothing of him, and he is
     * still sending. */
    oc_call_engine_set_volume(ea, ub, 0.0f);
    CHECK(CALL_WAIT(4000, heard(&ta, 1) < 100));
    oc_call_engine_stats(eb, &st);
    CHECK(st.sent > 100 && !st.muted);
    oc_call_engine_set_volume(ea, ub, 1.0f);

    /* erik may not end it; dana, who started it, may -- for everyone. */
    uint32_t errs = mb->call_error_seq;
    oc_client_call_end(b, 1);
    CHECK(CALL_WAIT(3000, mb->call_error_seq != errs && mb->call_error == OC_ERR_NOT_CALL_STARTER));
    oc_client_call_end(a, 1);
    CHECK(CALL_WAIT(3000, !ma->in_call && !mb->in_call && !calls_in(ma, 1) && !calls_in(mb, 1)));
    oc_call_engine_stats(eb, &st);
    CHECK(!st.active);

    /* A missed call: dana starts one inviting faye, nobody comes, dana leaves.
     * faye's history gains a line that is a call event, not something dana said. */
    uint64_t inv_c[1] = { uc };
    oc_client_call_start(a, 1, inv_c, 1);
    CHECK(CALL_WAIT(3000, ma->in_call));
    CHECK(CALL_WAIT(3000, calls_in(mc, 1)));
    oc_client_call_leave(a, 1);
    CHECK(CALL_WAIT(3000, !ma->in_call && !calls_in(mc, 1)));
    int missed = 0;
    CHECK(CALL_WAIT(3000, ({
        const oc_channel *ch = oc_model_channel((oc_model *)mc, 1);
        missed = 0;
        for (size_t i = 0; ch && i < ch->n_msgs; i++)
            if (ch->msgs[i].kind == OC_MSG_KIND_CALL && ch->msgs[i].author_id == ua &&
                ch->msgs[i].body && strcmp(ch->msgs[i].body, "Missed call") == 0) missed = 1;
        missed; })));

    oc_client_set_call_media(a, NULL, NULL);
    oc_client_set_call_media(b, NULL, NULL);
    oc_client_stop(c);
    oc_call_engine_free(ea);
    oc_call_engine_free(eb);
    oc_call_engine_free(ec);
}

/* The device key (ARCH-113): made once and kept beside the token, the same one
 * read back, a version 2 entry upgraded in place, and forgotten with the
 * session. */
static void test_device_key(void) {
    mock_reset();
    oc_secret sec = { mock_get, mock_put, mock_del, mock_each, NULL, NULL };
    oc_store *s = oc_store_open("ignored");
    CHECK(s != NULL);
    if (!s) return;
    oc_store_set_secret(s, &sec);
    uint8_t sk[32], pk[32], sk2[32], pk2[32], pkc[32];
    CHECK(oc_store_device_key(s, "acme:443", sk, pk) == 1);
    CHECK(oc_x25519_public(sk, pkc) == 0 && memcmp(pkc, pk, 32) == 0);
    CHECK(oc_store_device_key(s, "acme:443", sk2, pk2) == 1);
    CHECK(memcmp(sk, sk2, 32) == 0 && memcmp(pk, pk2, 32) == 0);
    CHECK(mock_ver_of("acme:443") == 3);
    /* Another workspace, another key. */
    CHECK(oc_store_device_key(s, "other:443", sk2, pk2) == 1 && memcmp(pk, pk2, 32) != 0);
    /* Signing out forgets it: the next one is new. */
    uint8_t tok[OC_SESSION_TOKEN_LEN] = { 1 };
    oc_store_save_session(s, "acme:443", tok, 0, "dana");
    oc_store_clear_session(s, "acme:443");
    CHECK(oc_store_device_key(s, "acme:443", sk2, pk2) == 1 && memcmp(pk, pk2, 32) != 0);
    /* No credential store: a key for this session only. */
    oc_store *none = oc_store_open("ignored");
    CHECK(oc_store_device_key(none, "acme:443", sk2, pk2) == 0);
    oc_store_close(none);
    oc_store_close(s);
}

int run_client_core_tests(void) {
    printf("test_client_core: sidebar, resolve, last-error, secret-routing, connect+auth, channel-list, send round-trip, unread (what a badge counts), thread-reply notices, backfill, attachments, webhooks, client-settings, profile, seen-by, persisted store, v3 workspace upgrade, workspace book, cached history, session reconnect, offline outbox\n");

    test_group_dm_title();
    test_sidebar();
    test_new_channel_takes_the_default_level();
    test_notify_scan();
    test_thread_notices();
    test_unread_counts_what_notifies();
    test_capabilities();
    test_addressable_targets();
    test_pins();
    test_resolve();
    test_last_error();
    test_secret_routing();
    test_store_no_persistence_without_keyring();
    test_workspace_book();
    test_session_owner();
    test_store_legacy_entry();
    test_device_key();

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

    /* Pin the process timezone before any client connects, so the offset the
     * core sends on connect (ARCH-103) is a value that cannot be confused with
     * an unwritten column: +05:30 is non-zero AND not a whole hour, so neither
     * the schema default nor a truncating bug can produce it by accident. */
    const char *tz_saved = getenv("TZ");
    char tz_saved_buf[64] = "";
    if (tz_saved) snprintf(tz_saved_buf, sizeof tz_saved_buf, "%s", tz_saved);
    setenv("TZ", "Asia/Kolkata", 1);
    tzset();

    /* The daemon this test drives speaks with a stub voice (ARCH-111). */
    oc_netloop_set_tts(&CORE_TTS);
    oc_netloop_set_stt(&CORE_STT);

    /* A relay, and the tap in front of it that the daemon advertises (calls). */
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        g_relay.udp = socket(AF_INET, SOCK_DGRAM, 0);
        g_tap.front = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ra; memset(&ra, 0, sizeof ra);
        ra.sin_family = AF_INET; ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        struct sockaddr_in ta = ra;
        CHECK(bind(g_relay.udp, (struct sockaddr *)&ra, sizeof ra) == 0);
        CHECK(bind(g_tap.front, (struct sockaddr *)&ta, sizeof ta) == 0);
        socklen_t l = sizeof ra; getsockname(g_relay.udp, (struct sockaddr *)&ra, &l);
        l = sizeof ta; getsockname(g_tap.front, (struct sockaddr *)&ta, &l);
        g_tap.relay_port = ntohs(ra.sin_port);
        pthread_mutex_init(&g_tap.mu, NULL);
        g_relay.ipc = sv[1];
        g_relay.stop = 0;
        CHECK(pthread_create(&g_relay_th, NULL, relay_thread, NULL) == 0);
        CHECK(pthread_create(&g_tap_th, NULL, tap_thread, NULL) == 0);
        oc_netloop_set_audio(sv[0], ntohs(ta.sin_port));
    }

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

        /* The UTC offset is refreshed on connect (ARCH-103). It used to be
         * written only as a field on SET_SCHEDULE, so it was captured whenever
         * the user last edited their quiet hours and never again — and the
         * offset is precisely what changes without the schedule changing, when
         * the user travels or daylight saving turns over. Asserted against the
         * daemon's own row: the client says nothing about this, so the model
         * cannot be the witness. */
        {
            int expect = oc_utc_offset_min();
            CHECK(expect == 330);          /* Asia/Kolkata, per the TZ set above */
            int got = -9999;
            for (int i = 0; i < 200 && got != expect; i++) {   /* the job is async */
                sqlite3 *rdb = NULL;
                if (sqlite3_open_v2("build/itest_core.db", &rdb,
                                    SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
                    sqlite3_stmt *st = NULL;
                    /* Identity is `subject`, namespaced by source (AUTH.md §4);
                     * there is no username column. */
                    if (sqlite3_prepare_v2(rdb, "SELECT tz_offset_min FROM users"
                                                " WHERE subject='local:dana';",
                                           -1, &st, NULL) == SQLITE_OK &&
                        sqlite3_step(st) == SQLITE_ROW)
                        got = sqlite3_column_int(st, 0);
                    sqlite3_finalize(st);
                    sqlite3_close(rdb);
                }
                if (got != expect) usleep(10000);
            }
            CHECK(got == expect);
        }
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
        /* The SCHEDULE (REQ-136): a per-weekday one, so the round trip proves the
         * rows survive and not merely the two base integers. The window is the
         * ALLOWED range now — 07:00 to 22:00 is the working day. */
        { oc_schedule_day d[2] = { { 1, 1, 540, 1020 }, { 0, 0, 0, 0 } };  /* Mon 9-5, Sun off */
          oc_client_set_schedule(a, OC_DND_CUSTOM, -300, 420, 1320, d, 2); }
        CHECK(WAIT_FOR(a, m->dnd_mode == OC_DND_CUSTOM && m->allow_start_min == 420 &&
                          m->allow_end_min == 1320 && m->n_sched_days == 2 &&
                          m->tz_offset_min == -300));
        /* Stored — and returned — in WEEKDAY order, not the order they were sent:
         * a schedule is a week, and a client rendering seven rows should not have
         * to sort them or care how the last edit happened to be assembled. */
        { const oc_model *sm = oc_client_model(a);
          CHECK(sm->sched_days[0].weekday == 0 && sm->sched_days[0].enabled == 0);
          CHECK(sm->sched_days[1].weekday == 1 && sm->sched_days[1].start_min == 540); }
        /* Opening the prefs overlay re-syncs and the channel level survives. */
        oc_client_toggle_prefs(a, 1);
        CHECK(WAIT_FOR(a, m->prefs_open &&
                          oc_model_channel((oc_model *)m, 1)->notify_level == OC_NOTIFY_MENTIONS));
        /* Turning the schedule off keeps nothing behind — including the days,
         * which would otherwise apply again the moment it was re-enabled. */
        oc_client_set_schedule(a, OC_DND_OFF, -300, 0, 0, NULL, 0);
        CHECK(WAIT_FOR(a, m->dnd_mode == OC_DND_OFF && m->n_sched_days == 0));

        /* Keywords and priority people (REQ-135), and the client-side matcher
         * that has to agree with the server's — same list, same scanner. */
        { char terms[2][OC_KEYWORD_MAX] = { "deploy", "release train" };
          oc_client_set_keywords(a, terms, 2); }
        CHECK(WAIT_FOR(a, m->n_kw_terms == 2));
        { const oc_model *km = oc_client_model(a);
          CHECK(oc_model_keyword_hit(km, "ship the deploy now", 19, NULL, NULL));
          CHECK(oc_model_keyword_hit(km, "the release train leaves", 24, NULL, NULL));
          /* "deploy" must not match "deployment": a highlight-word that fires on
           * every longer word containing it is a word you turn off. */
          CHECK(!oc_model_keyword_hit(km, "the deployment is fine", 22, NULL, NULL)); }
        /* A real person: the daemon refuses an id that is nobody, so a test using
         * 4242 would pass on a list that silently stayed empty. */
        uint64_t vip_id = oc_model_user_id(oc_client_model(a), "erik");
        CHECK(vip_id != 0);
        { uint64_t vip[1] = { vip_id }; oc_client_set_priority(a, vip, 1); }
        CHECK(WAIT_FOR(a, m->n_pri_people == 1 && oc_model_is_priority(m, vip_id)));
        CHECK(!oc_model_is_priority(oc_client_model(a), 999999));
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

        /* video messages (REQ-162/165): dana asks for an earlier file's bytes and
         * posts a video message straight after, so the post has to wait its turn in
         * the transfer queue rather than be refused as busy; both complete. erik
         * sees the media facts and the summary line, fetches the 9 MiB video — past
         * the 8 MiB inline ceiling — with progress, and fetches the poster. A post
         * cancelled while queued never goes out. */
        {
            uint64_t earlier = channel_attach(oc_client_model(b), 1, (char[128]){0}, 128, NULL);
            CHECK(earlier != 0);
            const size_t VN = 9u * 1024u * 1024u, PN = 3000;
            uint8_t *video = malloc(VN), *poster = malloc(PN);
            uint8_t *vcopy = malloc(VN), *pcopy = malloc(PN);
            CHECK(video && poster && vcopy && pcopy);
            if (video && poster && vcopy && pcopy) {
                for (size_t k = 0; k < VN; k++) video[k] = (uint8_t)(k * 13u + (k >> 16));
                for (size_t k = 0; k < PN; k++) poster[k] = (uint8_t)(k * 5u + 1u);
                memcpy(vcopy, video, VN); memcpy(pcopy, poster, PN);

                oc_client_fetch_attachment(a, earlier);
                uint64_t tag = oc_client_post_video(a, 1, 0, video, VN, poster, PN,
                                                    65000, 1280, 720, "");
                video = poster = NULL;                          /* the core owns them */
                CHECK(tag != 0);
                int posted = 0;
                for (int t = 0; t < 6 && !posted; t++) posted = WAIT_FOR(a, m->media_posted_tag == tag);
                CHECK(posted);
                size_t elen = 0;
                uint8_t *ebytes = NULL;
                CHECK(WAIT_FOR(a, (ebytes = take_fetched(a, earlier, &elen)) != NULL));
                CHECK(elen == 150000);
                free(ebytes);

                const oc_msg *vm = NULL;
                CHECK(WAIT_FOR(b, (vm = channel_video(m, 1)) != NULL));
                vm = channel_video(oc_client_model(b), 1);
                if (vm) {
                    const oc_attachment *va = &vm->attach[0];
                    CHECK(va->duration_ms == 65000 && va->width == 1280 && va->height == 720);
                    CHECK(va->poster_id != 0 && va->size == VN);
                    CHECK(strcmp(va->mime, "video/mp4") == 0);
                    /* Named for whose and when: "dana-video-YYYYMMDDHHMMSS.mp4". */
                    CHECK(strncmp(va->filename, "dana-video-", 11) == 0 && strlen(va->filename) == 11 + 14 + 4 &&
                          strcmp(va->filename + 25, ".mp4") == 0);
                    char prev[96];
                    oc_model_msg_preview(vm, prev, sizeof prev);
                    CHECK(strcmp(prev, "\xF0\x9F\x8E\xA5 Video message (1:05)") == 0);

                    uint64_t vid = va->id, pid = va->poster_id;
                    CHECK(oc_client_fetch_media(b, vid) == vid);
                    size_t vlen = 0;
                    uint8_t *vbytes = NULL;
                    for (int t = 0; t < 6 && !vbytes; t++)
                        WAIT_FOR(b, (vbytes = take_fetched(b, vid, &vlen)) != NULL);
                    CHECK(vbytes && vlen == VN && memcmp(vbytes, vcopy, VN) == 0);
                    free(vbytes);
                    CHECK(oc_client_model(b)->xfer_tag == vid &&
                          oc_client_model(b)->xfer_done == VN && oc_client_model(b)->xfer_total == VN);

                    oc_client_fetch_attachment(b, pid);
                    size_t plen = 0;
                    uint8_t *pbytes = NULL;
                    CHECK(WAIT_FOR(b, (pbytes = take_fetched(b, pid, &plen)) != NULL));
                    CHECK(pbytes && plen == PN && memcmp(pbytes, pcopy, PN) == 0);
                    free(pbytes);
                }

                /* Cancelled while queued behind another post: only the other lands. */
                uint8_t *v2 = malloc(2 * 1024 * 1024), *p2 = malloc(100);
                uint8_t *v3 = malloc(1000), *p3 = malloc(100);
                if (v2 && p2 && v3 && p3) {
                    memset(v2, 2, 2 * 1024 * 1024); memset(p2, 2, 100);
                    memset(v3, 3, 1000); memset(p3, 3, 100);
                    uint64_t t2 = oc_client_post_video(a, 1, 0, v2, 2 * 1024 * 1024, p2, 100, 1000, 640, 360, "first");
                    uint64_t t3 = oc_client_post_video(a, 1, 0, v3, 1000, p3, 100, 1000, 640, 360, "second");
                    oc_client_cancel_transfer(a, t2);
                    int second = 0;
                    for (int t = 0; t < 6 && !second; t++) second = WAIT_FOR(a, m->media_posted_tag == t3);
                    CHECK(second);
                    CHECK(WAIT_FOR(b, channel_has_body(m, 1, "second")));
                    CHECK(!channel_has_body(oc_client_model(b), 1, "first"));
                    CHECK(channel_video_count(oc_client_model(b), 1) == 2);
                } else {
                    free(v2); free(p2); free(v3); free(p3);
                }
            }
            free(video); free(poster); free(vcopy); free(pcopy);
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
        /* Talking mode (REQ-291-295, ARCH-111): the daemon's voices arrive with
         * the connection, messages that arrive while it is on are queued and
         * their speech fetched in order, and what has nothing to say is skipped
         * without stalling the queue. */
        {
            const oc_model *ma = oc_client_model(a);
            CHECK(WAIT_FOR(a, oc_model_tts_available(m)));
            CHECK(oc_model_tts_voice_count(ma) == 2);
            CHECK(strcmp(oc_model_tts_voice(ma, 0)->id, "core-voice-m") == 0);
            CHECK(strcmp(oc_model_tts_voice(ma, 1)->label, "Core High") == 0);
            CHECK(strcmp(oc_model_tts_preview(ma), "This is a test voice.") == 0);
            CHECK(strcmp(oc_model_tts_voice_label(ma, "core-voice-f"), "Core High") == 0);
            CHECK(strcmp(oc_model_tts_voice_label(ma, "nope"), "") == 0);

            /* Off by default: what was said before the toggle is not read. */
            CHECK(oc_model_listening_channel(ma) == 0);
            oc_client_send(b, 1, "Said before anyone was listening.");
            CHECK(WAIT_FOR(a, channel_has_body(m, 1, "Said before anyone was listening.")));
            CHECK(oc_model_listen_queued(ma) == 0);

            oc_client_listen(a, 1, 1);
            CHECK(oc_model_listening_channel(ma) == 1);

            /* From now on: the speech of what arrives, in order. */
            oc_client_send(b, 1, "The first thing to be read aloud.");
            uint8_t *mp4 = NULL;
            size_t mlen = 0;
            uint64_t spoken = 0;
            for (int t = 0; t < 8 && !spoken; t++)
                WAIT_FOR(a, (spoken = oc_model_listen_take_audio((oc_model *)m, &mp4, &mlen)) != 0);
            CHECK(spoken != 0 && mp4 != NULL && mlen > 0);
            if (mp4) {
                /* What arrives is playable: an audio-only Opus MP4. */
                oc_mp4_info info;
                CHECK(oc_mp4_parse(mp4, mlen, &info) == 0);
                CHECK(!info.video.present && info.audio.present && info.duration_ms > 0);
                oc_mp4_info_free(&info);
                free(mp4);
                mp4 = NULL;
            }
            CHECK(oc_model_listen_playing(ma) == spoken);

            /* The next message waits its turn while that one is playing. */
            oc_client_send(b, 1, "The second thing to be read aloud.");
            CHECK(WAIT_FOR(a, m->n_listen_queue == 1));
            oc_client_listen_done(a);                       /* the frontend finished playing */
            uint64_t second = 0;
            for (int t = 0; t < 8 && !second; t++)
                WAIT_FOR(a, (second = oc_model_listen_take_audio((oc_model *)m, &mp4, &mlen)) != 0);
            CHECK(second != 0 && second != spoken);
            free(mp4);
            mp4 = NULL;
            oc_client_listen_done(a);

            /* What the listener types is not read back to them. They know what
             * they just said, and the queue plays end to end, so it would land
             * on top of the next person's reply. */
            oc_client_send(a, 1, "Typed by the listener, not read aloud.");
            CHECK(WAIT_FOR(a, channel_has_body(m, 1, "Typed by the listener, not read aloud.")));
            /* Asserted on what is OFFERED TO PLAY, not on the queue: an empty
             * queue proves nothing here, because the pump takes the only entry
             * out of it the moment nothing else is playing. */
            uint64_t mine = 0;
            for (int t = 0; t < 4 && !mine; t++)
                WAIT_FOR(a, (mine = oc_model_listen_take_audio((oc_model *)m, &mp4, &mlen)) != 0);
            CHECK(mine == 0);
            free(mp4);
            mp4 = NULL;

            /* A message with nothing to say is passed over, and the one after it
             * is still spoken: the queue does not stall on it (REQ-294). */
            uint32_t skipped_before = oc_model_listen_skipped(ma);
            oc_client_send(b, 1, "\xF0\x9F\x8E\x89");
            oc_client_send(b, 1, "And on to the next one.");
            uint64_t third = 0;
            for (int t = 0; t < 8 && !third; t++)
                WAIT_FOR(a, (third = oc_model_listen_take_audio((oc_model *)m, &mp4, &mlen)) != 0);
            CHECK(third != 0);
            CHECK(oc_model_listen_skipped(ma) > skipped_before);
            free(mp4);
            mp4 = NULL;
            oc_client_listen_done(a);

            /* Turning it off forgets the queue; a later message is not spoken. */
            oc_client_listen(a, 1, 0);
            CHECK(oc_model_listening_channel(ma) == 0 && oc_model_listen_queued(ma) == 0);
            oc_client_send(b, 1, "Nobody is listening to this one.");
            CHECK(WAIT_FOR(a, channel_has_body(m, 1, "Nobody is listening to this one.")));
            uint8_t *none = NULL;
            size_t nlen = 0;
            CHECK(oc_model_listen_take_audio((oc_model *)ma, &none, &nlen) == 0);
        }

        /* Voice input (REQ-296-300, ARCH-112), with the stub recognizer. */
        {
            const oc_model *ma = oc_client_model(a);
            CHECK(WAIT_FOR(a, oc_model_stt_available(m)));
            CHECK(ma->stt_max_ms == 30000 && strcmp(ma->stt_lang, "en-US") == 0);

            /* Push to talk: the words come back for the composer they were
             * spoken into, with the spoken mention made a mention, and nothing
             * is posted. */
            size_t before_b = 0;
            stt_say(a, OC_STT_MODE_PTT, 1, 1, 5);
            char *words = NULL;
            CHECK(WAIT_FOR(a, oc_model_stt_take_words((oc_model *)m, 1, 0, &words)));
            CHECK(words && strcmp(words, "spoken 5 @erik") == 0);
            free(words);
            words = NULL;
            CHECK(!channel_has_body(oc_client_model(b), 1, "spoken 5 @erik"));
            (void)before_b;

            /* Words for another conversation wait for it. */
            stt_say(a, OC_STT_MODE_PTT, 2, 1, 6);
            uint32_t answered = ma->stt_answered;
            CHECK(WAIT_FOR(a, m->stt_answered > answered));
            CHECK(!oc_model_stt_take_words((oc_model *)ma, 1, 0, &words));
            CHECK(oc_model_stt_take_words((oc_model *)ma, 2, 0, &words) && strcmp(words, "spoken 6 @erik") == 0);
            free(words);
            words = NULL;

            /* Free talk: each segment is posted by the daemon, in order, as
             * dana; erik reads them as ordinary messages. The client never sent
             * a SEND for them. */
            stt_say(a, OC_STT_MODE_FREE, 1, 1, 7);
            stt_say(a, OC_STT_MODE_FREE, 1, 1, 8);
            CHECK(WAIT_FOR(b, channel_has_body(m, 1, "spoken 7 @erik") && channel_has_body(m, 1, "spoken 8 @erik")));
            CHECK(WAIT_FOR(a, channel_has_body(m, 1, "spoken 8 @erik")));
            {
                const oc_model *mb = oc_client_model(b);
                uint64_t id7 = 0, id8 = 0, author = 0;
                for (size_t i = 0; i < mb->n_channels; i++) {
                    if (mb->channels[i].channel_id != 1) continue;
                    for (size_t j = 0; j < mb->channels[i].n_msgs; j++) {
                        const char *body = mb->channels[i].msgs[j].body;
                        if (body && !strcmp(body, "spoken 7 @erik")) { id7 = mb->channels[i].msgs[j].message_id; author = mb->channels[i].msgs[j].author_id; }
                        if (body && !strcmp(body, "spoken 8 @erik")) id8 = mb->channels[i].msgs[j].message_id;
                    }
                }
                CHECK(id7 != 0 && id8 > id7);               /* in the order spoken */
                CHECK(author == ma->user_id);               /* as the speaker */
            }

            /* A refusal reaches the model as one, with its reason. */
            uint32_t errs = ma->stt_error_seq;
            stt_say(a, OC_STT_MODE_PTT, 1, 3, 0);
            CHECK(WAIT_FOR(a, m->stt_error_seq > errs));
            CHECK(ma->stt_error_code == OC_ERR_STT_UNAVAILABLE);

            /* The microphone has one owner: while dictation holds it, nothing
             * else can open it, and stopping gives it back. */
            setenv("OPENCHIME_TEST_AUDIO", "synthetic", 1);
            int derr = 0;
            oc_dictate *dict = oc_dictate_start(a, OC_STT_MODE_FREE, 1, 0, NULL, ma->stt_max_ms, &derr);
            CHECK(dict != NULL && derr == OC_DICTATE_OK);
            int aerr = 0;
            oc_audio_dev *other = oc_audio_capture_open(NULL, 48000, 1, &aerr);
            CHECK(other == NULL && aerr == OC_AUDIO_BUSY);
            oc_dictate *second = oc_dictate_start(a, OC_STT_MODE_PTT, 1, 0, NULL, ma->stt_max_ms, &derr);
            CHECK(second == NULL && derr == OC_DICTATE_BUSY);
            oc_dictate_stop(dict, 0);
            other = oc_audio_capture_open(NULL, 48000, 1, &aerr);
            CHECK(other != NULL);
            oc_audio_close(other);
            unsetenv("OPENCHIME_TEST_AUDIO");
        }

        test_calls_e2e(a, b, arg.port);

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
            /* Wrong password, but the stored token authenticates it anyway —
             * for the SAME account, which is whose token it is. */
            uint64_t faye_id = 0;
            oc_client *s2 = oc_client_start_secure("127.0.0.1", arg.port, "faye:WRONG-pw",
                                                   sp, &store_sec);
            CHECK(s2 != NULL);
            if (s2) {
                CHECK(WAIT_FOR(s2, m->authed && m->user_id != 0));
                faye_id = oc_client_model(s2)->user_id;
                oc_client_stop(s2);
            }

            /* ...and signing in as SOMEBODY ELSE at the same address is that
             * somebody else. A workspace holds one credential, so faye's token
             * is sitting right there; riding in on it would authenticate gil as
             * faye, with the credential he gave never consulted — two accounts on
             * one machine, and the second one is the first one.
             *
             * The book is written first, exactly as the Win32 client writes it in
             * connect_start: the account it is ABOUT to try, before there is an
             * answer. Without this line the test passes against a fix that reads
             * the book instead of the token's own account — which is how the
             * first attempt at this fix shipped and failed on the real client. */
            {
                oc_store *poison = oc_store_open(sp);
                if (poison) {
                    oc_store_set_secret(poison, &store_sec);
                    oc_store_workspace_remember(poison, inst, inst, "gil",
                                                (uint64_t)time(NULL) * 1000);
                    oc_store_close(poison);
                }
            }
            oc_client *s3 = oc_client_start_secure("127.0.0.1", arg.port, "gil:pw-gil",
                                                   sp, &store_sec);
            CHECK(s3 != NULL);
            if (s3) {
                CHECK(WAIT_FOR(s3, m->authed && m->user_id != 0));
                uint64_t gil_id = oc_client_model(s3)->user_id;
                CHECK(faye_id != 0 && gil_id != 0 && gil_id != faye_id);
                oc_client_stop(s3);
            }

            /* The token now belongs to gil, so his relaunch is still silent: a
             * wrong password rides in on it exactly as faye's did. */
            oc_client *s4 = oc_client_start_secure("127.0.0.1", arg.port, "gil:WRONG-pw",
                                                   sp, &store_sec);
            CHECK(s4 != NULL);
            if (s4) {
                CHECK(WAIT_FOR(s4, m->authed && m->user_id != 0));
                CHECK(oc_client_model(s4)->user_id != faye_id);
                oc_client_stop(s4);
            }

            /* An entry with a token but no recorded account — what an older
             * client left behind — authenticates with the password. Proved by a
             * WRONG password FAILING: "the right password works" would be true
             * either way and proves nothing about which credential was used. */
            {
                oc_store *legacy = oc_store_open(sp);
                if (legacy) {
                    oc_store_set_secret(legacy, &store_sec);
                    uint8_t tok[OC_SESSION_TOKEN_LEN];
                    if (oc_store_load_session(legacy, inst, tok, NULL, 0)) {
                        oc_store_clear_session(legacy, inst);           /* drops the account */
                        oc_store_save_session(legacy, inst, tok, 0, ""); /* token, owner unknown */
                    }
                    char who[64];
                    CHECK(oc_store_session_user(legacy, inst, who, sizeof who) == 0);
                    oc_store_close(legacy);
                }
                oc_client *sl = oc_client_start_secure("127.0.0.1", arg.port, "gil:WRONG-pw",
                                                       sp, &store_sec);
                CHECK(sl != NULL);
                if (sl) {
                    int authed = 0;
                    for (int i = 0; i < 200 && !authed; i++) { oc_client_tick(sl); authed = oc_client_model(sl)->authed; usleep(10000); }
                    CHECK(!authed);
                    oc_client_stop(sl);
                }
            }

            /* And faye, whose token was replaced, authenticates with her
             * password rather than as gil. */
            oc_client *s5 = oc_client_start_secure("127.0.0.1", arg.port, "faye:pw-faye",
                                                   sp, &store_sec);
            CHECK(s5 != NULL);
            if (s5) {
                CHECK(WAIT_FOR(s5, m->authed && m->user_id != 0));
                CHECK(oc_client_model(s5)->user_id == faye_id);
                oc_client_stop(s5);
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
    oc_netloop_set_audio(-1, 0);
    g_relay.stop = 1;
    pthread_join(g_relay_th, NULL);
    g_tap.stop = 1;
    pthread_join(g_tap_th, NULL);
    close(g_relay.udp);
    close(g_tap.front);
    for (int i = 0; i < g_tap.n_cl; i++) close(g_tap.cl[i].up);
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    unlink("build/itest_core.db");
    unlink("build/itest_core.db-wal");
    unlink("build/itest_core.db-shm");

    /* Put the process timezone back: it is global state, and a later suite
     * reading a local time should not inherit this one's fixture. */
    if (tz_saved_buf[0]) setenv("TZ", tz_saved_buf, 1);
    else                 unsetenv("TZ");
    tzset();

    return failures;
}
