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
#include <sys/socket.h>
#include <stdlib.h>
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

/* Open a TLS connection that does NOT offer the oc/1 ALPN, so the daemon routes
 * it to the HTTP handler (ARCH-54) — used to drive the webhook endpoint. */
static int http_client_open(client *c, int port, const uint8_t *pin) {
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
    if (oc_tls_client_init_ex(&c->cli, pin, 0) != 0) return -1;   /* no ALPN */
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
}

/* Read the full HTTP response (until the server closes) into `buf`. */
static size_t http_read_response(client *c, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap - 1) {
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, (uint8_t *)buf + total, cap - 1 - total, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        if (n == 0) break;                 /* closed */
        total += n;
        if (st != OC_TLS_OK) break;
    }
    buf[total] = '\0';
    return total;
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

static int read_frame_raw(client *c, oc_header *hdr, oc_rbuf *payload) {
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

/* Presence/typing are tenant-wide async notifications the server may inject into
 * any connection at any time (a peer coming online, a member typing). Verticals
 * that aren't testing presence use this wrapper so those frames don't desync
 * their expected read stream; the presence/typing test uses read_frame_raw. */
static int read_frame(client *c, oc_header *hdr, oc_rbuf *payload) {
    for (;;) {
        int r = read_frame_raw(c, hdr, payload);
        if (r != 0) return r;
        if (hdr->msg_type != OC_MSG_PRESENCE_UPDATE && hdr->msg_type != OC_MSG_TYPING_UPDATE)
            return 0;
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
    oc_send s = {0};
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
    oc_send s = {0};
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
    oc_send s = {0}; s.channel_id = 1; memcpy(s.idem, idem, OC_IDEM_SIZE);
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
    oc_send s1 = {0}; s1.channel_id = cid; memset(s1.idem, 0x11, OC_IDEM_SIZE);
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
    oc_send s2 = {0}; s2.channel_id = cid; memset(s2.idem, 0x22, OC_IDEM_SIZE);
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
    oc_send s = {0}; s.channel_id = 1; memset(s.idem, 0x5B, OC_IDEM_SIZE);
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
    oc_send s = {0}; s.channel_id = 1; memset(s.idem, 0x71, OC_IDEM_SIZE);
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
    oc_send_reply sr = {0}; sr.channel_id = 1; memset(sr.idem, 0x72, OC_IDEM_SIZE);
    sr.parent_id = mid; sr.body = oc_slice_str("first reply");
    CHECK(oc_encode_send_reply(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    for (int i = 0; i < 2; i++) {          /* bob: SEND_ACK + THREAD_REPLY */
        CHECK(read_frame(&b, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_SEND_ACK || hdr.msg_type == OC_MSG_THREAD_REPLY);
    }
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    oc_thread_reply tr = {0}; CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK);
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

/* Full-text search over the wire (REQ-080): a member searches and gets matching
 * messages as snippets, and a private channel's messages never surface to a
 * non-member (the member-scoping security property, REQ-031). */
static void test_search_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    uint8_t buf[256]; oc_wbuf w; oc_header hdr; oc_rbuf p;

    /* alice posts a distinctive message to the shared channel. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s = {0}; s.channel_id = 1; memset(s.idem, 0x91, OC_IDEM_SIZE);
    s.body = oc_slice_str("orbital laser schematics");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint64_t m1 = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) { oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK); m1 = ack.message_id; }
    }
    CHECK(m1 != 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);

    /* bob searches and finds it, with the right ids and a non-empty snippet. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_search sq = { oc_slice_str("orbital"), 10 };
    CHECK(oc_encode_search(&w, OC_PROTOCOL_VERSION, &sq) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SEARCH_RESULTS);
    oc_search_result_entry se[8]; uint16_t n = 0;
    CHECK(oc_decode_search_results(&p, se, 8, &n, NULL) == OC_OK);
    CHECK(n == 1 && se[0].message_id == m1 && se[0].channel_id == 1 && se[0].author_id == ua);
    CHECK(se[0].snippet.len > 0);

    /* alice creates a private channel and posts to it. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cch = { oc_slice_str("bunker"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cch) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t cid = ci.channel_id;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s2 = {0}; s2.channel_id = cid; memset(s2.idem, 0x92, OC_IDEM_SIZE);
    s2.body = oc_slice_str("nuclear launch codes");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s2) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    for (int i = 0; i < 2; i++) { CHECK(read_frame(&a, &hdr, &p) == 0); }   /* ACK + BROADCAST */

    /* bob (not a member of the private channel) cannot find it. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_search sq2 = { oc_slice_str("nuclear"), 10 };
    CHECK(oc_encode_search(&w, OC_PROTOCOL_VERSION, &sq2) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SEARCH_RESULTS);
    CHECK(oc_decode_search_results(&p, se, 8, &n, NULL) == OC_OK && n == 0);

    /* alice, a member, does find it. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_search(&w, OC_PROTOCOL_VERSION, &sq2) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SEARCH_RESULTS);
    CHECK(oc_decode_search_results(&p, se, 8, &n, NULL) == OC_OK && n == 1 && se[0].channel_id == cid);

    client_close(&a);
    client_close(&b);
}

/* Presence + typing over the wire (REQ-120/121): a connecting user is announced
 * online (and gets a snapshot of who's online), an away change propagates, a
 * typing signal reaches other channel members, and a disconnect announces
 * offline. */
static void test_presence_typing(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);   /* first user: no presence yet */

    client b;
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w;

    /* bob connecting -> alice sees bob online; bob gets a snapshot incl. alice. */
    CHECK(read_frame_raw(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_PRESENCE_UPDATE);
    oc_presence_update pu; CHECK(oc_decode_presence_update(&p, &pu) == OC_OK);
    CHECK(pu.user_id == ub && pu.status == OC_PRESENCE_ONLINE);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_PRESENCE_UPDATE);
    CHECK(oc_decode_presence_update(&p, &pu) == OC_OK && pu.user_id == ua && pu.status == OC_PRESENCE_ONLINE);

    /* alice goes away -> bob sees it. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_presence sp = { OC_PRESENCE_AWAY };
    CHECK(oc_encode_set_presence(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_PRESENCE_UPDATE);
    CHECK(oc_decode_presence_update(&p, &pu) == OC_OK && pu.user_id == ua && pu.status == OC_PRESENCE_AWAY);

    /* bob types in the shared channel -> alice (a member) gets TYPING_UPDATE. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_typing ty = { 1 };
    CHECK(oc_encode_typing(&w, OC_PROTOCOL_VERSION, &ty) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame_raw(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_TYPING_UPDATE);
    oc_typing_update tu; CHECK(oc_decode_typing_update(&p, &tu) == OC_OK);
    CHECK(tu.channel_id == 1 && tu.user_id == ub);

    /* alice disconnects -> bob sees her offline. */
    client_close(&a);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_PRESENCE_UPDATE);
    CHECK(oc_decode_presence_update(&p, &pu) == OC_OK && pu.user_id == ua && pu.status == OC_PRESENCE_OFFLINE);

    client_close(&b);
}

/* Issue DOWNLOAD_BEGIN for `aid` and reassemble the streamed blob into `out`
 * (bounded by `cap`), verifying DOWNLOAD_INFO's digest matches `expect_sha` and
 * that chunk sequence numbers are contiguous. Returns the bytes received. */
static size_t download_attachment(client *c, uint64_t aid, uint8_t *out, size_t cap,
                                  const uint8_t expect_sha[32]) {
    uint8_t hb[64]; oc_wbuf w; oc_wbuf_init(&w, hb, sizeof hb);
    oc_download_begin dlb = { aid };
    CHECK(oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &dlb) == OC_OK);
    CHECK(send_frame(c, hb, w.len) == 0);

    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DOWNLOAD_INFO);
    oc_download_info di; CHECK(oc_decode_download_info(&p, &di) == OC_OK);
    CHECK(di.attachment_id == aid && di.total_size <= cap);
    if (expect_sha) CHECK(di.sha256.len == 32 && memcmp(di.sha256.ptr, expect_sha, 32) == 0);

    size_t off = 0; uint32_t exp = 0;
    for (;;) {
        CHECK(read_frame(c, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_DOWNLOAD_CHUNK) {
            oc_download_chunk dc; CHECK(oc_decode_download_chunk(&p, &dc) == OC_OK);
            CHECK(dc.attachment_id == aid && dc.seq == exp && off + dc.data.len <= cap);
            memcpy(out + off, dc.data.ptr, dc.data.len);   /* copy before the next read */
            off += dc.data.len; exp++;
        } else if (hdr.msg_type == OC_MSG_DOWNLOAD_END) {
            oc_download_end de; CHECK(oc_decode_download_end(&p, &de) == OC_OK && de.attachment_id == aid);
            break;
        } else {
            CHECK(0 && "unexpected frame during download");
        }
    }
    CHECK(off == di.total_size);
    return off;
}

/* Attachments over the wire (REQ-140/141, ARCH-69). A multi-chunk blob is
 * uploaded and streamed back byte-for-byte; a second user retrieves the same
 * blob from a shared channel; and a non-member is refused a private one — all
 * proxied through the daemon, with access control on the message-channel gate. */
static void test_attachments_vertical(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0; CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    oc_header hdr; oc_rbuf p;
    uint8_t *fbuf = malloc(OC_MAX_FRAME_SIZE);   /* one chunk frame can be ~64 KiB */
    CHECK(fbuf != NULL);
    oc_wbuf w;

    /* A ~150 KiB payload spans several chunks (exercises the pump + windowing). */
    const size_t total = 150000;
    uint8_t *payload = malloc(total);
    CHECK(payload != NULL);
    for (size_t i = 0; i < total; i++) payload[i] = (uint8_t)(i * 31u + 7u);

    uint8_t idem[OC_IDEM_SIZE]; memset(idem, 0xA1, sizeof idem);

    /* UPLOAD_BEGIN on the public default channel. */
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_begin ub = { OC_DEFAULT_CHANNEL, {0}, oc_slice_str("data.bin"), oc_slice_str("application/octet-stream"), total };
    memcpy(ub.idem, idem, OC_IDEM_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);

    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
    oc_upload_ready urd; CHECK(oc_decode_upload_ready(&p, &urd) == OC_OK);
    uint64_t aid = urd.attachment_id;
    CHECK(aid != 0 && urd.chunk_size > 0 && urd.chunk_size <= OC_ATTACH_CHUNK_SIZE);

    /* Stream the payload, acked chunk by chunk. */
    uint32_t seq = 0; size_t off = 0;
    while (off < total) {
        size_t n = total - off; if (n > urd.chunk_size) n = urd.chunk_size;
        oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
        oc_upload_chunk uc = { aid, seq, { payload + off, n } };
        CHECK(oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) == OC_OK);
        CHECK(send_frame(&a, fbuf, w.len) == 0);
        CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_ACK);
        oc_upload_ack uak; CHECK(oc_decode_upload_ack(&p, &uak) == OC_OK);
        CHECK(uak.attachment_id == aid && uak.acked_through == seq + 1);
        off += n; seq++;
    }

    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_end ue = { aid };
    CHECK(oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_OK);
    oc_upload_ok uok; CHECK(oc_decode_upload_ok(&p, &uok) == OC_OK);
    CHECK(uok.attachment_id == aid && uok.size == total && uok.sha256.len == 32);
    uint8_t digest[32]; memcpy(digest, uok.sha256.ptr, 32);

    /* Download it back and reassemble; bytes and digest must match. */
    uint8_t *got = malloc(total); CHECK(got != NULL);
    size_t glen = download_attachment(&a, aid, got, total, digest);
    CHECK(glen == total && memcmp(got, payload, total) == 0);

    /* A second user in the same (public) channel retrieves the same blob. */
    client b;
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ubid = 0; CHECK(do_auth(&b, "bob", "pw-bob", &ubid) == 0);
    memset(got, 0, total);
    glen = download_attachment(&b, aid, got, total, digest);
    CHECK(glen == total && memcmp(got, payload, total) == 0);

    /* Linking (REQ-140): alice references the attachment in a SEND to the shared
     * channel; bob receives the BROADCAST with the attachment metadata inline. */
    uint8_t midem[OC_IDEM_SIZE]; memset(midem, 0xC3, sizeof midem);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_send sm = {0};
    sm.channel_id = OC_DEFAULT_CHANNEL; memcpy(sm.idem, midem, OC_IDEM_SIZE);
    sm.body = oc_slice_str("here's the file");
    sm.n_attach = 1; sm.attach_ids[0] = aid;
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);
    oc_broadcast bcast; CHECK(oc_decode_broadcast(&p, &bcast) == OC_OK);
    CHECK(bcast.n_attach == 1 && bcast.attach[0].id == aid && bcast.attach[0].size == total);
    CHECK(bcast.attach[0].filename.len == 8 && memcmp(bcast.attach[0].filename.ptr, "data.bin", 8) == 0);
    /* Drain alice's own SEND_ACK + BROADCAST so her stream is clean for the next step. */
    for (int i = 0; i < 2; i++) { CHECK(read_frame(&a, &hdr, &p) == 0); }

    /* Access control (REQ-141): an attachment on a private channel alice creates
     * is refused to bob, a non-member. */
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_create_channel cc = { oc_slice_str("vault"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t priv = ci.channel_id;

    const char *small = "secret bytes";
    size_t slen = strlen(small);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_begin ub2 = { priv, {0}, oc_slice_str("s.txt"), oc_slice_str("text/plain"), slen };
    memset(ub2.idem, 0xB2, OC_IDEM_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub2) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
    CHECK(oc_decode_upload_ready(&p, &urd) == OC_OK);
    uint64_t paid = urd.attachment_id;
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_chunk uc2 = { paid, 0, { (const uint8_t *)small, slen } };
    CHECK(oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc2) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_ACK);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_end ue2 = { paid };
    CHECK(oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue2) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_OK);

    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_download_begin dlb = { paid };
    CHECK(oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &dlb) == OC_OK);
    CHECK(send_frame(&b, fbuf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK);
    CHECK(er.code == OC_ERR_FORBIDDEN);

    free(payload); free(got); free(fbuf);
    client_close(&a);
    client_close(&b);
}

