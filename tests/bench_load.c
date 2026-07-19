/* Capacity benchmark load client (REQ-210/211). Opens N concurrent pinned-TLS
 * connections to a running daemon — each authenticates, then does a series of
 * timed SEND -> SEND_ACK round-trips in its own private channel (isolated from
 * broadcast fan-out so the number is per-connection latency) — and holds the
 * connections open. Reports connection success, round-trip latency percentiles,
 * and aggregate throughput. The daemon's resident memory is sampled externally
 * (Scripts/bench.sh) while this runs, so the two together answer "how many
 * concurrent connections fit in the lean memory profile, at what latency".
 *
 * Not part of `make test` (it drives a live daemon). Links only the shared wire
 * modules, like tests/e2e_client.c.
 *
 * Usage: bench_load <host> <port> <n_conns> <hold_seconds> [<sends_per_conn>]
 */

#include "protocol.h"
#include "framebuf.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netdb.h>
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

typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; } client;

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

static int dial(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *rp; char ports[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf(ports, sizeof ports, "%d", port);
    if (getaddrinfo(host, ports, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    return fd;
}

static int client_open(client *c, const char *host, int port) {
    c->fd = dial(host, port);
    if (c->fd < 0) return -1;
    /* Bound every blocking read so a worker can never hang the run (auth does a
     * 600k-iteration PBKDF2 on the daemon's single writer, so AUTH_OK can lag
     * under a connection burst). recv() blocks up to this, so no busy-spin. */
    /* Generous, because AUTH is deliberately expensive: a 600k-iteration PBKDF2
     * on the single writer thread, serialized across all connections. At ~2
     * logins/s a burst of N clients takes N/2 seconds to drain, and a 10s
     * timeout silently capped usable concurrency at ~20 — every connection past
     * that gave up mid-auth and was counted as a failure, which then skewed the
     * per-connection memory figure it was feeding. */
    { struct timeval tv = { 180, 0 }; setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
    if (oc_tls_client_init(&c->cli, NULL) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
    return 0;
}

static void client_close(client *c) {
    oc_tls_conn_free(&c->conn);
    oc_tls_client_free(&c->cli);
    oc_framebuf_free(&c->fb);
    if (c->fd >= 0) close(c->fd);
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
            /* Skip tenant-wide async notifications (presence/typing) so they
             * don't get mistaken for the reply we're waiting on. */
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
    oc_hello h = { 1, 1, oc_slice_str("bench") };
    if (oc_encode_hello(&w, &h) != OC_OK || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) return -1;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_CHALLENGE) return -1;
    return 0;
}

static int do_auth(client *c, const char *user, const char *pass) {
    uint8_t cbuf[256]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass)) != OC_OK) return -1;
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != OC_OK || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    return 0;
}

/* --- Worker ------------------------------------------------------------- */

static volatile int g_connected = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static double g_deadline_ms;

typedef struct {
    int idx; const char *host; int port; int sends; int nusers;
    int connected;
    double *lat; int nlat;   /* round-trip latencies, ms */
} worker;

static void *run_worker(void *arg) {
    worker *W = arg;
    client c;
    if (client_open(&c, W->host, W->port) != 0) return NULL;
    if (do_handshake(&c) != 0) { client_close(&c); return NULL; }
    char user[32]; snprintf(user, sizeof user, "bench%d", W->idx % W->nusers);
    if (do_auth(&c, user, "benchpw") != 0) { client_close(&c); return NULL; }

    W->connected = 1;
    pthread_mutex_lock(&g_mu); g_connected++; pthread_mutex_unlock(&g_mu);

    uint64_t cid = 0;
    uint8_t b[256]; oc_wbuf w; oc_header hdr; oc_rbuf p;
    if (W->sends > 0) {
        /* A private channel only this connection's user belongs to -> a SEND
         * echoes only to itself (needs distinct users, one per conn), so the
         * round-trip is pure per-connection latency with no fan-out. */
        char cname[32]; snprintf(cname, sizeof cname, "bench-%d", W->idx);
        oc_wbuf_init(&w, b, sizeof b);
        oc_create_channel cc = { oc_slice_str(cname), 0 };
        if (oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK &&
            write_all(&c.conn, b, w.len) == 0 &&
            read_frame(&c, &hdr, &p) == 0 && hdr.msg_type == OC_MSG_CHANNEL_INFO) {
            oc_channel_info ci; if (oc_decode_channel_info(&p, &ci) == OC_OK) cid = ci.channel_id;
        }
    }

    W->lat = calloc((size_t)(W->sends > 0 ? W->sends : 1), sizeof(double));
    if (!cid) W->sends = 0;   /* idle-only (memory measurement) */
    for (int i = 0; i < W->sends; i++) {
        uint8_t idem[OC_IDEM_SIZE];
        memset(idem, 0, sizeof idem);
        idem[0] = (uint8_t)W->idx; idem[1] = (uint8_t)(W->idx >> 8); idem[2] = (uint8_t)i;
        oc_wbuf_init(&w, b, sizeof b);
        oc_send s = {0}; s.channel_id = cid; memcpy(s.idem, idem, OC_IDEM_SIZE);
        s.body = oc_slice_str("ping");
        double t0 = now_ms();
        if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) != OC_OK || write_all(&c.conn, b, w.len) != 0) break;
        /* Read until this send's ACK (draining its own broadcast). */
        int got = 0;
        for (int k = 0; k < 8 && !got; k++) {
            if (read_frame(&c, &hdr, &p) != 0) { got = -1; break; }
            if (hdr.msg_type == OC_MSG_SEND_ACK) {
                oc_send_ack a; if (oc_decode_send_ack(&p, &a) == OC_OK && memcmp(a.idem, idem, OC_IDEM_SIZE) == 0) got = 1;
            }
        }
        if (got != 1) break;
        W->lat[W->nlat++] = now_ms() - t0;
        usleep(150000);   /* pace under the per-connection send-rate limit */
    }

    /* Hold the connection open (idle) until the run's deadline. */
    double left = g_deadline_ms - now_ms();
    if (left > 0) usleep((useconds_t)(left * 1000.0));
    client_close(&c);
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <host> <port> <n_conns> <hold_s> [sends_per_conn] [n_users]\n", argv[0]); return 2; }
    const char *host = argv[1]; int port = atoi(argv[2]);
    int n = atoi(argv[3]); int hold_s = atoi(argv[4]);
    int sends = argc > 5 ? atoi(argv[5]) : 10;
    int nusers = argc > 6 ? atoi(argv[6]) : n;
    if (n < 1 || n > 4096) { fprintf(stderr, "n_conns out of range\n"); return 2; }
    if (nusers < 1) nusers = 1;

    g_deadline_ms = now_ms() + (double)n * 2.0 + (double)hold_s * 1000.0;  /* +stagger */
    worker *ws = calloc((size_t)n, sizeof *ws);
    pthread_t *th = calloc((size_t)n, sizeof *th);
    for (int i = 0; i < n; i++) {
        ws[i].idx = i; ws[i].host = host; ws[i].port = port; ws[i].sends = sends; ws[i].nusers = nusers;
        pthread_create(&th[i], NULL, run_worker, &ws[i]);
        usleep(2000);   /* stagger setup: real clients don't all connect at once */
    }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);

    int ok = 0, total_sends = 0; size_t total_lat = 0;
    for (int i = 0; i < n; i++) { if (ws[i].connected) ok++; total_sends += ws[i].nlat; total_lat += (size_t)ws[i].nlat; }
    double *all = calloc(total_lat ? total_lat : 1, sizeof(double));
    size_t k = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < ws[i].nlat; j++) all[k++] = ws[i].lat[j];
    qsort(all, k, sizeof(double), cmp_d);

    double p50 = k ? all[k * 50 / 100] : 0, p90 = k ? all[k * 90 / 100] : 0;
    double p99 = k ? all[(k * 99) / 100] : 0, mx = k ? all[k - 1] : 0;
    printf("connections_ok=%d/%d sends=%d rtt_ms p50=%.2f p90=%.2f p99=%.2f max=%.2f\n",
           ok, n, total_sends, p50, p90, p99, mx);
    for (int i = 0; i < n; i++) free(ws[i].lat);
    free(all); free(ws); free(th);
    return ok == n ? 0 : 1;
}
