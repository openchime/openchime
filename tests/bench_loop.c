/* Event-loop load harness: how long the net loop's turns take, and what clients
 * see, under the loads a 1-100 person workspace puts on it.
 *
 * Runs the daemon in this process -- the writer thread, and the net loop, with
 * the call relay in it, on a thread -- and drives it with real TLS clients on loopback. Each
 * scenario resets the loop's turn statistics (netloop.h), runs, and prints the
 * turn p50/p99/max beside the latency the clients measured. It is a measuring
 * instrument, not a test: nothing here passes or fails, and it is not part of
 * `make test`. `make bench-loop` builds it; the numbers it prints before and
 * after a change to the loop are recorded in docs/TESTING.md §5.
 *
 * Usage: bench_loop [scenario ...]   scenarios: chat fanout storm backfill call call-tcp
 *                                     (none named: all of them, in that order)
 *
 * Sign-in uses a cheap password hash (the accounts are made with 2048 PBKDF2
 * iterations, as the tests make theirs), because the hash runs on the writer
 * thread, not the loop; at the production 600k a storm would measure the writer. */

#include "netloop.h"
#include "audio.h"
#include "config.h"
#include "dbwriter.h"
#include "framebuf.h"
#include "protocol.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define N_USERS   150
#define N_CALL    10

static int      g_port;
static uint16_t g_audio_port;

static uint64_t mono_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* --- samples ---------------------------------------------------------------- */

typedef struct { pthread_mutex_t mu; uint64_t *v; size_t n, cap; } samples;

static void samples_init(samples *s) { memset(s, 0, sizeof *s); pthread_mutex_init(&s->mu, NULL); }
static void samples_add(samples *s, uint64_t us) {
    pthread_mutex_lock(&s->mu);
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        uint64_t *g = realloc(s->v, nc * sizeof *g);
        if (g) { s->v = g; s->cap = nc; }
    }
    if (s->n < s->cap) s->v[s->n++] = us;
    pthread_mutex_unlock(&s->mu);
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static uint64_t pct(samples *s, double p) {
    if (!s->n) return 0;
    size_t i = (size_t)((double)(s->n - 1) * p / 100.0 + 0.5);
    return s->v[i];
}
static void samples_print(const char *what, samples *s) {
    qsort(s->v, s->n, sizeof *s->v, cmp_u64);
    printf("  %-22s n=%-7zu p50=%6.2fms p99=%7.2fms max=%7.2fms\n", what, s->n,
           pct(s, 50) / 1000.0, pct(s, 99) / 1000.0, s->n ? s->v[s->n - 1] / 1000.0 : 0.0);
}
static void samples_free(samples *s) { free(s->v); pthread_mutex_destroy(&s->mu); }

static void loop_print(void) {
    oc_netloop_stats st;
    oc_netloop_stats_get(&st);
    printf("  %-22s n=%-7llu p50=%6.2fms p99=%7.2fms max=%7.2fms\n", "loop turn",
           (unsigned long long)st.turns,
           oc_netloop_stats_pct_us(&st, 50) / 1000.0,
           oc_netloop_stats_pct_us(&st, 99) / 1000.0,
           st.turn_max_us / 1000.0);
}

/* --- a client --------------------------------------------------------------- */

typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; uint64_t uid; } bclient;

static int write_all(bclient *c, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        oc_tls_status st = oc_tls_write(&c->conn, buf + sent, len - sent, &n);
        if (st == OC_TLS_WANT_WRITE || st == OC_TLS_WANT_READ) continue;
        if (st != OC_TLS_OK) return -1;
        sent += n;
    }
    return 0;
}

/* 1 frame, 0 nothing within the socket's read timeout, -1 closed or broken. */
static int read_frame(bclient *c, oc_header *hdr, oc_rbuf *p) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r == 1) return oc_parse_frame(frame, flen, hdr, p) == OC_OK ? 1 : -1;
        if (r < 0) return -1;
        uint8_t buf[16384]; size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, buf, sizeof buf, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) return 0;
        if (st != OC_TLS_OK || n == 0) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

