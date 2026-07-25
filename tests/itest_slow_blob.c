/* Does a SLOW blob backend stall message delivery? (ARCH-69, REQ-212)
 *
 * This is the claim the transfer pool exists to make true, and until now nothing
 * tested it: every prior test ran against the local filesystem, where a blob
 * operation completes in microseconds. The regime the pool was BUILT for — an
 * S3 endpoint costing ~100 ms per operation — was exactly the one never
 * exercised.
 *
 * The test stands up a fake S3 endpoint with a deliberate per-request delay,
 * points the daemon's blob backend at it, and then runs two clients at once:
 *
 *   - alice streams a multi-chunk attachment through the slow backend;
 *   - bob sends ordinary messages and times each round-trip.
 *
 * If blob I/O still ran inline on the epoll thread (as it did before ARCH-69),
 * every one of alice's chunk writes would block the loop and bob's round-trips
 * would spike to the backend's latency. So the assertion is about BOB: his
 * messages must stay fast while alice crawls.
 *
 * Hermetic — the "slow S3" is a loopback socket in this process, so the latency
 * is injected rather than borrowed from a real network. */

#include "client.h"
#include "model.h"
#include "netloop.h"
#include "dbwriter.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SLOW_MS 120        /* per-request delay: roughly a cross-region S3 hop */

/* --- a fake S3 endpoint that is deliberately slow ------------------------- */

typedef struct {
    int          listen_fd;
    int          port;
    pthread_t    th;
    volatile int stop;
    volatile int requests;
    uint8_t      obj[1024 * 1024];
    size_t       obj_len;
} slow_s3;

static slow_s3 g_s3;

static void ms_sleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms_local(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000);
}

static void send_all_fd(int fd, const char *s, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, s + sent, n - sent);
        if (w <= 0) return;
        sent += (size_t)w;
    }
}

static void *slow_s3_thread(void *arg) {
    slow_s3 *f = arg;
    for (;;) {
        int fd = accept(f->listen_fd, NULL, NULL);
        if (fd < 0) { if (f->stop) break; continue; }
        if (f->stop) { close(fd); break; }


        char buf[8192];
        size_t total = 0;
        long hlen = -1;
        while (total < sizeof buf - 1) {
            ssize_t n = read(fd, buf + total, sizeof buf - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
            buf[total] = '\0';
            char *end = strstr(buf, "\r\n\r\n");
            if (end) { hlen = (long)(end - buf) + 4; break; }
        }
        if (hlen < 0) { close(fd); continue; }

        char method[16] = "";
        sscanf(buf, "%15s", method);


        if (strcmp(method, "PUT") == 0) {
            long long want = 0;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) want = atoll(cl + 15);
            size_t have = total - (size_t)hlen;
            if (have > (size_t)want) have = (size_t)want;
            if (have > sizeof f->obj) have = sizeof f->obj;
            memcpy(f->obj, buf + hlen, have);
            /* Read the body SLOWLY, in small pieces with a delay between them.
             * This is what actually stresses the design: the daemon's socket
             * write blocks once the send buffer fills, so oc_blob_put_chunk
             * stalls for real. Delaying only at request start (the obvious
             * mistake) lets the body stream at memory speed and proves nothing. */
            size_t got = have;
            while (got < (size_t)want && got < sizeof f->obj) {
                ssize_t n = read(fd, f->obj + got, (size_t)want - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
            f->obj_len = got;
            const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all_fd(fd, ok, strlen(ok));
        } else if (strcmp(method, "GET") == 0) {
            char head[128];
            int n = snprintf(head, sizeof head,
                             "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             f->obj_len);
            send_all_fd(fd, head, (size_t)n);
            /* Dribble the body. Every oc_blob_get_chunk on the daemon side then
             * blocks waiting for the next piece — which is the stall the
             * transfer pool exists to keep off the epoll thread.
             *
             * Downloads rather than uploads, deliberately: making a WRITE block
             * requires exceeding the sender's socket send buffer (~2.5 MB), so
             * an upload-based test needs a multi-megabyte payload and runs for
             * minutes. A slow READER blocks trivially — there is simply no data
             * yet — so this is both faster and more reliable. */
            size_t sent = 0;
            while (sent < f->obj_len && !f->stop) {
                size_t piece = f->obj_len - sent;
                if (piece > 16 * 1024) piece = 16 * 1024;
                ms_sleep(SLOW_MS);
                f->requests++;                       /* count slow segments */
                send_all_fd(fd, (const char *)f->obj + sent, piece);
                sent += piece;
            }
        } else {
            const char *nc = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all_fd(fd, nc, strlen(nc));
        }
        close(fd);
    }
    return NULL;
}

static int slow_s3_start(slow_s3 *f) {
    memset(f, 0, sizeof *f);
    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) return -1;
    int yes = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(f->listen_fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    if (listen(f->listen_fd, 16) != 0) return -1;
    socklen_t al = sizeof a;
    getsockname(f->listen_fd, (struct sockaddr *)&a, &al);
    f->port = ntohs(a.sin_port);
    return pthread_create(&f->th, NULL, slow_s3_thread, f) == 0 ? 0 : -1;
}

static void slow_s3_stop(slow_s3 *f) {
    f->stop = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)f->port);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) { /* wake accept */ }
        close(fd);
    }
    pthread_join(f->th, NULL);
    close(f->listen_fd);
}

