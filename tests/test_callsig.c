/* Getting back into a call (callsig.h, PROTOCOL.md §5.17): what the call
 * signalling does when this device is put out of a call by something other than
 * its own act, driven frame by frame with a fake engine and a fake clock. */

#include "callsig.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>

#define CH   7u
#define CALL 1234u
#define ME   42u
#define BOB  43u

/* The frames callsig wrote, by type. */
static int g_sent[0x200];
static int fake_write(void *wctx, const uint8_t *buf, size_t len) {
    (void)wctx;
    oc_header hdr; oc_rbuf p;
    if (oc_parse_frame(buf, len, &hdr, &p) == OC_OK && hdr.msg_type < 0x200) g_sent[hdr.msg_type]++;
    return 0;
}

/* The engine: starts, stops, and says its path is lost when told to. */
static int g_started, g_stopped, g_path_lost;
static int f_start(void *c, const char *h, uint16_t port, const uint8_t *t, size_t tl, uint64_t u, uint8_t s) {
    (void)c; (void)h; (void)port; (void)t; (void)tl; (void)u; (void)s;
    g_started++;
    g_path_lost = 0;
    return 0;
}
static void f_roster(void *c, uint32_t e, const oc_call_part *p, int n) { (void)c; (void)e; (void)p; (void)n; }
static void f_tx(void *c, uint32_t e, uint8_t s, const uint8_t *k) { (void)c; (void)e; (void)s; (void)k; }
static void f_rx(void *c, uint64_t u, uint8_t s, uint32_t e, const uint8_t *k) { (void)c; (void)u; (void)s; (void)e; (void)k; }
static void f_stop(void *c) { (void)c; g_stopped++; }
static int  f_lost(void *c) { (void)c; return g_path_lost; }
static const oc_call_media FAKE = { f_start, f_roster, f_tx, f_rx, f_stop, NULL, 0, NULL, NULL, f_lost };

/* The events callsig raised since the last look, by type; freed. */
static int g_ev[OC_EV_CALL_LOST + 1];
static void drain(oc_queue *q) {
    memset(g_ev, 0, sizeof g_ev);
    oc_ev *e;
    while ((e = oc_queue_try_pop(q)) != NULL) {
        if (e->type >= 0 && e->type <= OC_EV_CALL_LOST) g_ev[e->type]++;
        oc_ev_free(e);
    }
}

static void feed(oc_callsig *cs, oc_queue *q, const uint8_t *buf, size_t len) {
    oc_header hdr; oc_rbuf p;
    CHECK(oc_parse_frame(buf, len, &hdr, &p) == OC_OK);
    CHECK(oc_callsig_frame(cs, hdr.msg_type, &p, "relay", fake_write, NULL, q) == 1);
}

/* A part: `user` at `slot` with a device key of bytes `key` (0 = this device's). */
static oc_call_part part(const oc_callsig *cs, uint64_t user, uint8_t slot, uint8_t key) {
    oc_call_part pt; memset(&pt, 0, sizeof pt);
    pt.user_id = user;
    pt.slot = slot;
    if (key) memset(pt.device_key, key, OC_CALL_DEVICE_KEY_LEN);
    else memcpy(pt.device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN);
    return pt;
}

static void joined(oc_callsig *cs, oc_queue *q, uint32_t epoch) {
    oc_call_part parts[2] = { part(cs, ME, 0, 0), part(cs, BOB, 1, 0xB0) };
    static const uint8_t tok[16];
    oc_call_joined jd = { CH, CALL, 9999, { tok, sizeof tok }, 0, epoch, ME, 1, 2, parts };
    uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_joined(&w, OC_PROTOCOL_VERSION, &jd) == OC_OK);
    feed(cs, q, buf, w.len);
}

static void roster(oc_callsig *cs, oc_queue *q, const oc_call_part *parts, uint16_t n, uint32_t epoch) {
    oc_call_roster ro = { CH, CALL, epoch, n, (oc_call_part *)parts };
    uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_roster(&w, OC_PROTOCOL_VERSION, &ro) == OC_OK);
    feed(cs, q, buf, w.len);
}

static void state(oc_callsig *cs, oc_queue *q, uint64_t call_id, int ended) {
    uint64_t parts[2] = { ME, BOB };
    oc_call_state st = { CH, call_id, ME, 1, (uint8_t)ended, (uint16_t)(ended ? 0 : 2), parts, 0, NULL, 0 };
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_state(&w, OC_PROTOCOL_VERSION, &st) == OC_OK);
    feed(cs, q, buf, w.len);
}