/* Incoming webhooks over the wire (REQ-170, ARCH-32/54): a client mints a
 * per-channel token; a separate non-oc/1 (HTTP) TLS connection POSTs JSON to
 * /webhook/<token>; the channel member receives the message as a BROADCAST and
 * the sender gets 200. An unknown token gets 404. */
static void test_webhook_vertical(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0; CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[256]; oc_wbuf w;

    /* Mint a webhook for the default channel. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_webhook cw = { OC_DEFAULT_CHANNEL, oc_slice_str("ci") };
    CHECK(oc_encode_create_webhook(&w, OC_PROTOCOL_VERSION, &cw) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_WEBHOOK_INFO);
    oc_webhook_info wi; CHECK(oc_decode_webhook_info(&p, &wi) == OC_OK);
    CHECK(wi.channel_id == OC_DEFAULT_CHANNEL && wi.token.len == 32);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", wi.token.ptr[i]);

    /* POST to the webhook over a non-oc/1 (HTTP) connection. */
    client h;
    CHECK(http_client_open(&h, port, pin) == 0);
    const char *body = "{\"text\":\"from webhook\"}";
    char req[512];
    int rn = snprintf(req, sizeof req,
        "POST /webhook/%s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", hex, strlen(body), body);
    CHECK(write_all(&h.conn, (const uint8_t *)req, (size_t)rn) == 0);

    /* alice (a member) receives the posted message as a BROADCAST authored by
     * the webhook's creator. */
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);
    oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
    CHECK(bc.channel_id == OC_DEFAULT_CHANNEL && bc.author_id == ua);
    CHECK(bc.body.len == 12 && memcmp(bc.body.ptr, "from webhook", 12) == 0);
    CHECK(bc.author_name.len == 2 && memcmp(bc.author_name.ptr, "ci", 2) == 0);  /* label override */

    /* The sender gets a 200. */
    char resp[512];
    http_read_response(&h, resp, sizeof resp);
    CHECK(strncmp(resp, "HTTP/1.1 200", 12) == 0);
    client_close(&h);

    /* An unknown token gets a 404. */
    client h2;
    CHECK(http_client_open(&h2, port, pin) == 0);
    rn = snprintf(req, sizeof req,
        "POST /webhook/%064d HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\n"
        "Content-Length: 2\r\nConnection: close\r\n\r\nhi", 0);
    CHECK(write_all(&h2.conn, (const uint8_t *)req, (size_t)rn) == 0);
    http_read_response(&h2, resp, sizeof resp);
    CHECK(strncmp(resp, "HTTP/1.1 404", 12) == 0);
    client_close(&h2);

    /* Management: the channel lists its webhook, then a delete removes it. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_list_webhooks lw = { OC_DEFAULT_CHANNEL };
    CHECK(oc_encode_list_webhooks(&w, OC_PROTOCOL_VERSION, &lw) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_WEBHOOK_LIST);
    oc_webhook_list_entry ents[8]; uint16_t nents = 0;
    CHECK(oc_decode_webhook_list(&p, ents, 8, &nents) == OC_OK && nents == 1);
    CHECK(ents[0].webhook_id == wi.webhook_id);
    CHECK(ents[0].label.len == 2 && memcmp(ents[0].label.ptr, "ci", 2) == 0);

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_delete_webhook dw = { wi.webhook_id };
    CHECK(oc_encode_delete_webhook(&w, OC_PROTOCOL_VERSION, &dw) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_WEBHOOK_DELETED);
    oc_webhook_deleted wdd; CHECK(oc_decode_webhook_deleted(&p, &wdd) == OC_OK);
    CHECK(wdd.webhook_id == wi.webhook_id);

    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_webhooks(&w, OC_PROTOCOL_VERSION, &lw) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_WEBHOOK_LIST);
    CHECK(oc_decode_webhook_list(&p, ents, 8, &nents) == OC_OK && nents == 0);

    client_close(&a);
}

/* Notification preferences sync across a user's devices (REQ-130/131): setting a
 * channel level or DND on one connection pushes a fresh NOTIFY_PREFS snapshot to
 * all of the same user's connections. */
