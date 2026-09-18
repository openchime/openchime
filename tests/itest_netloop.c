/* Integration test for the event loop (netloop.c) end to end: TLS handshake,
 * HELLO->WELCOME, version REJECT, and the message vertical — two clients
 * authenticate, one SENDs, and both receive the BROADCAST while the sender is
 * acked. Runs the non-blocking epoll server (with a real DB-writer thread) in a
 * thread and drives it with blocking TLS clients. */

#include "netloop.h"
#include "audio.h"
#include "config.h"
#include "dbwriter.h"
#include "framebuf.h"
#include "protocol.h"
#include "tls.h"
#include "tts_render.h"
#include "stt_render.h"
#include "check.h"

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

/* An audio sidecar driven on a thread so the call e2e has a live media relay.
 * The net loop restarts it when it exits (REQ-150), so starting one is a function
 * the net loop can call again: each start makes a new IPC socketpair and a new
 * thread on the same UDP socket, exactly as main.c forks a new process. */
static struct { int ipc_fd, udp_fd; volatile sig_atomic_t stop; } g_audio_arg;
static pthread_t g_audio_th;
static int       g_audio_running;
static int       g_audio_side = -1;      /* the sidecar's end of the current IPC socket */
static int       g_audio_daemon = -1;    /* the net loop's end */
static volatile int g_audio_starts;
static volatile int g_audio_refuse;      /* make the next restart fail */
static void *audio_thread(void *p) {
    (void)p;
    oc_audio_sidecar_run(g_audio_arg.ipc_fd, g_audio_arg.udp_fd, &g_audio_arg.stop);
    return NULL;
}
static int audio_start(void *ctx) {
    (void)ctx;
    if (g_audio_refuse) return -1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    g_audio_arg.ipc_fd = sv[1];
    g_audio_arg.stop = 0;
    if (pthread_create(&g_audio_th, NULL, audio_thread, NULL) != 0) { close(sv[0]); close(sv[1]); return -1; }
    g_audio_side = sv[1];
    g_audio_daemon = sv[0];
    g_audio_running = 1;
    g_audio_starts++;
    return sv[0];
}
/* The sidecar exits: its thread stops and its end of the IPC socket closes, which
 * is what the net loop sees when a sidecar process dies. */
static void audio_kill(void) {
    if (!g_audio_running) return;
    g_audio_arg.stop = 1;
    pthread_join(g_audio_th, NULL);
    g_audio_running = 0;
    close(g_audio_side);
    g_audio_side = -1;
}

/* token(16) + seq(u16 BE) + payload -> the relay. */
static void udp_send_audio(int fd, const struct sockaddr_in *to, const uint8_t *tok,
                           uint16_t seq, const char *payload) {
    uint8_t pkt[128]; size_t pl = payload ? strlen(payload) : 0;
    memcpy(pkt, tok, OC_AUDIO_TOKEN_LEN);
    pkt[16] = (uint8_t)(seq >> 8); pkt[17] = (uint8_t)seq;
    if (pl) memcpy(pkt + OC_AUDIO_C2S_HDR, payload, pl);
    sendto(fd, pkt, OC_AUDIO_C2S_HDR + pl, 0, (const struct sockaddr *)to, sizeof *to);
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
static int mk_udp_client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 1, 0 };   /* 1 s — generous ceiling for the audio relay
                                     * round-trip under CI load (loopback UDP
                                     * doesn't drop). */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
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
    /* Concurrent TLS setup across threads (test_concurrent_load opens 8 clients
     * at once) is safe: the vendored mbedTLS is built with MBEDTLS_THREADING. */
    if (oc_tls_client_init(&c->cli, pin) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
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
    if (oc_tls_client_init_ex(&c->cli, pin, http_alpn) != 0) return -1;
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
        /* Also skip the post-AUTH_OK WORKSPACE_INFO and TTS_INFO pushes (like
         * presence/typing, they arrive unsolicited and would desync a fixed
         * expected-frame stream). */
        if (hdr->msg_type != OC_MSG_PRESENCE_UPDATE &&
            hdr->msg_type != OC_MSG_TYPING_UPDATE &&
            hdr->msg_type != OC_MSG_WORKSPACE_INFO &&
            hdr->msg_type != OC_MSG_CAPABILITIES &&
            hdr->msg_type != OC_MSG_TTS_INFO &&
            hdr->msg_type != OC_MSG_STT_INFO)
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
    return (ch.methods & OC_AUTH_LOCAL) ? 0 : -1;
}