/* A fresh callsig in the call, at time `t`. */
static void setup(oc_callsig *cs, oc_queue *q, uint64_t t) {
    oc_callsig_init(cs);
    cs->have_key = 1;
    memset(cs->pk, 0xA0, sizeof cs->pk);
    oc_callsig_set_media(cs, &FAKE, NULL);
    memset(g_sent, 0, sizeof g_sent);
    g_started = g_stopped = g_path_lost = 0;
    oc_callsig_tick(cs, t, fake_write, NULL, q);
    joined(cs, q, 2);
    drain(q);
    CHECK(cs->in_call && g_started == 1);
}

static void test_reconnect(void) {
    oc_callsig cs; oc_queue q; oc_queue_init(&q);

    /* The connection goes: media stops, the call is being got back into --
     * not left -- and nothing is sent while there is no connection. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    drain(&q);
    CHECK(!cs.in_call && g_stopped == 1);
    CHECK(g_ev[OC_EV_CALL_REJOINING] == 1 && g_ev[OC_EV_CALL_LEFT] == 0);
    oc_callsig_tick(&cs, 3000, NULL, NULL, &q);
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 0);

    /* Signed in again, and the daemon still reports the call: one join for it,
     * and its CALL_JOINED is the way back in. */
    oc_callsig_authed(&cs, 4000);
    state(&cs, &q, CALL, 0);
    state(&cs, &q, CALL, 0);                 /* told twice, joined once */
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 1);
    joined(&cs, &q, 3);
    drain(&q);
    CHECK(cs.in_call && !cs.rj.on && g_started == 2 && g_ev[OC_EV_CALL_JOINED] == 1);
    oc_callsig_destroy(&cs);

    /* The call ended meanwhile: given up, no join. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_authed(&cs, 3000);
    state(&cs, &q, CALL, 1);
    drain(&q);
    CHECK(g_ev[OC_EV_CALL_LOST] == 1 && !cs.rj.on && g_sent[OC_MSG_CALL_JOIN] == 0);
    oc_callsig_destroy(&cs);

    /* A new call in its place is not the one being got back into. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_authed(&cs, 3000);
    state(&cs, &q, CALL + 1, 0);
    drain(&q);
    CHECK(g_ev[OC_EV_CALL_LOST] == 1 && g_sent[OC_MSG_CALL_JOIN] == 0);
    oc_callsig_destroy(&cs);

    /* ...nor is an old one ending: that changes nothing. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_authed(&cs, 3000);
    state(&cs, &q, CALL - 1, 1);
    drain(&q);
    CHECK(cs.rj.on && g_ev[OC_EV_CALL_LOST] == 0);
    oc_callsig_destroy(&cs);

    /* Signed in, and the call never reported: over, given up at the wait. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_authed(&cs, 3000);
    oc_callsig_tick(&cs, 3000 + OC_CALLSIG_STATE_WAIT_MS - 1, fake_write, NULL, &q);
    CHECK(cs.rj.on);
    oc_callsig_tick(&cs, 3000 + OC_CALLSIG_STATE_WAIT_MS, fake_write, NULL, &q);
    drain(&q);
    CHECK(!cs.rj.on && g_ev[OC_EV_CALL_LOST] == 1);
    oc_callsig_destroy(&cs);

    /* No connection for the whole window: given up, even with none to send on. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_tick(&cs, 2000 + OC_CALLSIG_REJOIN_MS, NULL, NULL, &q);
    drain(&q);
    CHECK(!cs.rj.on && g_ev[OC_EV_CALL_LOST] == 1);
    oc_callsig_destroy(&cs);

    /* The rejoin refused (the call filled meanwhile): given up. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    oc_callsig_authed(&cs, 3000);
    state(&cs, &q, CALL, 0);
    oc_callsig_refused(&cs, &q);
    drain(&q);
    CHECK(!cs.rj.on && g_ev[OC_EV_CALL_LOST] == 1);
    oc_callsig_destroy(&cs);

    /* Leaving while getting back in: left, and the call reported afterwards is
     * not joined. */
    setup(&cs, &q, 1000);
    oc_callsig_lost(&cs, &q, 2000);
    { oc_cmd c; memset(&c, 0, sizeof c); c.type = OC_CMD_CALL_LEAVE; c.channel_id = CH;
      CHECK(oc_callsig_command(&cs, &c, NULL, NULL, fake_write, NULL, &q) == 0); }
    oc_callsig_authed(&cs, 3000);
    state(&cs, &q, CALL, 0);
    drain(&q);
    CHECK(!cs.rj.on && g_ev[OC_EV_CALL_LEFT] == 1 && g_sent[OC_MSG_CALL_JOIN] == 0);
    oc_callsig_destroy(&cs);

    /* Closing on purpose: a leave goes out, and nothing is got back into. */
    setup(&cs, &q, 1000);
    oc_callsig_quit(&cs, fake_write, NULL, &q);
    oc_callsig_lost(&cs, &q, 2000);
    drain(&q);
    CHECK(g_sent[OC_MSG_CALL_LEAVE] == 1 && !cs.rj.on && g_ev[OC_EV_CALL_LEFT] == 1);
    CHECK(g_ev[OC_EV_CALL_REJOINING] == 0);
    oc_callsig_destroy(&cs);
    oc_queue_destroy(&q);
}