/* Read until a frame of `type` arrives, within `ms`. */
static int read_until(bclient *c, uint16_t type, oc_rbuf *p, int ms) {
    uint64_t end = mono_us() + (uint64_t)ms * 1000u;
    oc_header hdr;
    while (mono_us() < end) {
        int r = read_frame(c, &hdr, p);
        if (r < 0) return -1;
        if (r == 1 && hdr.msg_type == type) return 0;
    }
    return -1;
}

static int bc_open(bclient *c, int rcv_timeout_ms) {
    memset(c, 0, sizeof *c);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)g_port);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    int one = 1; setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { rcv_timeout_ms / 1000, (rcv_timeout_ms % 1000) * 1000 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (oc_tls_client_init(&c->cli, NULL) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    uint64_t end = mono_us() + 30000000u;
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) break;
        if ((st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) || mono_us() > end) return -1;
    }
    return oc_framebuf_init(&c->fb);
}

static void bc_close(bclient *c) {
    oc_framebuf_free(&c->fb);
    oc_tls_conn_free(&c->conn);
    oc_tls_client_free(&c->cli);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* HELLO, then AUTH as u<i>; returns 0 once AUTH_OK has arrived. */
static int bc_login(bclient *c, int i) {
    uint8_t buf[512]; oc_wbuf w; oc_rbuf p;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_hello h = { OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION, oc_slice_str("bench") };
    if (oc_encode_hello(&w, &h) != OC_OK || write_all(c, buf, w.len) != 0) return -1;
    if (read_until(c, OC_MSG_AUTH_CHALLENGE, &p, 30000) != 0) return -1;
    char user[16]; snprintf(user, sizeof user, "u%d", i);
    uint8_t cred[128]; oc_wbuf cw; oc_wbuf_init(&cw, cred, sizeof cred);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str("pw")) != OC_OK) return -1;
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, oc_slice_str("local"), { cred, cw.len }, { NULL, 0 }, { NULL, 0 } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != OC_OK || write_all(c, buf, w.len) != 0) return -1;
    if (read_until(c, OC_MSG_AUTH_OK, &p, 60000) != 0) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    c->uid = ok.user_id;
    return 0;
}

/* SEND to #general, body "T<send time in µs>", idempotency token from the time. */
static int bc_send_stamp(bclient *c, uint64_t t) {
    char body[48]; snprintf(body, sizeof body, "T%llu", (unsigned long long)t);
    uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s; memset(&s, 0, sizeof s);
    s.channel_id = OC_DEFAULT_CHANNEL;
    memcpy(s.idem, &t, sizeof t); memcpy(s.idem + 8, &c->uid, sizeof c->uid);
    s.body = oc_slice_str(body);
    if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) != OC_OK) return -1;
    return write_all(c, buf, w.len);
}

/* --- chat and fanout: N connected, S of them posting to #general -------------- */

typedef struct {
    int idx, senders, secs; double rate;   /* sends per second per sender */
    samples *ack, *deliver;
    int ok;
} chat_arg;

static volatile int g_gate;   /* clients wait here until all are signed in */
static volatile int g_ready_n;