/* What the last do_auth was told of voice input: the capability, and the cap. */
static int      g_auth_stt;
static uint32_t g_auth_stt_max_ms;

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
    oc_search sq = { .query = oc_slice_str("orbital"), .limit = 10 };
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
    oc_search sq2 = { .query = oc_slice_str("nuclear"), .limit = 10 };
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
        memset(sm.idem, 0xD1, OC_IDEM_SIZE);
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
        oc_auth au = { OC_AUTH_LOCAL, { cbuf, cw.len } };
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

    arg2.stop = 1;
    pthread_join(th2, NULL);
    oc_dbwriter_stop(dbw2);
    oc_tls_server_free(&srv2);
    if (by_env) unsetenv("OPENCHIME_STT");
    else        oc_netloop_set_stt(&STUB_STT);
    unlink("build/itest_stt_off.db");
    unlink("build/itest_stt_off.db-wal");
    unlink("build/itest_stt_off.db-shm");
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

/* One small upload in a single chunk; returns the attachment id, or 0. */
static uint64_t upload_small(client *c, uint8_t *fbuf, uint64_t ch, uint8_t idem_byte,
                             const char *name, const char *mime, const uint8_t *data, size_t len) {
    oc_header hdr; oc_rbuf p; oc_wbuf w;
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_begin ub = { ch, {0}, oc_slice_str(name), oc_slice_str(mime), len };
    memset(ub.idem, idem_byte, OC_IDEM_SIZE);
    if (oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub) != OC_OK || send_frame(c, fbuf, w.len) != 0) return 0;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_UPLOAD_READY) return 0;
    oc_upload_ready urd;
    if (oc_decode_upload_ready(&p, &urd) != OC_OK) return 0;
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_chunk uc = { urd.attachment_id, 0, { data, len } };
    if (oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) != OC_OK || send_frame(c, fbuf, w.len) != 0) return 0;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_UPLOAD_ACK) return 0;
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_upload_end ue = { urd.attachment_id };
    if (oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue) != OC_OK || send_frame(c, fbuf, w.len) != 0) return 0;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_UPLOAD_OK) return 0;
    return urd.attachment_id;
}

/* A video message over the wire (REQ-162/165, ARCH-110): poster and video
 * upload as ordinary attachments, ATTACH_MEDIA_SET is refused for a bad poster
 * and accepted for a good one, and the member who receives the SEND sees the
 * media fields inline and can fetch the poster. */