static void test_notify_prefs_vertical(int port, const uint8_t *pin) {
    client a1, a2;                       /* same user, two devices */
    CHECK(client_open(&a1, port, pin) == 0); CHECK(do_handshake(&a1) == 0);
    CHECK(client_open(&a2, port, pin) == 0); CHECK(do_handshake(&a2) == 0);
    uint64_t ua = 0, ua2 = 0;
    CHECK(do_auth(&a1, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&a2, "alice", "pw-alice", &ua2) == 0);
    CHECK(ua == ua2);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w;
    oc_notify_pref_entry ents[16]; uint16_t n; oc_set_dnd dnd;

    /* Set a channel level on device 1 -> both devices get the snapshot. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_notify_pref sp = { OC_DEFAULT_CHANNEL, OC_NOTIFY_MENTIONS };
    CHECK(oc_encode_set_notify_pref(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    client *devs[2] = { &a1, &a2 };
    for (int d = 0; d < 2; d++) {
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_NOTIFY_PREFS);
        CHECK(oc_decode_notify_prefs(&p, ents, 16, &n, &dnd) == OC_OK && n == 1);
        CHECK(ents[0].channel_id == OC_DEFAULT_CHANNEL && ents[0].level == OC_NOTIFY_MENTIONS);
    }

    /* Set DND on device 2 -> both devices get the snapshot (pref persists). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_dnd sd = { 1, 1320, 480 };
    CHECK(oc_encode_set_dnd(&w, OC_PROTOCOL_VERSION, &sd) == OC_OK);
    CHECK(send_frame(&a2, buf, w.len) == 0);
    for (int d = 0; d < 2; d++) {
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_NOTIFY_PREFS);
        CHECK(oc_decode_notify_prefs(&p, ents, 16, &n, &dnd) == OC_OK);
        CHECK(dnd.enabled == 1 && dnd.start_min == 1320 && dnd.end_min == 480 && n == 1);
    }

    client_close(&a1);
    client_close(&a2);
}

/* Audio call signaling over the wire (REQ-150/152): joining a channel's call
 * returns a roster and pushes roster updates to the other participants; a
 * disconnect mid-call drops the participant but keeps the call for the rest; a
 * non-member is refused. (Phase A — signaling only, no audio relay yet.) */
static void test_call_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w; uint64_t parts[32];

    /* alice joins the default channel's call -> CALL_JOINED with just herself. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cj = { OC_DEFAULT_CHANNEL };
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    oc_call_joined jd; CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.channel_id == OC_DEFAULT_CHANNEL && jd.count == 1 && parts[0] == ua);

    /* bob joins -> bob gets a 2-person roster; alice gets a CALL_ROSTER update. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.count == 2);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_ROSTER);
    oc_call_roster ro; CHECK(oc_decode_call_roster(&p, &ro, parts, 32) == OC_OK && ro.count == 2);

    /* bob disconnects mid-call -> alice's roster drops to just herself (REQ-152). */
    client_close(&b);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_ROSTER);
    CHECK(oc_decode_call_roster(&p, &ro, parts, 32) == OC_OK && ro.count == 1 && parts[0] == ua);

    /* A non-member is refused: carol tries to join a private channel's call. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("callvault"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t priv = ci.channel_id;

    client cclient;
    CHECK(client_open(&cclient, port, pin) == 0); CHECK(do_handshake(&cclient) == 0);
    uint64_t uc = 0; CHECK(do_auth(&cclient, "carol", "pw", &uc) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cjv = { priv };
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cjv) == OC_OK);
    CHECK(send_frame(&cclient, buf, w.len) == 0);
    CHECK(read_frame(&cclient, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_NOT_A_MEMBER);

    client_close(&cclient);
    client_close(&a);
}

/* Direct messages over the wire (REQ-050): OPEN_DM creates a kind=DM channel,
 * pushes a CHANNEL_INFO to the peer, the two participants exchange messages
 * through it, and a third user cannot post to it. */
