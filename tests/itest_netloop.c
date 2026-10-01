/* Integration test for the event loop (netloop.c) end to end: TLS handshake,
 * HELLO->WELCOME, version REJECT, and the message vertical — two clients
 * authenticate, one SENDs, and both receive the BROADCAST while the sender is
 * acked. Runs the non-blocking epoll server (with a real DB-writer thread) in a
 * thread and drives it with blocking TLS clients. */

#include "netloop.h"
#include "audio.h"
#include "config.h"
#include "dbwriter.h"
#include "ioloop.h"
#include "framebuf.h"
#include "protocol.h"
#include "tls.h"
#include "tts_render.h"
#include "stt_render.h"
#include "check.h"
#include "sqlite3.h"
#include "signin.h"       /* a verifier and its challenge, as a client makes them */
#include "localissuer.h"   /* a token the daemon did not sign, to refuse */
#include "devicecodes.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
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

/* token + seq(u16 BE) + payload -> the relay. */
static void udp_send_audio_n(int fd, const struct sockaddr_in *to, const uint8_t *tok, size_t tlen,
                             uint16_t seq, const char *payload) {
    uint8_t pkt[128]; size_t pl = payload ? strlen(payload) : 0;
    memcpy(pkt, tok, tlen);
    pkt[tlen] = (uint8_t)(seq >> 8); pkt[tlen + 1] = (uint8_t)seq;
    if (pl) memcpy(pkt + tlen + 2, payload, pl);
    sendto(fd, pkt, tlen + 2 + pl, 0, (const struct sockaddr *)to, sizeof *to);
}
static void udp_send_audio(int fd, const struct sockaddr_in *to, const uint8_t *tok,
                           uint16_t seq, const char *payload) {
    udp_send_audio_n(fd, to, tok, OC_AUDIO_TOKEN_RAND, seq, payload);
}
/* Receive a forwarded datagram: sender(u64) seq(u16) payload. -1 on timeout. */
static int udp_recv_audio(int fd, uint64_t *sender, uint16_t *seq, char *out, size_t cap) {
    uint8_t pkt[256];
    ssize_t n = recv(fd, pkt, sizeof pkt, 0);
    if (n < (ssize_t)OC_AUDIO_S2C_HDR) return -1;
    uint64_t s = 0; for (int i = 0; i < 8; i++) s = (s << 8) | pkt[i];
    *sender = s; *seq = (uint16_t)((pkt[8] << 8) | pkt[9]);
    size_t pl = (size_t)n - OC_AUDIO_S2C_HDR; if (pl > cap) pl = cap;
    memcpy(out, pkt + OC_AUDIO_S2C_HDR, pl);
    return (int)pl;
}
static void udp_timeout(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static int mk_udp_client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    udp_timeout(fd, 1000);   /* 1 s -- a generous ceiling for the audio relay
                              * round-trip under CI load (loopback UDP doesn't
                              * drop); a read waits this long only when nothing
                              * comes, which a check here never expects */
    return fd;
}

/* The ready hook (netloop.h): when it runs, the listener must already take
 * connections -- a managed box claims its binding from it, and central reads
 * the claim as a workspace that is up. */
static int g_ready_calls, g_ready_connected, g_ready_port;
static void on_ready(void *ctx) {
    (void)ctx;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)g_ready_port);
    /* The loop has not served a cycle yet, so this is the kernel's backlog
     * accepting it: which is what "the listener is bound" means. */
    g_ready_connected = fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    if (fd >= 0) close(fd);
    oc_netloop_set_ready(NULL, NULL);   /* the other loops in this suite are not asked */
    __atomic_add_fetch(&g_ready_calls, 1, __ATOMIC_RELEASE);
}

static void *loop_thread(void *p) {
    struct loop_arg *a = (struct loop_arg *)p;
    /* The daemon loads config once in main() before serving; here the test is the
     * startup owner, so load it after the test's setenv() and before the loop. */
    char cfgerr[128];
    oc_config_load(cfgerr, sizeof cfgerr);
    oc_netloop_run(a->port, a->srv, a->dbw, &a->stop);
    return NULL;
}

/* Stop a loop and wait for it. The loop looks at its flag between turns, and an
 * idle one takes a turn only every half second, so a connection to its port
 * wakes it now rather than whenever that comes round. */
static void stop_loop(struct loop_arg *a, pthread_t th) {
    __atomic_store_n(&a->stop, 1, __ATOMIC_RELEASE);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)a->port);
    if (fd >= 0) { (void)connect(fd, (struct sockaddr *)&sa, sizeof sa); close(fd); }
    pthread_join(th, NULL);
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

/* `prefix` (may be NULL) is written to the socket before TLS begins — what a TCP
 * forwarder's PROXY header is. */
static int g_client_v6;   /* clients connect to ::1 instead of 127.0.0.1 */

static int client_open_with(client *c, int port, const uint8_t *pin,
                            const uint8_t *prefix, size_t prefix_len) {
    struct sockaddr_storage addr;
    socklen_t alen;
    memset(&addr, 0, sizeof addr);
    if (g_client_v6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback; a6->sin6_port = htons((uint16_t)port);
        alen = sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
        a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); a4->sin_port = htons((uint16_t)port);
        alen = sizeof *a4;
    }
    c->fd = -1;
    for (int i = 0; i < 200; i++) {
        int fd = socket(addr.ss_family, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, alen) == 0) { c->fd = fd; break; }
        close(fd);
        usleep(20000);
    }
    if (c->fd < 0) return -1;
    /* A READ DEADLINE, so a missing frame fails this suite instead of hanging
     * it. Learned the hard way: a schema change broke an upsert on the daemon
     * side, the reply never came, and a blocking read turned a one-line bug
     * into a twenty-minute hang with no output — the least useful failure mode
     * a test can have. 20 s is far longer than any legitimate wait here (the
     * slowest case is the concurrent-load client's TLS handshake) and far
     * shorter than a person's patience. */
    {
        struct timeval tv = { 20, 0 };
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (prefix && write(c->fd, prefix, prefix_len) != (ssize_t)prefix_len) return -1;
    /* Concurrent TLS setup across threads (test_concurrent_load opens 8 clients
     * at once) is safe: the vendored mbedTLS is built with MBEDTLS_THREADING. */
    if (oc_tls_client_init(&c->cli, pin) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
}

static int client_open(client *c, int port, const uint8_t *pin) {
    return client_open_with(c, port, pin, NULL, 0);
}

/* The ALPN list an ordinary HTTPS client offers — curl's default, and what a
 * webhook sender such as GitHub presents. Deliberately not "no ALPN": offering
 * nothing is the one shape that never exercised the server's ALPN selection,
 * which is how a server list of oc/1 alone (refusing every real sender with
 * `no_application_protocol`) went unnoticed here. */
static const char *http_alpn[] = { "h2", OC_ALPN_HTTP11, NULL };

/* Open a TLS connection as such a client, so the daemon routes it to the HTTP
 * handler (ARCH-54) — used to drive the webhook endpoint. */
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
    {
        struct timeval tv = { 20, 0 };   /* the suite's read deadline, as client_open's */
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (oc_tls_client_init_ex(&c->cli, pin, http_alpn) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
}

/* Read the full HTTP response (until the server closes) into `buf` -- or what
 * came within twenty seconds, the suite's read deadline, so an answer that never
 * comes fails the check instead of hanging the suite. */
static size_t http_read_response(client *c, char *buf, size_t cap) {
    size_t total = 0;
    struct timeval t0, t; gettimeofday(&t0, NULL);
    while (total < cap - 1) {
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, (uint8_t *)buf + total, cap - 1 - total, &n);
        gettimeofday(&t, NULL);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000 > 20000) break;
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
        /* Also skip the post-AUTH_OK WORKSPACE_INFO and TTS_INFO pushes (like
         * presence/typing, they arrive unsolicited and would desync a fixed
         * expected-frame stream). */
        if (hdr->msg_type != OC_MSG_PRESENCE_UPDATE &&
            hdr->msg_type != OC_MSG_TYPING_UPDATE &&
            hdr->msg_type != OC_MSG_WORKSPACE_INFO &&
            hdr->msg_type != OC_MSG_CAPABILITIES &&
            hdr->msg_type != OC_MSG_TTS_INFO &&
            hdr->msg_type != OC_MSG_STT_INFO &&
            hdr->msg_type != OC_MSG_CALL_STATE &&
            hdr->msg_type != OC_MSG_CHANNEL_GROUPS &&  /* beside every CHANNEL_INFO (REQ-309) */
            hdr->msg_type != OC_MSG_GROUP_INFO &&      /* the groups, unasked after a sign-in's */
            hdr->msg_type != OC_MSG_GROUPS_END &&      /*   user list (REQ-307) */
            hdr->msg_type != OC_MSG_ALERTS_SUMMARY)    /* an owner's or admin's, at sign-in (REQ-263) */
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
    /* Track the codec, not a literal: hardcoding 1 here meant a protocol bump
     * failed this test rather than the mismatch it is supposed to detect. */
    if (send_hello(c, OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) return -1;
    oc_welcome wel;
    if (oc_decode_welcome(&p, &wel) != OC_OK) return -1;
    /* The daemon follows WELCOME with AUTH_CHALLENGE advertising its methods. */
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_CHALLENGE) return -1;
    oc_auth_challenge ch;
    if (oc_decode_auth_challenge(&p, &ch) != OC_OK) return -1;
    for (uint8_t i = 0; i < ch.n_sources; i++)
        if (ch.sources[i].kind == OC_SOURCE_LOCAL) return 0;
    return -1;
}

/* What the last do_auth was told of voice input: the capability, and the cap. */
static int      g_auth_stt;
static uint32_t g_auth_stt_max_ms;

static oc_alerts_summary g_auth_alerts;   /* what the last owner's or admin's sign-in was told */
static int do_auth(client *c, const char *user, const char *pass, uint64_t *user_id) {
    uint8_t cbuf[256]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass)) != OC_OK) return -1;
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, oc_slice_str("local"), { cbuf, cw.len }, { NULL, 0 } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != 0) return -1;
    if (write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    *user_id = ok.user_id;
    /* The daemon pushes WORKSPACE_INFO immediately after AUTH_OK (before the
     * presence snapshot). Consume it here so both read_frame and the raw
     * presence/typing test start from a clean stream. */
    if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WORKSPACE_INFO) return -1;
    /* Then what this daemon offers (REQ-295), told to every client at auth. */
    if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_CAPABILITIES) return -1;
    {
        oc_capabilities cp;
        if (oc_decode_capabilities(&p, &cp) != OC_OK) return -1;
        g_auth_stt = 0;
        for (uint8_t k = 0; k < cp.count; k++)
            if (cp.names[k].len == 3 && memcmp(cp.names[k].ptr, OC_CAP_STT, 3) == 0) g_auth_stt = 1;
    }
    /* Then read-aloud's voices, sent whether or not it is offered. */
    if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_TTS_INFO) return -1;
    { oc_tts_info ti; if (oc_decode_tts_info(&p, &ti) != OC_OK) return -1; }
    /* Then voice input's recognizer, likewise sent whether or not it is offered. */
    if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_STT_INFO) return -1;
    { oc_stt_info si; if (oc_decode_stt_info(&p, &si) != OC_OK) return -1; g_auth_stt_max_ms = si.max_segment_ms; }
    /* Then the pause (REQ-278), which outlives the session that set it and so is
     * told at auth rather than only on request. Asserted, not skipped: a fresh
     * account is not paused, and reading it here keeps every later test's stream
     * positional. */
    if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_SNOOZE) return -1;
    { oc_snooze sn; if (oc_decode_snooze(&p, &sn) != OC_OK) return -1; }
    /* Then, for an owner or admin only, the daemon's critical failures (REQ-263). */
    if (ok.role == OC_ROLE_OWNER || ok.role == OC_ROLE_ADMIN) {
        if (read_frame_raw(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_ALERTS_SUMMARY) return -1;
        oc_alerts_summary sm;
        if (oc_decode_alerts_summary(&p, &sm) != OC_OK) return -1;
        g_auth_alerts = sm;
    }
    return 0;
}

static void test_version_reject(int port, const uint8_t *pin) {
    client c;
    CHECK(client_open(&c, port, pin) == 0);
    CHECK(send_hello(&c, OC_PROTOCOL_VERSION + 1, OC_PROTOCOL_VERSION + 1) == 0);  /* too new */
    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(&c, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_REJECT);
    oc_reject rej;
    CHECK(oc_decode_reject(&p, &rej) == OC_OK);
    CHECK(rej.code == OC_ERR_VERSION_TOO_NEW);
    client_close(&c);

    /* And the previous version is refused as TOO_OLD. This is what a version
     * bump has to buy to be worth making: v8 reassigned TYPING/TYPING_UPDATE to
     * 0x007E/0x007F, so a v7 peer's TYPING frame would arrive as PROFILE_INFO —
     * the daemon advertises min == max == OC_PROTOCOL_VERSION precisely so that
     * pair never reaches a decoder. */
    client d;
    CHECK(client_open(&d, port, pin) == 0);
    CHECK(send_hello(&d, OC_PROTOCOL_VERSION - 1, OC_PROTOCOL_VERSION - 1) == 0);
    CHECK(read_frame(&d, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_REJECT);
    CHECK(oc_decode_reject(&p, &rej) == OC_OK);
    CHECK(rej.code == OC_ERR_VERSION_TOO_OLD);
    client_close(&d);
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
    /* A third member who never touches the thread. THREAD_REPLY's `participant`
     * byte is per-recipient (REQ-061) and the fan-out encodes the frame twice to
     * carry it, so proving it needs someone the answer differs for. */
    client d;
    CHECK(client_open(&d, port, pin) == 0);
    CHECK(do_handshake(&d) == 0);
    uint64_t ud = 0;
    CHECK(do_auth(&d, "carol", "pw", &ud) == 0);

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
    CHECK(read_frame(&d, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_BROADCAST);

    /* bob replies: he gets a SEND_ACK, and every member gets a THREAD_REPLY (not
     * a BROADCAST — the reply stays out of the main scroll). */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send_reply sr = {0}; sr.channel_id = 1; memset(sr.idem, 0x72, OC_IDEM_SIZE);
    sr.parent_id = mid; sr.body = oc_slice_str("first reply");
    CHECK(oc_encode_send_reply(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    oc_thread_reply tr = {0};
    for (int i = 0; i < 2; i++) {          /* bob: SEND_ACK + THREAD_REPLY */
        CHECK(read_frame(&b, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_SEND_ACK || hdr.msg_type == OC_MSG_THREAD_REPLY);
        if (hdr.msg_type == OC_MSG_THREAD_REPLY) {
            CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK);
            CHECK(tr.participant == 1);    /* he wrote the reply */
        }
    }
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK);
    CHECK(tr.parent_id == mid && tr.author_id == ub && tr.reply_count == 1);
    CHECK(tr.body.len == 11 && memcmp(tr.body.ptr, "first reply", 11) == 0);
    CHECK(tr.participant == 1);            /* alice wrote the root */
    /* The same reply, the same instant, the other answer: carol is in the
     * channel and not in the thread. One encoding could not have carried both,
     * which is the whole reason the fan-out encodes two. */
    CHECK(read_frame(&d, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    oc_thread_reply trc = {0};
    CHECK(oc_decode_thread_reply(&p, &trc) == OC_OK);
    CHECK(trc.message_id == tr.message_id);
    CHECK(trc.participant == 0);
    CHECK(trc.body.len == 11 && memcmp(trc.body.ptr, "first reply", 11) == 0);

    /* alice opens the thread: the reply is streamed, then a THREAD terminator. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_list_thread lt = { 1, mid };
    CHECK(oc_encode_list_thread(&w, OC_PROTOCOL_VERSION, &lt) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_THREAD_REPLY);
    CHECK(oc_decode_thread_reply(&p, &tr) == OC_OK && tr.parent_id == mid);
    /* A replay reports participation truthfully — it is the same question, and
     * the client suppresses its own toasts for history it asked for. */
    CHECK(tr.participant == 1);
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
        } else if (hdr.msg_type == OC_MSG_REACTION_UPDATED) {
            /* A backfill also carries the reaction state of what it replays, so
             * reactions survive a reload on a client that caches nothing. */
            oc_reaction_updated ru; CHECK(oc_decode_reaction_updated(&p, &ru) == OC_OK);
        } else if (hdr.msg_type == OC_MSG_BACKFILL_DONE) break;
        else CHECK(0 /* unexpected frame during backfill */);
    }
    CHECK(saw_meta == 1);

    client_close(&a);
    client_close(&b);
    client_close(&c);
    client_close(&d);
}

/* Presence + typing over the wire (REQ-120/121): a connecting user is announced
 * online (and gets a snapshot of who's online), an away change propagates, a
 * typing signal reaches other channel members, and a disconnect announces
 * offline. */
/* Drafts across two connections of the SAME user (REQ-223, ARCH-101). This is
 * the half no unit test can reach: the fan-out rule is "every connection of this
 * user EXCEPT the one that wrote it", and getting that backwards is invisible
 * until two devices are actually attached. */
static void test_drafts_vertical(int port, const uint8_t *pin) {
    client a, b;
    uint64_t ua = 0, ub = 0;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    /* alice's SECOND device, not a second person. */
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    CHECK(do_auth(&b, "alice", "pw-alice", &ub) == 0);
    CHECK(ua == ub);

    oc_header hdr; oc_rbuf p; uint8_t buf[512]; oc_wbuf w;

    /* Device A writes a draft; device B is told, and A is NOT — echoing it back
     * to the writer is how a client overwrites the composer being typed in. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_draft sd = { 1, 0, oc_slice_str(""), oc_slice_str("half a thought") };
    CHECK(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &sd) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);

    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DRAFT);
    oc_draft d;
    CHECK(oc_decode_draft(&p, &d) == OC_OK);
    CHECK(d.channel_id == 1 && d.thread_root == 0 && d.updated_ms != 0);
    CHECK(d.body.len == 14 && memcmp(d.body.ptr, "half a thought", 14) == 0);

    /* B lists and sees it, which is also what a cold start does. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_drafts(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DRAFT);
    CHECK(oc_decode_draft(&p, &d) == OC_OK && d.channel_id == 1);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DRAFTS);
    oc_drafts dl; CHECK(oc_decode_drafts(&p, &dl) == OC_OK && dl.count == 1);

    /* Deleting arrives as the same frame with an empty body. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_draft del = { 1, 0, oc_slice_str(""), oc_slice_str("") };
    CHECK(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &del) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DRAFT);
    CHECK(oc_decode_draft(&p, &d) == OC_OK && d.body.len == 0);

    /* And SENDING clears it server-side: write one, send a message, and the
     * list comes back empty without anyone asking it to. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_draft again = { 1, 0, oc_slice_str(""), oc_slice_str("about to send") };
    CHECK(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &again) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DRAFT);

    {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_send sm = {0};
        sm.channel_id = 1;
        memset(sm.idem, 0x5A, OC_IDEM_SIZE);   /* used by no other send here: a repeat is a replay, acknowledged and not sent */
        sm.body = oc_slice_str("about to send");
        CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
    }
    /* And the WRITER is never told about its own drafts. Asserted here rather
     * than asserted in a comment: A has written two drafts and just sent a
     * message, so if the fan-out included the originator there would be a DRAFT
     * frame waiting in front of A's SEND_ACK. Drain until the ack and require
     * that nothing on the way is one. */
    for (int i = 0; i < 8; i++) {
        CHECK(read_frame_raw(&a, &hdr, &p) == 0);
        CHECK(hdr.msg_type != OC_MSG_DRAFT);
        if (hdr.msg_type == OC_MSG_SEND_ACK) break;
    }
    /* Drain until the list answers; the send also broadcasts to both devices. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_drafts(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    for (int i = 0; i < 8; i++) {
        CHECK(read_frame_raw(&b, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_DRAFTS) {
            CHECK(oc_decode_drafts(&p, &dl) == OC_OK && dl.count == 0);
            break;
        }
        CHECK(hdr.msg_type != OC_MSG_DRAFT);   /* a cleared draft must not be listed */
    }

    client_close(&a);
    client_close(&b);
}

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

/* Do-not-disturb is visible to OTHER PEOPLE, and both halves of it are
 * (REQ-122/136/278). The badge exists for the sender — someone deciding whether
 * to write — so a colleague inside their configured quiet hours reading as
 * ordinarily interruptible is the one case it was built for.
 *
 * Driven end to end through a real netloop because that is where the defect
 * lived: the rule itself (oc_notify_quiet) was always right and always unit
 * tested, and the presence path simply never asked it. */
static void wait_presence_dnd(client *c, uint64_t of_user, int want) {
    oc_header hdr; oc_rbuf p;
    /* The schedule half is announced by the maintenance TICK, not by the frame
     * that changed it, so this reads until the answer arrives; presence frames
     * for other users and other transitions can legitimately precede it.
     *
     * With a receive deadline, because the failure being guarded against is a
     * frame that never comes — and a test that blocks forever on that reports a
     * hung suite rather than a broken assertion, which is the harder of the two
     * to read. Five seconds is many times the tick's own 500 ms. */
    struct timeval tv = { 5, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int got = 0;
    for (int i = 0; i < 40; i++) {
        if (read_frame_raw(c, &hdr, &p) != 0) break;      /* timed out or closed */
        if (hdr.msg_type != OC_MSG_PRESENCE_UPDATE) continue;
        oc_presence_update pu;
        CHECK(oc_decode_presence_update(&p, &pu) == OC_OK);
        if (pu.user_id != of_user) continue;
        if (pu.dnd == (want ? 1 : 0)) { got = 1; break; }
    }
    tv.tv_sec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(got);   /* no PRESENCE_UPDATE carrying the expected dnd bit */
}

static void set_schedule(client *c, uint8_t mode, int16_t tz,
                         uint16_t start, uint16_t end) {
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_schedule sc = { mode, tz, start, end, 0, NULL };
    CHECK(oc_encode_set_schedule(&w, OC_PROTOCOL_VERSION, &sc) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
}

static void set_tz(client *c, int16_t off) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_tz_offset tz = { off };
    CHECK(oc_encode_set_tz_offset(&w, OC_PROTOCOL_VERSION, &tz) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
}

/* An offset that puts this user's local clock at `want_local` right now, inside
 * the range SET_TZ_OFFSET accepts. Computed rather than hardcoded because the
 * assertions below are about a real wall clock: a fixed "quiet after 18:00"
 * window would pass or fail depending on the hour the suite happened to run. */
static int16_t offset_for_local(int want_local) {
    int utc_min = (int)(((long long)time(NULL) / 60) % 1440);
    int off = want_local - utc_min;
    if (off < -720) off += 1440;        /* UTC-12 is the westmost real offset */
    return (int16_t)off;
}

static void set_snooze(client *c, uint32_t minutes) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_snooze sn = { minutes };
    CHECK(oc_encode_set_snooze(&w, OC_PROTOCOL_VERSION, &sn) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
}

static void test_presence_dnd(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    (void)ub;

    /* Quiet ALWAYS: the window is the hours notifications are ALLOWED, and an
     * empty one allows nothing. Independent of the wall clock, which is what
     * makes this the assertion the issue is actually about — before the fix the
     * schedule reached the presence path through nothing at all. */
    set_schedule(&a, OC_DND_EVERY_DAY, 0, 0, 0);
    wait_presence_dnd(&b, ua, 1);

    /* And it CLEARS. A latch would satisfy the line above and be useless. */
    set_schedule(&a, OC_DND_OFF, 0, 0, 0);
    wait_presence_dnd(&b, ua, 0);

    /* The offset is consulted, and SET_TZ_OFFSET reaches the net thread.
     *
     * Allowed 00:00-12:00, with the offset chosen so alice's local clock is at
     * 13:20 — quiet — and then moved so it reads 05:00, which is not. Nothing
     * about the schedule changes between the two: only where she is. Both
     * offsets are computed from the current time so each step is a real
     * transition whatever hour this runs at.
     *
     * The second half is the one that was unwired: SET_TZ_OFFSET returned no
     * result at all, so the net thread went on evaluating against whatever
     * offset AUTH_OK had seeded. */
    set_schedule(&a, OC_DND_EVERY_DAY, offset_for_local(800), 0, 720);
    wait_presence_dnd(&b, ua, 1);
    set_tz(&a, offset_for_local(300));
    wait_presence_dnd(&b, ua, 0);

    /* The pause still works, and works ALONGSIDE the schedule rather than
     * instead of it — the regression this change could most easily cause. */
    set_snooze(&a, 30);
    wait_presence_dnd(&b, ua, 1);
    set_snooze(&a, 0);
    wait_presence_dnd(&b, ua, 0);

    /* Leave the account as it was found: these tests share one daemon. The
     * schedule is already off in effect, so this asserts no further frame —
     * waiting for one would be waiting for a transition that does not happen. */
    set_schedule(&a, OC_DND_OFF, 0, 0, 0);

    client_close(&a);
    client_close(&b);
}

/* Disconnecting mid-upload (ARCH-69). The dangerous shape once blob I/O moved
 * off the net thread: a client vanishes while a chunk write is still with a
 * worker, so the completion arrives for a connection that has already been
 * freed. The handle travels with the job precisely so that path can release it
 * instead of touching freed memory. Under ASan this is what would catch a
 * use-after-free or a double free; without a sanitizer it mostly proves the
 * daemon survives and keeps serving.
 *
 * Repeated, and at several points in the transfer, because the race only opens
 * in the window where a job is actually in flight. */
static void test_upload_abandoned(int port, const uint8_t *pin) {
    uint8_t *fbuf = malloc(OC_MAX_FRAME_SIZE);
    CHECK(fbuf != NULL);
    if (!fbuf) return;

    for (int round = 0; round < 4; round++) {
        client a;
        if (client_open(&a, port, pin) != 0) { CHECK(0); break; }
        if (do_handshake(&a) != 0) { CHECK(0); client_close(&a); break; }
        uint64_t ua = 0;
        if (do_auth(&a, "alice", "pw-alice", &ua) != 0) { CHECK(0); client_close(&a); break; }

        oc_header hdr; oc_rbuf p; oc_wbuf w;
        const size_t total = 200000;              /* several chunks */
        uint8_t idem[OC_IDEM_SIZE];
        memset(idem, (uint8_t)(0xC0 + round), sizeof idem);

        oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
        oc_upload_begin ub = { OC_DEFAULT_CHANNEL, {0}, oc_slice_str("gone.bin"),
                               oc_slice_str("application/octet-stream"), total };
        memcpy(ub.idem, idem, OC_IDEM_SIZE);
        CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub) == OC_OK);
        CHECK(send_frame(&a, fbuf, w.len) == 0);
        CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
        oc_upload_ready urd;
        CHECK(oc_decode_upload_ready(&p, &urd) == OC_OK);

        /* Push `round` chunks, then drop the connection WITHOUT reading the
         * acks — so a write is very likely still in flight server-side. */
        uint8_t *chunk = calloc(1, urd.chunk_size ? urd.chunk_size : 1);
        CHECK(chunk != NULL);
        if (chunk) {
            for (int i = 0; i < round; i++) {
                oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
                oc_upload_chunk uc = { urd.attachment_id, (uint32_t)i,
                                       { chunk, urd.chunk_size } };
                if (oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) != OC_OK) break;
                if (send_frame(&a, fbuf, w.len) != 0) break;
            }
            free(chunk);
        }
        client_close(&a);                          /* vanish mid-transfer */
    }

    /* The daemon must still be healthy: a fresh client completes a normal
     * upload afterwards. This is the real assertion — the loop above only
     * creates the hazard. */
    client b;
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub_id = 0;
    CHECK(do_auth(&b, "alice", "pw-alice", &ub_id) == 0);

    oc_header hdr; oc_rbuf p; oc_wbuf w;
    const size_t small = 1024;
    uint8_t *payload = malloc(small);
    CHECK(payload != NULL);
    if (payload) {
        for (size_t i = 0; i < small; i++) payload[i] = (uint8_t)(i * 7u + 3u);
        uint8_t idem2[OC_IDEM_SIZE]; memset(idem2, 0xD9, sizeof idem2);
        oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
        oc_upload_begin ub2 = { OC_DEFAULT_CHANNEL, {0}, oc_slice_str("after.bin"),
                                oc_slice_str("application/octet-stream"), small };
        memcpy(ub2.idem, idem2, OC_IDEM_SIZE);
        CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub2) == OC_OK);
        CHECK(send_frame(&b, fbuf, w.len) == 0);
        CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
        oc_upload_ready urd2;
        CHECK(oc_decode_upload_ready(&p, &urd2) == OC_OK);

        oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
        oc_upload_chunk uc = { urd2.attachment_id, 0, { payload, small } };
        CHECK(oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) == OC_OK);
        CHECK(send_frame(&b, fbuf, w.len) == 0);
        CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_ACK);

        oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
        oc_upload_end ue = { urd2.attachment_id };
        CHECK(oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue) == OC_OK);
        CHECK(send_frame(&b, fbuf, w.len) == 0);
        CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_OK);
        free(payload);
    }
    client_close(&b);
    free(fbuf);
}

