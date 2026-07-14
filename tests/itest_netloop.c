/* Integration test for the event loop (netloop.c) end to end: TLS handshake,
 * HELLO->WELCOME, version REJECT, and the message vertical — two clients
 * authenticate, one SENDs, and both receive the BROADCAST while the sender is
 * acked. Runs the non-blocking epoll server (with a real DB-writer thread) in a
 * thread and drives it with blocking TLS clients. */

#include "netloop.h"
#include "dbwriter.h"
#include "framebuf.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

struct loop_arg {
    int                   port;
    oc_tls_server        *srv;
    oc_dbwriter          *dbw;
    volatile sig_atomic_t stop;
};

static void *loop_thread(void *p) {
    struct loop_arg *a = (struct loop_arg *)p;
    oc_netloop_run(a->port, a->srv, a->dbw, &a->stop);
    return NULL;
}

/* A connected + TLS-handshaked client. */
typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; } client;

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

static int client_open(client *c, int port, const uint8_t *pin) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    c->fd = -1;
    for (int i = 0; i < 200; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) { c->fd = fd; break; }
        close(fd);
        usleep(20000);
    }
    if (c->fd < 0) return -1;
    if (oc_tls_client_init(&c->cli, pin) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
}

static void client_close(client *c) {
    oc_framebuf_free(&c->fb);
    oc_tls_conn_free(&c->conn);
    oc_tls_client_free(&c->cli);
    close(c->fd);
}

static int write_all(oc_tls_conn *c, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        if (oc_tls_write(c, buf + sent, len - sent, &n) != OC_TLS_OK) return -1;
        sent += n;
    }
    return 0;
}

static int read_frame(client *c, oc_header *hdr, oc_rbuf *payload) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r == 1) return oc_parse_frame(frame, flen, hdr, payload) == OC_OK ? 0 : -1;
        if (r < 0) return -1;
        uint8_t buf[4096]; size_t n = 0;
        if (oc_tls_read(&c->conn, buf, sizeof buf, &n) != OC_TLS_OK) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

static int send_hello(client *c, uint16_t mn, uint16_t mx) {
    uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_hello h = { mn, mx, oc_slice_str("itest") };
    if (oc_encode_hello(&w, &h) != OC_OK) return -1;
    return write_all(&c->conn, buf, w.len);
}

static int do_handshake(client *c) {
    if (send_hello(c, 1, 1) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) return -1;
    oc_welcome wel;
    if (oc_decode_welcome(&p, &wel) != OC_OK) return -1;
    /* The daemon follows WELCOME with AUTH_CHALLENGE advertising its methods. */
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_CHALLENGE) return -1;
    oc_auth_challenge ch;
    if (oc_decode_auth_challenge(&p, &ch) != OC_OK) return -1;
    return (ch.methods & OC_AUTH_LOCAL) ? 0 : -1;
}

static int do_auth(client *c, const char *user, const char *pass, uint64_t *user_id) {
    uint8_t cbuf[256]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass)) != OC_OK) return -1;
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != 0) return -1;
    if (write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    *user_id = ok.user_id;
    return 0;
}

static void test_version_reject(int port, const uint8_t *pin) {
    client c;
    CHECK(client_open(&c, port, pin) == 0);
    CHECK(send_hello(&c, 5, 5) == 0);            /* too new */
    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(&c, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_REJECT);
    oc_reject rej;
    CHECK(oc_decode_reject(&p, &rej) == OC_OK);
    CHECK(rej.code == OC_ERR_VERSION_TOO_NEW);
    client_close(&c);
}