static void *chat_client(void *vp) {
    chat_arg *a = vp;
    bclient c;
    if (bc_open(&c, 50) != 0 || bc_login(&c, a->idx) != 0) { bc_close(&c); return NULL; }
    __atomic_add_fetch(&g_ready_n, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) usleep(1000);

    int sender = a->idx < a->senders;
    uint64_t period = (uint64_t)(1000000.0 / a->rate);
    uint64_t start = mono_us(), end = start + (uint64_t)a->secs * 1000000u;
    uint64_t next = start + (uint64_t)(a->idx * 7919 % 1000) * period / 1000;
    uint64_t pending[64]; int np = 0;
    while (mono_us() < end + 2000000u) {
        uint64_t now = mono_us();
        if (sender && now >= next && now < end) {
            if (bc_send_stamp(&c, now) != 0) break;
            if (np < 64) pending[np++] = now;
            next += period;
        }
        oc_header hdr; oc_rbuf p;
        int r = read_frame(&c, &hdr, &p);
        if (r < 0) break;
        if (r == 0) { if (now >= end && np == 0) break; continue; }
        uint64_t at = mono_us();
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack;
            if (oc_decode_send_ack(&p, &ack) == OC_OK) {
                uint64_t t; memcpy(&t, ack.idem, sizeof t);
                for (int k = 0; k < np; k++)
                    if (pending[k] == t) { samples_add(a->ack, at - t); pending[k] = pending[--np]; break; }
            }
        } else if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast bc;
            if (oc_decode_broadcast(&p, &bc) == OC_OK && bc.body.len > 1 && bc.body.ptr[0] == 'T') {
                char tmp[32]; size_t l = bc.body.len < 31 ? bc.body.len : 31;
                memcpy(tmp, bc.body.ptr, l); tmp[l] = 0;
                uint64_t t = strtoull(tmp + 1, NULL, 10);
                if (t && bc.author_id != c.uid) samples_add(a->deliver, at - t);
            }
        }
    }
    a->ok = 1;
    bc_close(&c);
    return NULL;
}

static void run_chat(const char *name, int clients, int senders, double rate, int secs) {
    printf("%s: %d connected, %d posting %.1f/s to #general for %ds\n", name, clients, senders, rate, secs);
    samples ack, del; samples_init(&ack); samples_init(&del);
    chat_arg *args = calloc((size_t)clients, sizeof *args);
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    g_gate = 0; g_ready_n = 0;
    for (int i = 0; i < clients; i++) {
        args[i] = (chat_arg){ i, senders, secs, rate, &ack, &del, 0 };
        pthread_create(&th[i], NULL, chat_client, &args[i]);
    }
    for (int i = 0; i < 3000 && __atomic_load_n(&g_ready_n, __ATOMIC_ACQUIRE) < clients; i++) usleep(10000);
    usleep(300000);   /* let the sign-in presence fan-out settle before measuring */
    oc_netloop_stats_reset();
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    int ok = 0;
    for (int i = 0; i < clients; i++) { pthread_join(th[i], NULL); ok += args[i].ok; }
    printf("  clients completed %d/%d\n", ok, clients);
    loop_print();
    samples_print("SEND->SEND_ACK", &ack);
    samples_print("SEND->BROADCAST", &del);
    samples_free(&ack); samples_free(&del); free(args); free(th);
}

/* --- storm: everyone signs in at once --------------------------------------- */

typedef struct { int idx; samples *s; int ok; } storm_arg;

static void *storm_client(void *vp) {
    storm_arg *a = vp;
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) usleep(200);
    uint64_t t0 = mono_us();
    bclient c;
    if (bc_open(&c, 1000) == 0 && bc_login(&c, a->idx) == 0) {
        samples_add(a->s, mono_us() - t0);
        a->ok = 1;
    }
    usleep(500000);
    bc_close(&c);
    return NULL;
}

static void run_storm(int clients) {
    printf("storm: %d clients connect, handshake and sign in at once\n", clients);
    samples s; samples_init(&s);
    storm_arg *args = calloc((size_t)clients, sizeof *args);
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    g_gate = 0;
    for (int i = 0; i < clients; i++) { args[i] = (storm_arg){ i, &s, 0 }; pthread_create(&th[i], NULL, storm_client, &args[i]); }
    usleep(200000);
    oc_netloop_stats_reset();
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    int ok = 0;
    for (int i = 0; i < clients; i++) { pthread_join(th[i], NULL); ok += args[i].ok; }
    printf("  signed in %d/%d\n", ok, clients);
    loop_print();
    samples_print("connect->AUTH_OK", &s);
    samples_free(&s); free(args); free(th);
}

/* --- backfill: many clients replaying a long history at once ------------------ */

typedef struct { int idx; samples *s; int ok, frames, closed; } bf_arg;