static void test_video_message_vertical(int port, const uint8_t *pin) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0 && do_handshake(&a) == 0);
    uint64_t ua = 0; CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(client_open(&b, port, pin) == 0 && do_handshake(&b) == 0);
    uint64_t ub = 0; CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);
    oc_header hdr; oc_rbuf p; oc_wbuf w;
    uint8_t *fbuf = malloc(OC_MAX_FRAME_SIZE);
    CHECK(fbuf != NULL);
    if (!fbuf) return;

    uint8_t jpeg[600], mp4[4000];
    for (size_t i = 0; i < sizeof jpeg; i++) jpeg[i] = (uint8_t)(i * 7u);
    for (size_t i = 0; i < sizeof mp4; i++) mp4[i] = (uint8_t)(i * 13u);
    uint64_t poster = upload_small(&a, fbuf, OC_DEFAULT_CHANNEL, 0xD1, "poster.jpg", "image/jpeg", jpeg, sizeof jpeg);
    uint64_t video  = upload_small(&a, fbuf, OC_DEFAULT_CHANNEL, 0xD2, "Video message.mp4", "video/mp4", mp4, sizeof mp4);
    CHECK(poster && video);

    /* The video named as its own poster: refused with an ordinary ERROR. */
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_attach_media_set ms = { video, OC_MEDIA_VIDEO_MESSAGE, 12345, 1280, 720, video };
    CHECK(oc_encode_attach_media_set(&w, OC_PROTOCOL_VERSION, &ms) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_MEDIA_INVALID);

    ms.poster_id = poster;
    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    CHECK(oc_encode_attach_media_set(&w, OC_PROTOCOL_VERSION, &ms) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ATTACH_MEDIA_OK);
    oc_attach_media_ok mo; CHECK(oc_decode_attach_media_ok(&p, &mo) == OC_OK && mo.attachment_id == video);

    oc_wbuf_init(&w, fbuf, OC_MAX_FRAME_SIZE);
    oc_send sm = {0};
    sm.channel_id = OC_DEFAULT_CHANNEL; memset(sm.idem, 0xD3, OC_IDEM_SIZE);
    sm.body = oc_slice_str("a quick update");
    sm.n_attach = 1; sm.attach_ids[0] = video;
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK);
    CHECK(send_frame(&a, fbuf, w.len) == 0);
    int got = 0;
    for (int i = 0; i < 8 && !got; i++) {
        if (read_frame(&b, &hdr, &p) != 0) break;
        if (hdr.msg_type != OC_MSG_BROADCAST) continue;
        oc_broadcast bc;
        CHECK(oc_decode_broadcast(&p, &bc) == OC_OK);
        got = 1;
        CHECK(bc.n_attach == 1 && bc.attach[0].id == video);
        CHECK(bc.attach[0].media_kind == OC_MEDIA_VIDEO_MESSAGE && bc.attach[0].duration_ms == 12345);
        CHECK(bc.attach[0].width == 1280 && bc.attach[0].height == 720 && bc.attach[0].poster_id == poster);
    }
    CHECK(got);
    uint8_t back[sizeof jpeg];
    CHECK(download_attachment(&b, poster, back, sizeof back, NULL) == sizeof jpeg);
    CHECK(memcmp(back, jpeg, sizeof jpeg) == 0);

    free(fbuf);
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

/* Full audio path (REQ-150/151): two participants join a call, each gets a UDP
 * endpoint + bearer token in CALL_JOINED, and one participant's audio is relayed
 * to the other by the sidecar, tagged with the sender's user id. */
/* The relay exits mid-call (REQ-150). The net loop must notice, start another on
 * the same port, and hand it every live participant's token -- otherwise the new
 * relay drops everything as unknown and every call goes silent. Then, when a
 * restart is impossible, a join is refused openly instead of being handed a port
 * nothing listens on. */