static void test_dm_vertical(int port, const uint8_t *pin) {
    client a, b, c;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(client_open(&c, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_handshake(&b) == 0);
    CHECK(do_handshake(&c) == 0);
    uint64_t ua = 0, ub = 0, uc = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);

    uint8_t buf[256]; oc_wbuf w; oc_header hdr; oc_rbuf p;

    /* alice opens a DM with bob: alice gets CHANNEL_INFO(kind=DM), bob is pushed one. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_open_dm od = { ub };
    CHECK(oc_encode_open_dm(&w, OC_PROTOCOL_VERSION, &od) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    CHECK(ci.kind == OC_CHANNEL_KIND_DM && ci.joined == 1);
    uint64_t dm = ci.channel_id;
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci2; CHECK(oc_decode_channel_info(&p, &ci2) == OC_OK);
    CHECK(ci2.channel_id == dm && ci2.kind == OC_CHANNEL_KIND_DM);

    /* alice messages the DM; both participants receive the BROADCAST. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s = {0}; s.channel_id = dm; memset(s.idem, 0x4D, OC_IDEM_SIZE);
    s.body = oc_slice_str("hi bob, dm here");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint64_t mid = 0;
    for (int i = 0; i < 2; i++) {          /* alice: SEND_ACK + own BROADCAST */
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) { oc_send_ack ack; CHECK(oc_decode_send_ack(&p, &ack) == OC_OK); mid = ack.message_id; }
    }
    CHECK(mid != 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);
    oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
    CHECK(bc.channel_id == dm && bc.author_id == ua && bc.message_id == mid);

    /* carol (not a participant) cannot post to the DM. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s2 = {0}; s2.channel_id = dm; memset(s2.idem, 0x4E, OC_IDEM_SIZE);
    s2.body = oc_slice_str("intruding");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s2) == OC_OK);
    CHECK(send_frame(&c, buf, w.len) == 0);
    CHECK(read_frame(&c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error e; CHECK(oc_decode_error(&p, &e) == OC_OK);
    CHECK(e.code == OC_ERR_NOT_A_MEMBER && e.fatal == 0);

    /* Self-DM (REQ-055): alice opens a DM with herself and messages it; the
     * BROADCAST echoes back to her (she is the only participant). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_open_dm self = { ua };
    CHECK(oc_encode_open_dm(&w, OC_PROTOCOL_VERSION, &self) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info sci; CHECK(oc_decode_channel_info(&p, &sci) == OC_OK);
    CHECK(sci.kind == OC_CHANNEL_KIND_DM && sci.joined == 1);
    uint64_t selfdm = sci.channel_id;
    CHECK(selfdm != dm);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s3 = {0}; s3.channel_id = selfdm; memset(s3.idem, 0x4F, OC_IDEM_SIZE);
    s3.body = oc_slice_str("note to self");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s3) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    int got_ack = 0, got_bc = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) got_ack = 1;
        else if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast sb; CHECK(oc_decode_broadcast(&p, &sb) == OC_OK);
            CHECK(sb.channel_id == selfdm && sb.author_id == ua);
            got_bc = 1;
        }
    }
    CHECK(got_ack && got_bc);

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

/* Per-connection send rate limit (REQ-190): a burst past the window cap is
 * throttled with non-fatal SEND_RATE_LIMITED errors while the in-budget sends
 * still succeed. */