/* An upload cancelled and begun again before the first one's create is answered
 * (REQ-140, ARCH-69). The writer is held, so both creates are queued behind
 * BEGIN, CANCEL, BEGIN; released, the first answer comes back while the second
 * upload awaits its own. It must be taken for what it is -- the cancelled
 * upload's -- and the second upload streams, finishes and downloads under its
 * own attachment: its name, its size. Taken for the second's, the upload would
 * be streaming into the first's row, promised 1000 bytes, and 3000 are refused. */
static void test_upload_restarted(int port, const uint8_t *pin, oc_dbwriter *dbw) {
    client a;
    uint64_t ua = 0;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "alice", "pw-alice", &ua) == 0);
    uint8_t *fbuf = malloc(OC_MAX_FRAME_SIZE), *payload = malloc(3000);
    CHECK(fbuf && payload);
    if (!fbuf || !payload) { free(fbuf); free(payload); client_close(&a); return; }
    for (int i = 0; i < 3000; i++) payload[i] = (uint8_t)(i * 13u + 1u);
    oc_header hdr; oc_rbuf p; oc_wbuf w;

    oc_dbwriter_hold(dbw, OC_DBW_HOLD_ALL);
    oc_upload_begin first = { OC_DEFAULT_CHANNEL, {0}, oc_slice_str("first.bin"), oc_slice_str("application/octet-stream"), 1000 };
    memset(first.idem, 0xA1, OC_IDEM_SIZE);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &first) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    oc_transfer_cancel tc = { 0 };
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_transfer_cancel(&w, OC_PROTOCOL_VERSION, &tc) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    oc_upload_begin second = { OC_DEFAULT_CHANNEL, {0}, oc_slice_str("second.bin"), oc_slice_str("application/octet-stream"), 3000 };
    memset(second.idem, 0xB2, OC_IDEM_SIZE);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &second) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    for (int i = 0; i < 500 && oc_dbwriter_jobs_waiting(dbw, OC_JOB_ATTACH_CREATE) < 2; i++) usleep(10000);
    CHECK(oc_dbwriter_jobs_waiting(dbw, OC_JOB_ATTACH_CREATE) == 2);   /* both asked, neither answered */
    oc_dbwriter_hold(dbw, 0);

    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
    oc_upload_ready rd = { 0 };
    CHECK(oc_decode_upload_ready(&p, &rd) == OC_OK && rd.attachment_id);
    oc_upload_chunk uc = { rd.attachment_id, 0, { payload, 3000 } };
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_ACK);   /* all 3000 taken */
    oc_upload_end ue = { rd.attachment_id };
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_OK);
    oc_upload_ok ok = { 0 };
    CHECK(oc_decode_upload_ok(&p, &ok) == OC_OK && ok.attachment_id == rd.attachment_id && ok.size == 3000);

    /* Stored as the second upload: its name and its size, not the first's. */
    oc_download_begin db = { rd.attachment_id };
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &db) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_DOWNLOAD_INFO);
    oc_download_info di;
    memset(&di, 0, sizeof di);
    CHECK(oc_decode_download_info(&p, &di) == OC_OK && di.total_size == 3000 &&
          di.filename.len == 10 && !memcmp(di.filename.ptr, "second.bin", 10));
    client_close(&a);

    /* The same with a refusal: the cancelled upload's create fails (a channel
     * that does not exist), and its error is not the next upload's -- which is
     * made ready, not ended. */
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "alice", "pw-alice", &ua) == 0);
    oc_dbwriter_hold(dbw, OC_DBW_HOLD_ALL);
    first.channel_id = 987654321;
    memset(first.idem, 0xA3, OC_IDEM_SIZE);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &first) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_transfer_cancel(&w, OC_PROTOCOL_VERSION, &tc) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    memset(second.idem, 0xB4, OC_IDEM_SIZE);
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &second) == OC_OK && send_frame(&a, fbuf, w.len) == 0);
    for (int i = 0; i < 500 && oc_dbwriter_jobs_waiting(dbw, OC_JOB_ATTACH_CREATE) < 2; i++) usleep(10000);
    CHECK(oc_dbwriter_jobs_waiting(dbw, OC_JOB_ATTACH_CREATE) == 2);
    oc_dbwriter_hold(dbw, 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_UPLOAD_READY);
    free(fbuf); free(payload);
    client_close(&a);
}

/* Read-aloud over the wire (REQ-291-295, ARCH-111), with a stub engine in place
 * of the voice model: a message is asked for, rendered, streamed and cached; the
 * second ask is served from the cache without rendering again; a message with
 * nothing to say and one in a channel the asker cannot read are refused. */
static int g_stub_says;                       /* renders the stub actually did */

static void *stub_open(void *ctx, char *err, size_t cap) { (void)ctx; (void)err; (void)cap; static int t; return &t; }
static void stub_close(void *e) { (void)e; }
static const char *stub_voice_id(int v) { return v % 2 == 0 ? "test-voice-m" : "test-voice-f"; }
static const char *stub_voice_label(int v) { return v % 2 == 0 ? "Test Low" : "Test High"; }

static int stub_say(void *e, const char *segment, int voice, float **pcm, size_t *n,
                    char *err, size_t cap) {
    (void)e; (void)voice; (void)err; (void)cap;
    __sync_fetch_and_add(&g_stub_says, 1);
    size_t samples = strlen(segment) * 240;   /* 10 ms a character at 24 kHz */
    *pcm = malloc(samples * sizeof **pcm);
    if (!*pcm) return -1;
    for (size_t i = 0; i < samples; i++) (*pcm)[i] = (float)(0.25 * sin(2 * M_PI * 300.0 * (double)i / 24000.0));
    *n = samples;
    return 0;
}

static const oc_tts_engine STUB_TTS = {
    .version = "itest-stub-1", .lang = "en-US", .rate = 24000, .voices = 2, .ctx = NULL,
    .voice_id = stub_voice_id, .voice_label = stub_voice_label,
    .preview = "A test voice reads this.",
    .open = stub_open, .close = stub_close, .say = stub_say,
};

/* One rendering's download, after whatever request frame asked for it. A message's
 * speech and a voice's audition (message 0) answer identically. Returns the total
 * bytes, or 0 if the daemon refused (with the reason in *code). */
static uint64_t read_audio(client *c, uint64_t message_id, uint32_t *duration_ms, uint16_t *code);

/* Ask for a message's speech and consume the whole stream. */
static uint64_t fetch_audio(client *c, uint64_t message_id, uint32_t *duration_ms, uint16_t *code) {
    uint8_t buf[64];
    oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_audio_get ag = { message_id };
    CHECK(oc_encode_audio_get(&w, OC_PROTOCOL_VERSION, &ag) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
    return read_audio(c, message_id, duration_ms, code);
}

/* Hear voice `voice_id` say the audition sentence (REQ-292). */
static uint64_t fetch_preview(client *c, const char *voice_id, uint32_t *duration_ms, uint16_t *code) {
    uint8_t buf[128];
    oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_voice_preview_get vp = { oc_slice_str(voice_id) };
    CHECK(oc_encode_voice_preview_get(&w, OC_PROTOCOL_VERSION, &vp) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
    return read_audio(c, 0, duration_ms, code);
}

static uint64_t read_audio(client *c, uint64_t message_id, uint32_t *duration_ms, uint16_t *code) {

    oc_header hdr;
    oc_rbuf p;
    *code = 0;
    if (duration_ms) *duration_ms = 0;
    /* A listener is an ordinary connection: a message posted to a channel it is
     * in arrives while it waits for speech, and is not part of this answer. */
    do {
        if (read_frame(c, &hdr, &p) != 0) return 0;
    } while (hdr.msg_type == OC_MSG_BROADCAST);
    if (hdr.msg_type == OC_MSG_ERROR) {
        oc_error e;
        CHECK(oc_decode_error(&p, &e) == OC_OK);
        *code = e.code;
        return 0;
    }
    CHECK(hdr.msg_type == OC_MSG_AUDIO_INFO);
    oc_audio_info ai;
    CHECK(oc_decode_audio_info(&p, &ai) == OC_OK);
    CHECK(ai.message_id == message_id);
    if (duration_ms) *duration_ms = ai.duration_ms;
    uint64_t got = 0;
    uint32_t expect_seq = 0;
    for (;;) {
        CHECK(read_frame(c, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_AUDIO_END) {
            oc_audio_end ae;
            CHECK(oc_decode_audio_end(&p, &ae) == OC_OK && ae.message_id == message_id);
            break;
        }
        CHECK(hdr.msg_type == OC_MSG_AUDIO_CHUNK);
        oc_audio_chunk ac;
        CHECK(oc_decode_audio_chunk(&p, &ac) == OC_OK);
        CHECK(ac.message_id == message_id && ac.seq == expect_seq++);
        got += ac.data.len;
    }
    CHECK(got == ai.total_size);
    return got;
}

/* Send one message and return its id. */
static uint64_t say_something(client *c, uint64_t channel_id, const char *body, uint8_t tag) {
    uint8_t buf[512];
    oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s = {0};
    s.channel_id = channel_id;
    memset(s.idem, tag, OC_IDEM_SIZE);
    s.body = oc_slice_str(body);
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
    uint64_t id = 0;
    for (int i = 0; i < 2; i++) {
        oc_header hdr;
        oc_rbuf p;
        CHECK(read_frame(c, &hdr, &p) == 0);
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack;
            CHECK(oc_decode_send_ack(&p, &ack) == OC_OK);
            id = ack.message_id;
        }
    }
    return id;
}

/* A rendering is served between its render and its row (ARCH-111): with the
 * writer held, every row waits, so each rendering lives only in the net thread's
 * keeping -- and is served from it, for as many renderings as are waiting, not
 * the last few. Seventeen are rendered, more than a sixteen-place ring keeps; the
 * first asked for again renders nothing. Released, the rows are written and it
 * is served from its row, still without rendering. */
static void test_read_aloud_store_window(int port, const uint8_t *pin, oc_dbwriter *dbw) {
    client a;
    uint64_t ua = 0;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "alice", "pw-alice", &ua) == 0);
    enum { N = 17 };
    uint64_t mids[N];
    for (int i = 0; i < N; i++) {
        char text[64]; snprintf(text, sizeof text, "Status line %d is ready for review.", i);
        mids[i] = say_something(&a, OC_DEFAULT_CHANNEL, text, (uint8_t)(0xC0 + i));
        CHECK(mids[i] != 0);
    }
    oc_dbwriter_hold(dbw, OC_DBW_HOLD_WRITER);
    uint16_t code = 0;
    uint64_t first_bytes = 0;
    for (int i = 0; i < N; i++) {
        int before = g_stub_says;
        uint64_t bytes = fetch_audio(&a, mids[i], NULL, &code);
        CHECK(code == 0 && bytes > 0 && g_stub_says == before + 1);    /* each rendered once */
        if (i == 0) first_bytes = bytes;
    }
    CHECK(oc_dbwriter_jobs_waiting(dbw, OC_JOB_TTS_STORE) == N);      /* not one row written */
    int rendered = g_stub_says;
    CHECK(fetch_audio(&a, mids[0], NULL, &code) == first_bytes && code == 0);
    CHECK(g_stub_says == rendered);                                   /* kept, though the oldest */
    oc_dbwriter_hold(dbw, 0);
    for (int i = 0; i < 500 && oc_dbwriter_jobs_waiting(dbw, OC_JOB_TTS_STORE) > 0; i++) usleep(10000);
    CHECK(fetch_audio(&a, mids[0], NULL, &code) == first_bytes && code == 0);
    CHECK(fetch_audio(&a, mids[N - 1], NULL, &code) > 0 && code == 0);
    CHECK(g_stub_says == rendered);                                   /* and from its row after */
    client_close(&a);
}

static void test_read_aloud_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    /* The capability is told at auth, with the stub's voices. */
    {
        client cap;
        CHECK(client_open(&cap, port, pin) == 0);
        CHECK(do_handshake(&cap) == 0);
        uint8_t cbuf[256];
        oc_wbuf cw;
        oc_wbuf_init(&cw, cbuf, sizeof cbuf);
        CHECK(oc_encode_local_credential(&cw, oc_slice_str("bob"), oc_slice_str("pw-bob")) == OC_OK);
        uint8_t buf[512];
        oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_auth au = { OC_AUTH_LOCAL, oc_slice_str("local"), { cbuf, cw.len }, { NULL, 0 } };
        CHECK(oc_encode_auth(&w, OC_PROTOCOL_VERSION, &au) == OC_OK);
        CHECK(write_all(&cap.conn, buf, w.len) == 0);
        oc_header hdr;
        oc_rbuf p;
        int saw = 0, saw_caps = 0;
        for (int i = 0; i < 5 && !saw; i++) {
            CHECK(read_frame_raw(&cap, &hdr, &p) == 0);
            if (hdr.msg_type == OC_MSG_CAPABILITIES) {
                /* Read-aloud and voice input are both running (stub engines),
                 * so both are offered. */
                oc_capabilities cp;
                CHECK(oc_decode_capabilities(&p, &cp) == OC_OK);
                int tts = 0, stt = 0;
                for (uint8_t k = 0; k < cp.count; k++) {
                    if (cp.names[k].len == 3 && memcmp(cp.names[k].ptr, "tts", 3) == 0) tts = 1;
                    if (cp.names[k].len == 3 && memcmp(cp.names[k].ptr, "stt", 3) == 0) stt = 1;
                }
                CHECK(tts == 1 && stt == 1);
                saw_caps = 1;
                continue;
            }
            if (hdr.msg_type != OC_MSG_TTS_INFO) continue;
            CHECK(saw_caps);                      /* the capability comes first */
            oc_tts_info ti;
            CHECK(oc_decode_tts_info(&p, &ti) == OC_OK);
            CHECK(ti.count == 2);
            CHECK(ti.model_version.len == strlen(STUB_TTS.version));
            CHECK(ti.voices[0].id.len == 12 && memcmp(ti.voices[0].id.ptr, "test-voice-m", 12) == 0);
            CHECK(ti.voices[1].label.len == 9 && memcmp(ti.voices[1].label.ptr, "Test High", 9) == 0);
            /* Every voice says what it speaks. Asserted because a stub engine
             * with no language compiles clean and sends empty strings, and
             * every other assertion here would still pass. */
            CHECK(ti.voices[0].lang.len == 5 && memcmp(ti.voices[0].lang.ptr, "en-US", 5) == 0);
            CHECK(ti.voices[1].lang.len == 5 && memcmp(ti.voices[1].lang.ptr, "en-US", 5) == 0);
            saw = 1;
        }
        CHECK(saw);
        client_close(&cap);
    }

    /* The daemon warms one audition per voice when its loop starts, in the
     * background, through this same stub. Everything below counts renders, so let
     * the warming finish first: on a slow runner it otherwise lands between a
     * "before" and an "after" and reads as a cache that does not cache. */
    for (int i = 0; i < 500 && g_stub_says < STUB_TTS.voices; i++) usleep(20000);

    uint64_t mid = say_something(&a, OC_DEFAULT_CHANNEL, "Deploy finished. Logs are clean.", 0xB1);
    CHECK(mid != 0);

    /* First ask renders; the stream is a real audio-only MP4. */
    int before = g_stub_says;
    uint32_t dur = 0;
    uint16_t code = 0;
    uint64_t bytes = fetch_audio(&a, mid, &dur, &code);
    CHECK(code == 0 && bytes > 0 && dur > 0);
    CHECK(g_stub_says > before);

    /* Second ask is served from the cache: the same bytes, no new render. */
    int after_first = g_stub_says;
    uint32_t dur2 = 0;
    uint64_t bytes2 = fetch_audio(&a, mid, &dur2, &code);
    CHECK(code == 0 && bytes2 == bytes && dur2 == dur);
    CHECK(g_stub_says == after_first);

    /* Another listener gets the same rendering, again without rendering. */
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    uint64_t bytes3 = fetch_audio(&b, mid, NULL, &code);
    CHECK(code == 0 && bytes3 == bytes && g_stub_says == after_first);

    /* Nothing to say: emoji alone is not renderable (REQ-294). */
    uint64_t emoji_id = say_something(&a, OC_DEFAULT_CHANNEL, "\xF0\x9F\x9A\x80", 0xB2);
    CHECK(emoji_id != 0);
    CHECK(fetch_audio(&a, emoji_id, NULL, &code) == 0 && code == OC_ERR_NOT_RENDERABLE);

    /* Hearing a voice before choosing it (REQ-292): the audition sentence in that
     * voice, downloaded as message 0. It is ALREADY RENDERED -- the daemon warms
     * one audition per voice at startup, because all of them say the same
     * sentence and waiting two seconds to hear a voice you are choosing is the
     * whole cost of the feature. So the ask costs a cache read and no render. */
    int before_preview = g_stub_says;
    uint32_t pdur = 0;
    uint64_t pbytes = fetch_preview(&a, "test-voice-f", &pdur, &code);
    CHECK(code == 0 && pbytes > 0 && pdur > 0);
    CHECK(g_stub_says == before_preview);
    /* The warming rendered every voice, once: two voices, two renders, and they
     * happened before anybody asked. */
    CHECK(before_preview >= 2);
    /* Another listener gets the same bytes, still without rendering. */
    int after_preview = g_stub_says;
    uint64_t pbytes2 = fetch_preview(&b, "test-voice-f", NULL, &code);
    CHECK(code == 0 && pbytes2 == pbytes && g_stub_says == after_preview);
    /* A different voice is a different rendering -- also already warmed. */
    CHECK(fetch_preview(&a, "test-voice-m", NULL, &code) > 0 && code == 0);
    CHECK(g_stub_says == after_preview);
    /* A voice this daemon does not have is refused, not guessed at. */
    CHECK(fetch_preview(&a, "no-such-voice", NULL, &code) == 0 && code == OC_ERR_TTS_UNAVAILABLE);
    /* A preview asked for while a message's speech is on its way is refused, and
     * the speech still arrives whole: auditioning a voice must not abort a listen.
     * Both requests go in one write so the preview meets the transfer in flight. */
    {
        uint8_t two[256];
        oc_wbuf w2;
        oc_wbuf_init(&w2, two, sizeof two);
        oc_audio_get ag = { mid };
        oc_voice_preview_get vp = { oc_slice_str("test-voice-f") };
        CHECK(oc_encode_audio_get(&w2, OC_PROTOCOL_VERSION, &ag) == OC_OK);
        CHECK(oc_encode_voice_preview_get(&w2, OC_PROTOCOL_VERSION, &vp) == OC_OK);
        CHECK(write_all(&b.conn, two, w2.len) == 0);
        CHECK(read_audio(&b, 0, NULL, &code) == 0 && code == OC_ERR_TRANSFER_PROTOCOL);
        CHECK(read_audio(&b, mid, NULL, &code) == bytes && code == 0);
    }

    /* An unknown message is unknown, not a render. */
    CHECK(fetch_audio(&a, mid + 100000, NULL, &code) == 0 && code == OC_ERR_UNKNOWN_MESSAGE);

    /* A private channel alice is in and bob is not: speech obeys the same gate
     * as reading, because it IS the message (REQ-291). */
    uint8_t buf[256];
    oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("whisper"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    oc_header hdr;
    oc_rbuf p;
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci;
    CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t priv_id = say_something(&a, ci.channel_id, "Only for the room.", 0xB3);
    CHECK(priv_id != 0);
    CHECK(fetch_audio(&b, priv_id, NULL, &code) == 0 && code == OC_ERR_FORBIDDEN);
    /* ...and alice, who is in it, is served. */
    CHECK(fetch_audio(&a, priv_id, NULL, &code) > 0 && code == 0);

    client_close(&a);
    client_close(&b);
}


/* Voice input over the wire (REQ-296-300, ARCH-112), with a stub recognizer in
 * place of the model. The stub reads a code from the first sample and a marker
 * from the second: 1 says "segment <marker> at bob", 2 hears nothing, 3 fails. */
static void *stt_stub_open(void *ctx, char *err, size_t cap) { (void)ctx; (void)err; (void)cap; static int t; return &t; }
static void stt_stub_close(void *e) { (void)e; }
static int stt_stub_hear(void *e, const int16_t *pcm, size_t n, char **text, char *err, size_t cap) {
    (void)e;
    int code = n > 0 ? pcm[0] : 0, marker = n > 1 ? pcm[1] : 0;
    if (code == 3) { snprintf(err, cap, "stub failure"); return -1; }
    char buf[64] = "";
    if (code == 1) snprintf(buf, sizeof buf, "segment %d at bob", marker);
    if (code == 4) {                          /* slow: still being heard when its speaker leaves */
        struct timespec ts = { 0, 300 * 1000000L };
        nanosleep(&ts, NULL);
        snprintf(buf, sizeof buf, "late words %d", marker);
    }
    *text = strdup(buf);
    return *text ? 0 : -1;
}
static const oc_stt_engine STUB_STT = {
    .version = "itest-stt-1", .lang = "en-US", .ctx = NULL,
    .open = stt_stub_open, .close = stt_stub_close, .hear = stt_stub_hear,
};

