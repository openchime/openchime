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
    printf("itest_netloop: handshake, version REJECT, two-client AUTH+SEND+BROADCAST, backfill, edit/delete, logout\n");

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