static void test_send_rate_limit(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    /* Fire OC_SEND_RATE_MAX + 5 sends back-to-back (one window). The first
     * OC_SEND_RATE_MAX are accepted (each -> SEND_ACK + BROADCAST to us), the
     * rest are rejected (each -> one ERROR). */
    const int max = 30;   /* mirrors OC_SEND_RATE_MAX in netloop.c */
    const int over = 5;
    for (int i = 0; i < max + over; i++) {
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_send s = {0}; s.channel_id = 1;
        memset(s.idem, 0, OC_IDEM_SIZE); s.idem[0] = (uint8_t)i; s.idem[1] = 0x5A;
        s.body = oc_slice_str("flood");
        CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
    }

    /* Exactly max*(ACK+BROADCAST) + over*ERROR frames come back. */
    int acks = 0, bcasts = 0, limited = 0;
    for (int i = 0; i < max * 2 + over; i++) {
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) acks++;
        else if (hdr.msg_type == OC_MSG_BROADCAST) bcasts++;
        else if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error e; CHECK(oc_decode_error(&p, &e) == OC_OK);
            CHECK(e.code == OC_ERR_SEND_RATE_LIMITED && e.fatal == 0);
            limited++;
        } else CHECK(0 /* unexpected frame */);
    }
    CHECK(acks == max && bcasts == max && limited == over);

    client_close(&a);
}