static void *bf_client(void *vp) {
    bf_arg *a = vp;
    bclient c;
    if (bc_open(&c, 100) != 0 || bc_login(&c, a->idx) != 0) { bc_close(&c); return NULL; }
    __atomic_add_fetch(&g_ready_n, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) usleep(1000);
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_cursor cur = { OC_DEFAULT_CHANNEL, 1 };
    oc_backfill_request req = { 1, &cur };
    uint64_t t0 = mono_us();
    if (oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) != OC_OK || write_all(&c, buf, w.len) != 0) {
        bc_close(&c); return NULL;
    }
    uint64_t end = t0 + 60000000u;
    while (mono_us() < end) {
        oc_header hdr; oc_rbuf p;
        int r = read_frame(&c, &hdr, &p);
        if (r < 0) { a->closed = 1; break; }
        if (r == 0) continue;
        if (hdr.msg_type == OC_MSG_BROADCAST) a->frames++;
        if (hdr.msg_type == OC_MSG_BACKFILL_DONE) { samples_add(a->s, mono_us() - t0); a->ok = 1; break; }
    }
    bc_close(&c);
    return NULL;
}

static void run_backfill(oc_dbwriter *dbw, int clients, int msgs, size_t body) {
    printf("backfill: %d messages of %zu bytes; %d clients replay all of them at once\n", msgs, body, clients);
    static int seeded;
    if (!seeded) {
        char *big = malloc(body);
        memset(big, 'b', body);
        for (int i = 0; i < msgs; i++) {
            oc_job *j = oc_job_new(OC_JOB_SEND, 0);
            if (!j) break;
            j->user_id = 1; j->channel_id = OC_DEFAULT_CHANNEL;
            memset(j->idem, 0, OC_IDEM_LEN);
            j->idem[0] = (uint8_t)i; j->idem[1] = 0xBF; j->idem[2] = (uint8_t)(i >> 8);
            oc_job_set_body(j, big, body);
            oc_dbwriter_submit(dbw, j);
        }
        free(big);
        seeded = 1;
        usleep(2000000);   /* the writer commits them; nothing is waiting on the results */
    }
    samples s; samples_init(&s);
    bf_arg *args = calloc((size_t)clients, sizeof *args);
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    g_gate = 0; g_ready_n = 0;
    for (int i = 0; i < clients; i++) { args[i] = (bf_arg){ 20 + i, &s, 0, 0, 0 }; pthread_create(&th[i], NULL, bf_client, &args[i]); }
    for (int i = 0; i < 3000 && __atomic_load_n(&g_ready_n, __ATOMIC_ACQUIRE) < clients; i++) usleep(10000);
    usleep(200000);
    oc_netloop_stats_reset();
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    int ok = 0, closed = 0, frames = 0;
    for (int i = 0; i < clients; i++) { pthread_join(th[i], NULL); ok += args[i].ok; closed += args[i].closed; frames += args[i].frames; }
    printf("  completed %d/%d, dropped by the daemon %d, messages received %d\n", ok, clients, closed, frames);
    loop_print();
    samples_print("request->BACKFILL_DONE", &s);
    samples_free(&s); free(args); free(th);
}

/* --- call: ten people on the relay, one sharing, beside ordinary chat --------- */

typedef struct {
    int idx, secs, sharer, tcp;
    uint8_t token[OC_AUDIO_TOKEN_MAX]; size_t tlen;
    int udp;
    samples *audio, *video;
    bclient c;
    int ok;
} call_arg;

static int call_join(call_arg *a) {
    if (bc_open(&a->c, 100) != 0 || bc_login(&a->c, 100 + a->idx) != 0) return -1;
    uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_call_join cj; memset(&cj, 0, sizeof cj);
    cj.channel_id = OC_DEFAULT_CHANNEL; cj.codecs = OC_CALL_CODEC_VP9;
    if (oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) != OC_OK || write_all(&a->c, buf, w.len) != 0) return -1;
    oc_rbuf p;
    if (read_until(&a->c, OC_MSG_CALL_JOINED, &p, 10000) != 0) return -1;
    oc_call_part parts[OC_MAX_CALL_PARTICIPANTS]; oc_call_joined jd;
    if (oc_decode_call_joined(&p, &jd, parts, OC_MAX_CALL_PARTICIPANTS) != OC_OK) return -1;
    if (jd.token.len > sizeof a->token) return -1;
    memcpy(a->token, jd.token.ptr, jd.token.len);
    a->tlen = jd.token.len;
    a->udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 0, 100000 };
    setsockopt(a->udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return 0;
}