/* Send one whole segment: BEGIN, the samples in chunks, END. */
static void stt_send(client *c, uint32_t id, uint8_t mode, uint64_t channel, uint64_t root, uint8_t tag,
                     uint32_t samples, int16_t code, int16_t marker) {
    uint8_t *buf = malloc(OC_MAX_FRAME_SIZE);
    CHECK(buf != NULL);
    oc_wbuf w;
    oc_wbuf_init(&w, buf, OC_MAX_FRAME_SIZE);
    oc_stt_begin sb = { id, mode, channel, root, {0}, samples };
    memset(sb.idem, tag, OC_IDEM_SIZE);
    CHECK(oc_encode_stt_begin(&w, OC_PROTOCOL_VERSION, &sb) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
    uint8_t *pcm = calloc((size_t)samples ? samples : 1, 2);
    CHECK(pcm != NULL);
    if (samples > 0) { pcm[0] = (uint8_t)code; pcm[1] = (uint8_t)(code >> 8); }
    if (samples > 1) { pcm[2] = (uint8_t)marker; pcm[3] = (uint8_t)(marker >> 8); }
    uint32_t seq = 0;
    for (size_t off = 0; off < (size_t)samples * 2; off += 60000) {
        size_t len = (size_t)samples * 2 - off < 60000 ? (size_t)samples * 2 - off : 60000;
        oc_wbuf_init(&w, buf, OC_MAX_FRAME_SIZE);
        oc_stt_chunk ch = { id, seq++, { pcm + off, len } };
        CHECK(oc_encode_stt_chunk(&w, OC_PROTOCOL_VERSION, &ch) == OC_OK);
        CHECK(send_frame(c, buf, w.len) == 0);
    }
    oc_wbuf_init(&w, buf, OC_MAX_FRAME_SIZE);
    oc_stt_end se = { id };
    CHECK(oc_encode_stt_end(&w, OC_PROTOCOL_VERSION, &se) == OC_OK);
    CHECK(send_frame(c, buf, w.len) == 0);
    free(pcm);
    free(buf);
}

/* Read until segment `id` is answered: its STT_TEXT (1, text and message id
 * out) or an ERROR naming it (0, the code out). BROADCAST bodies seen on the
 * way are appended to `bodies` (newline-separated) when it is given. */
static int stt_await(client *c, uint32_t id, char *text, size_t tcap, uint64_t *mid, uint16_t *code,
                     char *bodies, size_t bcap) {
    for (int i = 0; i < 40; i++) {
        oc_header hdr;
        oc_rbuf p;
        if (read_frame(c, &hdr, &p) != 0) return -1;
        if (hdr.msg_type == OC_MSG_STT_TEXT) {
            oc_stt_text t;
            CHECK(oc_decode_stt_text(&p, &t) == OC_OK);
            if (t.segment_id != id) continue;
            if (text) snprintf(text, tcap, "%.*s", (int)t.text.len, (const char *)t.text.ptr);
            if (mid) *mid = t.message_id;
            return 1;
        }
        if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error e;
            CHECK(oc_decode_error(&p, &e) == OC_OK);
            if (e.context.len == 4) {
                uint32_t sid = (uint32_t)e.context.ptr[0] << 24 | (uint32_t)e.context.ptr[1] << 16 |
                               (uint32_t)e.context.ptr[2] << 8 | e.context.ptr[3];
                if (sid == id) { if (code) *code = e.code; return 0; }
            }
            continue;
        }
        if (hdr.msg_type == OC_MSG_BROADCAST && bodies) {
            oc_broadcast b;
            if (oc_decode_broadcast(&p, &b) == OC_OK) {
                size_t n = strlen(bodies);
                snprintf(bodies + n, bcap - n, "%.*s\n", (int)b.body.len, (const char *)b.body.ptr);
            }
        }
    }
    return -1;
}

/* Read `c` until `want` BROADCAST bodies have arrived, appending them. */
static void read_broadcasts(client *c, int want, char *bodies, size_t cap) {
    for (int i = 0, got = 0; i < 40 && got < want; i++) {
        oc_header hdr;
        oc_rbuf p;
        CHECK(read_frame(c, &hdr, &p) == 0);
        if (hdr.msg_type != OC_MSG_BROADCAST) continue;
        oc_broadcast b;
        CHECK(oc_decode_broadcast(&p, &b) == OC_OK);
        size_t n = strlen(bodies);
        snprintf(bodies + n, cap - n, "%.*s\n", (int)b.body.len, (const char *)b.body.ptr);
        got++;
    }
}