/* Per-connection output-buffer cap: a client that stops reading while the daemon
 * fans a large backfill at it has its connection dropped rather than being
 * allowed to grow the daemon's memory without bound. Seeds through the writer to
 * bypass the wire send limit, and shrinks its own receive window so the cap is
 * reached with a modest backlog. */
static void test_out_buffer_cap(int port, const uint8_t *pin, oc_dbwriter *dbw, uint64_t flooder) {
    CHECK(flooder != 0);
    static uint8_t big[60000];
    memset(big, 'x', sizeof big);
    for (int i = 0; i < 60; i++) {           /* ~3.6 MB, well over the 1 MiB cap */
        oc_job *j = oc_job_new(OC_JOB_SEND, 0);
        if (!j) { CHECK(0); return; }
        j->user_id = flooder; j->channel_id = 1;
        memset(j->idem, 0, OC_IDEM_LEN); j->idem[0] = (uint8_t)i; j->idem[1] = 0xC7;
        oc_job_set_body(j, big, sizeof big);
        oc_dbwriter_submit(dbw, j);
    }
    usleep(400000);   /* let the writer persist them */

    client v;
    CHECK(client_open(&v, port, pin) == 0);
    CHECK(do_handshake(&v) == 0);
    uint64_t uv = 0;
    CHECK(do_auth(&v, "flooder", "pw", &uv) == 0);
    int rb = 8192;    /* tiny receive window: TCP backpressure hits fast */
    setsockopt(v.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);

    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { 1, 0 };
    oc_backfill_request req = { 1, &cur };
    CHECK(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK);
    CHECK(write_all(&v.conn, buf, w.len) == 0);

    usleep(600000);   /* stay silent: the daemon fills our buffer past the cap */

    /* The daemon dropped us mid-backfill: we never see a clean BACKFILL_DONE. */
    int done = 0, closed = 0;
    for (int i = 0; i < 4000; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame(&v, &hdr, &p) != 0) { closed = 1; break; }
        if (hdr.msg_type == OC_MSG_BACKFILL_DONE) { done = 1; break; }
    }
    CHECK(closed == 1 && done == 0);
    client_close(&v);
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