static void *call_rx(void *vp) {
    call_arg *a = vp;
    uint64_t end = mono_us() + (uint64_t)a->secs * 1000000u + 1000000u;
    uint8_t pkt[1500];
    while (mono_us() < end) {
        ssize_t n = recv(a->udp, pkt, sizeof pkt, 0);
        if (n < (ssize_t)(OC_AUDIO_S2C_HDR + 9)) continue;
        uint64_t t; memcpy(&t, pkt + OC_AUDIO_S2C_HDR + 1, sizeof t);
        samples_add(pkt[OC_AUDIO_S2C_HDR] == 'V' ? a->video : a->audio, mono_us() - t);
    }
    return NULL;
}

static void *call_tx(void *vp) {
    call_arg *a = vp;
    struct sockaddr_in relay; memset(&relay, 0, sizeof relay);
    relay.sin_family = AF_INET; relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(g_audio_port);
    uint8_t pkt[1300]; uint16_t seq = 0;
    memcpy(pkt, a->token, a->tlen);
    /* The relay learns the address from the first packet. */
    pkt[a->tlen] = 0; pkt[a->tlen + 1] = 0;
    sendto(a->udp, pkt, a->tlen + 2, 0, (struct sockaddr *)&relay, sizeof relay);
    usleep(200000);
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) usleep(1000);
    uint64_t start = mono_us(), end = start + (uint64_t)a->secs * 1000000u;
    uint64_t next_a = start, next_v = start;
    while (mono_us() < end) {
        uint64_t now = mono_us();
        if (now >= next_a) {   /* audio: 50 packets a second, ~100 bytes */
            size_t h = a->tlen; pkt[h] = (uint8_t)(seq >> 8); pkt[h + 1] = (uint8_t)seq; seq++;
            pkt[h + 2] = 'A'; memcpy(pkt + h + 3, &now, sizeof now);
            sendto(a->udp, pkt, h + 2 + 86, 0, (struct sockaddr *)&relay, sizeof relay);
            next_a += 20000;
        }
        if (a->sharer && now >= next_v) {   /* a share at its ceiling: ~300 packets a second */
            size_t h = a->tlen; pkt[h] = (uint8_t)(seq >> 8); pkt[h + 1] = (uint8_t)seq; seq++;
            pkt[h + 2] = 'V'; memcpy(pkt + h + 3, &now, sizeof now);
            sendto(a->udp, pkt, h + 2 + 1136, 0, (struct sockaddr *)&relay, sizeof relay);
            next_v += 3333;
        }
        uint64_t nx = a->sharer && next_v < next_a ? next_v : next_a;
        uint64_t t = mono_us();
        if (nx > t) usleep((useconds_t)(nx - t));
    }
    a->ok = 1;
    return NULL;
}

/* A participant whose media goes by its connection (CALL_MEDIA): one thread both
 * sends -- the mbedTLS context is not shared between threads -- and reads,
 * polling the socket briefly between packets. */