static void test_message_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);

    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(ua != 0 && ub != 0 && ua != ub);

    /* alice sends to the default channel. */
    uint8_t idem[OC_IDEM_SIZE];
    memset(idem, 0x7E, sizeof idem);
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s;
    s.channel_id = 1;
    memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str("hello everyone");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(write_all(&a.conn, buf, w.len) == 0);

    /* alice receives SEND_ACK then the BROADCAST (order not guaranteed between
     * the two, but both must arrive); read two frames and classify. */
    uint64_t acked_id = 0, bcast_id_a = 0;
    for (int i = 0; i < 2; i++) {
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK);
            CHECK(memcmp(ack.idem, idem, OC_IDEM_SIZE) == 0);
            acked_id = ack.message_id;
        } else if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
            CHECK(bc.author_id == ua);
            CHECK(bc.body.len == 14 && memcmp(bc.body.ptr, "hello everyone", 14) == 0);
            bcast_id_a = bc.message_id;
        } else {
            CHECK(0 /* unexpected frame to sender */);
        }
    }
    CHECK(acked_id != 0 && acked_id == bcast_id_a);

    /* bob receives the same BROADCAST. */
    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(&b, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_BROADCAST);
    oc_broadcast bcb;
    CHECK(oc_decode_broadcast(&p, &bcb) == OC_OK);
    CHECK(bcb.message_id == acked_id);
    CHECK(bcb.author_id == ua);
    CHECK(bcb.body.len == 14 && memcmp(bcb.body.ptr, "hello everyone", 14) == 0);

    client_close(&a);
    client_close(&b);
}

/* A reconnecting client backfills messages it missed (REQ-101). */
static void test_backfill_reconnect(int port, const uint8_t *pin) {
    /* A sends a distinctive message and learns its id. */
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "bf-sender", "pw", &ua) == 0);

    uint8_t idem[OC_IDEM_SIZE];
    memset(idem, 0x5C, sizeof idem);
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s;
    s.channel_id = 1;
    memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str("backfill me");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(write_all(&a.conn, buf, w.len) == 0);

    uint64_t mx = 0;
    for (int i = 0; i < 2; i++) {          /* SEND_ACK + own BROADCAST */
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK);
            mx = ack.message_id;
        }
    }
    CHECK(mx != 0);

    /* A fresh connection (a reconnect) authenticates and backfills from 0. */
    client c;
    CHECK(client_open(&c, port, pin) == 0);
    CHECK(do_handshake(&c) == 0);
    uint64_t uc = 0;
    CHECK(do_auth(&c, "bf-reader", "pw", &uc) == 0);

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { 1, 0 };
    oc_backfill_request req = { 1, &cur };
    CHECK(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK);
    CHECK(write_all(&c.conn, buf, w.len) == 0);

    /* Replayed BROADCASTs (ascending id) then a BACKFILL_DONE; mx must appear. */
    int saw_mx = 0;
    uint64_t hw = 0, prev = 0;
    for (int i = 0; i < 1000; i++) {
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&c, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast b; CHECK(oc_decode_broadcast(&p, &b) == OC_OK);
            CHECK(b.message_id > prev);     /* ascending order */
            prev = b.message_id;
            if (b.message_id == mx) {
                saw_mx = 1;
                CHECK(b.body.len == 11 && memcmp(b.body.ptr, "backfill me", 11) == 0);
            }
        } else if (hdr.msg_type == OC_MSG_BACKFILL_DONE) {
            oc_backfill_done d; CHECK(oc_decode_backfill_done(&p, &d) == OC_OK);
            hw = d.high_water;
            break;
        } else {
            CHECK(0 /* unexpected frame during backfill */);
            break;
        }
    }
    CHECK(saw_mx == 1);
    CHECK(hw >= mx);

    client_close(&a);
    client_close(&c);
}

/* Edit + delete over the wire (REQ-032/051/052): the author edits, the fan-out
 * reaches every member, an owner moderator-deletes another user's message, and a
 * follow-up edit on the tombstone is refused with a non-fatal ERROR. */