static void test_call_sidecar_restart(int port, const uint8_t *pin, uint16_t audio_port) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w; uint64_t parts[32];
    uint8_t atok[OC_AUDIO_TOKEN_LEN], btok[OC_AUDIO_TOKEN_LEN];
    oc_call_join cj = { OC_DEFAULT_CHANNEL };
    oc_call_joined jd;

    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK && send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == OC_AUDIO_TOKEN_LEN);
    memcpy(atok, jd.token.ptr, OC_AUDIO_TOKEN_LEN);
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK && send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == OC_AUDIO_TOKEN_LEN);
    memcpy(btok, jd.token.ptr, OC_AUDIO_TOKEN_LEN);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_ROSTER);

    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(audio_port);
    int sa = mk_udp_client(), sb = mk_udp_client();
    char tmp[64]; uint64_t sender; uint16_t seq;

    /* The relay dies with both of them in the call... */
    int before = g_audio_starts;
    audio_kill();
    for (int i = 0; i < 50 && g_audio_starts == before; i++) usleep(20000);
    CHECK(g_audio_starts == before + 1);                       /* ...and is started again */

    /* The new relay knows nobody's address, so both announce themselves -- using
     * the tokens they were given BEFORE the restart. */
    udp_send_audio(sb, &relay, btok, 0, NULL);
    udp_send_audio(sa, &relay, atok, 0, NULL);
    usleep(120000);
    while (udp_recv_audio(sa, &sender, &seq, tmp, sizeof tmp) >= 0) {}
    while (udp_recv_audio(sb, &sender, &seq, tmp, sizeof tmp) >= 0) {}

    /* alice speaks, and bob hears her: the new relay was given their tokens. */
    udp_send_audio(sa, &relay, atok, 9, "back");
    usleep(80000);
    char body[64];
    int n = udp_recv_audio(sb, &sender, &seq, body, sizeof body);
    CHECK(n == 4 && sender == ua && seq == 9 && memcmp(body, "back", 4) == 0);

    /* Now it dies and cannot be brought back: a new join is refused, openly. */
    g_audio_refuse = 1;
    audio_kill();
    usleep(300000);
    client c;
    CHECK(client_open(&c, port, pin) == 0); CHECK(do_handshake(&c) == 0);
    uint64_t uc = 0;
    CHECK(do_auth(&c, "carol", "pw", &uc) == 0);
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK && send_frame(&c, buf, w.len) == 0);
    CHECK(read_frame(&c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_ERROR);
    oc_error er; CHECK(oc_decode_error(&p, &er) == OC_OK && er.code == OC_ERR_CALL_UNAVAILABLE);

    (void)ub;
    close(sa); close(sb);
    client_close(&c);
    client_close(&a);
    client_close(&b);
}

static void test_call_udp_vertical(int port, const uint8_t *pin, uint16_t audio_port) {
    client a, b;
    CHECK(client_open(&a, port, pin) == 0); CHECK(do_handshake(&a) == 0);
    CHECK(client_open(&b, port, pin) == 0); CHECK(do_handshake(&b) == 0);
    uint64_t ua = 0, ub = 0;
    CHECK(do_auth(&a, "alice", "pw-alice", &ua) == 0);
    CHECK(do_auth(&b, "bob", "pw-bob", &ub) == 0);

    oc_header hdr; oc_rbuf p; uint8_t buf[128]; oc_wbuf w; uint64_t parts[32];
    uint8_t atok[OC_AUDIO_TOKEN_LEN], btok[OC_AUDIO_TOKEN_LEN];

    /* alice joins -> CALL_JOINED with the real UDP port + a 16-byte token. */
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cj = { OC_DEFAULT_CHANNEL };
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&a, buf, w.len) == 0);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    oc_call_joined jd; CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK);
    CHECK(jd.udp_port == audio_port && jd.token.len == OC_AUDIO_TOKEN_LEN);
    memcpy(atok, jd.token.ptr, OC_AUDIO_TOKEN_LEN);

    /* bob joins -> his own token; alice gets a roster update. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) == OC_OK);
    CHECK(send_frame(&b, buf, w.len) == 0);
    CHECK(read_frame(&b, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_JOINED);
    CHECK(oc_decode_call_joined(&p, &jd, parts, 32) == OC_OK && jd.token.len == OC_AUDIO_TOKEN_LEN);
    memcpy(btok, jd.token.ptr, OC_AUDIO_TOKEN_LEN);
    CHECK(read_frame(&a, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CALL_ROSTER);

    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(audio_port);
    int sa = mk_udp_client(), sb = mk_udp_client();

    /* Each side sends a hello so the sidecar learns its UDP address. */
    udp_send_audio(sb, &relay, btok, 0, NULL);
    udp_send_audio(sa, &relay, atok, 0, NULL);
    usleep(120000);
    { char tmp[64]; uint64_t s; uint16_t sq;   /* discard any forwarded helloes */
      while (udp_recv_audio(sa, &s, &sq, tmp, sizeof tmp) >= 0) {}
      while (udp_recv_audio(sb, &s, &sq, tmp, sizeof tmp) >= 0) {} }

    /* alice speaks -> bob receives it, tagged with alice's user id. */
    udp_send_audio(sa, &relay, atok, 7, "hey");
    usleep(80000);
    uint64_t sender; uint16_t seq; char body[64];
    int n = udp_recv_audio(sb, &sender, &seq, body, sizeof body);
    CHECK(n == 3 && sender == ua && seq == 7 && memcmp(body, "hey", 3) == 0);

    close(sa); close(sb);
    client_close(&a);
    client_close(&b);
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
    /* The token reaches a client as HEX: it gets shared in a message, so it has
     * to be text. (It is still random bytes on the daemon's side.) */
    CHECK(ic.token.len == 2 * OC_INVITE_TOKEN_LEN && ic.role == OC_ROLE_MEMBER);
    char token[2 * OC_INVITE_TOKEN_LEN + 1];
    memcpy(token, ic.token.ptr, ic.token.len);
    token[ic.token.len] = '\0';
    for (size_t ti = 0; ti < ic.token.len; ti++)
        CHECK((token[ti] >= '0' && token[ti] <= '9') || (token[ti] >= 'a' && token[ti] <= 'f'));

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