static void test_voice_input_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    char text[256];
    uint64_t mid = 99;
    uint16_t code = 0;

    /* Offered at auth, with the cap a client cuts speech inside. */
    CHECK(g_auth_stt == 1 && g_auth_stt_max_ms == 30000);

    /* Push to talk: the words come back to the speaker only, with a spoken
     * "at bob" turned into a mention, and nothing is posted. */
    stt_send(&a, 1, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, 0x61, 16000, 1, 5);
    CHECK(stt_await(&a, 1, text, sizeof text, &mid, &code, NULL, 0) == 1);
    CHECK(strcmp(text, "segment 5 @bob") == 0 && mid == 0);

    /* Free talk: two segments sent back to back are posted by the daemon, as
     * alice, in order; everyone gets them as ordinary BROADCASTs and alice's
     * segments close with the message ids. No SEND came from the client. */
    char a_bodies[512] = "", b_bodies[512] = "";
    uint64_t m1 = 0, m2 = 0;
    stt_send(&a, 2, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x62, 8000, 1, 7);
    stt_send(&a, 3, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x63, 8000, 1, 8);
    CHECK(stt_await(&a, 2, text, sizeof text, &m1, &code, a_bodies, sizeof a_bodies) == 1);
    CHECK(strcmp(text, "segment 7 @bob") == 0 && m1 != 0);
    CHECK(stt_await(&a, 3, text, sizeof text, &m2, &code, a_bodies, sizeof a_bodies) == 1);
    CHECK(strcmp(text, "segment 8 @bob") == 0 && m2 > m1);
    read_broadcasts(&b, 2, b_bodies, sizeof b_bodies);
    CHECK(strcmp(b_bodies, "segment 7 @bob\nsegment 8 @bob\n") == 0);

    /* The same token again is the same message, not a second one: a retried
     * segment posts once, exactly as a retried SEND does. */
    uint64_t m3 = 0;
    stt_send(&a, 4, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x62, 8000, 1, 7);
    CHECK(stt_await(&a, 4, text, sizeof text, &m3, &code, NULL, 0) == 1);
    CHECK(m3 == m1);

    /* Nothing heard: answered with no text and no message. */
    stt_send(&a, 5, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x65, 4000, 2, 0);
    CHECK(stt_await(&a, 5, text, sizeof text, &mid, &code, NULL, 0) == 1);
    CHECK(text[0] == 0 && mid == 0);

    /* Recognition failing is a refusal of that segment, not of the connection. */
    stt_send(&a, 6, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, 0x66, 4000, 3, 0);
    CHECK(stt_await(&a, 6, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_STT_UNAVAILABLE);

    /* Over the cap: refused at BEGIN, before any audio. */
    {
        uint8_t buf[128];
        oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_stt_begin sb = { 7, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, {0}, 31u * OC_STT_RATE };
        CHECK(oc_encode_stt_begin(&w, OC_PROTOCOL_VERSION, &sb) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        CHECK(stt_await(&a, 7, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_SEGMENT_TOO_LONG);
    }

    /* A chunk out of order voids the segment. */
    {
        uint8_t buf[128];
        oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_stt_begin sb = { 8, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, {0}, 100 };
        CHECK(oc_encode_stt_begin(&w, OC_PROTOCOL_VERSION, &sb) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        uint8_t two[4] = {0};
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_stt_chunk ch = { 8, 1, { two, sizeof two } };
        CHECK(oc_encode_stt_chunk(&w, OC_PROTOCOL_VERSION, &ch) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        CHECK(stt_await(&a, 8, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_TRANSFER_PROTOCOL);
    }

    /* Free talk into a private channel bob is not in: refused before it is
     * recognized, as a SEND there would be. */
    {
        uint8_t buf[256];
        oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_create_channel cc = { oc_slice_str("hushed"), 0 };
        CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        oc_header hdr;
        oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
        oc_channel_info ci;
        CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
        stt_send(&b, 9, OC_STT_MODE_FREE, ci.channel_id, 0, 0x69, 4000, 1, 1);
        CHECK(stt_await(&b, 9, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_NOT_A_MEMBER);
    }

    /* Free talk into an archived channel: refused as a SEND there is (REQ-035). */
    {
        uint8_t buf[256];
        oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_create_channel cc = { oc_slice_str("stilled"), 1 };
        CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        oc_header hdr;
        oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
        oc_channel_info ci;
        CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
        uint64_t stilled = ci.channel_id;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_update_channel uc = { stilled, OC_CHUP_ARCHIVE, oc_slice_str("") };
        CHECK(oc_encode_update_channel(&w, OC_PROTOCOL_VERSION, &uc) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
        int archived = 0;                      /* committed before anything is spoken there */
        for (int i = 0; i < 20 && !archived; i++) {
            CHECK(read_frame(&a, &hdr, &p) == 0);
            if (hdr.msg_type != OC_MSG_CHANNEL_INFO) continue;
            CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
            archived = ci.channel_id == stilled && ci.archived;
        }
        CHECK(archived);
        stt_send(&a, 10, OC_STT_MODE_FREE, stilled, 0, 0x6A, 4000, 1, 1);
        CHECK(stt_await(&a, 10, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_CHANNEL_ARCHIVED);
    }

    /* A speaker who leaves takes what they said with them: a segment still being
     * heard when its connection closes is never posted. The worker hears in
     * order, so had it been posted it would reach bob before the next one. */
    {
        client c;
        CHECK(client_open(&c, port, pin) == 0);
        CHECK(do_handshake(&c) == 0);
        uint64_t uc = 0;
        CHECK(do_auth(&c, "alice", "pw-alice", &uc) == 0);
        stt_send(&c, 11, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x6B, 4000, 4, 1);
        client_close(&c);
        struct timespec ts = { 0, 100 * 1000000L };
        nanosleep(&ts, NULL);                  /* the close is seen while it is heard */
        char bodies[256] = "";
        stt_send(&a, 12, OC_STT_MODE_FREE, OC_DEFAULT_CHANNEL, 0, 0x6C, 4000, 1, 12);
        CHECK(stt_await(&a, 12, text, sizeof text, &mid, &code, NULL, 0) == 1 && mid != 0);
        read_broadcasts(&b, 1, bodies, sizeof bodies);
        CHECK(strcmp(bodies, "segment 12 @bob\n") == 0);
    }

    client_close(&a);
    client_close(&b);
}

/* OPENCHIME_STT_RATE: segments one connection may send a minute. The window
 * opens at a connection's first segment, so a fresh one sending one more than
 * the limit meets it within the minute whatever the clock says. */
static void test_voice_input_rate(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "carol", "pw", &ua) == 0);
    uint16_t code = 0;
    char text[64];
    for (uint32_t i = 0; i < 60; i++) {
        stt_send(&a, 100 + i, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, 0x70, 1, 2, 0);
        CHECK(stt_await(&a, 100 + i, text, sizeof text, NULL, &code, NULL, 0) == 1);
    }
    stt_send(&a, 160, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, 0x70, 1, 2, 0);
    CHECK(stt_await(&a, 160, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_STT_UNAVAILABLE);
    client_close(&a);
}

/* --- the sign-in pages (AUTH.md §8.10) ---------------------------------------- */

/* One request to the pages over TLS, as a browser sends it; the status, and the
 * whole response in `resp`. `origin` NULL sends none. */
static int web_call(int port, const uint8_t *pin, const char *method, const char *path,
                    const char *origin, const char *ctype, const char *body, char *resp, size_t cap) {
    client h;
    resp[0] = '\0';
    if (http_client_open(&h, port, pin) != 0) return -1;
    char req[8192], ob[160] = "";
    if (origin) snprintf(ob, sizeof ob, "Origin: %s\r\n", origin);
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: web.test\r\n%s%s%s%sContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s", method, path, ob, ctype ? "Content-Type: " : "",
                     ctype ? ctype : "", ctype ? "\r\n" : "", bl, body ? body : "");
    int rc = -1;
    if (n > 0 && (size_t)n < sizeof req && write_all(&h.conn, (const uint8_t *)req, (size_t)n) == 0) {
        http_read_response(&h, resp, cap);
        if (strncmp(resp, "HTTP/1.1 ", 9) == 0) rc = atoi(resp + 9);
    }
    client_close(&h);
    return rc;
}

#define FORM "application/x-www-form-urlencoded"
#define GOOD_ORIGIN "https://web.test"

/* The token a sign-in's 303 carries to the callback, into `tok`. 1 if there. */
static int token_of(const char *resp, char *tok, size_t cap) {
    const char *l = strstr(resp, "\r\nLocation: http://127.0.0.1:5/cb?token=");
    if (!l) return 0;
    l += strlen("\r\nLocation: http://127.0.0.1:5/cb?token=");
    size_t n = strcspn(l, "\r\n");
    if (n == 0 || n >= cap) return 0;
    memcpy(tok, l, n); tok[n] = '\0';
    return 1;
}

/* Present a token on the AUTH path, as the client does after the browser: 0 and
 * the user on AUTH_OK, else the ERROR's code. */
static int token_auth(int port, const uint8_t *pin, const char *tok, const char *verifier, uint64_t *uid) {
    client a;
    if (client_open(&a, port, pin) != 0 || do_handshake(&a) != 0) return -1;
    uint8_t buf[4096]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth au = { OC_AUTH_OIDC, oc_slice_str("local"), oc_slice_str(tok), oc_slice_str(verifier) };
    int rc = -1;
    oc_header hdr; oc_rbuf p;
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &au) == OC_OK && write_all(&a.conn, buf, w.len) == 0 &&
        read_frame_raw(&a, &hdr, &p) == 0) {
        if (hdr.msg_type == OC_MSG_AUTH_OK) {
            oc_auth_ok ok;
            if (oc_decode_auth_ok(&p, &ok) == OC_OK) { *uid = ok.user_id; rc = 0; }
        } else if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error e;
            if (oc_decode_error(&p, &e) == OC_OK) rc = e.code;
        }
    }
    client_close(&a);
    return rc;
}

/* A sign-in on the pages, for `user`: the status, and the token if one came. */
static int web_signin(int port, const uint8_t *pin, const char *user, const char *pw, const char *challenge,
                      char *tok, size_t cap, char *resp, size_t rcap) {
    char body[512];
    snprintf(body, sizeof body, "redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s&username=%s&password=%s",
             challenge, user, pw);
    int st = web_call(port, pin, "POST", "/signin", GOOD_ORIGIN, FORM, body, resp, rcap);
    if (tok) { tok[0] = '\0'; token_of(resp, tok, cap); }
    return st;
}

/* The pages end to end, on a daemon of their own with the test knob OFF -- the
 * product as shipped: a password is taken only by a page, which ends with an ID
 * token the client presents on AUTH. */
static void test_web_signin(int port) {
    const char *knob = getenv("OPENCHIME_TEST_PASSWORD_AUTH");
    char saved[8] = "";
    if (knob) snprintf(saved, sizeof saved, "%s", knob);
    unsetenv("OPENCHIME_TEST_PASSWORD_AUTH");
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin) == 0);
    unlink("build/itest_web.db"); unlink("build/itest_web.db-wal"); unlink("build/itest_web.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_web.db");
    CHECK(dbw2 != NULL);
    if (!dbw2) { oc_tls_server_free(&srv2); goto restore; }
    CHECK(!oc_dbwriter_password_frames(dbw2) && oc_dbwriter_local_browser(dbw2));
    uint8_t setup[OC_INVITE_TOKEN_LEN];
    CHECK(oc_dbwriter_setup_invite(dbw2, setup) == 1);
    /* The members, made before the loop runs: the helper takes the writer's
     * next result, which is the loop's once it does. */
    uint64_t mia = oc_dbwriter_register_local(dbw2, "mia", "pw-mia", OC_ROLE_MEMBER, 2048);
    CHECK(mia != 0);
    CHECK(oc_dbwriter_register_local(dbw2, "pat", "pw-old", OC_ROLE_MEMBER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw2, "zed", "pw-zed", OC_ROLE_MEMBER, 2048) != 0);
    char setup_hex[2 * OC_INVITE_TOKEN_LEN + 1];
    for (size_t i = 0; i < sizeof setup; i++) snprintf(setup_hex + 2 * i, 3, "%02x", setup[i]);
    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    char resp[16384], tok[2048], body[1024], ver[OC_SIGNIN_VERIFIER_LEN + 1], ch[OC_SIGNIN_CHALLENGE_LEN + 1];
    CHECK(oc_signin_verifier(ver, ch) == 0);
    char path[512];

    /* The form, with the headers every page carries: nothing framed, cached or
     * run, and forms posted only to itself and the client's callback. */
    snprintf(path, sizeof path, "/signin?redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s", ch);
    CHECK(web_call(port, pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "Content-Security-Policy: default-src 'none'") != NULL);
    CHECK(strstr(resp, "form-action 'self' http://127.0.0.1:5\r\n") != NULL);
    CHECK(strstr(resp, "X-Frame-Options: DENY") && strstr(resp, "Cache-Control: no-store") &&
          strstr(resp, "Referrer-Policy: no-referrer"));
    CHECK(strstr(resp, "autocomplete=\"current-password\"") != NULL);
    CHECK(strstr(resp, "action=\"signin\"") != NULL);                    /* relative: direct or tunnel */
    /* Not a sign-in's link: the redirect is not loopback, or there is no challenge. */
    CHECK(web_call(port, pin, "GET", "/signin?redirect_uri=https%3A%2F%2Fevil.example%2Fcb&nonce=x", NULL, NULL,
                   NULL, resp, sizeof resp) == 400);
    snprintf(path, sizeof path, "/signin?redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=short");
    CHECK(web_call(port, pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 400);
    /* What the page puts back is escaped. */
    snprintf(path, sizeof path, "/signin?redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s"
             "&username=%%22%%3E%%3Cscript%%3E", ch);
    CHECK(web_call(port, pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "<script>") == NULL && strstr(resp, "&quot;&gt;&lt;script&gt;") != NULL);

    /* A sign-in link carrying an invitation opens sign-up; the first owner signs
     * up with the setup token and goes on signed in. */
    snprintf(path, sizeof path, "/signin?redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s&invite=%s",
             ch, setup_hex);
    CHECK(web_call(port, pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "name=\"confirm\"") != NULL && strstr(resp, setup_hex) != NULL);
    snprintf(body, sizeof body, "redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s&invite=%s"
             "&username=wes&password=pw-wes&confirm=pw-other", ch, setup_hex);
    CHECK(web_call(port, pin, "POST", "/signup", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "don&#39;t match") != NULL);
    snprintf(body, sizeof body, "redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s&invite=%s"
             "&username=wes&password=pw-wes&confirm=pw-wes", ch, setup_hex);
    CHECK(web_call(port, pin, "POST", "/signup", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 303);
    CHECK(token_of(resp, tok, sizeof tok));
    uint64_t wes = 0;
    CHECK(token_auth(port, pin, tok, ver, &wes) == 0 && wes != 0);
    /* Once only: the token, and the invitation. */
    uint64_t again = 0;
    CHECK(token_auth(port, pin, tok, ver, &again) == OC_ERR_AUTH_INVALID_TOKEN);
    CHECK(web_call(port, pin, "POST", "/signup", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "invitation isn&#39;t valid") != NULL);

    /* A sign-in: the token names the account by id, and is good only with the
     * verifier whose hash it was bound to. */
    CHECK(web_signin(port, pin, "mia", "wrong", ch, tok, sizeof tok, resp, sizeof resp) == 200);
    CHECK(!tok[0] && strstr(resp, "username or password isn&#39;t right") != NULL);
    CHECK(web_signin(port, pin, "mia", "pw-mia", ch, tok, sizeof tok, resp, sizeof resp) == 303 && tok[0]);
    uint64_t got = 0;
    CHECK(token_auth(port, pin, tok, "not-the-verifier-of-this-challenge-at-all", &got) ==
          OC_ERR_AUTH_INVALID_TOKEN);
    CHECK(token_auth(port, pin, tok, ver, &got) == 0 && got == mia);    /* not spent by the refusal */

    /* A token this daemon did not sign, and one past its time, are refused. */
    {
        char *k = NULL, *iss = NULL;
        CHECK(oc_local_issuer_generate(&k, &iss) == 0);
        sqlite3 *db = NULL;
        char real_iss[128] = "";
        if (sqlite3_open("build/itest_web.db", &db) == SQLITE_OK) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, "SELECT issuer FROM local_issuer WHERE id=1;", -1, &st, NULL) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW)
                snprintf(real_iss, sizeof real_iss, "%s", (const char *)sqlite3_column_text(st, 0));
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
        CHECK(real_iss[0] != '\0');
        oc_local_issuer *other = oc_local_issuer_open(k, real_iss);
        CHECK(other != NULL);
        char *forged = other ? oc_local_issuer_mint(other, mia, ch, (uint64_t)time(NULL)) : NULL;
        CHECK(forged && token_auth(port, pin, forged, ver, &got) == OC_ERR_AUTH_INVALID_TOKEN);
        free(forged);
        oc_local_issuer_close(other);
        free(k); free(iss);
    }

    /* Only a post from the page itself: no Origin, another site's, or not a form. */
    snprintf(body, sizeof body, "redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5%%2Fcb&nonce=%s&username=mia&password=pw-mia", ch);
    CHECK(web_call(port, pin, "POST", "/signin", NULL, FORM, body, resp, sizeof resp) == 403);
    CHECK(web_call(port, pin, "POST", "/signin", "https://evil.example", FORM, body, resp, sizeof resp) == 403);
    CHECK(web_call(port, pin, "POST", "/signin", "http://web.test", FORM, body, resp, sizeof resp) == 403);
    CHECK(web_call(port, pin, "POST", "/signin", GOOD_ORIGIN, "application/json", body, resp, sizeof resp) == 403);
    /* ...and only to a loopback callback. */
    snprintf(body, sizeof body, "redirect_uri=https%%3A%%2F%%2Fevil.example%%2Fcb&nonce=%s&username=mia&password=pw-mia", ch);
    CHECK(web_call(port, pin, "POST", "/signin", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 400);

    /* A password change on its page: the old one stops working, the new one works. */
    CHECK(web_call(port, pin, "GET", "/account/password", NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "autocomplete=\"new-password\"") != NULL);
    CHECK(web_call(port, pin, "POST", "/account/password", GOOD_ORIGIN, FORM,
                   "username=pat&current=pw-bad&password=pw-new&confirm=pw-new", resp, sizeof resp) == 200);
    CHECK(strstr(resp, "current password isn&#39;t right") != NULL);
    CHECK(web_call(port, pin, "POST", "/account/password", GOOD_ORIGIN, FORM,
                   "username=pat&current=pw-old&password=pw-new&confirm=pw-new", resp, sizeof resp) == 200);
    CHECK(strstr(resp, "Password changed") != NULL);
    CHECK(web_signin(port, pin, "pat", "pw-old", ch, tok, sizeof tok, resp, sizeof resp) == 200 && !tok[0]);
    CHECK(web_signin(port, pin, "pat", "pw-new", ch, tok, sizeof tok, resp, sizeof resp) == 303 && tok[0]);

    /* A removed member is refused, whatever they type. */
    {
        sqlite3 *db = NULL;
        CHECK(sqlite3_open("build/itest_web.db", &db) == SQLITE_OK);
        sqlite3_busy_timeout(db, 5000);
        char sql[96];
        snprintf(sql, sizeof sql, "UPDATE users SET disabled=1 WHERE id=%llu;", (unsigned long long)mia);
        CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(db);
    }
    CHECK(web_signin(port, pin, "mia", "pw-mia", ch, tok, sizeof tok, resp, sizeof resp) == 200 && !tok[0]);

    /* Outside the knob, a password in a frame is refused: AUTH local,
     * REDEEM_INVITE, and CHANGE_PASSWORD from a signed-in connection. */
    {
        client a;
        CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0);
        uint64_t u = 0;
        CHECK(do_auth(&a, "pat", "pw-new", &u) != 0);
        client_close(&a);
        client b;
        CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0);
        uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_redeem_invite ri = { oc_slice_str(setup_hex), oc_slice_str("eve"), oc_slice_str("pw-eve") };
        CHECK(oc_encode_redeem_invite(&w, OC_PROTOCOL_VERSION, &ri) == OC_OK && write_all(&b.conn, buf, w.len) == 0);
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame_raw(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
        client_close(&b);
    }

    /* The per-account limiter stands in front of the page as of the frame. */
    for (int i = 0; i < 5; i++)
        CHECK(web_signin(port, pin, "zed", "wrong", ch, tok, sizeof tok, resp, sizeof resp) == 200);
    CHECK(web_signin(port, pin, "zed", "pw-zed", ch, tok, sizeof tok, resp, sizeof resp) == 429 && !tok[0]);
    CHECK(strstr(resp, "Too many attempts") != NULL);

    stop_loop(&arg2, th2);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    unlink("build/itest_web.db"); unlink("build/itest_web.db-wal"); unlink("build/itest_web.db-shm");
restore:
    if (saved[0]) setenv("OPENCHIME_TEST_PASSWORD_AUTH", saved, 1);
}

/* --- device codes (AUTH.md §8.11) -------------------------------------------- */

/* AUTH_DEVICE_BEGIN on `a`: 0 with the codes, else the ERROR's code. */
static int device_begin(client *a, const char *challenge, char *dcode, size_t dcap, char *ucode, size_t ucap,
                        unsigned *interval, unsigned *expires, char *path, size_t pcap) {
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth_device_begin b = { oc_slice_str("local"), oc_slice_str(challenge) };
    if (oc_encode_auth_device_begin(&w, OC_PROTOCOL_VERSION, &b) != OC_OK || write_all(&a->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame_raw(a, &hdr, &p) != 0) return -1;
    if (hdr.msg_type == OC_MSG_ERROR) { oc_error e; return oc_decode_error(&p, &e) == OC_OK ? e.code : -1; }
    oc_auth_device d;
    if (hdr.msg_type != OC_MSG_AUTH_DEVICE || oc_decode_auth_device(&p, &d) != OC_OK) return -1;
    snprintf(dcode, dcap, "%.*s", (int)d.device_code.len, (const char *)d.device_code.ptr);
    snprintf(ucode, ucap, "%.*s", (int)d.user_code.len, (const char *)d.user_code.ptr);
    snprintf(path, pcap, "%.*s", (int)d.verification_path.len, (const char *)d.verification_path.ptr);
    *interval = d.interval_s; *expires = d.expires_in_s;
    return 0;
}

/* AUTH_DEVICE_POLL: 0 with the token, else the ERROR's code (its message in `why`). */
static int device_poll(client *a, const char *dcode, char *tok, size_t cap, char *why, size_t wcap) {
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth_device_poll dp = { oc_slice_str(dcode) };
    if (oc_encode_auth_device_poll(&w, OC_PROTOCOL_VERSION, &dp) != OC_OK || write_all(&a->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame_raw(a, &hdr, &p) != 0) return -1;
    if (hdr.msg_type == OC_MSG_ERROR) {
        oc_error e;
        if (oc_decode_error(&p, &e) != OC_OK) return -1;
        if (why) snprintf(why, wcap, "%.*s", (int)e.message.len, (const char *)e.message.ptr);
        return e.code;
    }
    oc_auth_device_token t;
    if (hdr.msg_type != OC_MSG_AUTH_DEVICE_TOKEN || oc_decode_auth_device_token(&p, &t) != OC_OK) return -1;
    snprintf(tok, cap, "%.*s", (int)t.token.len, (const char *)t.token.ptr);
    return 0;
}

static int user_code_shape(const char *u) {
    if (strlen(u) != 9 || u[4] != '-') return 0;
    for (int i = 0; i < 9; i++) if (i != 4 && !strchr("BCDFGHJKLMNPQRSTVWXZ", u[i])) return 0;
    return 1;
}

/* A daemon of its own for device codes, knob off; `ttl` the codes' life (0 the
 * default). */
struct dev_daemon { oc_tls_server srv; uint8_t pin[OC_TLS_FINGERPRINT_LEN]; oc_dbwriter *dbw;
                    struct loop_arg arg; pthread_t th; uint64_t dee; };
static int dev_daemon_start(struct dev_daemon *d, int port, uint64_t ttl) {
    memset(d, 0, sizeof *d);
    const char *knob = getenv("OPENCHIME_TEST_PASSWORD_AUTH");
    int had = knob != NULL;
    unsetenv("OPENCHIME_TEST_PASSWORD_AUTH");
    if (oc_tls_server_init(&d->srv, NULL, NULL) != 0 || oc_tls_server_fingerprint(&d->srv, d->pin) != 0) return -1;
    unlink("build/itest_dev.db"); unlink("build/itest_dev.db-wal"); unlink("build/itest_dev.db-shm");
    d->dbw = oc_dbwriter_start("build/itest_dev.db");
    if (had) setenv("OPENCHIME_TEST_PASSWORD_AUTH", "1", 1);
    if (!d->dbw) return -1;
    d->dee = oc_dbwriter_register_local(d->dbw, "dee", "pw-dee", OC_ROLE_MEMBER, 2048);
    oc_netloop_set_device_ttl_ms(ttl);
    d->arg.port = port; d->arg.srv = &d->srv; d->arg.dbw = d->dbw; d->arg.stop = 0;
    return pthread_create(&d->th, NULL, loop_thread, &d->arg) == 0 && d->dee ? 0 : -1;
}
static void dev_daemon_stop(struct dev_daemon *d) {
    stop_loop(&d->arg, d->th);
    oc_netloop_set_device_ttl_ms(0);
    oc_dbwriter_stop(d->dbw);
    oc_tls_server_free(&d->srv);
    unlink("build/itest_dev.db"); unlink("build/itest_dev.db-wal"); unlink("build/itest_dev.db-shm");
}

/* A terminal signs in with a code: asked for on the protocol, entered and
 * approved on the /device page, collected by a poll, presented on AUTH. */
static void test_device_signin(int port) {
    struct dev_daemon d;
    CHECK(dev_daemon_start(&d, port, 0) == 0);
    const uint8_t *pin = d.pin;
    char ver[OC_SIGNIN_VERIFIER_LEN + 1], ch[OC_SIGNIN_CHALLENGE_LEN + 1];
    CHECK(oc_signin_verifier(ver, ch) == 0);
    char dcode[64], ucode[16], vpath[32], tok[2048], why[64], resp[16384], body[512], path[160];
    unsigned interval = 0, expires = 0;

    client a;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0);
    CHECK(device_begin(&a, ch, dcode, sizeof dcode, ucode, sizeof ucode, &interval, &expires, vpath, sizeof vpath) == 0);
    CHECK(strlen(dcode) == OC_DEVICE_CODE_LEN && user_code_shape(ucode) && strcmp(vpath, "/device") == 0);
    CHECK(interval == 5 && expires == 600);
    /* Not yet; and a poll sooner than the interval makes it longer. */
    CHECK(device_poll(&a, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_PENDING);
    CHECK(device_poll(&a, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_SLOW_DOWN);
    CHECK(strcmp(why, "poll every 10 seconds") == 0);
    /* The code a person sees does not collect the token. */
    CHECK(device_poll(&a, ucode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_EXPIRED);

    /* The page: a form for the code; then, for the code -- typed in lower case,
     * without its dash -- who is asking and from where. */
    CHECK(web_call(port, pin, "GET", "/device", NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "name=\"code\"") && strstr(resp, "Content-Security-Policy: default-src 'none'"));
    char typed[16];
    snprintf(typed, sizeof typed, "%.4s%.4s", ucode, ucode + 5);
    for (char *t = typed; *t; t++) *t = (char)(*t - 'A' + 'a');
    snprintf(path, sizeof path, "/device?code=%s", typed);
    CHECK(web_call(port, pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 200);
    CHECK(strstr(resp, ucode) && strstr(resp, "from <b>127.0.0.1</b>") && strstr(resp, "Only go on if that was you"));
    /* Approve: a wrong password shows the form again; the right one signs the
     * terminal in, and its next poll collects the token. */
    snprintf(body, sizeof body, "code=%s&username=dee&password=wrong&action=approve", ucode);
    CHECK(web_call(port, pin, "POST", "/device", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "isn&#39;t right") && strstr(resp, "from <b>127.0.0.1</b>"));
    CHECK(device_poll(&a, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_PENDING ||
          strcmp(why, "poll every 15 seconds") == 0);
    snprintf(body, sizeof body, "code=%s&username=dee&password=pw-dee&action=approve", ucode);
    CHECK(web_call(port, pin, "POST", "/device", NULL, FORM, body, resp, sizeof resp) == 403);   /* Origin */
    CHECK(web_call(port, pin, "POST", "/device", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "Go back to your terminal") != NULL);
    CHECK(device_poll(&a, dcode, tok, sizeof tok, why, sizeof why) == 0 && tok[0]);
    CHECK(device_poll(&a, dcode, tok + 1024, sizeof tok - 1024, why, sizeof why) == OC_ERR_AUTH_EXPIRED);   /* once */
    client_close(&a);
    uint64_t uid = 0;
    CHECK(token_auth(port, pin, tok, "not-the-verifier-of-this-challenge-at-all", &uid) == OC_ERR_AUTH_INVALID_TOKEN);
    CHECK(token_auth(port, pin, tok, ver, &uid) == 0 && uid == d.dee);

    /* "That wasn't me": the terminal is told, once. */
    client b;
    CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0);
    CHECK(device_begin(&b, ch, dcode, sizeof dcode, ucode, sizeof ucode, &interval, &expires, vpath, sizeof vpath) == 0);
    snprintf(body, sizeof body, "code=%s&action=deny", ucode);
    CHECK(web_call(port, pin, "POST", "/device", GOOD_ORIGIN, FORM, body, resp, sizeof resp) == 200);
    CHECK(strstr(resp, "Sign-in refused") != NULL);
    CHECK(device_poll(&b, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_DENIED);
    CHECK(device_poll(&b, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_EXPIRED);
    /* One source keeps at most five waiting. */
    for (int i = 0; i < 5; i++)
        CHECK(device_begin(&b, ch, dcode, sizeof dcode, ucode, sizeof ucode, &interval, &expires, vpath, sizeof vpath) == 0);
    CHECK(device_begin(&b, ch, dcode, sizeof dcode, ucode, sizeof ucode, &interval, &expires, vpath, sizeof vpath) ==
          OC_ERR_AUTH_RATE_LIMITED);
    client_close(&b);
    /* Codes cannot be walked: ten wrong lookups a minute, then refused. */
    for (int i = 0; i < 10; i++)
        CHECK(web_call(port, pin, "GET", "/device?code=BBBB-BBBB", NULL, NULL, NULL, resp, sizeof resp) == 200 &&
              strstr(resp, "isn&#39;t one we&#39;re waiting for"));
    CHECK(web_call(port, pin, "GET", "/device?code=BBBB-BBBB", NULL, NULL, NULL, resp, sizeof resp) == 429);
    dev_daemon_stop(&d);

    /* A code's life runs out. */
    CHECK(dev_daemon_start(&d, port + 1, 400) == 0);
    client e;
    CHECK(client_open(&e, port + 1, d.pin) == 0 && do_handshake(&e) == 0);
    CHECK(device_begin(&e, ch, dcode, sizeof dcode, ucode, sizeof ucode, &interval, &expires, vpath, sizeof vpath) == 0);
    usleep(600 * 1000);
    CHECK(device_poll(&e, dcode, tok, sizeof tok, why, sizeof why) == OC_ERR_AUTH_EXPIRED);
    snprintf(path, sizeof path, "/device?code=%s", ucode);
    CHECK(web_call(port + 1, d.pin, "GET", path, NULL, NULL, NULL, resp, sizeof resp) == 200 &&
          strstr(resp, "isn&#39;t one we&#39;re waiting for"));
    client_close(&e);
    dev_daemon_stop(&d);
}

/* Voice input absent (REQ-300): turned off by the operator, or with no engine --
 * which is how a daemon whose recognizer data is missing or does not match runs
 * (main.c hands the loop none). Either way it is not offered, a segment is
 * refused, and the rest of the daemon serves on. Its own loop, run after the
 * shared one has stopped: the recognizer worker is the loop's. */
static void test_voice_input_absent(int port, int by_env) {
    if (by_env) setenv("OPENCHIME_STT", "0", 1);
    else        oc_netloop_set_stt(NULL);
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);
    unlink("build/itest_stt_off.db");
    unlink("build/itest_stt_off.db-wal");
    unlink("build/itest_stt_off.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_stt_off.db");
    CHECK(dbw2 != NULL);
    CHECK(oc_dbwriter_register_local(dbw2, "alice", "pw-alice", OC_ROLE_OWNER, 2048) != 0);
    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    client a;
    CHECK(client_open(&a, port, pin2) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(g_auth_stt == 0 && g_auth_stt_max_ms == 0);
    uint16_t code = 0;
    stt_send(&a, 1, OC_STT_MODE_PTT, OC_DEFAULT_CHANNEL, 0, 0x71, 1600, 1, 1);
    CHECK(stt_await(&a, 1, NULL, 0, NULL, &code, NULL, 0) == 0 && code == OC_ERR_STT_UNAVAILABLE);
    /* The chunks and end of the refused segment are not an error of their own:
     * the connection still sends and is answered. */
    uint8_t buf[128];
    oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s = {0};
    s.channel_id = OC_DEFAULT_CHANNEL;
    memset(s.idem, 0x72, OC_IDEM_SIZE);
    s.body = oc_slice_str("typed instead");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    int acked = 0;
    for (int i = 0; i < 20 && !acked; i++) {
        oc_header hdr;
        oc_rbuf p;
        CHECK(read_frame(&a, &hdr, &p) == 0);
        acked = hdr.msg_type == OC_MSG_SEND_ACK;
    }
    CHECK(acked);
    client_close(&a);

    stop_loop(&arg2, th2);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    if (by_env) unsetenv("OPENCHIME_STT");
    else        oc_netloop_set_stt(&STUB_STT);
    unlink("build/itest_stt_off.db");
    unlink("build/itest_stt_off.db-wal");
    unlink("build/itest_stt_off.db-shm");
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
    /* Hex, and usable in the URL as-is: /webhook/<hex-token> is what the HTTP
     * endpoint has always parsed, so handing a client raw bytes meant the value
     * it showed could never have worked. This test used to do the conversion
     * itself, which is precisely the step no real frontend performed. */
    CHECK(wi.channel_id == OC_DEFAULT_CHANNEL && wi.token.len == 64);
    char hex[65];
    memcpy(hex, wi.token.ptr, 64); hex[64] = '\0';

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
/* One request answers with ALL of a user's notification state, in a fixed order:
 * NOTIFY_PREFS, then the pause, the schedule and the alert lists (REQ-135/136/278).
 * A test that read only the first would pass while three frames piled up in the
 * stream and desynchronised everything after it — which is exactly what happened. */
static void read_notify_tail(client *c) {
    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SNOOZE);
    { oc_snooze sn; CHECK(oc_decode_snooze(&p, &sn) == OC_OK); }
    CHECK(read_frame(c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SCHEDULE);
    { oc_schedule sc; oc_schedule_day days[OC_SCHEDULE_DAYS];
      CHECK(oc_decode_schedule(&p, &sc, days) == OC_OK); }
    CHECK(read_frame(c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ALERT_PREFS);
    { oc_slice t[OC_MAX_KEYWORDS]; uint8_t nt = 0; uint64_t pe[OC_MAX_PRIORITY]; uint8_t np = 0;
      CHECK(oc_decode_alert_prefs(&p, t, OC_MAX_KEYWORDS, &nt, pe, OC_MAX_PRIORITY, &np) == OC_OK); }
}

static void test_notify_prefs_vertical(int port, const uint8_t *pin) {
    client a1, a2;                       /* same user, two devices */
    CHECK(client_open(&a1, port, pin) == 0); CHECK(do_handshake(&a1) == 0);
    CHECK(client_open(&a2, port, pin) == 0); CHECK(do_handshake(&a2) == 0);
    uint64_t ua = 0, ua2 = 0;
    CHECK(do_auth(&a1, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&a2, "alice", "pw-alice", &ua2) == 0);
    CHECK(ua == ua2);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w;
    oc_notify_pref_entry ents[16]; uint16_t n;

    /* Set a channel level on device 1 -> both devices get the snapshot. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_notify_pref sp = { OC_DEFAULT_CHANNEL, OC_NOTIFY_MENTIONS };
    CHECK(oc_encode_set_notify_pref(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    client *devs[2] = { &a1, &a2 };
    for (int d = 0; d < 2; d++) {
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_NOTIFY_PREFS);
        CHECK(oc_decode_notify_prefs(&p, ents, 16, &n, NULL) == OC_OK && n == 1);
        CHECK(ents[0].channel_id == OC_DEFAULT_CHANNEL && ents[0].level == OC_NOTIFY_MENTIONS);
        /* The rest of the notification state travels with it, each in its own
         * frame — asserted rather than skipped, because "the snapshot arrived"
         * and "the snapshot arrived complete" are different claims. */
        read_notify_tail(devs[d]);
    }

    /* Set the SCHEDULE on device 2 -> both devices get it, in its own frame
     * (REQ-136). The window states the hours notifications are ALLOWED, which is
     * the opposite sense from the window it replaced — the reason SET_DND was
     * retired rather than redefined. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_schedule_day cust[2] = { { 1, 1, 540, 1020 }, { 5, 0, 0, 0 } };  /* Mon 09:00-17:00, Fri off */
    oc_schedule sc_in = { OC_DND_CUSTOM, -300, 480, 1320, 2, cust };
    CHECK(oc_encode_set_schedule(&w, OC_PROTOCOL_VERSION, &sc_in) == OC_OK);
    CHECK(send_frame(&a2, buf, w.len) == 0);
    for (int d = 0; d < 2; d++) {
        oc_schedule sc; oc_schedule_day days[OC_SCHEDULE_DAYS];
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SCHEDULE);
        CHECK(oc_decode_schedule(&p, &sc, days) == OC_OK);
        CHECK(sc.mode == OC_DND_CUSTOM && sc.tz_offset_min == -300);
        CHECK(sc.start_min == 480 && sc.end_min == 1320 && sc.count == 2);
        CHECK(days[0].weekday == 1 && days[0].enabled == 1 &&
              days[0].start_min == 540 && days[0].end_min == 1020);
        CHECK(days[1].weekday == 5 && days[1].enabled == 0);
    }

    /* The global default (REQ-134) rides the same snapshot and syncs the same way:
     * a client must never have to infer what "no per-channel row" means. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_notify_default nd = { OC_NOTIFY_NONE };
    CHECK(oc_encode_set_notify_default(&w, OC_PROTOCOL_VERSION, &nd) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    for (int d = 0; d < 2; d++) {
        uint8_t dflt = 0xFF;
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_NOTIFY_PREFS);
        CHECK(oc_decode_notify_prefs(&p, ents, 16, &n, &dflt) == OC_OK);
        CHECK(dflt == OC_NOTIFY_NONE);
        /* ... and the per-channel override is still there, untouched. */
        CHECK(n == 1 && ents[0].level == OC_NOTIFY_MENTIONS);
        read_notify_tail(devs[d]);
    }

    /* --- pausing (REQ-278) ----------------------------------------
     * The pause is the OTHER mechanism, and the vertical is what proves they
     * are separate: setting one must not disturb the other, both of the user's
     * devices learn the instant, and the other user learns only the fact. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_snooze ss = { 30 };
    CHECK(oc_encode_set_snooze(&w, OC_PROTOCOL_VERSION, &ss) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    uint64_t until = 0;
    for (int d = 0; d < 2; d++) {
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SNOOZE);
        oc_snooze sn; CHECK(oc_decode_snooze(&p, &sn) == OC_OK);
        CHECK(sn.until_ms > 0);
        if (d == 0) until = sn.until_ms; else CHECK(sn.until_ms == until);
    }

    /* Ending it early is the same op with 0 minutes — and the schedule set
     * above is untouched by either act, which is the whole point of naming the
     * two mechanisms apart. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_set_snooze off = { 0 };
    CHECK(oc_encode_set_snooze(&w, OC_PROTOCOL_VERSION, &off) == OC_OK);
    CHECK(send_frame(&a2, buf, w.len) == 0);
    for (int d = 0; d < 2; d++) {
        CHECK(read_frame(devs[d], &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SNOOZE);
        oc_snooze sn; CHECK(oc_decode_snooze(&p, &sn) == OC_OK && sn.until_ms == 0);
    }
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_notify_prefs(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    CHECK(read_frame(&a1, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_NOTIFY_PREFS);
    CHECK(oc_decode_notify_prefs(&p, ents, 16, &n, NULL) == OC_OK);
    /* The snapshot carries the schedule beside it, still as it was set: a pause
     * and a schedule are two mechanisms, and neither act touched the other. */
    { oc_schedule sc; oc_schedule_day days[OC_SCHEDULE_DAYS];
      CHECK(read_frame(&a1, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SNOOZE);
      { oc_snooze sn0; CHECK(oc_decode_snooze(&p, &sn0) == OC_OK && sn0.until_ms == 0); }
      CHECK(read_frame(&a1, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SCHEDULE);
      CHECK(oc_decode_schedule(&p, &sc, days) == OC_OK);
      CHECK(sc.mode == OC_DND_CUSTOM && sc.count == 2 && sc.tz_offset_min == -300);
      CHECK(read_frame(&a1, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ALERT_PREFS); }

    client_close(&a1);
    client_close(&a2);
}

/* The next frame of `type`, passing over anything else the daemon sends meanwhile
 * (CALL_STATE fan-outs, presence, a missed-call BROADCAST). */
static int read_type(client *c, uint16_t type, oc_header *hdr, oc_rbuf *p) {
    for (int i = 0; i < 64; i++) {
        if (read_frame_raw(c, hdr, p) != 0) return -1;
        if (hdr->msg_type == type) return 0;
    }
    return -1;
}

/* Send a CALL_JOIN: `key` fills the device key, `inv` names whom a start invites. */
static int call_join(client *c, uint64_t ch, uint8_t key, const uint64_t *inv, uint16_t ninv) {
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cj = { ch, {0}, ninv, inv, OC_CALL_CODEC_VP9 };
    memset(cj.device_key, key, OC_CALL_DEVICE_KEY_LEN);
    if (oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) != OC_OK) return -1;
    return send_frame(c, buf, w.len);
}

static int call_simple(client *c, uint16_t type, uint64_t ch) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_result rc = OC_E_MALFORMED;
    if (type == OC_MSG_CALL_LEAVE)   { oc_call_leave m = { ch };   rc = oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &m); }
    if (type == OC_MSG_CALL_DECLINE) { oc_call_decline m = { ch }; rc = oc_encode_call_decline(&w, OC_PROTOCOL_VERSION, &m); }
    if (type == OC_MSG_CALL_END)     { oc_call_end m = { ch };     rc = oc_encode_call_end(&w, OC_PROTOCOL_VERSION, &m); }
    return rc == OC_OK ? send_frame(c, buf, w.len) : -1;
}

static int call_invite(client *c, uint64_t ch, const uint64_t *users, uint16_t n) {
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_invite ci = { ch, n, users };
    if (oc_encode_call_invite(&w, OC_PROTOCOL_VERSION, &ci) != OC_OK) return -1;
    return send_frame(c, buf, w.len);
}

static int read_state(client *c, oc_call_state *st, uint64_t *parts, uint64_t *inv) {
    oc_header hdr; oc_rbuf p;
    if (read_type(c, OC_MSG_CALL_STATE, &hdr, &p) != 0) return -1;
    return oc_decode_call_state(&p, st, parts, 32, inv, 32) == OC_OK ? 0 : -1;
}

static int read_error(client *c, uint16_t *code) {
    oc_header hdr; oc_rbuf p; oc_error er;
    if (read_type(c, OC_MSG_ERROR, &hdr, &p) != 0 || oc_decode_error(&p, &er) != OC_OK) return -1;
    *code = er.code;
    return 0;
}

/* The daemon's critical failures on the wire (REQ-263): an owner signing in is
 * told the counts at once, and told again as they change; the entries are read
 * and acknowledged; a member is told nothing, and may ask for nothing. */
static int read_summary(client *c, oc_alerts_summary *sm) {
    oc_header hdr; oc_rbuf p;
    if (read_type(c, OC_MSG_ALERTS_SUMMARY, &hdr, &p) != 0) return -1;
    return oc_decode_alerts_summary(&p, sm) == OC_OK ? 0 : -1;
}
static void test_alerts_wire(int port, const uint8_t *pin, oc_dbwriter *dbw) {
    oc_dbwriter_alert(dbw, "test.wire", "a thing the daemon cannot fix alone");
    usleep(100000);                                   /* stored, and told to nobody signed in */
    client a, b;
    uint64_t ua = 0, ub = 0;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "alice", "pw-alice", &ua) == 0);
    oc_alerts_summary sm = g_auth_alerts;
    CHECK(sm.unacked >= 1 && sm.current >= 1);                                      /* at sign-in */
    uint32_t before = sm.unacked;

    /* bob, a member: no summary at sign-in, and a refusal when he asks. */
    CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0 && do_auth(&b, "bob", "pw-bob", &ub) == 0);
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_alerts_list(&w, OC_PROTOCOL_VERSION) == OC_OK && send_frame(&b, buf, w.len) == 0);
    oc_header hdr; oc_rbuf p; int told = 0, refused = 0;
    for (int i = 0; i < 64 && !refused; i++) {
        if (read_frame_raw(&b, &hdr, &p) != 0) break;
        if (hdr.msg_type == OC_MSG_ALERTS_SUMMARY || hdr.msg_type == OC_MSG_ALERTS) told = 1;
        if (hdr.msg_type == OC_MSG_ERROR) { oc_error er; refused = oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_FORBIDDEN; }
    }
    CHECK(refused && !told);

    /* alice reads them, newest first, and acknowledges this one. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_alerts_list(&w, OC_PROTOCOL_VERSION) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_ALERTS, &hdr, &p) == 0);
    static oc_alert got[OC_MAX_ALERTS]; oc_alerts al = { 0, NULL };
    CHECK(oc_decode_alerts(&p, &al, got, OC_MAX_ALERTS) == OC_OK && al.count >= 1);
    uint64_t id = 0;
    for (uint16_t i = 0; i < al.count; i++)
        if (got[i].key.len == 9 && !memcmp(got[i].key.ptr, "test.wire", 9) && got[i].current && !got[i].acked) id = got[i].id;
    CHECK(id != 0);
    oc_alert_ack ack = { id };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_alert_ack(&w, OC_PROTOCOL_VERSION, &ack) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_summary(&a, &sm) == 0 && sm.unacked == before - 1);

    /* A change reaches her unasked; it ends as it began. */
    oc_dbwriter_alert_clear(dbw, "test.wire");
    CHECK(read_summary(&a, &sm) == 0 && sm.current == 0);
    client_close(&b);
    client_close(&a);
}

/* A delete taken back, over the wire (REQ-052): carol deletes, alice sees the
 * tombstone; carol restores, alice is sent the message whole again; alice may
 * not restore what carol deleted, owner though she is. */
static void test_restore_wire(int port, const uint8_t *pin) {
    client a, b;
    uint64_t ua = 0, ub = 0;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "carol", "pw", &ua) == 0);
    CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0 && do_auth(&b, "alice", "pw-alice", &ub) == 0);
    oc_header hdr; oc_rbuf p;
    /* A room of their own, public, that alice joins: what earlier tests did to
     * #general's membership is not this test's business. */
    uint64_t room = 0;
    {
        uint8_t cb[128]; oc_wbuf cw; oc_wbuf_init(&cw, cb, sizeof cb);
        oc_create_channel cc = { oc_slice_str("undo-room"), 1 };
        CHECK(oc_encode_create_channel(&cw, OC_PROTOCOL_VERSION, &cc) == OC_OK && send_frame(&a, cb, cw.len) == 0);
        oc_channel_info ci;
        CHECK(read_type(&a, OC_MSG_CHANNEL_INFO, &hdr, &p) == 0 && oc_decode_channel_info(&p, &ci) == OC_OK);
        room = ci.channel_id;
        oc_channel_ref jr = { room };
        oc_wbuf_init(&cw, cb, sizeof cb);
        CHECK(oc_encode_join_channel(&cw, OC_PROTOCOL_VERSION, &jr) == OC_OK && send_frame(&b, cb, cw.len) == 0);
        CHECK(read_type(&b, OC_MSG_CHANNEL_INFO, &hdr, &p) == 0);
    }
    uint64_t mid = 0;
    {
        uint8_t sb[512]; oc_wbuf sw; oc_wbuf_init(&sw, sb, sizeof sb);
        oc_send sm = {0};
        sm.channel_id = room;
        memset(sm.idem, 0x5A, OC_IDEM_SIZE);   /* used by no other send here: a repeat is a replay, acknowledged and not sent */
        sm.body = oc_slice_str("taking this back");
        CHECK(oc_encode_send(&sw, OC_PROTOCOL_VERSION, &sm) == OC_OK && send_frame(&a, sb, sw.len) == 0);
        oc_send_ack ack;
        CHECK(read_type(&a, OC_MSG_SEND_ACK, &hdr, &p) == 0 && oc_decode_send_ack(&p, &ack) == OC_OK);
        mid = ack.message_id;
    }
    CHECK(mid != 0);
    CHECK(read_type(&b, OC_MSG_BROADCAST, &hdr, &p) == 0);
    uint8_t buf[64]; oc_wbuf w;
    oc_delete d = { room, mid };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_delete(&w, OC_PROTOCOL_VERSION, &d) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&b, OC_MSG_MSG_DELETED, &hdr, &p) == 0);
    /* bob cannot take back alice's delete. */
    oc_restore rs = { room, mid };
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_restore(&w, OC_PROTOCOL_VERSION, &rs) == OC_OK && send_frame(&b, buf, w.len) == 0);
    uint16_t code = 0;
    CHECK(read_error(&b, &code) == 0 && code == OC_ERR_FORBIDDEN);
    /* alice can; bob is sent it whole. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_restore(&w, OC_PROTOCOL_VERSION, &rs) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&b, OC_MSG_MSG_RESTORED, &hdr, &p) == 0);
    oc_broadcast back;
    CHECK(oc_decode_broadcast(&p, &back) == OC_OK && back.message_id == mid && back.author_id == ua &&
          back.body.len == 16 && !memcmp(back.body.ptr, "taking this back", 16));
    client_close(&b);
    client_close(&a);
}

/* A leave sent while the same connection's join is still being checked (REQ-301,
 * ARCH-73). The readers are held, and JOIN, LEAVE, INVITE sent: two checks
 * queued means the loop has read all three, the leave between them. Released,
 * the join's answer finds the join left and does nothing, so the invitation,
 * checked after it, is refused -- alice is in no call to invite anyone to. Her
 * next join is the one she is in: her first CALL_JOINED carries its key. */