static void test_in_session(void) {
    oc_callsig cs; oc_queue q; oc_queue_init(&q);

    /* A roster without this device but with this user on another: it moved
     * there. Out, and no rejoin. */
    setup(&cs, &q, 1000);
    { oc_call_part parts[2] = { part(&cs, ME, 2, 0xEE), part(&cs, BOB, 1, 0xB0) };
      roster(&cs, &q, parts, 2, 3); }
    drain(&q);
    CHECK(!cs.in_call && !cs.rj.on && g_ev[OC_EV_CALL_LEFT] == 1 && g_sent[OC_MSG_CALL_JOIN] == 0);
    oc_callsig_destroy(&cs);

    /* A roster without this user at all: swept -- its path went. Straight back
     * in, on the connection it has. */
    setup(&cs, &q, 1000);
    { oc_call_part parts[1] = { part(&cs, BOB, 1, 0xB0) };
      roster(&cs, &q, parts, 1, 3); }
    drain(&q);
    CHECK(g_ev[OC_EV_CALL_REJOINING] == 1 && g_sent[OC_MSG_CALL_JOIN] == 1 && g_stopped == 1);
    joined(&cs, &q, 4);
    CHECK(cs.in_call && !cs.rj.on);
    oc_callsig_destroy(&cs);

    /* The engine says its path is lost: back in for a fresh token, at once. */
    setup(&cs, &q, 1000);
    g_path_lost = 1;
    oc_callsig_tick(&cs, 2000, fake_write, NULL, &q);
    drain(&q);
    CHECK(g_ev[OC_EV_CALL_REJOINING] == 1 && g_sent[OC_MSG_CALL_JOIN] == 1 && g_stopped == 1);
    joined(&cs, &q, 3);
    CHECK(cs.in_call && g_started == 2 && !g_path_lost);
    oc_callsig_tick(&cs, 2100, fake_write, NULL, &q);
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 1);    /* a fresh path is not lost */

    /* A path that keeps failing: three rejoins in the window, then given up. */
    g_path_lost = 1; oc_callsig_tick(&cs, 3000, fake_write, NULL, &q); joined(&cs, &q, 4);
    g_path_lost = 1; oc_callsig_tick(&cs, 4000, fake_write, NULL, &q); joined(&cs, &q, 5);
    drain(&q);
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 3 && cs.in_call);
    g_path_lost = 1; oc_callsig_tick(&cs, 5000, fake_write, NULL, &q);
    drain(&q);
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 3 && !cs.in_call && !cs.rj.on && g_ev[OC_EV_CALL_LOST] == 1);
    /* ...the window counts from the first: once it has passed, one more is allowed. */
    joined(&cs, &q, 6);
    g_path_lost = 1; oc_callsig_tick(&cs, 2000 + OC_CALLSIG_REJOIN_MS, fake_write, NULL, &q);
    CHECK(g_sent[OC_MSG_CALL_JOIN] == 4);
    oc_callsig_destroy(&cs);
    oc_queue_destroy(&q);
}

int run_callsig_tests(void) {
    printf("test_callsig: getting back into a call -- after a reconnect, a sweep or a moved path; "
           "given up when the call is over, unreported, refused, or rejoined too often; not after a leave, "
           "a quit or a move to another device\n");
    test_reconnect();
    test_in_session();
    return failures;
}