static void *call_tcp_main(void *vp) {
    call_arg *a = vp;
    struct timeval tv = { 0, 2000 };
    setsockopt(a->c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t ct[1300]; uint16_t seq = 0;
    uint8_t frame[1400];
    /* A keepalive puts this participant on the connection transport. */
    oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
    oc_call_media_pkt ka = { 0, seq++, { NULL, 0 } };
    if (oc_encode_call_media_up(&w, OC_PROTOCOL_VERSION, &ka) == OC_OK) write_all(&a->c, frame, w.len);
    while (!__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE)) usleep(1000);
    uint64_t start = mono_us(), end = start + (uint64_t)a->secs * 1000000u;
    uint64_t next_a = start;
    while (mono_us() < end + 1000000u) {
        uint64_t now = mono_us();
        if (now < end && now >= next_a) {
            ct[0] = 'A'; memcpy(ct + 1, &now, sizeof now);
            oc_wbuf_init(&w, frame, sizeof frame);
            oc_call_media_pkt m = { 0, seq++, { ct, 86 } };
            if (oc_encode_call_media_up(&w, OC_PROTOCOL_VERSION, &m) == OC_OK) write_all(&a->c, frame, w.len);
            next_a += 20000;
        }
        oc_header hdr; oc_rbuf p;
        int r = read_frame(&a->c, &hdr, &p);
        if (r < 0) break;
        if (r == 1 && hdr.msg_type == OC_MSG_CALL_MEDIA) {
            oc_call_media_pkt m;
            if (oc_decode_call_media_down(&p, &m) == OC_OK && m.ct.len >= 9) {
                uint64_t t; memcpy(&t, m.ct.ptr + 1, sizeof t);
                samples_add(m.ct.ptr[0] == 'V' ? a->video : a->audio, mono_us() - t);
            }
        }
    }
    a->ok = 1;
    return NULL;
}

static void run_call(int secs, int n_tcp) {
    printf("call%s: %d on the relay (50 audio packets/s each, one sharing at ~300/s), %d of them "
           "by the connection, beside 40 connected with 2 posting 2/s\n", n_tcp ? "-tcp" : "", N_CALL, n_tcp);
    samples au, vi; samples_init(&au); samples_init(&vi);
    call_arg *ca = calloc(N_CALL, sizeof *ca);
    int joined = 0;
    for (int i = 0; i < N_CALL; i++) {
        ca[i].idx = i; ca[i].secs = secs; ca[i].sharer = (i == 0);
        ca[i].tcp = i >= N_CALL - n_tcp;
        ca[i].audio = &au; ca[i].video = &vi; ca[i].udp = -1;
        if (call_join(&ca[i]) == 0) joined++;
    }
    printf("  joined %d/%d\n", joined, N_CALL);

    /* Chat beside the call, so the loop has ordinary work while the relay runs. */
    int clients = 40, senders = 2;
    samples ack, del; samples_init(&ack); samples_init(&del);
    chat_arg *args = calloc((size_t)clients, sizeof *args);
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    g_gate = 0; g_ready_n = 0;
    for (int i = 0; i < clients; i++) {
        args[i] = (chat_arg){ 50 + i, 0, secs, 2.0, &ack, &del, 0 };
        args[i].senders = 50 + senders;   /* idx 50 and 51 post */
        pthread_create(&th[i], NULL, chat_client, &args[i]);
    }
    pthread_t rx[N_CALL], tx[N_CALL];
    for (int i = 0; i < N_CALL; i++) if (ca[i].udp >= 0) {
        if (ca[i].tcp) { pthread_create(&tx[i], NULL, call_tcp_main, &ca[i]); continue; }
        pthread_create(&rx[i], NULL, call_rx, &ca[i]);
        pthread_create(&tx[i], NULL, call_tx, &ca[i]);
    }
    for (int i = 0; i < 3000 && __atomic_load_n(&g_ready_n, __ATOMIC_ACQUIRE) < clients; i++) usleep(10000);
    usleep(300000);
    oc_netloop_stats_reset();
    __atomic_store_n(&g_gate, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < N_CALL; i++) if (ca[i].udp >= 0) { pthread_join(tx[i], NULL); if (!ca[i].tcp) pthread_join(rx[i], NULL); }
    for (int i = 0; i < clients; i++) pthread_join(th[i], NULL);
    loop_print();
    samples_print("relay audio", &au);
    samples_print("relay video", &vi);
    samples_print("SEND->BROADCAST", &del);
    for (int i = 0; i < N_CALL; i++) { if (ca[i].udp >= 0) close(ca[i].udp); bc_close(&ca[i].c); }
    samples_free(&au); samples_free(&vi); samples_free(&ack); samples_free(&del);
    free(ca); free(args); free(th);
}