static void test_call_leave_while_joining(int port, const uint8_t *pin, oc_dbwriter *dbw) {
    client a;
    uint64_t ua = 0;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0 && do_auth(&a, "alice", "pw-alice", &ua) == 0);
    const uint64_t ch = OC_DEFAULT_CHANNEL;
    oc_header hdr; oc_rbuf p; oc_call_part parts[32];

    oc_dbwriter_hold(dbw, OC_DBW_HOLD_ALL);
    CHECK(call_join(&a, ch, 0xA1, NULL, 0) == 0);
    CHECK(call_simple(&a, OC_MSG_CALL_LEAVE, ch) == 0);
    uint64_t someone[1] = { ua + 1 };
    CHECK(call_invite(&a, ch, someone, 1) == 0);
    for (int i = 0; i < 500 && oc_dbwriter_jobs_waiting(dbw, OC_JOB_CALL_AUTH) < 2; i++) usleep(10000);
    CHECK(oc_dbwriter_jobs_waiting(dbw, OC_JOB_CALL_AUTH) == 2);   /* the join and the invitation, unanswered */
    oc_dbwriter_hold(dbw, 0);
    uint16_t code = 0;
    CHECK(read_error(&a, &code) == 0 && code == OC_ERR_NOT_IN_CALL);  /* no call: the join was void */

    CHECK(call_join(&a, ch, 0xA2, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    oc_call_joined jd;
    memset(&jd, 0, sizeof jd);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.count == 1 && parts[0].user_id == ua);
    CHECK(parts[0].device_key[0] == 0xA2);                          /* this join's, not the left one's */
    CHECK(call_simple(&a, OC_MSG_CALL_LEAVE, ch) == 0);
    client_close(&a);
}

/* Calls over the wire (REQ-150-152, REQ-301-305): a start with invitations,
 * joining, the roster with slots, keys and epochs, sealed keys forwarded only
 * between participants for the current epoch, the cap, declining, ending, the
 * missed-call line, one call per connection, and a non-member refused. */