static void test_edit_delete_vertical(int port, const uint8_t *pin) {
    client a, b;   /* a = owner (moderator), b = author */
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);   /* owner */
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);        /* member */

    /* bob sends; learn the id from his SEND_ACK, and drain both members' BROADCAST. */
    uint8_t idem[OC_IDEM_SIZE]; memset(idem, 0x3D, sizeof idem);
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s; s.channel_id = 1; memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str("first draft");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(write_all(&b.conn, buf, w.len) == 0);

    uint64_t mid = 0;
    for (int i = 0; i < 2; i++) {
        oc_header hdr; oc_rbuf p; CHECK(read_frame(&b, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK);
            mid = ack.message_id;
        }
    }
    CHECK(mid != 0);
    { oc_header hdr; oc_rbuf p; CHECK(read_frame(&a, &hdr, &p) == 0);
      CHECK(hdr.msg_type == OC_MSG_BROADCAST); }

    /* bob edits his own message -> both members receive MSG_EDITED, new body. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_edit e = { 1, mid, oc_slice_str("second draft") };
    CHECK(oc_encode_edit(&w, OC_PROTOCOL_VERSION, &e) == OC_OK);
    CHECK(write_all(&b.conn, buf, w.len) == 0);
    for (int who = 0; who < 2; who++) {
        client *cc = who ? &a : &b;
        oc_header hdr; oc_rbuf p; CHECK(read_frame(cc, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_MSG_EDITED);
        oc_msg_edited m; CHECK(oc_decode_msg_edited(&p, &m) == OC_OK);
        CHECK(m.message_id == mid && m.author_id == ub);
        CHECK(m.body.len == 12 && memcmp(m.body.ptr, "second draft", 12) == 0);
    }

    /* alice (owner) moderator-deletes bob's message -> both get MSG_DELETED with
     * deleted_by == alice, author == bob (self vs moderator, REQ-032). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_delete d = { 1, mid };
    CHECK(oc_encode_delete(&w, OC_PROTOCOL_VERSION, &d) == OC_OK);
    CHECK(write_all(&a.conn, buf, w.len) == 0);
    for (int who = 0; who < 2; who++) {
        client *cc = who ? &a : &b;
        oc_header hdr; oc_rbuf p; CHECK(read_frame(cc, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_MSG_DELETED);
        oc_msg_deleted m; CHECK(oc_decode_msg_deleted(&p, &m) == OC_OK);
        CHECK(m.message_id == mid && m.author_id == ub && m.deleted_by == ua);
    }

    /* A follow-up edit on the tombstone is refused with a non-fatal ERROR whose
     * context echoes the offending message id (8 bytes, big-endian). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_edit e2 = { 1, mid, oc_slice_str("third") };
    CHECK(oc_encode_edit(&w, OC_PROTOCOL_VERSION, &e2) == OC_OK);
    CHECK(write_all(&b.conn, buf, w.len) == 0);
    { oc_header hdr; oc_rbuf p; CHECK(read_frame(&b, &hdr, &p) == 0);
      CHECK(hdr.msg_type == OC_MSG_ERROR);
      oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK);
      CHECK(er.code == OC_ERR_UNKNOWN_MESSAGE && er.fatal == 0);
      CHECK(er.context.len == 8);
      uint64_t ctx = 0; for (int i = 0; i < 8; i++) ctx = (ctx << 8) | er.context.ptr[i];
      CHECK(ctx == mid);
    }

    client_close(&a);
    client_close(&b);
}

static int send_frame(client *c, const uint8_t *buf, size_t len) {
    return write_all(&c->conn, buf, len);
}

/* Channel management over the wire (REQ-031/033/050): create a private channel,
 * a non-member is refused, an invite grants access + pushes CHANNEL_INFO to the
 * invitee, both members then exchange a BROADCAST, and a third user never sees
 * the private channel in LIST_CHANNELS. */