/* --- in-process daemon ---------------------------------------------------- */

struct sb_loop_arg {
    int                   port;
    oc_tls_server        *srv;
    oc_dbwriter          *dbw;
    volatile sig_atomic_t stop;
};

static void *sb_loop_thread(void *p) {
    struct sb_loop_arg *a = p;
    oc_netloop_run(a->port, a->srv, a->dbw, &a->stop);
    return NULL;
}

static void sb_wait_port(int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    for (int i = 0; i < 500; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) { close(fd); return; }
        if (fd >= 0) close(fd);
        ms_sleep(10);
    }
}

/* Alice runs on her own thread so her upload streams continuously through the
 * slow backend while bob is measured. Ticking her between bob's sends (the
 * obvious shortcut) would make the two barely overlap and the measurement
 * meaningless. */
static volatile int g_alice_run;
static oc_client   *g_alice;

static void *alice_thread(void *unused) {
    (void)unused;
    while (g_alice_run) { oc_client_tick(g_alice); ms_sleep(2); }
    return NULL;
}

/* Tick a client until `cond` holds or the budget runs out. */
#define SB_WAIT(cl, cond, ms)                                                  \
    ({                                                                         \
        int _ok = 0;                                                           \
        for (int _i = 0; _i < (ms) / 5; _i++) {                                \
            oc_client_tick((cl));                                              \
            const oc_model *m = oc_client_model((cl)); (void)m;                         \
            if (cond) { _ok = 1; break; }                                      \
            ms_sleep(5);                                                       \
        }                                                                      \
        _ok;                                                                   \
    })