static void test_call_vertical(int port, const uint8_t *pin) {
    client a, b, c, d;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    CHECK(client_open(&c, port, pin) == 0); CHECK(do_handshake(&c) == 0);
    CHECK(client_open(&d, port, pin) == 0); CHECK(do_handshake(&d) == 0);
    uint64_t ua = 0, ub = 0, uc = 0, ud = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    CHECK(do_auth(&d, "bf-reader", "pw", &ud) == 0);
    const uint64_t ch = OC_DEFAULT_CHANNEL;

    oc_header hdr; oc_rbuf p; oc_call_part parts[32]; uint64_t sp[32], si[32];
    oc_call_state st; uint16_t code = 0;

    /* alice starts, inviting bob: she is in it alone, at slot 0, epoch 1. */
    uint64_t inv_b[1] = { ub };
    CHECK(call_join(&a, ch, 0xA1, inv_b, 1) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    oc_call_joined jd; CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.channel_id == ch && jd.count == 1 && parts[0].user_id == ua && jd.slot == 0);
    CHECK(jd.epoch == 1 && jd.starter == ua && jd.started_at > 0 && jd.call_id != 0);
    CHECK(parts[0].device_key[0] == 0xA1);
    uint64_t call_id = jd.call_id;

    /* bob hears of it: a CALL_STATE naming alice in it and himself invited. */
    CHECK(read_state(&b, &st, sp, si) == 0);
    CHECK(st.call_id == call_id && st.starter == ua && !st.ended);
    CHECK(st.n_parts == 1 && sp[0] == ua && st.n_invited == 1 && si[0] == ub);

    /* bob joins: two participants, epoch 2, each with a key and a slot of its
     * own; alice's roster says the same, and the invitation is taken. */
    CHECK(call_join(&b, ch, 0xB2, NULL, 0) == 0);
    CHECK(read_type(&b, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.count == 2 && jd.epoch == 2);
    CHECK(jd.slot == 1 && jd.call_id == call_id && jd.starter == ua);
    CHECK(read_type(&a, OC_MSG_CALL_ROSTER, &hdr, &p) == 0);
    oc_call_roster ro; CHECK(oc_decode_call_roster(&p, &ro, parts, 32) == OC_OK);
    CHECK(ro.count == 2 && ro.epoch == 2);
    int seen_b = 0;
    for (int i = 0; i < ro.count; i++) if (parts[i].user_id == ub && parts[i].device_key[5] == 0xB2) seen_b = 1;
    CHECK(seen_b);
    CHECK(read_state(&c, &st, sp, si) == 0 && st.n_parts == 1);    /* carol saw the start... */
    CHECK(read_state(&c, &st, sp, si) == 0 && st.n_parts == 2 && st.n_invited == 0);   /* ...and the join */

    /* Sealed keys: alice's copy for bob reaches bob; a copy for carol, who is
     * not in the call, reaches nobody; a copy for an old epoch is dropped. */
    {
        uint8_t s1[OC_CALL_SEALED_LEN], s2[OC_CALL_SEALED_LEN], s3[OC_CALL_SEALED_LEN];
        memset(s1, 0x11, sizeof s1); memset(s2, 0x22, sizeof s2); memset(s3, 0x33, sizeof s3);
        uint8_t buf[512]; oc_wbuf w;
        oc_call_key_entry stale_e[1] = { { ub, { s3, sizeof s3 } } };
        oc_call_key stale = { ch, call_id, 1, 1, stale_e };
        oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_call_key(&w, OC_PROTOCOL_VERSION, &stale) == OC_OK && send_frame(&a, buf, w.len) == 0);
        oc_call_key_entry e[2] = { { uc, { s2, sizeof s2 } }, { ub, { s1, sizeof s1 } } };
        oc_call_key ck = { ch, call_id, 2, 2, e };
        oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_call_key(&w, OC_PROTOCOL_VERSION, &ck) == OC_OK && send_frame(&a, buf, w.len) == 0);
        CHECK(read_type(&b, OC_MSG_CALL_KEY_FOR, &hdr, &p) == 0);
        oc_call_key_for kf; CHECK(oc_decode_call_key_for(&p, &kf) == OC_OK);
        CHECK(kf.sender == ua && kf.epoch == 2 && kf.call_id == call_id);
        CHECK(kf.sealed.len == OC_CALL_SEALED_LEN && kf.sealed.ptr[0] == 0x11);   /* not the stale 0x33 */
        /* carol is not in the call, so she may not send keys into it. */
        oc_call_key_entry ce[1] = { { ua, { s2, sizeof s2 } } };
        oc_call_key cck = { ch, call_id, 2, 1, ce };
        oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_call_key(&w, OC_PROTOCOL_VERSION, &cck) == OC_OK && send_frame(&c, buf, w.len) == 0);
        CHECK(read_error(&c, &code) == 0 && code == OC_ERR_NOT_IN_CALL);
    }

    /* The cap is 3 here: two in the call and two invited would be four. */
    uint64_t two[2] = { uc, ud };
    CHECK(call_invite(&a, ch, two, 2) == 0);
    CHECK(read_error(&a, &code) == 0 && code == OC_ERR_CALL_FULL);
    CHECK(call_invite(&b, ch, two, 1) == 0);               /* anyone in the call may invite */
    CHECK(read_state(&c, &st, sp, si) == 0 && st.n_invited == 1 && si[0] == uc);
    CHECK(call_simple(&c, OC_MSG_CALL_DECLINE, ch) == 0);  /* carol declines */
    CHECK(read_state(&a, &st, sp, si) == 0);
    while (st.n_invited != 0 && read_state(&a, &st, sp, si) == 0) {}
    CHECK(st.n_invited == 0 && st.n_parts == 2);
    /* A full call refuses a join: d fills it, then carol cannot get in. */
    CHECK(call_join(&d, ch, 0xD4, NULL, 0) == 0);
    CHECK(read_type(&d, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(call_join(&c, ch, 0xC3, NULL, 0) == 0);
    CHECK(read_error(&c, &code) == 0 && code == OC_ERR_CALL_FULL);
    CHECK(call_simple(&d, OC_MSG_CALL_LEAVE, ch) == 0);

    /* Only the starter ends it; a leave naming another conversation is nothing. */
    CHECK(call_simple(&b, OC_MSG_CALL_END, ch) == 0);
    CHECK(read_error(&b, &code) == 0 && code == OC_ERR_NOT_CALL_STARTER);
    CHECK(call_simple(&b, OC_MSG_CALL_LEAVE, ch + 999) == 0);
    /* bob disconnects: alice's roster drops to herself in a new epoch (REQ-152). */
    client_close(&b);
    uint32_t last_epoch = 0;
    for (int i = 0; i < 4; i++) {
        CHECK(read_type(&a, OC_MSG_CALL_ROSTER, &hdr, &p) == 0);
        CHECK(oc_decode_call_roster(&p, &ro, parts, 32) == OC_OK);
        last_epoch = ro.epoch;
        if (ro.count == 1) break;
    }
    CHECK(ro.count == 1 && parts[0].user_id == ua && last_epoch == 5);   /* joins 1,2,3(d); leaves 4(d),5(b) */

    /* alice ends it for everyone: carol is told it is over. No missed call --
     * bob and d joined. */
    CHECK(call_simple(&a, OC_MSG_CALL_END, ch) == 0);
    do { CHECK(read_state(&c, &st, sp, si) == 0); } while (!st.ended && st.call_id == call_id);
    CHECK(st.ended && st.call_id == call_id);

    /* A start nobody takes: alice invites carol and leaves. The call ends, and a
     * "Missed call" line lands in the conversation, authored by alice. */
    uint64_t inv_c[1] = { uc };
    CHECK(call_join(&a, ch, 0xA1, inv_c, 1) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.call_id != call_id);
    CHECK(call_simple(&a, OC_MSG_CALL_LEAVE, ch) == 0);
    CHECK(read_type(&c, OC_MSG_BROADCAST, &hdr, &p) == 0);
    oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
    CHECK(bc.kind == OC_MSG_KIND_CALL && bc.author_id == ua && bc.channel_id == ch);
    CHECK(bc.body.len == 11 && memcmp(bc.body.ptr, "Missed call", 11) == 0);

    /* A non-member is refused: carol tries to join a private channel's call. */
    uint8_t buf[128]; oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("callvault"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_CHANNEL_INFO, &hdr, &p) == 0);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t priv = ci.channel_id;
    CHECK(call_join(&c, priv, 0xC3, NULL, 0) == 0);
    CHECK(read_error(&c, &code) == 0 && code == OC_ERR_NOT_A_MEMBER);

    /* One call per connection: alice in the default channel's call, then in the
     * private channel's -- the first ends (she was alone in it). */
    CHECK(call_join(&a, ch, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    uint64_t first = jd.call_id;
    CHECK(call_join(&a, priv, 0xA1, NULL, 0) == 0);
    do { CHECK(read_state(&a, &st, sp, si) == 0); } while (st.call_id != first || !st.ended);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.channel_id == priv);
    CHECK(call_simple(&a, OC_MSG_CALL_END, priv) == 0);

    client_close(&d);
    client_close(&c);
    client_close(&a);
}

/* Who is told a call's state (REQ-303): every connection of the conversation's
 * members, each once -- alice's second device included, though she is both a
 * member and the participant -- and nobody outside it. A private conversation,
 * so carol, connected and signed in, is not its audience. */
static void test_call_state_audience(int port, const uint8_t *pin) {
    client a1, a2, c;
    CHECK(client_open(&a1, port, pin) == 0); CHECK(do_handshake(&a1) == 0);
    CHECK(client_open(&a2, port, pin) == 0); CHECK(do_handshake(&a2) == 0);
    CHECK(client_open(&c, port, pin) == 0); CHECK(do_handshake(&c) == 0);
    uint64_t ua = 0, uc = 0;
    CHECK(do_auth(&a1, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&a2, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    oc_header hdr; oc_rbuf p;
    uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("stateaudience"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK);
    CHECK(send_frame(&a1, buf, w.len) == 0);
    CHECK(read_type(&a1, OC_MSG_CHANNEL_INFO, &hdr, &p) == 0);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t priv = ci.channel_id;

    CHECK(call_join(&a1, priv, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a1, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    oc_call_joined jd; oc_call_part parts[32];
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(call_simple(&a1, OC_MSG_CALL_END, priv) == 0);

    /* Each of alice's connections: the start, then the end, and no copy of
     * either -- a second copy of the start would come before the end. */
    client *mine[2] = { &a1, &a2 };
    for (int k = 0; k < 2; k++) {
        oc_call_state st; uint64_t sp[32], si[32];
        do { CHECK(read_state(mine[k], &st, sp, si) == 0); } while (st.call_id != jd.call_id);
        CHECK(!st.ended && st.n_parts == 1 && sp[0] == ua);
        CHECK(read_state(mine[k], &st, sp, si) == 0);
        CHECK(st.call_id == jd.call_id && st.ended);
    }
    /* carol: told nothing about it. Her own END for the conversation is refused,
     * and nothing about the call comes before that answer. */
    CHECK(call_simple(&c, OC_MSG_CALL_END, priv) == 0);
    int told = 0, answered = 0;
    for (int i = 0; i < 64 && !answered; i++) {
        if (read_frame_raw(&c, &hdr, &p) != 0) break;
        if (hdr.msg_type == OC_MSG_ERROR) answered = 1;
        if (hdr.msg_type == OC_MSG_CALL_STATE) {
            oc_call_state st; uint64_t sp[32], si[32];
            if (oc_decode_call_state(&p, &st, sp, 32, si, 32) == OC_OK && st.call_id == jd.call_id) told = 1;
        }
    }
    CHECK(answered && !told);
    client_close(&c);
    client_close(&a2);
    client_close(&a1);
}

static int call_share(client *c, uint64_t ch, int on) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_share m = { ch, (uint8_t)on };
    if (oc_encode_call_share(&w, OC_PROTOCOL_VERSION, &m) != OC_OK) return -1;
    return send_frame(c, buf, w.len);
}

/* The next CALL_STATE for `call_id` whose sharer is `sharer`, passing over the
 * earlier ones; -1 if it never comes. */
static int wait_sharer(client *c, uint64_t call_id, uint64_t sharer, oc_call_state *st) {
    uint64_t sp[32], si[32];
    for (int i = 0; i < 16; i++) {
        if (read_state(c, st, sp, si) != 0) return -1;
        if (st->call_id == call_id && st->sharer == sharer) return 0;
    }
    return -1;
}

/* Screen sharing's signalling (REQ-161): each participant's codecs on the
 * roster; a share named on CALL_STATE; a start taking over from the sharer; a
 * stop from someone not sharing changing nothing; a sharer who leaves or
 * disconnects no longer sharing; and someone outside the call refused. */
static void test_call_share(int port, const uint8_t *pin) {
    client a, b, c;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    CHECK(client_open(&c, port, pin) == 0); CHECK(do_handshake(&c) == 0);
    uint64_t ua = 0, ub = 0, uc = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    const uint64_t ch = OC_DEFAULT_CHANNEL;
    oc_header hdr; oc_rbuf p; oc_call_part parts[32]; oc_call_joined jd; oc_call_state st;
    uint16_t code = 0;

    CHECK(call_join(&a, ch, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    uint64_t call_id = jd.call_id;
    CHECK(call_join(&b, ch, 0xB2, NULL, 0) == 0);
    CHECK(read_type(&b, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.count == 2);
    CHECK(parts[0].codecs == OC_CALL_CODEC_VP9 && parts[1].codecs == OC_CALL_CODEC_VP9);

    /* carol is not in the call: she cannot share into it. */
    CHECK(call_share(&c, ch, 1) == 0);
    CHECK(read_error(&c, &code) == 0 && code == OC_ERR_NOT_IN_CALL);

    /* alice shares; everyone told, carol too (she is in the audience). */
    CHECK(call_share(&a, ch, 1) == 0);
    CHECK(wait_sharer(&b, call_id, ua, &st) == 0 && !st.ended);
    CHECK(wait_sharer(&c, call_id, ua, &st) == 0);
    /* bob takes over; alice learns she is no longer sharing. */
    CHECK(call_share(&b, ch, 1) == 0);
    CHECK(wait_sharer(&a, call_id, ub, &st) == 0);
    /* alice's stop is not bob's: nothing changes, so the next state anyone sees
     * is bob's own stop. */
    CHECK(call_share(&a, ch, 0) == 0);
    CHECK(call_share(&b, ch, 0) == 0);
    { uint64_t sp[32], si[32];
      CHECK(read_state(&c, &st, sp, si) == 0 && st.sharer == ub);      /* the take-over */
      CHECK(read_state(&c, &st, sp, si) == 0 && st.sharer == 0 && st.n_parts == 2); }

    /* A sharer who leaves stops sharing. */
    CHECK(call_share(&b, ch, 1) == 0);
    CHECK(wait_sharer(&a, call_id, ub, &st) == 0);
    CHECK(call_simple(&b, OC_MSG_CALL_LEAVE, ch) == 0);
    CHECK(wait_sharer(&a, call_id, 0, &st) == 0 && st.n_parts == 1);
    /* ...and so does one who disconnects. */
    CHECK(call_join(&b, ch, 0xB2, NULL, 0) == 0);
    CHECK(read_type(&b, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(call_share(&b, ch, 1) == 0);
    CHECK(wait_sharer(&a, call_id, ub, &st) == 0);
    client_close(&b);
    CHECK(wait_sharer(&a, call_id, 0, &st) == 0 && st.n_parts == 1);

    CHECK(call_simple(&a, OC_MSG_CALL_END, ch) == 0);
    client_close(&c);
    client_close(&a);
}

/* Full audio path (REQ-150/151): two participants join a call, each gets a UDP
 * endpoint + bearer token in CALL_JOINED, and one participant's audio is relayed
 * to the other by the relay, tagged with the sender's user id. */
static void set_read_timeout(client *c, int ms);

static void test_call_udp_vertical(int port, const uint8_t *pin, uint16_t audio_port) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w; oc_call_part parts[32];
    uint8_t atok[OC_AUDIO_TOKEN_RAND], btok[OC_AUDIO_TOKEN_RAND];

    /* alice joins -> CALL_JOINED with the real UDP port + a 16-byte token. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cj = { OC_DEFAULT_CHANNEL, {0}, 0, NULL, OC_CALL_CODEC_VP9 };
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    oc_call_joined jd; CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.udp_port == audio_port && jd.token.len == OC_AUDIO_TOKEN_RAND);
    memcpy(atok, jd.token.ptr, OC_AUDIO_TOKEN_RAND);

    /* bob joins -> his own token; alice gets a roster update. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == OC_AUDIO_TOKEN_RAND);
    memcpy(btok, jd.token.ptr, OC_AUDIO_TOKEN_RAND);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_ROSTER);

    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(audio_port);
    int sa = mk_udp_client(), sb = mk_udp_client();

    /* Each side sends a hello so the relay learns its UDP address, and each
     * hears the keepalive echo of its own -- which says the relay has it. */
    uint64_t sender; uint16_t seq; char body[64];
    udp_send_audio(sb, &relay, btok, 0, NULL);
    CHECK(udp_recv_audio(sb, &sender, &seq, body, sizeof body) >= 0);
    udp_send_audio(sa, &relay, atok, 0, NULL);
    CHECK(udp_recv_audio(sa, &sender, &seq, body, sizeof body) >= 0);

    /* alice speaks -> bob receives it, tagged with alice's user id. */
    udp_send_audio(sa, &relay, atok, 7, "hey");
    int n = udp_recv_audio(sb, &sender, &seq, body, sizeof body);
    CHECK(n == 3 && sender == ua && seq == 7 && memcmp(body, "hey", 3) == 0);

    /* alice rejoins from the same connection: a fresh token, and the old one is
     * revoked, so nothing sent with it is relayed any more. */
    CHECK(call_join(&a, OC_DEFAULT_CHANNEL, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == OC_AUDIO_TOKEN_RAND);
    uint8_t atok2[OC_AUDIO_TOKEN_RAND];
    memcpy(atok2, jd.token.ptr, OC_AUDIO_TOKEN_RAND);
    CHECK(memcmp(atok2, atok, OC_AUDIO_TOKEN_RAND) != 0);
    /* The old token, then the new, from one socket: the relay handles them in
     * order, so if the old were relayed it would reach bob first. What bob
     * receives first is the new one -- no wait for "nothing" needed. */
    udp_send_audio(sa, &relay, atok, 8, "old");
    udp_send_audio(sa, &relay, atok2, 9, "new");
    n = udp_recv_audio(sb, &sender, &seq, body, sizeof body);
    CHECK(n == 3 && sender == ua && seq == 9 && memcmp(body, "new", 3) == 0);

    /* bob goes silent -- no keep-alive -- and the relay sweeps him: it reports
     * him GONE and the daemon takes him out of the call, so alice's roster says
     * who can actually be heard (CALLS.md §4). */
    /* alice keeps talking (a keepalive every 100 ms) while bob says nothing,
     * and alice's roster is watched between them until it holds her alone. */
    oc_netloop_set_relay_silence_ms(300);
    oc_call_roster ro;
    int gone = 0;
    set_read_timeout(&a, 100);
    for (int i = 0; i < 40 && !gone; i++) {
        udp_send_audio(sa, &relay, atok2, (uint16_t)(10 + i), NULL);
        if (read_type(&a, OC_MSG_CALL_ROSTER, &hdr, &p) == 0 &&
            oc_decode_call_roster(&p, &ro, parts, 32) == OC_OK && ro.count == 1 && parts[0].user_id == ua)
            gone = 1;
    }
    set_read_timeout(&a, 20000);
    CHECK(gone);
    oc_netloop_set_relay_silence_ms(0);

    close(sa); close(sb);
    client_close(&a);
    client_close(&b);
}

static int read_frame_or_quiet(client *c, oc_header *hdr, oc_rbuf *p);

/* CALL_MEDIA up: `payload` NULL for a keepalive. */
static int tcp_media_up(client *c, uint16_t seq, const char *payload) {
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_media_pkt m = { 0, seq, { (const uint8_t *)payload, payload ? strlen(payload) : 0 } };
    if (oc_encode_call_media_up(&w, OC_PROTOCOL_VERSION, &m) != OC_OK) return -1;
    return send_frame(c, buf, w.len);
}

/* The next CALL_MEDIA down, its payload copied to `body`: the payload's length,
 * or -1. */
static int tcp_media_down(client *c, uint64_t *sender, uint16_t *seq, char *body, size_t cap) {
    oc_header hdr; oc_rbuf p; oc_call_media_pkt m;
    if (read_type(c, OC_MSG_CALL_MEDIA, &hdr, &p) != 0 || oc_decode_call_media_down(&p, &m) != OC_OK) return -1;
    *sender = m.sender; *seq = m.seq;
    size_t n = m.ct.len < cap ? m.ct.len : cap;
    memcpy(body, m.ct.ptr, n);
    return (int)n;
}

/* Media over each pair of transports (PROTOCOL.md §5.17, REQ-180): alice on UDP,
 * bob and carol on the connection. What one sends reaches the others by their
 * own transport, tagged with the sender; a keepalive is answered to its sender
 * alone, on either; the latest packet's transport is the one a participant is
 * reached by, so bob can move to UDP and back mid-call; and a participant on the
 * connection who stops reading is sent less media, not disconnected -- chat
 * still reaches them. */
static void test_call_transports(int port, const uint8_t *pin, uint16_t audio_port) {
    client a, b, c;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    CHECK(client_open(&c, port, pin) == 0); CHECK(do_handshake(&c) == 0);
    uint64_t ua = 0, ub = 0, uc = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    oc_header hdr; oc_rbuf p; oc_call_part parts[32]; oc_call_joined jd;
    uint8_t atok[OC_AUDIO_TOKEN_RAND], btok[OC_AUDIO_TOKEN_RAND];
    const uint64_t ch = OC_DEFAULT_CHANNEL;
    CHECK(call_join(&a, ch, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0 && oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    memcpy(atok, jd.token.ptr, OC_AUDIO_TOKEN_RAND);
    CHECK(call_join(&b, ch, 0xB2, NULL, 0) == 0);
    CHECK(read_type(&b, OC_MSG_CALL_JOINED, &hdr, &p) == 0 && oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    memcpy(btok, jd.token.ptr, OC_AUDIO_TOKEN_RAND);
    CHECK(call_join(&c, ch, 0xC3, NULL, 0) == 0);
    CHECK(read_type(&c, OC_MSG_CALL_JOINED, &hdr, &p) == 0);

    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(audio_port);
    int sa = mk_udp_client(), sbu = mk_udp_client();
    uint64_t s; uint16_t q; char body[128]; int n;

    /* Keepalives: each hears its own echo, on its own transport. */
    udp_send_audio(sa, &relay, atok, 1, NULL);
    n = udp_recv_audio(sa, &s, &q, body, sizeof body);
    CHECK(n == 0 && s == ua && q == 1);
    CHECK(tcp_media_up(&b, 2, NULL) == 0);
    n = tcp_media_down(&b, &s, &q, body, sizeof body);
    CHECK(n == 0 && s == ub && q == 2);
    CHECK(tcp_media_up(&c, 3, NULL) == 0);
    n = tcp_media_down(&c, &s, &q, body, sizeof body);
    CHECK(n == 0 && s == uc && q == 3);

    /* UDP -> the connection: alice to bob and carol. Neither heard anyone's
     * keepalive but their own: the next media each reads is this. */
    udp_send_audio(sa, &relay, atok, 10, "u2t");
    n = tcp_media_down(&b, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ua && q == 10 && memcmp(body, "u2t", 3) == 0);
    n = tcp_media_down(&c, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ua && q == 10 && memcmp(body, "u2t", 3) == 0);

    /* The connection -> UDP, and -> the connection: bob to alice and carol. */
    CHECK(tcp_media_up(&b, 11, "t2x") == 0);
    n = udp_recv_audio(sa, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ub && q == 11 && memcmp(body, "t2x", 3) == 0);
    n = tcp_media_down(&c, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ub && q == 11 && memcmp(body, "t2x", 3) == 0);

    /* bob moves to UDP: his next packet comes by datagram, and so does what is
     * sent to him. Then back: one packet on the connection, and he is reached
     * there again. */
    udp_send_audio(sbu, &relay, btok, 12, NULL);
    n = udp_recv_audio(sbu, &s, &q, body, sizeof body);
    CHECK(n == 0 && s == ub && q == 12);
    udp_send_audio(sa, &relay, atok, 13, "now");
    n = udp_recv_audio(sbu, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ua && q == 13 && memcmp(body, "now", 3) == 0);
    n = tcp_media_down(&c, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ua && q == 13);
    CHECK(tcp_media_up(&b, 14, NULL) == 0);
    n = tcp_media_down(&b, &s, &q, body, sizeof body);
    CHECK(n == 0 && s == ub && q == 14);
    udp_send_audio(sa, &relay, atok, 15, "tcp");
    n = tcp_media_down(&b, &s, &q, body, sizeof body);
    CHECK(n == 3 && s == ua && q == 15 && memcmp(body, "tcp", 3) == 0);
    /* Not by datagram any more: a copy would have left with the one on the
     * connection just read, so a short wait is a full one here. */
    udp_timeout(sbu, 150);
    CHECK(udp_recv_audio(sbu, &s, &q, body, sizeof body) < 0);
    udp_timeout(sbu, 1000);

    /* carol stops reading. ~20 MB of media is sent her way: more than any
     * kernel buffer between her and the daemon holds, so most of it waits on the
     * daemon, far past the 1 MiB at which anything else would close her. She is
     * not closed -- media is dropped instead -- and a chat message sent after it
     * all still reaches her. */
    enum { FLOOD = 20000 };
    {
        struct timeval tv = { 10, 0 };
        setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t pkt[OC_AUDIO_TOKEN_RAND + 2 + 1000];
        memcpy(pkt, atok, OC_AUDIO_TOKEN_RAND);
        memset(pkt + OC_AUDIO_TOKEN_RAND + 2, 'm', 1000);
        for (int i = 0; i < FLOOD; i++) {
            uint16_t sq = (uint16_t)(100 + i);
            pkt[OC_AUDIO_TOKEN_RAND] = (uint8_t)(sq >> 8); pkt[OC_AUDIO_TOKEN_RAND + 1] = (uint8_t)sq;
            sendto(sa, pkt, sizeof pkt, 0, (const struct sockaddr *)&relay, sizeof relay);
            if (i % 100 == 99) usleep(2000);   /* paced: the relay's socket buffer is not under test */
        }
        usleep(300000);
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_send sm; memset(&sm, 0, sizeof sm);
        sm.channel_id = ch; memset(sm.idem, 0x7C, OC_IDEM_SIZE);
        sm.body = oc_slice_str("still here");
        CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK && send_frame(&a, buf, w.len) == 0);
        int media = 0, chat = 0, closed = 0;
        for (int i = 0; i < 20000 && !chat; i++) {
            int rr = read_frame_or_quiet(&c, &hdr, &p);
            if (rr < 0) { closed = 1; break; }
            if (rr == 0) break;
            if (hdr.msg_type == OC_MSG_CALL_MEDIA) media++;
            if (hdr.msg_type == OC_MSG_BROADCAST) {
                oc_broadcast bc;
                if (oc_decode_broadcast(&p, &bc) == OC_OK && bc.body.len == 10 &&
                    memcmp(bc.body.ptr, "still here", 10) == 0) chat = 1;
            }
        }
        CHECK(!closed && chat);
        CHECK(media > 0 && media < FLOOD);   /* some came; the rest were dropped, not queued */
    }

    CHECK(call_simple(&a, OC_MSG_CALL_LEAVE, ch) == 0);
    close(sa); close(sbu);
    client_close(&c);
    client_close(&b);
    client_close(&a);
}

/* A daemon behind a front door (AUDIO.md §4): CALL_JOINED names the port the
 * door forwards from, not the one the relay bound, and every token leads with
 * the routing prefix -- and the relay, which is told each token whole, relays
 * and sweeps by them as by any other. Its own loop, since both are read from the
 * environment at startup. */
static void test_call_routed(int port) {
    /* This loop runs beside the suite's main one, which holds its own socket:
     * this one gets another. */
    int udp2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in u2; memset(&u2, 0, sizeof u2);
    u2.sin_family = AF_INET; u2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(udp2, (struct sockaddr *)&u2, sizeof u2) == 0);
    socklen_t u2l = sizeof u2; getsockname(udp2, (struct sockaddr *)&u2, &u2l);
    uint16_t audio_port = ntohs(u2.sin_port);
    oc_netloop_set_audio(udp2, audio_port);
    setenv("OPENCHIME_AUDIO_TOKEN_PREFIX", "0a0b0c", 1);
    setenv("OPENCHIME_AUDIO_ADVERTISE_PORT", "40001", 1);
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);
    unlink("build/itest_routed.db"); unlink("build/itest_routed.db-wal"); unlink("build/itest_routed.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_routed.db");
    CHECK(dbw2 != NULL);
    CHECK(oc_dbwriter_register_local(dbw2, "alice", "pw-alice", OC_ROLE_OWNER,  2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw2, "bob",   "pw-bob",   OC_ROLE_MEMBER, 2048) != 0);
    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    client a, b;
    CHECK(client_open(&a, port, pin2) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin2) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    enum { TLEN = 3 + OC_AUDIO_TOKEN_RAND };
    static const uint8_t PREFIX[3] = { 0x0a, 0x0b, 0x0c };
    oc_header hdr; oc_rbuf p; oc_call_part parts[32]; oc_call_joined jd;
    uint8_t atok[TLEN], btok[TLEN];
    CHECK(call_join(&a, OC_DEFAULT_CHANNEL, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.udp_port == 40001 && jd.token.len == TLEN && memcmp(jd.token.ptr, PREFIX, 3) == 0);
    if (jd.token.len == TLEN) memcpy(atok, jd.token.ptr, TLEN);
    CHECK(call_join(&b, OC_DEFAULT_CHANNEL, 0xB2, NULL, 0) == 0);
    CHECK(read_type(&b, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.udp_port == 40001 && jd.token.len == TLEN && memcmp(jd.token.ptr, PREFIX, 3) == 0);
    if (jd.token.len == TLEN) memcpy(btok, jd.token.ptr, TLEN);
    CHECK(memcmp(atok + 3, btok + 3, OC_AUDIO_TOKEN_RAND) != 0);

    /* The door is not in this test: packets go to the port the relay bound. */
    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(audio_port);
    int sa = mk_udp_client(), sb = mk_udp_client();
    uint64_t sender; uint16_t seq; char body[64];
    int n = -1;
    for (int i = 0; i < 10 && n != 6; i++) {
        udp_send_audio_n(sb, &relay, btok, TLEN, 0, NULL);
        udp_send_audio_n(sa, &relay, atok, TLEN, 5, "routed");
        while ((n = udp_recv_audio(sb, &sender, &seq, body, sizeof body)) == 0) {}
    }
    CHECK(n == 6 && sender == ua && seq == 5 && memcmp(body, "routed", 6) == 0);

    /* alice rejoins: the revoke the daemon sends names her old token whole, so
     * nothing sent with it is relayed any more, and the new one is. (The relay's
     * GONE is read by the main loop, which holds none of these connections;
     * test_audio sees a prefixed token come back whole in one.) */
    CHECK(call_join(&a, OC_DEFAULT_CHANNEL, 0xA1, NULL, 0) == 0);
    CHECK(read_type(&a, OC_MSG_CALL_JOINED, &hdr, &p) == 0);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == TLEN);
    uint8_t atok2[TLEN];
    if (jd.token.len == TLEN) memcpy(atok2, jd.token.ptr, TLEN);
    n = -1;
    for (int i = 0; i < 10 && n != 3; i++) {
        udp_send_audio_n(sa, &relay, atok2, TLEN, 7, "new");
        while ((n = udp_recv_audio(sb, &sender, &seq, body, sizeof body)) == 0) {}
    }
    CHECK(n == 3 && sender == ua && seq == 7 && memcmp(body, "new", 3) == 0);
    /* The new token is in use, so the rejoin is applied: the old one, then the
     * new, from one socket -- if the old were still relayed it would reach bob
     * first. */
    udp_send_audio_n(sa, &relay, atok, TLEN, 6, "old");
    udp_send_audio_n(sa, &relay, atok2, TLEN, 8, "new");
    while ((n = udp_recv_audio(sb, &sender, &seq, body, sizeof body)) == 0) {}
    CHECK(n == 3 && sender == ua && seq == 8 && memcmp(body, "new", 3) == 0);

    (void)ub;
    close(sa); close(sb);
    client_close(&a);
    client_close(&b);
    stop_loop(&arg2, th2);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    /* Put the shared configuration back: the main loop reads it too. */
    unsetenv("OPENCHIME_AUDIO_TOKEN_PREFIX");
    unsetenv("OPENCHIME_AUDIO_ADVERTISE_PORT");
    oc_netloop_set_audio(-1, 0);   /* its loop gave the socket back */
    close(udp2);
    char cfgerr[128];
    oc_config_load(cfgerr, sizeof cfgerr);
    unlink("build/itest_routed.db"); unlink("build/itest_routed.db-wal"); unlink("build/itest_routed.db-shm");
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
    oc_invite_user iu = { OC_ROLE_MEMBER, { NULL, 0 } };
    CHECK(oc_encode_invite_user(&w, OC_PROTOCOL_VERSION, &iu) == OC_OK);
    CHECK(send_frame(&owner, buf, w.len) == 0);
    CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_INVITE_CREATED);
    oc_invite_created ic; CHECK(oc_decode_invite_created(&p, &ic) == OC_OK);
    /* The token reaches a client as HEX: it gets shared in a message, so it has
     * to be text. (It is still random bytes on the daemon's side.) */
    CHECK(ic.token.len == 2 * OC_INVITE_TOKEN_LEN && ic.role == OC_ROLE_MEMBER);
    char token[2 * OC_INVITE_TOKEN_LEN + 1];
    memcpy(token, ic.token.ptr, ic.token.len);
    token[ic.token.len] = '\0';
    for (size_t ti = 0; ti < ic.token.len; ti++)
        CHECK((token[ti] >= '0' && token[ti] <= '9') || (token[ti] >= 'a' && token[ti] <= 'f'));

    /* This workspace signs people in with passwords only, so an invite bound to
     * an address could never be spent: it is refused, the connection lives on,
     * and the reason says what to do instead. */
    {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_invite_user ie = { OC_ROLE_MEMBER, oc_slice_str("lee@partner.example") };
        CHECK(oc_encode_invite_user(&w, OC_PROTOCOL_VERSION, &ie) == OC_OK);
        CHECK(send_frame(&owner, buf, w.len) == 0);
        CHECK(read_frame(&owner, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
        oc_error er;
        CHECK(oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_INVITE_UNREDEEMABLE &&
              !er.fatal && er.message.len > 0);
    }

    /* A fresh client redeems the invite: pre-auth account creation -> AUTH_OK. */
    client nh;
    CHECK(client_open(&nh, port, pin) == 0);
    CHECK(do_handshake(&nh) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_redeem_invite ri = { oc_slice_str(token), oc_slice_str("newhire"), oc_slice_str("nhpw") };
    CHECK(oc_encode_redeem_invite(&w, OC_PROTOCOL_VERSION, &ri) == OC_OK);
    CHECK(send_frame(&nh, buf, w.len) == 0);
    CHECK(read_frame(&nh, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_AUTH_OK);
    oc_auth_ok ok; CHECK(oc_decode_auth_ok(&p, &ok) == OC_OK);
    uint64_t unh = ok.user_id;
    CHECK(unh != 0 && ok.role == OC_ROLE_MEMBER);
    /* Redeeming an invite authenticates, so it carries the pause too (REQ-278):
     * a brand new account is not paused, but the frame is unconditional — the
     * client must never have to infer "no pause" from silence. */
    CHECK(read_frame(&nh, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_SNOOZE);
    { oc_snooze sn0; CHECK(oc_decode_snooze(&p, &sn0) == OC_OK && sn0.until_ms == 0); }

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

/* Count the frames `c` receives of `type` (for an ERROR, only those with
 * `code`; for presence and typing, only those about `from_user`).
 *
 * With `fence`, first ask the daemon for the channel list and stop at its
 * answer: the loop answers a connection's requests in order, so every frame it
 * queued for `c` before that request is read before the answer -- an exact end
 * to the count, where waiting for a quiet second is only a guess that costs the
 * second. Without, stop once `max` have been counted. -1 if the end never came
 * (the read timed out). */
static int count_frames(client *c, uint16_t type, uint16_t code, uint64_t from_user,
                        uint8_t *last_status, int fence, int max) {
    if (fence) {
        uint8_t fb[64]; oc_wbuf w; oc_wbuf_init(&w, fb, sizeof fb);
        if (oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) != OC_OK || send_frame(c, fb, w.len) != 0)
            return -1;
    }
    int n = 0;
    for (;;) {
        oc_header hdr; oc_rbuf p;
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r < 0) return -1;
        if (r == 0) {
            uint8_t buf[4096]; size_t got = 0;
            if (oc_tls_read(&c->conn, buf, sizeof buf, &got) != OC_TLS_OK || !got) return -1;
            if (oc_framebuf_push(&c->fb, buf, got) != 0) return -1;
            continue;
        }
        if (oc_parse_frame(frame, flen, &hdr, &p) != OC_OK) continue;
        if (fence && hdr.msg_type == OC_MSG_CHANNEL_LIST) return n;
        if (hdr.msg_type != type) continue;
        if (type == OC_MSG_ERROR) {
            oc_error e;
            if (oc_decode_error(&p, &e) == OC_OK && e.code == code) n++;
        } else if (type == OC_MSG_PRESENCE_UPDATE) {
            oc_presence_update pu;
            if (oc_decode_presence_update(&p, &pu) == OC_OK && pu.user_id == from_user) {
                n++;
                if (last_status) *last_status = pu.status;
            }
        } else if (type == OC_MSG_TYPING_UPDATE) {
            oc_typing_update tu;
            if (oc_decode_typing_update(&p, &tu) == OC_OK && tu.user_id == from_user) n++;
        } else {
            n++;
        }
        if (!fence && max && n >= max) return n;
    }
}

/* Everything `c` has sent so far has been handled: its fence is answered. */
static int fenced(client *c) { return count_frames(c, 0, 0, 0, NULL, 1, 0) == 0; }

static void set_read_timeout(client *c, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* The frames that fan out are bounded per connection (ARCH-22): typing past its
 * limit is dropped; reactions and call signalling past theirs are refused with
 * SEND_RATE_LIMITED; presence past its limit is not lost but delayed, and what
 * finally goes out is the state as it stands. And one connection writing as
 * fast as it can is read at most a budget's worth per turn. */
static void test_fanout_limits(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0);
    uint64_t ua = 0; CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0);
    uint64_t ub = 0; CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    /* Every count below ends at a fence, never at a timeout; the timeout only
     * bounds a failure. The presence window is shortened so its last word does
     * not cost ten seconds. */
    set_read_timeout(&a, 5000); set_read_timeout(&b, 5000);
    oc_netloop_set_presence_rate_ms(600);
    CHECK(fenced(&a) && fenced(&b));               /* the sign-ins' presence, read past */

    uint8_t buf[128]; oc_wbuf w;

    /* Typing: ten in a burst, three reach the other member. */
    for (int i = 0; i < 10; i++) {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_typing ty = { 1 };
        CHECK(oc_encode_typing(&w, OC_PROTOCOL_VERSION, &ty) == OC_OK);
        CHECK(send_frame(&b, buf, w.len) == 0);
    }
    CHECK(fenced(&b));                              /* bob's burst is handled... */
    CHECK(count_frames(&a, OC_MSG_TYPING_UPDATE, 0, ub, NULL, 1, 0) == 3);   /* ...and alice has what it made */

    /* Presence: eight changes, away first and online last. Five go out now; the
     * last word follows once the window has passed, and it is ONLINE. */
    for (int i = 0; i < 8; i++) {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_set_presence sp = { i % 2 ? OC_PRESENCE_ONLINE : OC_PRESENCE_AWAY };
        CHECK(oc_encode_set_presence(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
    }
    uint8_t st = 0;
    CHECK(fenced(&a));
    CHECK(count_frames(&b, OC_MSG_PRESENCE_UPDATE, 0, ua, &st, 1, 0) == 5);
    CHECK(st == OC_PRESENCE_AWAY);
    CHECK(count_frames(&b, OC_MSG_PRESENCE_UPDATE, 0, ua, &st, 0, 1) == 1);   /* the window passes */
    CHECK(st == OC_PRESENCE_ONLINE);
    CHECK(count_frames(&b, OC_MSG_PRESENCE_UPDATE, 0, ua, &st, 1, 0) == 0);   /* and that was all */
    oc_netloop_set_presence_rate_ms(0);

    /* Reactions: thirty-five in a burst, the last five refused. */
    for (int i = 0; i < 35; i++) {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_react rc = { 1, 1, oc_slice_str(":+1:"), (uint8_t)(i % 2 ? OC_REACT_REMOVE : OC_REACT_ADD) };
        CHECK(oc_encode_react(&w, OC_PROTOCOL_VERSION, &rc) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
    }
    CHECK(count_frames(&a, OC_MSG_ERROR, OC_ERR_SEND_RATE_LIMITED, 0, NULL, 1, 0) == 5);

    /* Call signalling: sixty-five in a burst, the last five refused. */
    for (int i = 0; i < 65; i++) {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_call_share cs = { 1, 0 };
        CHECK(oc_encode_call_share(&w, OC_PROTOCOL_VERSION, &cs) == OC_OK);
        CHECK(send_frame(&a, buf, w.len) == 0);
    }
    CHECK(count_frames(&a, OC_MSG_ERROR, OC_ERR_SEND_RATE_LIMITED, 0, NULL, 1, 0) == 5);

    /* The read budget: two megabytes of frames written as one burst are read a
     * budget's worth per turn, however fast they arrive. Typing frames, because
     * past their limit they cost the daemon nothing but the read. */
    {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_typing ty = { 1 };
        CHECK(oc_encode_typing(&w, OC_PROTOCOL_VERSION, &ty) == OC_OK);
        size_t one = w.len, total = 2u << 20, n = total / one;
        uint8_t *burst = malloc(n * one);
        CHECK(burst != NULL);
        for (size_t i = 0; burst && i < n; i++) memcpy(burst + i * one, buf, one);
        oc_netloop_stats_reset();
        if (burst) CHECK(write_all(&b.conn, burst, n * one) == 0);
        oc_netloop_stats st2;
        for (int i = 0; i < 500; i++) {
            oc_netloop_stats_get(&st2);
            if (st2.bytes_read >= n * one) break;
            usleep(10000);
        }
        oc_netloop_stats_get(&st2);
        CHECK(st2.bytes_read >= n * one);
        CHECK(st2.turn_read_max <= 256u * 1024u + OC_READ_CHUNK);   /* OC_READ_BUDGET + a chunk */
        free(burst);
    }

    client_close(&a);
    client_close(&b);
}

/* Seed `n` messages of `len` bytes into #general through the writer, which
 * bypasses the wire send limit; `tag` keeps each batch's idempotency tokens
 * apart. Each is fanned out to whoever is connected, like any other send. */
static void seed_messages(oc_dbwriter *dbw, uint64_t author, int n, size_t len, uint8_t tag) {
    static uint8_t big[60000];
    memset(big, 'x', sizeof big);
    if (len > sizeof big) len = sizeof big;
    for (int i = 0; i < n; i++) {
        oc_job *j = oc_job_new(OC_JOB_SEND, 0);
        if (!j) { CHECK(0); return; }
        j->user_id = author; j->channel_id = 1;
        memset(j->idem, 0, OC_IDEM_LEN);
        j->idem[0] = (uint8_t)i; j->idem[1] = tag; j->idem[2] = (uint8_t)(i >> 8);
        oc_job_set_body(j, big, len);
        oc_dbwriter_submit(dbw, j);
    }
}

/* Wait until `author` has `n` messages stored: the writer takes seeded sends
 * asynchronously, and the loop owns its results, so the database is asked
 * directly, read-only. */
static int wait_messages_stored(uint64_t author, int n) {
    int got = -1;
    for (int i = 0; i < 1000 && got < n; i++) {
        sqlite3 *rdb = NULL;
        if (sqlite3_open_v2("build/itest_netloop.db", &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(rdb, "SELECT count(*) FROM messages WHERE author_id=?;", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)author);
                if (sqlite3_step(st) == SQLITE_ROW) got = sqlite3_column_int(st, 0);
            }
            sqlite3_finalize(st);
            sqlite3_close(rdb);
        }
        if (got < n) usleep(10000);
    }
    return got >= n;
}

/* read_frame, but telling apart the two ways of getting nothing: 1 a frame,
 * 0 nothing within the socket's read timeout, -1 the daemon closed or the
 * stream broke. A test about the daemon dropping a connection must not count
 * a quiet one as dropped. */
static int read_frame_or_quiet(client *c, oc_header *hdr, oc_rbuf *p) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r < 0) return -1;
        if (r == 1) {
            if (oc_parse_frame(frame, flen, hdr, p) != OC_OK) return -1;
            if (hdr->msg_type == OC_MSG_PRESENCE_UPDATE || hdr->msg_type == OC_MSG_TYPING_UPDATE) continue;
            return 1;
        }
        uint8_t buf[4096]; size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, buf, sizeof buf, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) return 0;
        if (st != OC_TLS_OK || n == 0) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

static int send_backfill_after(client *c, uint64_t after) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { 1, after };
    oc_backfill_request req = { 1, &cur };
    if (oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) != OC_OK) return -1;
    return write_all(&c->conn, buf, w.len);
}

/* A reconnect backfill far larger than the output cap is REPLAYED, a slice at a
 * time, to a reader that stops reading and then resumes: all of it arrives, in
 * ascending order, and a message sent while it replays arrives after
 * BACKFILL_DONE rather than inside it. Before replay was paced, the same
 * backfill went out in one go and the connection was dropped at the cap.
 *
 * And the cap still holds: a reader that stops reading during a replay while
 * the channel keeps producing is dropped once what is waiting for it passes the
 * cap, rather than growing the daemon's memory without bound. */
static void test_out_buffer_cap(int port, const uint8_t *pin, oc_dbwriter *dbw, uint64_t flooder) {
    CHECK(flooder != 0);
    /* ~28 MB: far more than any kernel buffer, so the daemon's own pacing is what
     * is being exercised, not TCP's. Bounded by OC_BACKFILL_MAX (500) on replay. */
    seed_messages(dbw, flooder, 480, 60000, 0xC7);
    CHECK(wait_messages_stored(flooder, 480));

    client v;
    CHECK(client_open(&v, port, pin) == 0);
    CHECK(do_handshake(&v) == 0);
    uint64_t uv = 0;
    CHECK(do_auth(&v, "flooder", "pw", &uv) == 0);
    /* The receive buffer is left as the kernel sizes it. Shrinking it after
     * the connection is up leaves TCP to the sender's zero-window probes, which
     * back off for longer than the silence below: the connection looks stalled
     * whatever the daemon does. ~28 MB outgrows any buffer anyway. */
    /* How long a silence means "nothing more is coming". A passing run never
     * waits it out -- the first loop below ends on the live message, the second
     * on the close -- so it can be long enough that a slow or paused machine is
     * not mistaken for a daemon that stopped. */
    { struct timeval tv = { 10, 0 }; setsockopt(v.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }

    /* An EXPLICIT, non-zero cursor: "everything after message 1", which is all
     * 480 of them. A cursor of 0 would mean "I hold no history, send me the
     * tail" and yield only OC_BACKFILL_TAIL messages. */
    CHECK(send_backfill_after(&v, 1) == 0);
    /* The replay has begun once its first message is here; ~28 MB behind it
     * cannot drain while this reader is silent, so it is still replaying when
     * the live message below is sent. */
    int replayed = 0, done = 0, closed = 0, ascending = 1, live_after_done = 0, live_early = 0;
    uint64_t last = 0;
    for (int i = 0; i < 1000 && !replayed; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame_or_quiet(&v, &hdr, &p) != 1) break;
        oc_broadcast bc;
        if (hdr.msg_type == OC_MSG_BROADCAST && oc_decode_broadcast(&p, &bc) == OC_OK) {
            last = bc.message_id;
            replayed = 1;
        }
    }
    CHECK(replayed == 1);

    /* Something new while the replay is part-sent. */
    client b;
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    {
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_send s; memset(&s, 0, sizeof s);
        s.channel_id = 1; memset(s.idem, 0x5E, OC_IDEM_SIZE);
        s.body = oc_slice_str("while you were replaying");
        CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK);
        CHECK(write_all(&b.conn, buf, w.len) == 0);
    }
    /* bob's own copy says the daemon has taken the send and fanned it out:
     * the live message is waiting behind the replay for the silent reader. */
    {
        oc_header hdr; oc_rbuf p; oc_broadcast bc; int mine = 0;
        for (int i = 0; i < 64 && !mine; i++)
            if (read_type(&b, OC_MSG_BROADCAST, &hdr, &p) == 0 && oc_decode_broadcast(&p, &bc) == OC_OK &&
                bc.body.len == 24 && memcmp(bc.body.ptr, "while you were replaying", 24) == 0) mine = 1;
        CHECK(mine);
    }

    for (int i = 0; i < 6000 && !live_after_done; i++) {
        oc_header hdr; oc_rbuf p;
        int rr = read_frame_or_quiet(&v, &hdr, &p);
        if (rr < 0) { closed = 1; break; }
        if (rr == 0) break;                          /* nothing more is coming */
        if (hdr.msg_type == OC_MSG_BACKFILL_DONE) {
            done = 1;
            if (live_early) break;                   /* the live one already came, too soon */
            continue;
        }
        if (hdr.msg_type != OC_MSG_BROADCAST) continue;
        oc_broadcast bc;
        if (oc_decode_broadcast(&p, &bc) != OC_OK) { CHECK(0); break; }
        /* Earlier suites posted to #general too, bob among them: the live
         * message is known by what it says. */
        int live = bc.body.len == 24 && memcmp(bc.body.ptr, "while you were replaying", 24) == 0;
        if (live) { if (done) live_after_done = 1; else live_early = 1; continue; }
        if (!done) {
            if (bc.message_id <= last) ascending = 0;
            last = bc.message_id;
            replayed++;
        }
    }
    if (closed || !done || replayed < 480 || live_early || !live_after_done)
        printf("  out_buffer_cap: closed=%d done=%d replayed=%d live_early=%d live_after_done=%d\n",
               closed, done, replayed, live_early, live_after_done);
    CHECK(closed == 0);
    CHECK(done == 1);
    CHECK(replayed >= 480 && replayed <= 500);   /* ours, plus earlier suites', up to OC_BACKFILL_MAX */
    CHECK(ascending);
    CHECK(live_early == 0 && live_after_done == 1);
    client_close(&b);

    /* The cap: replay again, stay silent, and let the channel produce ~1.5 MB
     * behind it. What waits for this reader passes 1 MiB and it is dropped.
     * Each step waits on something it can see rather than on time: the replay
     * has begun (its first message is here, and ~28 MB behind it cannot drain
     * into any kernel buffer while this reader is silent), and the channel's
     * new messages have been fanned out (bob, reading, has every one of them,
     * and the loop fans a message out to all its readers at once). */
    CHECK(send_backfill_after(&v, 1) == 0);
    int started = 0;
    for (int i = 0; i < 1000 && !started; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame_or_quiet(&v, &hdr, &p) != 1) break;
        started = hdr.msg_type == OC_MSG_BROADCAST;
    }
    CHECK(started);
    client w;
    CHECK(client_open(&w, port, pin) == 0);
    CHECK(do_handshake(&w) == 0);
    CHECK(do_auth(&w, "bob", "pw-bob", &ub) == 0);
    { struct timeval tv = { 10, 0 }; setsockopt(w.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
    seed_messages(dbw, flooder, 25, 60000, 0xC8);
    int seen = 0;
    for (int i = 0; i < 1000 && seen < 25; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame_or_quiet(&w, &hdr, &p) != 1) break;
        if (hdr.msg_type == OC_MSG_BROADCAST) seen++;
    }
    CHECK(seen == 25);
    client_close(&w);
    int closed2 = 0, frames2 = 0, done2 = 0, after2 = 0;
    for (int i = 0; i < 20000; i++) {
        oc_header hdr; oc_rbuf p;
        int rr = read_frame_or_quiet(&v, &hdr, &p);
        if (rr < 0) { closed2 = 1; break; }
        if (rr == 0) break;                          /* quiet, and still connected */
        frames2++;
        if (hdr.msg_type == OC_MSG_BACKFILL_DONE) done2 = 1;
        else if (done2 && hdr.msg_type == OC_MSG_BROADCAST) after2++;
    }
    if (!closed2) printf("  out_buffer_cap: not closed; frames=%d done=%d broadcasts_after_done=%d\n",
                         frames2, done2, after2);
    CHECK(closed2 == 1);
    client_close(&v);
}

/* Wait until `c` is told `uid` has presence `status`, passing over what it was
 * told before (a sign-in's snapshot among it); 0, or -1 if it never is. */
static int wait_presence_of(client *c, uint64_t uid, uint8_t status) {
    for (int i = 0; i < 4096; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame_raw(c, &hdr, &p) != 0) return -1;
        if (hdr.msg_type != OC_MSG_PRESENCE_UPDATE) continue;
        oc_presence_update pu;
        if (oc_decode_presence_update(&p, &pu) == OC_OK && pu.user_id == uid && pu.status == status) return 0;
    }
    return -1;
}

/* The loop's worst turn since the last reset, once the turn under way now has
 * been counted: a turn is recorded when the next begins (netloop.h). */
static uint64_t settled_turn_max(client *poke) {
    /* A turn's time is recorded when the next begins, and an idle loop begins
     * one only every half second: so `poke` asks for the channel list, twice,
     * and each answer is a turn -- cheap ones, far below what is measured. */
    oc_netloop_stats st;
    oc_netloop_stats_get(&st);
    uint64_t t0 = st.turns;
    for (int k = 1; k <= 2; k++) {
        uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        if (oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK) (void)send_frame(poke, buf, w.len);
        for (int i = 0; i < 300 && st.turns < t0 + (uint64_t)k; i++) {
            usleep(1000);
            oc_netloop_stats_get(&st);
        }
    }
    for (int i = 0; i < 300 && st.turns <= t0 + 1; i++) { usleep(10000); oc_netloop_stats_get(&st); }
    return st.turn_max_us;
}

/* A sign-in costs the loop about what telling everyone a presence change does
 * (ARCH-22). Both reach every one of a hundred people online; the sign-in also
 * sends the newcomer who each of them is, which with the indexes is a lookup per
 * person. Held by the connections the loop examines, not by time: a presence
 * change is one walk of everyone, a sign-in two (the newcomer's snapshot and the
 * announcement), and a walk per person -- how each person's presence and
 * do-not-disturb were found before the indexes -- is a hundred. The worst turn
 * of each is printed as a measurement; it is not a bound, since one preemption
 * of the loop inside the window moves it. */
static void test_login_bound(int port, const uint8_t *pin) {
    enum { N = 100, R = 4 };
    static client crowd[N];
    uint64_t uids[N];
    int open_ok = 1;
    for (int i = 0; i < N && open_ok; i++) {
        char un[16]; snprintf(un, sizeof un, "u%03d", i);
        open_ok = client_open(&crowd[i], port, pin) == 0 && do_handshake(&crowd[i]) == 0 &&
                  do_auth(&crowd[i], un, "pw", &uids[i]) == 0;
    }
    CHECK(open_ok);
    if (!open_ok) return;
    uint64_t presence_us = UINT64_MAX, login_us = UINT64_MAX, presence_v = UINT64_MAX, login_v = UINT64_MAX;
    for (int r = 0; r < R; r++) {
        uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_set_presence sp = { (uint8_t)(r % 2 ? OC_PRESENCE_ONLINE : OC_PRESENCE_AWAY) };
        CHECK(oc_encode_set_presence(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK);
        oc_netloop_stats_reset();
        CHECK(send_frame(&crowd[0], buf, w.len) == 0);
        CHECK(wait_presence_of(&crowd[N - 1], uids[0], sp.status) == 0);
        uint64_t m = settled_turn_max(&crowd[0]);
        if (m < presence_us) presence_us = m;
        oc_netloop_stats st; oc_netloop_stats_get(&st);
        if (st.live_visits < presence_v) presence_v = st.live_visits;
    }
    for (int r = 0; r < R; r++) {
        client b;
        uint64_t ub = 0;
        CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0);
        oc_netloop_stats_reset();
        CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
        CHECK(wait_presence_of(&crowd[N - 1], ub, OC_PRESENCE_ONLINE) == 0);
        uint64_t m = settled_turn_max(&crowd[0]);
        if (m < login_us) login_us = m;
        oc_netloop_stats st; oc_netloop_stats_get(&st);
        if (st.live_visits < login_v) login_v = st.live_visits;
        client_close(&b);
        CHECK(wait_presence_of(&crowd[N - 1], ub, OC_PRESENCE_OFFLINE) == 0);   /* gone: the next is a first sign-in */
    }
    printf("  sign-in beside %d people: %llu connections examined, worst turn %llu us; "
           "a presence change to them: %llu, %llu us\n", N, (unsigned long long)login_v,
           (unsigned long long)login_us, (unsigned long long)presence_v, (unsigned long long)presence_us);
    CHECK(presence_v >= N && presence_v < 2 * (N + 8));            /* one walk of everyone */
    CHECK(login_v <= 3 * presence_v);                              /* a few walks, never one per person */
    for (int i = 0; i < N; i++) client_close(&crowd[i]);
}

static uint64_t g_crowd_ids[100];   /* u000..u099, registered at setup */

/* The next GROUP_INFO for `handle`, its id; 0 if none came. */
static uint64_t read_group_info(client *c, const char *handle, uint16_t *n_members) {
    for (int i = 0; i < 64; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_type(c, OC_MSG_GROUP_INFO, &hdr, &p) != 0) return 0;
        uint64_t mem[OC_MAX_GROUP_MEMBERS]; oc_group_info gi;
        if (oc_decode_group_info(&p, &gi, mem, OC_MAX_GROUP_MEMBERS) != OC_OK) return 0;
        if (gi.handle.len == strlen(handle) && memcmp(gi.handle.ptr, handle, gi.handle.len) == 0) {
            if (n_members) *n_members = gi.count;
            return gi.group_id;
        }
    }
    return 0;
}

/* The next CHANNEL_INFO for `ch`: its joined flag, or -1. */
static int read_chinfo_joined(client *c, uint64_t ch) {
    for (int i = 0; i < 64; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_type(c, OC_MSG_CHANNEL_INFO, &hdr, &p) != 0) return -1;
        oc_channel_info ci;
        if (oc_decode_channel_info(&p, &ci) == OC_OK && ci.channel_id == ch) return ci.joined;
    }
    return -1;
}

/* Beside every CHANNEL_INFO of a channel goes its CHANNEL_GROUPS, none
 * included (REQ-309); and the groups come to a client at sign-in after its user
 * list, unasked (REQ-307). */
static void test_groups_unasked(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    oc_header hdr; oc_rbuf p;
    uint8_t buf[128]; oc_wbuf w;
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_users(&w, OC_PROTOCOL_VERSION) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_USER_LIST, &hdr, &p) == 0);
    CHECK(read_type(&a, OC_MSG_GROUPS_END, &hdr, &p) == 0);      /* never asked for */

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("nogroups"), 1 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame_raw(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    int saw = 0;
    for (int i = 0; i < 16 && !saw; i++) {
        if (read_frame_raw(&a, &hdr, &p) != 0) break;
        if (hdr.msg_type != OC_MSG_CHANNEL_GROUPS) continue;
        uint64_t gids[OC_MAX_CHANNEL_GROUPS]; oc_channel_groups cg;
        CHECK(oc_decode_channel_groups(&p, &cg, gids, OC_MAX_CHANNEL_GROUPS) == OC_OK);
        CHECK(cg.channel_id == ci.channel_id && cg.count == 0);
        saw = 1;
    }
    CHECK(saw);
    client_close(&a);
}

/* User groups over the wire (REQ-307-309): an admin creates one and everyone
 * hears of it; a member may not; a group given to a private channel brings its
 * member in -- told with the channel and its groups -- who can then post there,
 * cannot leave it, and is told the channel is gone when taken out of the group.
 * Carol plays the member here. */
static void test_groups_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "carol", "pw", &ub) == 0);   /* bob was removed by test_admin_vertical */
    oc_header hdr; oc_rbuf p;
    uint8_t buf[512]; oc_wbuf w;

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_group cg = { oc_slice_str("wirecrew"), oc_slice_str("Wire crew"), oc_slice_str("") };
    CHECK(oc_encode_create_group(&w, OC_PROTOCOL_VERSION, &cg) == OC_OK && send_frame(&b, buf, w.len) == 0);
    uint16_t code = 0;
    CHECK(read_error(&b, &code) == 0 && code == OC_ERR_FORBIDDEN);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint64_t g = read_group_info(&a, "wirecrew", NULL);
    CHECK(g != 0 && read_group_info(&b, "wirecrew", NULL) == g);          /* everyone hears */

    static oc_group_members_op op;
    op.group_id = g; op.count = 1; op.user_ids[0] = ub;
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_group_members_op(&w, OC_PROTOCOL_VERSION, OC_MSG_GROUP_ADD_MEMBERS, &op) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    uint16_t nm = 0;
    CHECK(read_group_info(&b, "wirecrew", &nm) == g && nm == 1);

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_create_channel cc = { oc_slice_str("crewroom"), 0 };
    CHECK(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_CHANNEL_INFO, &hdr, &p) == 0);
    oc_channel_info ci; CHECK(oc_decode_channel_info(&p, &ci) == OC_OK);
    uint64_t ch = ci.channel_id;

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_channel_group_op cgo = { ch, g };
    CHECK(oc_encode_channel_group_op(&w, OC_PROTOCOL_VERSION, OC_MSG_CHANNEL_ADD_GROUP, &cgo) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_chinfo_joined(&b, ch) == 1);                               /* bob is in, and told */
    CHECK(read_type(&b, OC_MSG_CHANNEL_GROUPS, &hdr, &p) == 0);
    uint64_t gids[OC_MAX_CHANNEL_GROUPS]; oc_channel_groups cgs;
    CHECK(oc_decode_channel_groups(&p, &cgs, gids, OC_MAX_CHANNEL_GROUPS) == OC_OK);
    CHECK(cgs.channel_id == ch && cgs.count == 1 && cgs.group_ids[0] == g);

    /* Alice's member list says carol is in only through the group. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_list_members lm = { ch };
    CHECK(oc_encode_list_members(&w, OC_PROTOCOL_VERSION, &lm) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_CHANNEL_VIA_GROUP, &hdr, &p) == 0);
    {
        uint64_t vids[OC_MAX_MEMBER_LIST]; oc_channel_via_group cv;
        CHECK(oc_decode_channel_via_group(&p, &cv, vids, OC_MAX_MEMBER_LIST) == OC_OK);
        CHECK(cv.channel_id == ch && cv.count == 1 && cv.user_ids[0] == ub);
    }

    /* He can post there, and alice hears it; an @group reaches him. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_send sm; memset(&sm, 0, sizeof sm);
    sm.channel_id = ch; memset(sm.idem, 0x47, OC_IDEM_SIZE);
    sm.body = oc_slice_str("in via @wirecrew");
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK && send_frame(&b, buf, w.len) == 0);
    CHECK(read_type(&a, OC_MSG_BROADCAST, &hdr, &p) == 0);
    oc_broadcast bc; CHECK(oc_decode_broadcast(&p, &bc) == OC_OK && bc.channel_id == ch && bc.author_id == ub);

    /* A group larger than the notice's eight names, none of them in the
     * channel: the notice names eight, and the total follows it (REQ-308). */
    {
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_create_group cb = { oc_slice_str("wirecrowd"), oc_slice_str("Crowd"), oc_slice_str("") };
        CHECK(oc_encode_create_group(&w, OC_PROTOCOL_VERSION, &cb) == OC_OK && send_frame(&a, buf, w.len) == 0);
        uint64_t gb = read_group_info(&a, "wirecrowd", NULL);
        CHECK(gb != 0);
        static oc_group_members_op bop;
        bop.group_id = gb; bop.count = 10;
        for (int k = 0; k < 10; k++) bop.user_ids[k] = g_crowd_ids[k];
        uint8_t big[256]; oc_wbuf bw; oc_wbuf_init(&bw, big, sizeof big);
        CHECK(oc_encode_group_members_op(&bw, OC_PROTOCOL_VERSION, OC_MSG_GROUP_ADD_MEMBERS, &bop) == OC_OK);
        CHECK(send_frame(&a, big, bw.len) == 0);
        uint16_t nb = 0;
        CHECK(read_group_info(&a, "wirecrowd", &nb) == gb && nb == 10);
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_send sb; memset(&sb, 0, sizeof sb);
        sb.channel_id = ch; memset(sb.idem, 0x48, OC_IDEM_SIZE);
        sb.body = oc_slice_str("heads up @wirecrowd");
        CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sb) == OC_OK && send_frame(&a, buf, w.len) == 0);
        CHECK(read_type(&a, OC_MSG_MENTION_UNRESOLVED, &hdr, &p) == 0);
        oc_mention_unresolved mu; CHECK(oc_decode_mention_unresolved(&p, &mu) == OC_OK && mu.count == 8);
        CHECK(read_frame_raw(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_MENTION_UNRESOLVED_MORE);
        oc_mention_unresolved_more um;
        CHECK(oc_decode_mention_unresolved_more(&p, &um) == OC_OK && um.total == 10 &&
              um.message_id == mu.message_id);
    }

    /* He cannot leave: the group keeps him in. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_channel_ref lc = { ch };
    CHECK(oc_encode_leave_channel(&w, OC_PROTOCOL_VERSION, &lc) == OC_OK && send_frame(&b, buf, w.len) == 0);
    CHECK(read_error(&b, &code) == 0 && code == OC_ERR_MEMBER_VIA_GROUP);

    /* Out of the group: out of the channel, and told. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_group_members_op(&w, OC_PROTOCOL_VERSION, OC_MSG_GROUP_REMOVE_MEMBERS, &op) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_chinfo_joined(&b, ch) == 0);

    oc_wbuf_init(&w, buf, sizeof buf);
    oc_group_ref gr = { g };
    CHECK(oc_encode_delete_group(&w, OC_PROTOCOL_VERSION, &gr) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_type(&b, OC_MSG_GROUP_DELETED, &hdr, &p) == 0);
    oc_group_ref gd; CHECK(oc_decode_group_ref(&p, &gd) == OC_OK && gd.group_id == g);
    client_close(&b);
    client_close(&a);
}

/* The two states where the specification promises UNEXPECTED_MSG_TYPE and the
 * daemon used to say nothing: a first frame that is not HELLO (socket closed
 * with nothing written) and a post-auth type no branch claims (ignored
 * silently). Our own client can trigger neither; a third-party implementer gets
 * either a bare close or a frame that vanishes with the connection still up,
 * which is the least debuggable outcome available. */
static void test_unexpected_msg_type(int port, const uint8_t *pin) {
    /* --- first frame is not HELLO ------------------------------------------ */
    {
        client c;
        CHECK(client_open(&c, port, pin) == 0);
        uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
        CHECK(write_all(&c.conn, buf, w.len) == 0);

        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&c, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_REJECT);
        /* REJECT, not ERROR: no version has been negotiated, and REJECT is the
         * frame frozen at 1 that any peer of any era can read. */
        CHECK(hdr.version == 1);
        oc_reject rej;
        CHECK(oc_decode_reject(&p, &rej) == OC_OK);
        CHECK(rej.code == OC_ERR_UNEXPECTED_MSG_TYPE);
        CHECK(read_frame(&c, &hdr, &p) != 0);      /* and then closed */
        client_close(&c);
    }

    /* --- post-auth type that no branch handles ---------------------------- */
    {
        client c;
        CHECK(client_open(&c, port, pin) == 0);
        CHECK(do_handshake(&c) == 0);
        uint64_t uid = 0;
        CHECK(do_auth(&c, "alice", "pw-alice", &uid) == 0);

        /* A well-formed frame whose msg_type is not defined at this version.
         * Built by encoding a real frame and patching the type in place —
         * oc_frame_begin/end are internal to protocol.c, and the point is the
         * dispatcher, not the encoder. msg_type is the u16 at offset 6. */
        uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
        buf[6] = 0xEE; buf[7] = 0xEE;
        CHECK(write_all(&c.conn, buf, w.len) == 0);

        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&c, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_ERROR);
        oc_error err;
        CHECK(oc_decode_error(&p, &err) == OC_OK);
        CHECK(err.code == OC_ERR_UNEXPECTED_MSG_TYPE);
        CHECK(err.fatal == 1);
        CHECK(read_frame(&c, &hdr, &p) != 0);      /* fatal means closed */
        client_close(&c);
    }
}