/* Concurrency load (robustness backlog #5): many clients connect, authenticate,
 * and send at once, exercising the accept path + writer + fan-out under
 * contention. Each must get an ack for every message it sent — proving the
 * two-thread (now three-thread) model stays correct and deadlock-free under
 * concurrent load. */
struct load_arg { int port; const uint8_t *pin; int tid; int ok; };
static void *load_client(void *vp) {
    struct load_arg *a = (struct load_arg *)vp;
    client c;
    if (client_open(&c, a->port, a->pin) != 0) return NULL;
    if (do_handshake(&c) != 0) { client_close(&c); return NULL; }
    uint64_t uid = 0;
    if (do_auth(&c, "alice", "pw-alice", &uid) != 0) { client_close(&c); return NULL; }

    const int K = 5;   /* under the 30/3s send rate limit */
    for (int i = 0; i < K; i++) {
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_send s = {0}; s.channel_id = 1;
        memset(s.idem, 0, OC_IDEM_SIZE);
        s.idem[0] = (uint8_t)i; s.idem[1] = (uint8_t)a->tid; s.idem[2] = 0xEE; /* globally unique */
        s.body = oc_slice_str("load");
        if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) != OC_OK) { client_close(&c); return NULL; }
        if (write_all(&c.conn, buf, w.len) != 0) { client_close(&c); return NULL; }
    }
    /* Read until our K acks arrive (draining the interleaved broadcasts). */
    int acks = 0;
    for (int i = 0; i < 4000 && acks < K; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame(&c, &hdr, &p) != 0) break;
        if (hdr.msg_type == OC_MSG_SEND_ACK) acks++;
    }
    client_close(&c);
    a->ok = (acks == K);
    return NULL;
}