/* --- the daemon, in this process ------------------------------------------- */

struct loop_arg { oc_tls_server *srv; oc_dbwriter *dbw; volatile sig_atomic_t stop; };
static volatile int g_up;
static void on_ready(void *ctx) { (void)ctx; __atomic_store_n(&g_up, 1, __ATOMIC_RELEASE); }
static void *loop_thread(void *p) {
    struct loop_arg *a = p;
    oc_netloop_run(g_port, a->srv, a->dbw, &a->stop);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("OPENCHIME_BLOB_DIR", "build/bench_blobs", 1);
    setenv("OPENCHIME_CALL_MAX", "10", 1);
    setenv("OPENCHIME_MAX_CONNS_PER_IP", "0", 1);
    char cfgerr[128];
    if (oc_config_load(cfgerr, sizeof cfgerr) != 0) { fprintf(stderr, "config: %s\n", cfgerr); return 1; }

    oc_tls_server srv;
    if (oc_tls_server_init(&srv, NULL, NULL) != 0) { fprintf(stderr, "tls init failed\n"); return 1; }

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua; memset(&ua, 0, sizeof ua);
    ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t ul = sizeof ua;
    if (bind(udp, (struct sockaddr *)&ua, sizeof ua) != 0 || getsockname(udp, (struct sockaddr *)&ua, &ul) != 0) return 1;
    g_audio_port = ntohs(ua.sin_port);
    oc_netloop_set_audio(udp, g_audio_port);   /* the loop's relay runs on it */

    const char *db = "build/bench_loop.db";
    unlink(db); unlink("build/bench_loop.db-wal"); unlink("build/bench_loop.db-shm");
    oc_dbwriter *dbw = oc_dbwriter_start(db);
    if (!dbw) { fprintf(stderr, "db start failed\n"); return 1; }
    for (int i = 0; i < N_USERS; i++) {
        char user[16]; snprintf(user, sizeof user, "u%d", i);
        if (!oc_dbwriter_register_local(dbw, user, "pw", i == 0 ? OC_ROLE_OWNER : OC_ROLE_MEMBER, 2048)) {
            fprintf(stderr, "register %s failed\n", user); return 1;
        }
    }

    g_port = 17000 + (int)(getpid() % 900);
    struct loop_arg la = { &srv, dbw, 0 };
    oc_netloop_set_ready(on_ready, NULL);
    pthread_t lth; pthread_create(&lth, NULL, loop_thread, &la);
    for (int i = 0; i < 500 && !__atomic_load_n(&g_up, __ATOMIC_ACQUIRE); i++) usleep(10000);
    if (!g_up) { fprintf(stderr, "the loop did not come up\n"); return 1; }

    const char *all[] = { "chat", "fanout", "storm", "backfill", "call", "call-tcp" };
    const char **run = argc > 1 ? (const char **)argv + 1 : all;
    int nrun = argc > 1 ? argc - 1 : (int)(sizeof all / sizeof *all);
    for (int k = 0; k < nrun; k++) {
        if      (strcmp(run[k], "chat") == 0)     run_chat("chat", 100, 1, 5.0, 5);
        else if (strcmp(run[k], "fanout") == 0)   run_chat("fanout", 100, 10, 2.0, 10);
        else if (strcmp(run[k], "storm") == 0)    run_storm(100);
        else if (strcmp(run[k], "backfill") == 0) run_backfill(dbw, 20, 500, 4096);
        else if (strcmp(run[k], "call") == 0)     run_call(5, 0);
        else if (strcmp(run[k], "call-tcp") == 0) run_call(5, 5);
        else { fprintf(stderr, "unknown scenario %s\n", run[k]); return 2; }
    }

    __atomic_store_n(&la.stop, 1, __ATOMIC_RELEASE);
    pthread_join(lth, NULL);
    oc_netloop_set_audio(-1, 0);
    close(udp);
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    return 0;
}