/* A post-handshake frame carrying a version other than the negotiated one is
 * refused with a fatal ERROR and the connection closed. Before this, hdr.version
 * was written by both sides and read by neither: the field cost two bytes a
 * frame and bought nothing, and a peer that disagreed about a layout misparsed
 * the payload instead of being told — surfacing as a decode failure somewhere
 * downstream, which is what "connection lost" usually turns out to be. */
static void test_frame_version_mismatch(int port, const uint8_t *pin) {
    client a;
    CHECK(client_open(&a, port, pin) == 0);
    CHECK(do_handshake(&a) == 0);
    uint64_t ua = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);

    /* A frame that is valid in every respect except its version stamp. */
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION + 1) == OC_OK);
    CHECK(write_all(&a.conn, buf, w.len) == 0);

    oc_header hdr; oc_rbuf p;
    CHECK(read_frame(&a, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_ERROR);
    oc_error err;
    CHECK(oc_decode_error(&p, &err) == OC_OK);
    CHECK(err.code == OC_ERR_VERSION_MISMATCH);
    CHECK(err.fatal == 1);
    /* The ERROR itself carries the negotiated version, not the offending one —
     * it has to be readable by the peer we are about to hang up on. */
    CHECK(hdr.version == OC_PROTOCOL_VERSION);
    /* Fatal means fatal: nothing further, and the socket closes. */
    CHECK(read_frame(&a, &hdr, &p) != 0);
    client_close(&a);

    /* The same frame at the negotiated version is answered normally — proving
     * the check rejects the version and not the frame. */
    client b;
    CHECK(client_open(&b, port, pin) == 0);
    CHECK(do_handshake(&b) == 0);
    uint64_t ub = 0;
    CHECK(do_auth(&b, "alice", "pw-alice", &ub) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(write_all(&b.conn, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0);
    CHECK(hdr.msg_type == OC_MSG_CHANNEL_LIST);
    client_close(&b);
}

/* Concurrency load: many clients connect, authenticate,
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

/* The per-address cap (OPENCHIME_MAX_CONNS_PER_IP, 2 here) on the TLS port, run
 * in test_http_stack's loop: two in and the third refused at accept, a close
 * giving its place back, and IPv6 counted by its /64 apart from 127.0.0.1. */
static void throttle_checks(int port, const uint8_t *pin2) {
    /* Two connections from loopback are within the cap. */
    client a, b;
    int in = -1;                     /* once the health connections' closes are counted */
    for (int i = 0; i < 100 && in != 0; i++) {
        in = client_open(&a, port, pin2);
        if (in != 0) usleep(20000);
    }
    CHECK(in == 0);
    CHECK(client_open(&b, port, pin2) == 0);
    /* The third is refused at accept — its TLS handshake never completes. */
    client c;
    int rc = client_open(&c, port, pin2);
    CHECK(rc != 0);
    if (rc == 0) client_close(&c);

    /* A close gives its place back: once the daemon has seen one go, the next
     * connection from the same address is let in, and the one after it is not. */
    client_close(&a);
    int back = -1;
    for (int i = 0; i < 100 && back != 0; i++) {
        back = client_open(&c, port, pin2);
        if (back != 0) usleep(20000);
    }
    CHECK(back == 0);
    client d;
    int over = client_open(&d, port, pin2);
    CHECK(over != 0);
    if (over == 0) client_close(&d);

    /* Over IPv6 the source is the /64, and ::1 is counted like any other
     * address: two are in, the third is not -- while the two IPv4 connections
     * still held count against 127.0.0.1, not against it. */
    int probe = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 p6; memset(&p6, 0, sizeof p6);
    p6.sin6_family = AF_INET6; p6.sin6_addr = in6addr_loopback;
    int have6 = probe >= 0 && bind(probe, (struct sockaddr *)&p6, sizeof p6) == 0;
    if (probe >= 0) close(probe);
    if (have6) {
        g_client_v6 = 1;
        client e, f, g;
        CHECK(client_open(&e, port, pin2) == 0);
        CHECK(client_open(&f, port, pin2) == 0);
        int third = client_open(&g, port, pin2);
        CHECK(third != 0);
        if (third == 0) client_close(&g);
        client_close(&e);
        client_close(&f);
        g_client_v6 = 0;
    } else {
        printf("  (no IPv6 loopback on this host: the IPv6 cap is not checked)\n");
    }

    client_close(&b);
    if (back == 0) client_close(&c);

}


/* A plaintext connection to `port`, retried while the loop comes up; -1 if none. */
static int plain_connect(int port) {
    struct sockaddr_in addr; memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    for (int i = 0; i < 200; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            struct timeval tv = { 5, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            return fd;
        }
        close(fd);
        usleep(20000);
    }
    return -1;
}

static size_t plain_read_all(int fd, char *buf, size_t cap) {
    size_t got = 0;
    while (got + 1 < cap) {
        ssize_t r = recv(fd, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    return got;
}

/* Send `req` on a fresh plaintext connection and read the answer to its close. */
static size_t plain_exchange(int port, const char *req, char *buf, size_t cap) {
    buf[0] = 0;
    int fd = plain_connect(port);
    if (fd < 0) return 0;
    size_t n = 0;
    if (send(fd, req, strlen(req), 0) == (ssize_t)strlen(req)) n = plain_read_all(fd, buf, cap);
    close(fd);
    return n;
}

/* The HTTP stack over the wire (ARCH-32, ARCH-25), on a loop of its own with a
 * health port and a cap of two connections per address:
 *   - /healthz answers OK and every other path the landing page, in plaintext,
 *     served by the loop's I/O threads;
 *   - on the TLS port an HTTP client gets 404 for a path nothing names and 405
 *     for the webhook under the wrong method (the post itself is
 *     test_webhook_vertical, unchanged);
 *   - a client that stalls is answered 408 on either port;
 *   - the health port counts against the same per-address cap as the TLS port;
 *   - and it is the loop's: once the loop stops, nothing answers it. */
static void test_http_stack(int port, int hport) {
    setenv("OPENCHIME_MAX_CONNS_PER_IP", "2", 1);
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);
    unlink("build/itest_http.db"); unlink("build/itest_http.db-wal"); unlink("build/itest_http.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_http.db");
    CHECK(dbw2 != NULL);

    oc_netloop_set_health_port(hport);
    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    static char resp[8192];
    CHECK(plain_exchange(hport, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", resp, sizeof resp) > 0);
    oc_netloop_set_health_port(-1);   /* read at start: this loop has it, no later one */
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "\r\n\r\nOK") &&
          strstr(resp, "Content-Type: text/plain\r\n"));
    CHECK(plain_exchange(hport, "GET /healthz?probe=1 HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "\r\n\r\nOK"));
    CHECK(plain_exchange(hport, "GET / HTTP/1.1\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
          strstr(resp, "text/html") && strstr(resp, "An OpenChime workspace is running here."));
    CHECK(plain_exchange(hport, "POST /anything/else HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi",
                         resp, sizeof resp) > 0);
    CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "An OpenChime workspace"));
    CHECK(plain_exchange(hport, "NOT HTTP\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strncmp(resp, "HTTP/1.1 400 ", 13) == 0);

    /* The TLS port's HTTP side: what it does not serve, it refuses there. */
    client h;
    CHECK(http_client_open(&h, port, pin2) == 0);
    const char *r1 = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(write_all(&h.conn, (const uint8_t *)r1, strlen(r1)) == 0);
    http_read_response(&h, resp, sizeof resp);
    CHECK(strncmp(resp, "HTTP/1.1 404 ", 13) == 0);
    client_close(&h);
    CHECK(http_client_open(&h, port, pin2) == 0);
    const char *r2 = "GET /webhook/00 HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(write_all(&h.conn, (const uint8_t *)r2, strlen(r2)) == 0);
    http_read_response(&h, resp, sizeof resp);
    CHECK(strncmp(resp, "HTTP/1.1 405 ", 13) == 0);
    client_close(&h);

    /* A stalled request's 408 is test_ioloop's, on both kinds of socket. */

    /* One cap for both ports: two idle health connections from loopback fill
     * it, so a third on either port is closed unanswered. */
    usleep(200000);                                       /* the closes above are counted out */
    int a = plain_connect(hport), b = plain_connect(hport);
    CHECK(a >= 0 && b >= 0);
    usleep(100000);
    int c3 = plain_connect(hport);
    CHECK(c3 >= 0);
    if (c3 >= 0) {
        (void)send(c3, "GET /healthz HTTP/1.1\r\n\r\n", 26, 0);
        CHECK(plain_read_all(c3, resp, sizeof resp) == 0);   /* closed at accept, nothing said */
        close(c3);
    }
    client t;
    int over = client_open(&t, port, pin2);
    CHECK(over != 0);
    if (over == 0) client_close(&t);
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    throttle_checks(port, pin2);

    stop_loop(&arg2, th2);
    /* The health port was the loop's: with the loop gone, nobody answers it. */
    {
        int g = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons((uint16_t)hport);
        CHECK(connect(g, (struct sockaddr *)&sa, sizeof sa) != 0);
        close(g);
    }
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    unsetenv("OPENCHIME_MAX_CONNS_PER_IP");
    unlink("build/itest_http.db"); unlink("build/itest_http.db-wal"); unlink("build/itest_http.db-shm");
}

/* Send AUTH_BEGIN; on AUTH_REDIRECT copy the URL out and return 0, on ERROR
 * return its code, else -1. */
static int auth_begin(client *c, const char *source, const char *redirect, const char *challenge,
                      char *url, size_t cap) {
    uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth_begin b = { oc_slice_str(source), oc_slice_str(redirect), oc_slice_str(challenge) };
    if (oc_encode_auth_begin(&w, OC_PROTOCOL_VERSION, &b) != OC_OK) return -1;
    if (write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0) return -1;
    if (hdr.msg_type == OC_MSG_ERROR) {
        oc_error e;
        return oc_decode_error(&p, &e) == OC_OK ? (int)e.code : -1;
    }
    if (hdr.msg_type != OC_MSG_AUTH_REDIRECT) return -1;
    oc_auth_redirect ar;
    if (oc_decode_auth_redirect(&p, &ar) != OC_OK || ar.authorize_url.len >= cap) return -1;
    memcpy(url, ar.authorize_url.ptr, ar.authorize_url.len);
    url[ar.authorize_url.len] = '\0';
    return 0;
}

/* A browser sign-in starts with the daemon building the relay's authorize URL
 * (AUTH.md §8.1). Its own loop, because it needs the relay source switched on. */
static void test_auth_begin(int port) {
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);
    unlink("build/itest_begin.db"); unlink("build/itest_begin.db-wal"); unlink("build/itest_begin.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_begin.db");
    CHECK(dbw2 != NULL);
    /* Any P-256 public key will do: nothing here presents a token. */
    static const char PEM[] =
        "-----BEGIN PUBLIC KEY-----\n"
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEDqHKx+q7OAQmDqj5y9NaFRRyMFoH\n"
        "tdCjHSfxAGUIA9+4UU024HTVfwxlAyEJ/AZuJbyJG5DrtbEcEXSVdX1U1A==\n"
        "-----END PUBLIC KEY-----\n";
    CHECK(oc_dbwriter_configure_oidc(dbw2, "https://central.example", "ws_7f3a 9c/21", PEM,
                                     "https://central.example") == 0);
    CHECK(oc_dbwriter_register_local(dbw2, "alice", "pw-alice", OC_ROLE_OWNER, 2048) != 0);

    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    static const char CH[] = "JBbiqONGWPaAmwXk_8bT6UnlPfrn65D32eZlJS-zGG0";
    client c;
    CHECK(client_open(&c, port, pin2) == 0);
    /* Both sources are offered: enabling the relay did not switch passwords off. */
    {
        CHECK(send_hello(&c, OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION) == 0);
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_WELCOME);
        CHECK(read_frame(&c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_AUTH_CHALLENGE);
        oc_auth_challenge ch;
        CHECK(oc_decode_auth_challenge(&p, &ch) == OC_OK);
        CHECK(ch.n_sources == 2);
        CHECK(ch.sources[0].kind == OC_SOURCE_LOCAL && ch.sources[1].kind == OC_SOURCE_RELAY);
        CHECK(ch.sources[1].id.len == 5 && memcmp(ch.sources[1].id.ptr, "relay", 5) == 0);
    }
    char url[2048];
    /* The whole URL is the daemon's, with everything it was given escaped. */
    CHECK(auth_begin(&c, "relay", "http://127.0.0.1:53111/cb?x=1&y=2", CH, url, sizeof url) == 0);
    CHECK(strcmp(url, "https://central.example/oidc/authorize?workspace=ws_7f3a%209c%2F21"
                      "&redirect_uri=http%3A%2F%2F127.0.0.1%3A53111%2Fcb%3Fx%3D1%26y%3D2"
                      "&nonce=JBbiqONGWPaAmwXk_8bT6UnlPfrn65D32eZlJS-zGG0") == 0);
    CHECK(auth_begin(&c, "relay", "http://localhost/cb", CH, url, sizeof url) == 0);
    CHECK(auth_begin(&c, "relay", "http://[::1]:9/", CH, url, sizeof url) == 0);
    /* Anything that is not loopback is refused, however much it looks like it. */
    static const char *const BAD[] = {
        "https://127.0.0.1/cb", "http://127.0.0.1.evil.example/cb", "http://127.0.0.1@evil.example/",
        "http://evil.example/127.0.0.1", "http://localhost.evil.example/", "http://127.0.0.1:99999999/",
        "http://127.0.0.1:/cb", "http://127.0.0.1/cb#frag", "http://127.0.0.1/c b", "app://callback", "",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
        CHECK(auth_begin(&c, "relay", BAD[i], CH, url, sizeof url) == OC_ERR_AUTH_INVALID_TOKEN);
    /* A challenge that cannot be a SHA-256, and a source nobody offered. */
    CHECK(auth_begin(&c, "relay", "http://127.0.0.1/cb", "short", url, sizeof url) == OC_ERR_AUTH_INVALID_TOKEN);
    CHECK(auth_begin(&c, "acme-sso", "http://127.0.0.1/cb", CH, url, sizeof url) == OC_ERR_AUTH_SOURCE_UNAVAILABLE);
    /* The local source signs in on the daemon's own pages (AUTH.md §8.10): the
     * answer is a path, and the client picks the origin. */
    CHECK(auth_begin(&c, "local", "http://127.0.0.1/cb", CH, url, sizeof url) == 0);
    CHECK(strcmp(url, "/signin?redirect_uri=http%3A%2F%2F127.0.0.1%2Fcb"
                      "&nonce=JBbiqONGWPaAmwXk_8bT6UnlPfrn65D32eZlJS-zGG0") == 0);
    CHECK(auth_begin(&c, "local", "https://evil.example/cb", CH, url, sizeof url) == OC_ERR_AUTH_INVALID_TOKEN);
    client_close(&c);

    /* A loop with no call relay -- the main loop holds the suite's audio socket,
     * so this one has none -- answers a call with CALL_UNAVAILABLE. */
    {
        client a;
        CHECK(client_open(&a, port, pin2) == 0); CHECK(do_handshake(&a) == 0);
        uint64_t ua = 0;
        CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
        oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w;
        oc_wbuf_init(&w, buf, sizeof buf);
        oc_call_join cj = { OC_DEFAULT_CHANNEL, {0}, 0, NULL, OC_CALL_CODEC_VP9 };
        CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK && send_frame(&a, buf, w.len) == 0);
        CHECK(read_type(&a, OC_MSG_ERROR, &hdr, &p) == 0);
        oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_CALL_UNAVAILABLE);
        client_close(&a);
    }

    stop_loop(&arg2, th2);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    unlink("build/itest_begin.db"); unlink("build/itest_begin.db-wal"); unlink("build/itest_begin.db-shm");
}

/* Behind a forwarder (proxyproto.h): the address the limits key on is the one the
 * forwarder's header names, believed only because the forwarder is trusted. Its
 * own loop, with loopback as the trusted forwarder and a per-address cap of two. */
static void test_proxy_header(int port) {
    setenv("OPENCHIME_TRUSTED_PROXIES", "127.0.0.0/8", 1);
    setenv("OPENCHIME_MAX_CONNS_PER_IP", "2", 1);
    oc_tls_server srv2;
    CHECK(oc_tls_server_init(&srv2, NULL, NULL) == 0);
    uint8_t pin2[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv2, pin2) == 0);
    unlink("build/itest_proxy.db"); unlink("build/itest_proxy.db-wal"); unlink("build/itest_proxy.db-shm");
    oc_dbwriter *dbw2 = oc_dbwriter_start("build/itest_proxy.db");
    CHECK(dbw2 != NULL);
    struct loop_arg arg2;
    arg2.port = port; arg2.srv = &srv2; arg2.dbw = dbw2; arg2.stop = 0;
    pthread_t th2;
    CHECK(pthread_create(&th2, NULL, loop_thread, &arg2) == 0);

    static const uint8_t SIG[12] = { 0x0D,0x0A,0x0D,0x0A,0x00,0x0D,0x0A,0x51,0x55,0x49,0x54,0x0A };
    uint8_t from7[28], from8[28];
    memcpy(from7, SIG, 12); from7[12] = 0x21; from7[13] = 0x11; from7[14] = 0; from7[15] = 12;
    memset(from7 + 16, 0, 12);
    from7[16] = 203; from7[17] = 0; from7[18] = 113; from7[19] = 7;
    memcpy(from8, from7, sizeof from8); from8[19] = 8;

    /* TLS begins on the byte after the header, and the session is an ordinary one. */
    client a, b, c, d, e;
    CHECK(client_open_with(&a, port, pin2, from7, sizeof from7) == 0);
    CHECK(do_handshake(&a) == 0);
    /* The cap counts the CLIENT the header names: two from .7, and the third is
     * refused — while .8, through the same forwarder, is somebody else. With the
     * forwarder's own address counted, all of these would be one client. */
    CHECK(client_open_with(&b, port, pin2, from7, sizeof from7) == 0);
    int third = client_open_with(&c, port, pin2, from7, sizeof from7);
    CHECK(third != 0);
    CHECK(client_open_with(&d, port, pin2, from8, sizeof from8) == 0);
    /* A trusted forwarder that sends no header is misconfigured, and is closed
     * rather than guessed about. */
    int bare = client_open(&e, port, pin2);
    CHECK(bare != 0);

    client_close(&a); client_close(&b); client_close(&d);
    if (third == 0) client_close(&c);
    if (bare == 0) client_close(&e);
    stop_loop(&arg2, th2);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    unsetenv("OPENCHIME_TRUSTED_PROXIES");
    unsetenv("OPENCHIME_MAX_CONNS_PER_IP");
    unlink("build/itest_proxy.db"); unlink("build/itest_proxy.db-wal"); unlink("build/itest_proxy.db-shm");
}

int run_netloop_tests(void) {
    printf("itest_netloop: handshake, version REJECT, two-client AUTH+SEND+BROADCAST, backfill, edit/delete, channels, reactions, threads, search, dm, drafts across two devices, load, rate-limit, out-cap, throttle, admin, logout\n");

    /* Attachment blobs go to a build-local dir (REQ-140); the daemon defaults to
     * /data/blobs, which isn't writable in the test sandbox. */
    setenv("OPENCHIME_BLOB_DIR", "build/itest_blobs", 1);
    /* A small call cap, so the call test can fill one (REQ-305). */
    setenv("OPENCHIME_CALL_MAX", "3", 1);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    /* A UDP socket for the loop's relay, so CALL_JOINED carries a real port and
     * token and the call tests can relay. */
    int audio_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua; memset(&ua, 0, sizeof ua);
    ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ua.sin_port = 0;
    CHECK(bind(audio_udp, (struct sockaddr *)&ua, sizeof ua) == 0);
    socklen_t ual = sizeof ua; getsockname(audio_udp, (struct sockaddr *)&ua, &ual);
    uint16_t audio_port = ntohs(ua.sin_port);
    oc_netloop_set_audio(audio_udp, audio_port);   /* the loop's relay runs on it */
    /* Read-aloud with a stub engine (ARCH-111): the wire, the cache and the gate
     * are the daemon's, and no voice model is needed to prove them. */
    oc_netloop_set_tts(&STUB_TTS);
    oc_netloop_set_stt(&STUB_STT);

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
    for (int i = 0; i < 100; i++) {   /* a crowd, for test_login_bound and the groups tests */
        char un[16]; snprintf(un, sizeof un, "u%03d", i);
        g_crowd_ids[i] = oc_dbwriter_register_local(dbw, un, "pw", OC_ROLE_MEMBER, 2048);
        CHECK(g_crowd_ids[i] != 0);
    }
    CHECK(flooder != 0);

    struct loop_arg arg;
    arg.port = 18000 + (int)(getpid() % 2000);
    arg.srv = &srv;
    arg.dbw = dbw;
    arg.stop = 0;
    g_ready_port = arg.port;
    __atomic_store_n(&g_ready_calls, 0, __ATOMIC_RELEASE);   /* per run: OC_TEST_REPEAT runs this again */
    g_ready_connected = 0;
    oc_netloop_set_ready(on_ready, NULL);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, loop_thread, &arg) == 0);
    for (int i = 0; i < 500 && !__atomic_load_n(&g_ready_calls, __ATOMIC_ACQUIRE); i++) usleep(10000);
    CHECK(__atomic_load_n(&g_ready_calls, __ATOMIC_ACQUIRE) == 1 && g_ready_connected);

    if (failures == 0) {
        test_version_reject(arg.port, pin);
        test_frame_version_mismatch(arg.port, pin);
        test_unexpected_msg_type(arg.port, pin);
        test_edit_delete_vertical(arg.port, pin);
        test_channels_vertical(arg.port, pin);
        test_threads_vertical(arg.port, pin);
        test_dm_vertical(arg.port, pin);
        test_drafts_vertical(arg.port, pin);
        test_presence_typing(arg.port, pin);
        test_fanout_limits(arg.port, pin);
        test_presence_dnd(arg.port, pin);
        test_read_aloud_vertical(arg.port, pin);
        test_read_aloud_store_window(arg.port, pin, dbw);
        test_voice_input_vertical(arg.port, pin);
        test_voice_input_rate(arg.port, pin);
        test_upload_abandoned(arg.port, pin);
        test_upload_restarted(arg.port, pin, dbw);
        test_webhook_vertical(arg.port, pin);
        test_notify_prefs_vertical(arg.port, pin);
        test_call_vertical(arg.port, pin);
        test_call_leave_while_joining(arg.port, pin, dbw);
        test_alerts_wire(arg.port, pin, dbw);
        test_restore_wire(arg.port, pin);
        test_call_state_audience(arg.port, pin);
        test_call_share(arg.port, pin);
        test_call_udp_vertical(arg.port, pin, audio_port);
        test_call_transports(arg.port, pin, audio_port);
        test_call_routed(arg.port + 126);
        test_concurrent_load(arg.port, pin);
        test_login_bound(arg.port, pin);
        test_send_rate_limit(arg.port, pin);
        test_out_buffer_cap(arg.port, pin, dbw, flooder);
        test_admin_vertical(arg.port, pin);
        test_groups_vertical(arg.port, pin);
        test_groups_unasked(arg.port, pin);
        test_http_stack(arg.port + 128, arg.port + 129);
        test_auth_begin(arg.port + 124);
        test_proxy_header(arg.port + 125);
    }

    stop_loop(&arg, th);
    unsetenv("OPENCHIME_CALL_MAX");

    if (failures == 0) {
        test_voice_input_absent(arg.port + 124, 1);
        test_voice_input_absent(arg.port + 125, 0);
        test_web_signin(arg.port + 128);
        test_device_signin(arg.port + 130);
    }

    oc_netloop_set_audio(-1, 0);
    close(audio_udp);

    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    unlink("build/itest_netloop.db");
    unlink("build/itest_netloop.db-wal");
    unlink("build/itest_netloop.db-shm");

    return failures;
}