static void test_concurrent_load(int port, const uint8_t *pin) {
    enum { N = 8 };
    pthread_t th[N];
    struct load_arg args[N];
    for (int i = 0; i < N; i++) {
        args[i].port = port; args[i].pin = pin; args[i].tid = i; args[i].ok = 0;
        CHECK(pthread_create(&th[i], NULL, load_client, &args[i]) == 0);
    }
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < N; i++) CHECK(args[i].ok == 1);
}

/* Per-IP connection throttle (robustness): a single source IP can hold at most
 * OPENCHIME_MAX_CONNS_PER_IP concurrent connections; the daemon closes excess
 * connections at accept, before spending a TLS context on them. Uses its own
 * loop so the tiny cap doesn't disturb the shared test loop. */
static void test_conn_throttle(int port) {
    setenv("OPENCHIME_MAX_CONNS_PER_IP", "2", 1);
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);

    unlink("build/itest_throttle.db");
    unlink("build/itest_throttle.db-wal");
    unlink("build/itest_throttle.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_throttle.db");
    CHECK(dbw2 != NULL);

    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    /* Two connections from loopback are within the cap. */
    client a, b;
    CHECK(client_open(&a, port, pin2) == 0);
    CHECK(client_open(&b, port, pin2) == 0);
    /* The third is refused at accept — its TLS handshake never completes. */
    client c;
    int rc = client_open(&c, port, pin2);
    CHECK(rc != 0);

    client_close(&a);
    client_close(&b);
    if (rc == 0) client_close(&c);

    arg2.stop = 1;
    pthread_join(th2, NULL);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    unsetenv("OPENCHIME_MAX_CONNS_PER_IP");
    unlink("build/itest_throttle.db");
    unlink("build/itest_throttle.db-wal");
    unlink("build/itest_throttle.db-shm");
}

int run_netloop_tests(void) {
    printf("itest_netloop: handshake, version REJECT, two-client AUTH+SEND+BROADCAST, backfill, edit/delete, channels, reactions, threads, search, dm, load, rate-limit, out-cap, throttle, admin, logout\n");

    /* Attachment blobs go to a build-local dir (REQ-140); the daemon defaults to
     * /data/blobs, which isn't writable in the test sandbox. */
    setenv("OPENCHIME_BLOB_DIR", "build/itest_blobs", 1);

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
    uint64_t flooder = oc_dbwriter_register_local(dbw, "flooder", "pw", OC_ROLE_MEMBER, 2048);
    CHECK(flooder != 0);

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
        test_search_vertical(arg.port, pin);
        test_dm_vertical(arg.port, pin);
        test_presence_typing(arg.port, pin);
        test_attachments_vertical(arg.port, pin);
        test_webhook_vertical(arg.port, pin);
        test_notify_prefs_vertical(arg.port, pin);
        test_call_vertical(arg.port, pin);
        test_concurrent_load(arg.port, pin);
        test_send_rate_limit(arg.port, pin);
        test_out_buffer_cap(arg.port, pin, dbw, flooder);
        test_admin_vertical(arg.port, pin);
        test_logout_closes(arg.port, pin);
        test_conn_throttle(arg.port + 123);
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
