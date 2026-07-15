/* Black-box end-to-end client for the integration harness
 * (Scripts/test-integration.sh). Connects to a *running* daemon (the compose
 * container) over TLS and drives the real vertical: two clients handshake,
 * authenticate, one SENDs, and both must receive the BROADCAST with the sender
 * acked. Exits 0 on success, 1 on any failure. Not part of the `make test`
 * binary — this talks to a deployed daemon, not in-process modules.
 *
 * Usage: e2e_client <host> <port>
 */

#include "protocol.h"
#include "framebuf.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAIL(msg) do { fprintf(stderr, "e2e: FAIL %s\n", (msg)); return -1; } while (0)

typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; } client;

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

static int dial(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) return fd;
        close(fd);
        usleep(50000);
    }
    return -1;
}

/* Connect + TLS handshake, trusting any cert (TOFU pin verified in unit tests;
 * here we exercise the deployed stack, not pinning). */
static int client_open(client *c, const char *host, int port) {
    c->fd = dial(host, port);
    if (c->fd < 0) return -1;
    if (oc_tls_client_init(&c->cli, NULL) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
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
        if (r == 1) {
            if (oc_parse_frame(frame, flen, hdr, payload) != OC_OK) return -1;
            /* Presence/typing are tenant-wide async notifications the server may
             * inject into any connection (a peer coming online, a member typing);
             * this black-box test doesn't exercise them, so skip them rather than
             * let a broadcast desync the expected stream. */
            if (hdr->msg_type == OC_MSG_PRESENCE_UPDATE || hdr->msg_type == OC_MSG_TYPING_UPDATE)
                continue;
            return 0;
        }
        if (r < 0) return -1;
        uint8_t buf[4096]; size_t n = 0;
        if (oc_tls_read(&c->conn, buf, sizeof buf, &n) != OC_TLS_OK) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

static int do_handshake(client *c) {
    uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_hello h = { 1, 1, oc_slice_str("e2e") };
    if (oc_encode_hello(&w, &h) != OC_OK || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) return -1;
    oc_welcome wel;
    if (oc_decode_welcome(&p, &wel) != OC_OK) return -1;
    /* The daemon follows WELCOME with AUTH_CHALLENGE (PROTOCOL.md §4.1). */
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_CHALLENGE) return -1;
    oc_auth_challenge ch;
    return oc_decode_auth_challenge(&p, &ch) == OC_OK ? 0 : -1;
}

static int do_auth(client *c, const char *user, const char *pass, uint64_t *uid) {
    uint8_t cbuf[256]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass)) != OC_OK) return -1;
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != 0 || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    *uid = ok.user_id;
    return 0;
}

static int run(const char *host, int port) {
    client a, b;
    if (client_open(&a, host, port) != 0) FAIL("connect A");
    if (client_open(&b, host, port) != 0) FAIL("connect B");
    if (do_handshake(&a) != 0 || do_handshake(&b) != 0) FAIL("handshake");

    uint64_t ua = 0, ub = 0;
    if (do_auth(&a, "e2e-alice", "e2e-pw", &ua) != 0 ||
        do_auth(&b, "e2e-bob",   "e2e-pw", &ub) != 0) FAIL("auth");
    if (ua == 0 || ub == 0) FAIL("user ids");

    uint8_t idem[OC_IDEM_SIZE];
    memset(idem, 0x42, sizeof idem);
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s;
    s.channel_id = 1;
    memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str("end to end");
    if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) != OC_OK || write_all(&a.conn, buf, w.len) != 0)
        FAIL("send");

    /* A gets SEND_ACK + BROADCAST (either order). */
    uint64_t acked = 0, bcast_a = 0;
    for (int i = 0; i < 2; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame(&a, &hdr, &p) != 0) FAIL("read A");
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack;
            if (oc_decode_send_ack(&p, &ack) != OC_OK) FAIL("decode ack");
            acked = ack.message_id;
        } else if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast bc;
            if (oc_decode_broadcast(&p, &bc) != OC_OK) FAIL("decode bcast A");
            bcast_a = bc.message_id;
        } else FAIL("unexpected frame to A");
    }
    if (acked == 0 || acked != bcast_a) FAIL("ack/broadcast mismatch");

    /* B gets the same BROADCAST. */
    oc_header hdr; oc_rbuf p;
    if (read_frame(&b, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_BROADCAST) FAIL("read B broadcast");
    oc_broadcast bcb;
    if (oc_decode_broadcast(&p, &bcb) != OC_OK) FAIL("decode bcast B");
    if (bcb.message_id != acked || bcb.author_id != ua) FAIL("B broadcast content");
    if (bcb.body.len != 10 || memcmp(bcb.body.ptr, "end to end", 10) != 0) FAIL("B broadcast body");

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <host> <port>\n", argv[0]); return 2; }
    if (run(argv[1], atoi(argv[2])) != 0) return 1;
    printf("e2e: OK — two clients, AUTH + SEND + BROADCAST against the daemon\n");
    return 0;
}