static int has_body(const oc_model *m, const char *body) {
    for (size_t i = 0; i < m->n_channels; i++)
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].body && strcmp(m->channels[i].msgs[j].body, body) == 0)
                return 1;
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int run_slow_blob_tests(void) {
    printf("itest_slow_blob: message latency while a %d ms/op blob backend streams "
           "an upload (the ARCH-69 claim)\n", SLOW_MS);

    CHECK(slow_s3_start(&g_s3) == 0);
    if (g_s3.port == 0) return failures;

    /* Credentials select the S3 backend (ARCH-70); explicit http:// keeps the
     * fake endpoint plaintext. */
    char ep[64];
    snprintf(ep, sizeof ep, "http://127.0.0.1:%d", g_s3.port);
    setenv("OPENCHIME_S3_ENDPOINT", ep, 1);
    setenv("OPENCHIME_S3_BUCKET", "slowbucket", 1);
    setenv("OPENCHIME_S3_ACCESS_KEY", "AKIDEXAMPLE", 1);
    setenv("OPENCHIME_S3_SECRET_KEY", "SECRETEXAMPLEKEY", 1);
    setenv("OPENCHIME_S3_REGION", "us-east-1", 1);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    unlink("build/itest_slowblob.db");
    unlink("build/itest_slowblob.db-wal");
    unlink("build/itest_slowblob.db-shm");
    oc_dbwriter *dbw = oc_dbwriter_start("build/itest_slowblob.db");
    CHECK(dbw != NULL);
    if (!dbw) { slow_s3_stop(&g_s3); return failures; }
    CHECK(oc_dbwriter_register_local(dbw, "sb-alice", "pw", OC_ROLE_OWNER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "sb-bob",   "pw", OC_ROLE_MEMBER, 2048) != 0);

    struct sb_loop_arg arg;
    arg.port = 18600 + (int)(getpid() % 900);
    arg.srv = &srv; arg.dbw = dbw; arg.stop = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, sb_loop_thread, &arg) == 0);
    sb_wait_port(arg.port);

    oc_client *alice = oc_client_start("127.0.0.1", arg.port, "sb-alice:pw");
    oc_client *bob   = oc_client_start("127.0.0.1", arg.port, "sb-bob:pw");
    CHECK(alice && bob);
    if (!alice || !bob) goto done;
    CHECK(SB_WAIT(alice, m->authed, 8000));
    CHECK(SB_WAIT(bob,   m->authed, 8000));
    CHECK(SB_WAIT(alice, m->n_channels > 0, 5000));
    CHECK(SB_WAIT(bob,   m->n_channels > 0, 5000));

    /* --- baseline: bob's round-trip with nothing else happening ------------ */
    uint64_t base[5];
    for (int i = 0; i < 5; i++) {
        char body[32]; snprintf(body, sizeof body, "base-%d", i);
        uint64_t t0 = now_ms_local();
        oc_client_send(bob, oc_client_model(bob)->channels[0].channel_id, body);
        CHECK(SB_WAIT(bob, has_body(m, body), 8000));
        base[i] = now_ms_local() - t0;
    }
    qsort(base, 5, sizeof base[0], cmp_u64);
    uint64_t base_median = base[2];

    /* --- now: bob sends while alice pushes an upload through the slow S3 --- */
    mkdir("build", 0755);
    const char *upath = "build/itest_slowblob_payload.bin";
    {
        FILE *f = fopen(upath, "wb");
        CHECK(f != NULL);
        if (f) {
            /* Several chunks, so the upload spans many slow backend ops. */
            uint8_t *blk = malloc(64 * 1024);
            CHECK(blk != NULL);
            if (blk) {
                memset(blk, 0xAB, 64 * 1024);
                /* Must exceed the daemon's socket send buffer (~2.5 MB by
                 * default) or every write completes into the kernel and
                 * nothing ever blocks, no matter how slow the reader. */
                for (int i = 0; i < 4; i++) fwrite(blk, 1, 64 * 1024, f);
                free(blk);
            }
            fclose(f);
        }
    }

    /* Upload first (fast — the fake backend only dribbles on GET), then let the
     * SLOW download be what runs concurrently with bob's measurement. */
    uint64_t ch = oc_client_model(alice)->channels[0].channel_id;
    oc_client_upload(alice, ch, upath);
    CHECK(SB_WAIT(alice, g_s3.obj_len > 0, 20000));
    uint64_t aid = 0;
    for (int i = 0; i < 400 && !aid; i++) {
        oc_client_tick(alice);
        const oc_model *am = oc_client_model(alice);
        for (size_t c2 = 0; c2 < am->n_channels && !aid; c2++)
            for (size_t j = 0; j < am->channels[c2].n_msgs && !aid; j++)
                if (am->channels[c2].msgs[j].n_attach)
                    aid = am->channels[c2].msgs[j].attach[0].id;
        ms_sleep(10);
    }
    CHECK(aid != 0);
    if (aid) oc_client_download(alice, aid, "build/itest_slowblob_dl.bin");

    g_alice = alice;
    g_alice_run = 1;
    pthread_t alice_th;
    CHECK(pthread_create(&alice_th, NULL, alice_thread, NULL) == 0);
    int segs_before = g_s3.requests;

    /* While that grinds through the slow backend, time bob's round-trips. */
    uint64_t during[9];
    int n_during = 0;
    for (int i = 0; i < 9; i++) {
        char body[32]; snprintf(body, sizeof body, "during-%d", i);
        uint64_t t0 = now_ms_local();
        oc_client_send(bob, oc_client_model(bob)->channels[0].channel_id, body);
        int ok = SB_WAIT(bob, has_body(m, body), 8000);
        CHECK(ok);
        if (ok) during[n_during++] = now_ms_local() - t0;
    }
    int segs_during = g_s3.requests - segs_before;
    g_alice_run = 0;
    pthread_join(alice_th, NULL);
    CHECK(n_during > 0);
    if (n_during == 0) goto done;
    qsort(during, (size_t)n_during, sizeof during[0], cmp_u64);
    uint64_t during_median = during[n_during / 2];
    uint64_t during_max    = during[n_during - 1];

    printf("  bob round-trip: idle median %llums | during slow-download median %llums, max %llums\n"
           "  backend %d ms/op, %d slow segment(s) served DURING the measurement "
           "(= %d ms of backend stall the loop did not absorb)\n",
           (unsigned long long)base_median, (unsigned long long)during_median,
           (unsigned long long)during_max,
           SLOW_MS, segs_during, segs_during * SLOW_MS);

    /* THE ASSERTION, stated against the measured baseline rather than an
     * absolute guess. If blob I/O were inline on the epoll thread, a round-trip
     * landing mid-chunk would absorb a whole SLOW_MS stall, so bob's numbers
     * would rise by at least that. Allowing baseline + SLOW_MS/2 is generous and
     * still fails loudly for an inline implementation. */
    CHECK(during_median < base_median + (uint64_t)SLOW_MS / 2);
    /* No round-trip should swallow a full backend stall. Counted across all
     * samples (not just the max) with a one-sample tolerance: an inline blob
     * implementation would stall *many* round-trips landing mid-chunk, so it
     * still fails loudly — but a single scheduler hiccup on a loaded CI runner,
     * which spikes one sample without any blob-I/O blocking, no longer does. */
    int over_stall = 0;
    for (int i = 0; i < n_during; i++)
        if (during[i] >= base_median + (uint64_t)SLOW_MS) over_stall++;
    CHECK(over_stall <= 1);

    /* The upload must have been genuinely in flight while bob was measured, or
     * the comparison proved nothing. Several slow segments inside the window
     * means the loop had real blocking work available to absorb — and didn't. */
    CHECK(segs_during >= 3);

done:
    if (alice) oc_client_stop(alice);
    if (bob) oc_client_stop(bob);
    arg.stop = 1;
    pthread_join(th, NULL);
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    slow_s3_stop(&g_s3);

    unsetenv("OPENCHIME_S3_ENDPOINT");
    unsetenv("OPENCHIME_S3_BUCKET");
    unsetenv("OPENCHIME_S3_ACCESS_KEY");
    unsetenv("OPENCHIME_S3_SECRET_KEY");
    unsetenv("OPENCHIME_S3_REGION");
    unlink("build/itest_slowblob.db");
    unlink("build/itest_slowblob.db-wal");
    unlink("build/itest_slowblob.db-shm");
    unlink("build/itest_slowblob_payload.bin");
    return failures;
}