/* Per-connection output-buffer cap: a client that stops reading while the daemon
 * fans a large backfill at it has its connection dropped rather than being
 * allowed to grow the daemon's memory without bound. Seeds through the writer to
 * bypass the wire send limit, and shrinks its own receive window so the cap is
 * reached with a modest backlog. */
static void test_out_buffer_cap(int port, const uint8_t *pin, oc_dbwriter *dbw, uint64_t flooder) {
    CHECK(flooder != 0);
    static uint8_t big[60000];
    memset(big, 'x', sizeof big);
    /* Seed ~28 MB — far more than any kernel send/recv buffer can absorb (the
     * daemon's SO_SNDBUF autotunes to a few MB). This makes the daemon's 1 MiB
     * userspace out-buffer the binding constraint, so the overflow-drop is
     * deterministic rather than depending on kernel buffer sizes. Bounded by
     * OC_BACKFILL_MAX (500) on replay. idem must be unique for >255 messages. */
    for (int i = 0; i < 480; i++) {
        oc_job *j = oc_job_new(OC_JOB_SEND, 0);
        if (!j) { CHECK(0); return; }
        j->user_id = flooder; j->channel_id = 1;
        memset(j->idem, 0, OC_IDEM_LEN);
        j->idem[0] = (uint8_t)i; j->idem[1] = 0xC7; j->idem[2] = (uint8_t)(i >> 8);
        oc_job_set_body(j, big, sizeof big);
        oc_dbwriter_submit(dbw, j);
    }
    usleep(1500000);   /* let the writer persist them (~28 MB) */

    client v;
    CHECK(client_open(&v, port, pin) == 0);
    CHECK(do_handshake(&v) == 0);
    uint64_t uv = 0;
    CHECK(do_auth(&v, "flooder", "pw", &uv) == 0);
    int rb = 8192;    /* tiny receive window: TCP backpressure hits fast */
    setsockopt(v.fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);

    /* An EXPLICIT, non-zero cursor: "everything after message 1", which is all
     * 480 of them. A cursor of 0 would mean "I hold no history, send me the
     * tail" and yield only OC_BACKFILL_TAIL messages — far too few to reach the
     * output cap this test is about. */
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { 1, 1 };
    oc_backfill_request req = { 1, &cur };
    CHECK(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK);
    CHECK(write_all(&v.conn, buf, w.len) == 0);

    usleep(1000000);   /* stay silent: the daemon fills our buffer past the cap */

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

    /* --- a defined type still works --------------------------------------- */
    {
        client c;
        CHECK(client_open(&c, port, pin) == 0);
        CHECK(do_handshake(&c) == 0);
        uint64_t uid = 0;
        CHECK(do_auth(&c, "alice", "pw-alice", &uid) == 0);
        uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
        CHECK(write_all(&c.conn, buf, w.len) == 0);
        oc_header hdr; oc_rbuf p;
        CHECK(read_frame(&c, &hdr, &p) == 0);
        CHECK(hdr.msg_type == OC_MSG_CHANNEL_LIST);
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
    printf("itest_netloop: handshake, version REJECT, two-client AUTH+SEND+BROADCAST, backfill, edit/delete, channels, reactions, threads, search, dm, drafts across two devices, load, rate-limit, out-cap, throttle, admin, logout\n");

    /* Attachment blobs go to a build-local dir (REQ-140); the daemon defaults to
     * /data/blobs, which isn't writable in the test sandbox. */
    setenv("OPENCHIME_BLOB_DIR", "build/itest_blobs", 1);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    /* Bring up an audio relay sidecar (on a thread) + wire the netloop to it, so
     * CALL_JOINED carries a real UDP port + token and the call e2e can relay. */
    int audio_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua; memset(&ua, 0, sizeof ua);
    ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ua.sin_port = 0;
    CHECK(bind(audio_udp, (struct sockaddr *)&ua, sizeof ua) == 0);
    socklen_t ual = sizeof ua; getsockname(audio_udp, (struct sockaddr *)&ua, &ual);
    uint16_t audio_port = ntohs(ua.sin_port);
    g_audio_arg.udp_fd = audio_udp;
    int adaemon = audio_start(NULL);
    CHECK(adaemon >= 0);
    oc_netloop_set_audio(adaemon, audio_port);
    oc_netloop_set_audio_respawn(audio_start, NULL);
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
        test_frame_version_mismatch(arg.port, pin);
        test_unexpected_msg_type(arg.port, pin);
        test_message_vertical(arg.port, pin);
        test_backfill_reconnect(arg.port, pin);
        test_edit_delete_vertical(arg.port, pin);
        test_channels_vertical(arg.port, pin);
        test_reactions_vertical(arg.port, pin);
        test_threads_vertical(arg.port, pin);
        test_search_vertical(arg.port, pin);
        test_dm_vertical(arg.port, pin);
        test_drafts_vertical(arg.port, pin);
        test_presence_typing(arg.port, pin);
        test_presence_dnd(arg.port, pin);
        test_read_aloud_vertical(arg.port, pin);
        test_voice_input_vertical(arg.port, pin);
        test_voice_input_rate(arg.port, pin);
        test_attachments_vertical(arg.port, pin);
        test_video_message_vertical(arg.port, pin);
        test_upload_abandoned(arg.port, pin);
        test_webhook_vertical(arg.port, pin);
        test_notify_prefs_vertical(arg.port, pin);
        test_call_vertical(arg.port, pin);
        test_call_udp_vertical(arg.port, pin, audio_port);
        test_call_sidecar_restart(arg.port, pin, audio_port);   /* last call test: leaves calls refused */
        test_concurrent_load(arg.port, pin);
        test_send_rate_limit(arg.port, pin);
        test_out_buffer_cap(arg.port, pin, dbw, flooder);
        test_admin_vertical(arg.port, pin);
        test_logout_closes(arg.port, pin);
        test_conn_throttle(arg.port + 123);
    }

    arg.stop = 1;
    pthread_join(th, NULL);

    if (failures == 0) {
        test_voice_input_absent(arg.port + 124, 1);
        test_voice_input_absent(arg.port + 125, 0);
    }

    /* Stop the audio sidecar: the netloop is done, so unwire it and close the
     * daemon IPC end (the sidecar exits on EOF), then join + close fds. */
    oc_netloop_set_audio(-1, 0);
    oc_netloop_set_audio_respawn(NULL, NULL);
    audio_kill();
    if (g_audio_daemon >= 0) close(g_audio_daemon);
    close(audio_udp);

    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    unlink("build/itest_netloop.db");
    unlink("build/itest_netloop.db-wal");
    unlink("build/itest_netloop.db-shm");

    return failures;
}