static void test_channels_vertical(int port, const uint8_t *pin) {
    client a, b, cc;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(client_open(&cc, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    CHECK(do_handshake(&cc) == 0);
    uint64_t ua = 0, ub = 0, uc = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(do_auth(&cc, "carol", "pw", &uc) == 0);

    uint8_t buf[256]; oc_wbuf w;
    oc_header hdr; oc_rbuf p;

    /* alice creates a private channel and auto-joins. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cch = { oc_slice_str("war-room"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cch) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info info; CHECK(oc_decode_channel_info(&p, &info) == OC_OK);
    CHECK(info.is_public == 0 && info.joined == 1);
    uint64_t cid = info.channel_id;

    /* bob is not a member: his SEND is refused with a non-fatal NOT_A_MEMBER. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s1; s1.channel_id = cid; memset(s1.idem, 0x11, OC_IDEM_SIZE);
    s1.body = oc_slice_str("intruding");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s1) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK);
    CHECK(er.code == OC_ERR_NOT_A_MEMBER && er.fatal == 0);

    /* alice invites bob: alice gets a CHANNEL_INFO ack, bob gets a pushed one. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_channel_member_op inv = { cid, ub };
    CHECK(oc_encode_invite_to_channel(&w, OC_PROTOCOL_VERSION, &inv) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info pushed; CHECK(oc_decode_channel_info(&p, &pushed) == OC_OK);
    CHECK(pushed.channel_id == cid && pushed.joined == 1);

    /* Now bob posts to the private channel; both members receive the BROADCAST. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s2; s2.channel_id = cid; memset(s2.idem, 0x22, OC_IDEM_SIZE);
    s2.body = oc_slice_str("war plans");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s2) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    uint64_t bmid = 0;
    for (int i = 0; i < 2; i++) {            /* bob: SEND_ACK + own BROADCAST */
        CHECK(read_frame(&b, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) { oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK); bmid = ack.message_id; }
    }
    CHECK(bmid != 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);
    oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
    CHECK(bc.channel_id == cid && bc.author_id == ub && bc.message_id == bmid);

    /* carol (a non-member) never sees the private channel in LIST_CHANNELS. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(send_frame(&cc, buf, w.len) == 0);
    CHECK(read_frame(&cc, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_LIST);
    oc_channel_list_entry ents[64]; uint16_t n = 0;
    CHECK(oc_decode_channel_list(&p, ents, 64, &n) == OC_OK);
    int saw_private = 0, saw_general = 0;
    for (uint16_t i = 0; i < n && i < 64; i++) {
        if (ents[i].channel_id == cid) saw_private = 1;
        if (ents[i].channel_id == 1)   saw_general = 1;
    }
    CHECK(saw_general == 1 && saw_private == 0);

    client_close(&a);
    client_close(&b);
    client_close(&cc);
}

/* Emoji reactions over the wire (REQ-070/071): two members react to a message,
 * each REACT fans a REACTION_UPDATED with the running aggregate to every member,
 * LIST_REACTIONS returns the reactor rows, and a remove toggles the count down. */
static void test_reactions_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    uint8_t buf[256]; oc_wbuf w; oc_header hdr; oc_rbuf p;

    /* alice posts; learn the id, drain both members' BROADCAST. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s; s.channel_id = 1; memset(s.idem, 0x5B, OC_IDEM_SIZE);
    s.body = oc_slice_str("react to this");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint64_t mid = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) { oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK); mid = ack.message_id; }
    }
    CHECK(mid != 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);

    /* alice adds :+1: -> both members receive REACTION_UPDATED, count 1. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_react rc = { 1, mid, oc_slice_str(":+1:"), OC_REACT_ADD };
    CHECK(oc_encode_react(&w, OC_PROTOCOL_VERSION, &rc) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    for (int who = 0; who < 2; who++) {
        client *cc = who ? &a : &b;
        CHECK(read_frame(cc, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_REACTION_UPDATED);
        oc_reaction_updated ru; CHECK(oc_decode_reaction_updated(&p, &ru) == OC_OK);
        CHECK(ru.message_id == mid && ru.user_id == ua && ru.op == OC_REACT_ADD && ru.count == 1);
        CHECK(ru.emoji.len == 4 && memcmp(ru.emoji.ptr, ":+1:", 4) == 0);
    }

    /* bob adds the same emoji -> count 2 for both. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_react rc2 = { 1, mid, oc_slice_str(":+1:"), OC_REACT_ADD };
    CHECK(oc_encode_react(&w, OC_PROTOCOL_VERSION, &rc2) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    for (int who = 0; who < 2; who++) {
        client *cc = who ? &a : &b;
        CHECK(read_frame(cc, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_REACTION_UPDATED);
        oc_reaction_updated ru; CHECK(oc_decode_reaction_updated(&p, &ru) == OC_OK);
        CHECK(ru.count == 2 && ru.user_id == ub);
    }

    /* LIST_REACTIONS returns both reactor rows. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_list_reactions lr = { 1, mid };
    CHECK(oc_encode_list_reactions(&w, OC_PROTOCOL_VERSION, &lr) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_REACTIONS);
    oc_reaction_entry ents[8]; uint16_t n = 0; uint64_t lmid = 0;
    CHECK(oc_decode_reactions(&p, ents, 8, &n, &lmid) == OC_OK);
    CHECK(lmid == mid && n == 2);

    /* bob toggles off -> count 1. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_react rc3 = { 1, mid, oc_slice_str(":+1:"), OC_REACT_REMOVE };
    CHECK(oc_encode_react(&w, OC_PROTOCOL_VERSION, &rc3) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    for (int who = 0; who < 2; who++) {
        client *cc = who ? &a : &b;
        CHECK(read_frame(cc, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_REACTION_UPDATED);
        oc_reaction_updated ru; CHECK(oc_decode_reaction_updated(&p, &ru) == OC_OK);
        CHECK(ru.op == OC_REACT_REMOVE && ru.count == 1);
    }

    client_close(&a);
    client_close(&b);
}

/* Threads over the wire (REQ-060): a reply is delivered as THREAD_REPLY (never a
 * BROADCAST in the main scroll), LIST_THREAD streams the replies + a THREAD
 * terminator, and a reconnect backfill carries a THREAD_META for the parent. */
static void test_threads_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    uint8_t buf[256]; oc_wbuf w; oc_header hdr; oc_rbuf p;

    /* alice posts a top-level message; learn its id, drain both BROADCASTs. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s; s.channel_id = 1; memset(s.idem, 0x71, OC_IDEM_SIZE);
    s.body = oc_slice_str("thread root");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint64_t mid = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) { oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK); mid = ack.message_id; }
    }
    CHECK(mid != 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);

    /* bob replies: he gets a SEND_ACK, and both members get a THREAD_REPLY (not
     * a BROADCAST — the reply stays out of the main scroll). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send_reply sr; sr.channel_id = 1; memset(sr.idem, 0x72, OC_IDEM_SIZE);
    sr.parent_id = mid; sr.body = oc_slice_str("first reply");
    CHECK(oc_encode_send_reply(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    for (int i = 0; i < 2; i++) {          /* bob: SEND_ACK + THREAD_REPLY */
        CHECK(read_frame(&b, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_SEND_ACK || hdr.msg_type == OC_MSG_THREAD_REPLY);
    }
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    oc_thread_reply tr; CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK);
    CHECK(tr.parent_id == mid && tr.author_id == ub && tr.reply_count == 1);
    CHECK(tr.body.len == 11 && memcmp(tr.body.ptr, "first reply", 11) == 0);

    /* alice opens the thread: the reply is streamed, then a THREAD terminator. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_list_thread lt = { 1, mid };
    CHECK(oc_encode_list_thread(&w, OC_PROTOCOL_VERSION, &lt) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK && tr.parent_id == mid);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD);
    oc_thread th; CHECK(oc_decode_thread(&p, &th) == OC_OK);
    CHECK(th.parent_id == mid && th.count == 1);

    /* A reconnecting member backfills the channel: the parent replays as a
     * BROADCAST followed by a THREAD_META carrying its reply count. */
    client c;
    CHECK(client_open(&c, port, pin) == 0);
    CHECK(do_handshake(&c) == 0);
    uint64_t uc = 0;
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { 1, 0 };
    oc_backfill_request req = { 1, &cur };
    CHECK(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK);
    CHECK(send_frame(&c, buf, w.len) == 0);
    int saw_meta = 0;
    for (int i = 0; i < 3000; i++) {
        CHECK(read_frame(&c, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_BROADCAST) { oc_broadcast bx; CHECK(oc_decode_broadcast(&p, &bx) == OC_OK); }
        else if (hdr.msg_type == OC_MSG_THREAD_META) {
            oc_thread_meta tm; CHECK(oc_decode_thread_meta(&p, &tm) == OC_OK);
            if (tm.message_id == mid) { saw_meta = 1; CHECK(tm.reply_count == 1); }
        } else if (hdr.msg_type == OC_MSG_BACKFILL_DONE) break;
        else CHECK(0 /* unexpected frame during backfill */);
    }
    CHECK(saw_meta == 1);

    client_close(&a);
    client_close(&b);
    client_close(&c);
}

/* Tenant admin ops over the wire (REQ-033): an owner mints an invite, a fresh
 * client redeems it to create + authenticate an account, the owner promotes that
 * user (SET_ROLE pushes USER_UPDATED to them), lists users, and removes a member
 * (whose connection is then dropped). */
static void test_admin_vertical(int port, const uint8_t *pin) {
    client owner;
    CHECK(client_open(&owner, port, pin) == 0);
    CHECK(do_handshake(&owner) == 0);
    uint64_t uo = 0;
    CHECK(do_auth(&owner, "alice", "pw-alice", &uo) == 0);   /* alice is owner */

    uint8_t buf[512]; oc_wbuf w; oc_header hdr; oc_rbuf p;

    /* Owner mints a member invite and receives the token. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_invite_user iu = { OC_ROLE_MEMBER };
    CHECK(oc_encode_invite_user(&w, OC_PROTOCOL_VERSION, &iu) == OC_OK);
    CHECK(send_frame(&owner, buf, w.len) == 0);
    CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_INVITE_CREATED);
    oc_invite_created ic; CHECK(oc_decode_invite_created(&p, &ic) == OC_OK);
    CHECK(ic.token.len == OC_INVITE_TOKEN_LEN && ic.role == OC_ROLE_MEMBER);
    uint8_t token[OC_INVITE_TOKEN_LEN]; memcpy(token, ic.token.ptr, OC_INVITE_TOKEN_LEN);

    /* A fresh client redeems the invite: pre-auth account creation -> AUTH_OK. */
    client nh;
    CHECK(client_open(&nh, port, pin) == 0);
    CHECK(do_handshake(&nh) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_redeem_invite ri = { { token, OC_INVITE_TOKEN_LEN }, oc_slice_str("newhire"), oc_slice_str("nhpw") };
    CHECK(oc_encode_redeem_invite(&w, OC_PROTOCOL_VERSION, &ri) == OC_OK);
    CHECK(send_frame(&nh, buf, w.len) == 0);
    CHECK(read_frame(&nh, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_AUTH_OK);
    oc_auth_ok ok; CHECK(oc_decode_auth_ok(&p, &ok) == OC_OK);
    uint64_t unh = ok.user_id;
    CHECK(unh != 0 && ok.role == OC_ROLE_MEMBER);

    /* Owner promotes the new hire to admin: owner acks, the hire is pushed the
     * new role on their live connection. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_role sr = { unh, OC_ROLE_ADMIN };
    CHECK(oc_encode_set_role(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK);
    CHECK(send_frame(&owner, buf, w.len) == 0);
    CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_USER_UPDATED);
    oc_user_updated uu; CHECK(oc_decode_user_updated(&p, &uu) == OC_OK);
    CHECK(uu.user_id == unh && uu.role == OC_ROLE_ADMIN);
    CHECK(read_frame(&nh, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_USER_UPDATED);
    CHECK(oc_decode_user_updated(&p, &uu) == OC_OK && uu.user_id == unh && uu.role == OC_ROLE_ADMIN);

    /* LIST_USERS returns the roster including the owner. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_users(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(send_frame(&owner, buf, w.len) == 0);
    CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_USER_LIST);
    oc_user_list_entry ents[64]; uint16_t n = 0;
    CHECK(oc_decode_user_list(&p, ents, 64, &n) == OC_OK);
    int saw_owner = 0;
    for (uint16_t i = 0; i < n && i < 64; i++)
        if (ents[i].user_id == uo) { saw_owner = 1; CHECK(ents[i].role == OC_ROLE_OWNER); }
    CHECK(saw_owner == 1);

    /* Owner removes a connected member (bob); bob's connection is dropped. */
    client bob;
    CHECK(client_open(&bob, port, pin) == 0);
    CHECK(do_handshake(&bob) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&bob, "bob", "pw-bob", &ub) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_remove_user ru = { ub };
    CHECK(oc_encode_remove_user(&w, OC_PROTOCOL_VERSION, &ru) == OC_OK);
    CHECK(send_frame(&owner, buf, w.len) == 0);
    CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_USER_UPDATED);
    CHECK(oc_decode_user_updated(&p, &uu) == OC_OK && uu.user_id == ub && uu.disabled == 1);
    /* bob receives the notice and then the connection closes. */
    int bob_closed = 0;
    for (int i = 0; i < 4; i++) {
        if (read_frame(&bob, &hdr, &p) != 0) { bob_closed = 1; break; }
    }
    CHECK(bob_closed == 1);

    client_close(&owner);
    client_close(&nh);
    client_close(&bob);
}

/* LOGOUT over the wire: the daemon revokes the session and drops the
 * connection (REQ-182). */
static void test_logout_closes(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_logout lo = { OC_LOGOUT_ALL, { NULL, 0 } };
    CHECK(oc_encode_logout(&w, OC_PROTOCOL_VERSION, &lo) == OC_OK);
    CHECK(write_all(&a.conn, buf, w.len) == 0);

    /* No reply frame — the server closes the connection after revoking. */
    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(&a, &hdr, &p) != 0);
    client_close(&a);
}

int run_netloop_tests(void) {
    printf("itest_netloop: handshake, version REJECT, two-client AUTH+SEND+BROADCAST, backfill, edit/delete, channels, reactions, threads, admin, logout\n");

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    unlink("build/itest_netloop.db");
    unlink("build/itest_netloop.db-wal");
    unlink("build/itest_netloop.db-shm");
    oc_dbwriter *dbw = oc_dbwriter_start("build/itest_netloop.db");
    CHECK(dbw != NULL);

    /* Provision the accounts the clients log in as, before the loop serves
     * traffic (register runs on the writer thread; no live consumer yet). */
    CHECK(oc_dbwriter_register_local(dbw, "alice",     "pw-alice", OC_ROLE_OWNER,  2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "bob",       "pw-bob",   OC_ROLE_MEMBER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "bf-sender", "pw",       OC_ROLE_MEMBER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "bf-reader", "pw",       OC_ROLE_MEMBER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "carol",     "pw",       OC_ROLE_MEMBER, 2048) != 0);

    struct loop_arg arg;
    arg.port = 18000 + (int)(getpid() % 2000);
    arg.srv = &srv;
    arg.dbw = dbw;
    arg.stop = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, loop_thread, &arg) == 0);

    if (failures == 0) {
        test_version_reject(arg.port, pin);
        test_message_vertical(arg.port, pin);
        test_backfill_reconnect(arg.port, pin);
        test_edit_delete_vertical(arg.port, pin);
        test_channels_vertical(arg.port, pin);
        test_reactions_vertical(arg.port, pin);
        test_threads_vertical(arg.port, pin);
        test_admin_vertical(arg.port, pin);
        test_logout_closes(arg.port, pin);
    }

    arg.stop = 1;
    pthread_join(th, NULL);
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    unlink("build/itest_netloop.db");
    unlink("build/itest_netloop.db-wal");
    unlink("build/itest_netloop.db-shm");

    return failures;
}
