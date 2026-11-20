/* Does a SLOW blob backend stall message delivery? (ARCH-69, REQ-212)
 *
 * This is the claim the transfer pool exists to make true, and until now nothing
 * tested it: every prior test ran against the local filesystem, where a blob
 * operation completes in microseconds. The regime the pool was BUILT for — an
 * S3 endpoint costing ~100 ms per operation — was exactly the one never
 * exercised.
 *
 * The test stands up a fake S3 endpoint that hands out a download's body one
 * segment at a time, each only when the test releases it, points the daemon's
 * blob backend at it, and then runs two clients at once:
 *
 *   - alice streams a multi-chunk attachment through the stalled backend;
 *   - bob sends ordinary messages while a segment is held.
 *
 * If blob I/O still ran inline on the epoll thread (as it did before ARCH-69),
 * the loop would block in the backend read until the test released it, and
 * bob's messages would not be delivered while the segment is held. So the
 * assertion is about BOB: his messages go through while alice's transfer is
 * stalled for as long as the test likes.
 *
 * Hermetic — the "slow S3" is a loopback socket in this process, so the stall
 * is injected rather than borrowed from a real network. */

#include "client.h"
#include "model.h"
#include "netloop.h"
#include "blobstore.h"
#include "config.h"
#include "dbwriter.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"
#include "testenv.h"
#include "testsignin.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* --- a fake S3 endpoint that is deliberately slow ------------------------- */

typedef struct {
    int          listen_fd;
    int          port;
    pthread_t    th;
    atomic_int   stop;          /* these are shared with the test thread */
    atomic_int   requests;
    uint8_t      obj[1024 * 1024];
    atomic_size_t obj_len;
    /* While `gate` is set, a PUT is read whole and then NOT answered until it is
     * cleared: the daemon's commit waits on the reply, so the upload cannot
     * finish. `held` counts the PUTs that have reached the gate. */
    atomic_int   gate, held;
    /* While `step` is set, each segment of a download's body waits to be
     * released: `holds` counts the segments that have reached the wait,
     * `released` how many the test has let go, `waiting` is set while one is
     * held. A stalled backend for as long as the test likes, with no clock. */
    atomic_int   step, holds, released, waiting;
} slow_s3;

static slow_s3 g_s3;

/* How long the fake backend holds a reply at most: past the whole test, so it
 * is the test that lets it go (or stops the backend), never the clock. */
#define SB_HOLD_MS 120000

/* The suite's own directory (oc_test_mkdtemp), and `name` under it. */
static char g_sbdir[512];
static const char *sbpath(char *out, size_t cap, const char *name) { return oc_test_path(out, cap, g_sbdir, name); }

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
            if (f->gate) {
                f->held++;
                (void)WAIT_UNTIL(!f->gate || f->stop, SB_HOLD_MS);
            }
            const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all_fd(fd, ok, strlen(ok));
        } else if (strcmp(method, "GET") == 0) {
            char head[128];
            int n = snprintf(head, sizeof head,
                             "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             f->obj_len);
            send_all_fd(fd, head, (size_t)n);
            /* Dribble the body, a segment at a time as the test releases each
             * (`step`). Every oc_blob_get_chunk on the daemon side then blocks
             * waiting for the next piece — which is the stall the transfer pool
             * exists to keep off the epoll thread.
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
                if (f->step) {
                    int k = ++f->holds;
                    f->waiting = 1;
                    (void)WAIT_UNTIL(f->released >= k || !f->step || f->stop, SB_HOLD_MS);
                    f->waiting = 0;
                }
                f->requests++;                       /* count segments served */
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
    int                   exited;   /* the loop returned (atomic): its bind was refused */
    oc_netloop_view       view;     /* the loop's own account: the port it bound */
};

/* A client signed in as a person is (AUTH.md §8.10): the page, then its token. */
static oc_client *sb_start(int port, const char *user, const char *pass) {
    char tok[OC_TEST_TOKEN_CAP], ver[OC_TEST_VERIFIER_CAP], why[256];
    if (oc_test_signin("127.0.0.1", port, NULL, user, pass, tok, sizeof tok, ver, sizeof ver, why, sizeof why) != 0) {
        printf("    %s: the sign-in page refused: %s\n", user, why);
        return NULL;
    }
    oc_signin_result res = { "local", ver, tok, NULL };
    return oc_client_start_signin_result(NULL, "127.0.0.1", port, &res, NULL, NULL, 1, NULL);
}

static void *sb_loop_thread(void *p) {
    struct sb_loop_arg *a = p;
    char cfgerr[128];
    oc_config_load(cfgerr, sizeof cfgerr);   /* load after the test's setenv() */
    oc_netloop_run_ex(0, a->srv, a->dbw, &a->stop, &a->view);   /* the kernel picks the port */
    __atomic_store_n(&a->exited, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* The loop on a port the kernel picks (bound as 0), up and listening, its
 * port read back from the loop itself. 0 or -1. */
static int sb_loop_start(struct sb_loop_arg *a, pthread_t *th) {
    memset(&a->view, 0, sizeof a->view);
    __atomic_store_n(&a->exited, 0, __ATOMIC_RELEASE);
    if (pthread_create(th, NULL, sb_loop_thread, a) != 0) return -1;
    if (!WAIT_UNTIL(__atomic_load_n(&a->view.proto_port, __ATOMIC_ACQUIRE) > 0 ||
                    __atomic_load_n(&a->exited, __ATOMIC_ACQUIRE), 10000)) return -1;
    if (__atomic_load_n(&a->exited, __ATOMIC_ACQUIRE)) { pthread_join(*th, NULL); return -1; }
    a->port = __atomic_load_n(&a->view.proto_port, __ATOMIC_ACQUIRE);
    return 0;
}

/* Alice runs on her own thread so her upload streams continuously through the
 * slow backend while bob is measured. Ticking her between bob's sends (the
 * obvious shortcut) would make the two barely overlap and the measurement
 * meaningless. */
static atomic_int g_alice_run;
static oc_client   *g_alice;

static void *alice_thread(void *unused) {
    (void)unused;
    (void)WAIT_UNTIL_DO(oc_client_tick(g_alice), !g_alice_run, SB_HOLD_MS);
    return NULL;
}

/* Tick a client until `cond` holds of its model `m`, or `ms` pass on the
 * monotonic clock (tests/check.h). */
#define SB_WAIT(cl, cond, ms)                                                  \
    WAIT_UNTIL_DO(oc_client_tick((cl)),                                        \
                  ({ const oc_model *m = oc_client_model((cl)); (void)m; (cond); }), (ms))

static int has_body(const oc_model *m, const char *body) {
    for (size_t i = 0; i < m->n_channels; i++)
        for (size_t j = 0; j < m->channels[i].n_msgs; j++)
            if (m->channels[i].msgs[j].body && strcmp(m->channels[i].msgs[j].body, body) == 0)
                return 1;
    return 0;
}

int run_slow_blob_tests(void) {
    printf("itest_slow_blob: messages delivered while a stalled blob backend holds "
           "a transfer (the ARCH-69 claim)\n");

    if (oc_test_mkdtemp("slow_blob", g_sbdir, sizeof g_sbdir) != 0) {
        printf("  FAIL itest_slow_blob: no directory of its own under $TMPDIR\n");
        return failures + 1;
    }
    char dbp[600], upath_b[600], dlpath[600];
    sbpath(dbp, sizeof dbp, "itest_slowblob.db");
    sbpath(upath_b, sizeof upath_b, "itest_slowblob_payload.bin");
    sbpath(dlpath, sizeof dlpath, "itest_slowblob_dl.bin");
    CHECK(slow_s3_start(&g_s3) == 0);
    if (g_s3.port == 0) { oc_test_rmtree(g_sbdir); return failures; }

    /* OPENCHIME_S3 selects the S3 backend (ARCH-70); http+s3:// keeps the fake
     * endpoint plaintext. The blob directory in front of it, the cache, is
     * given no room at all, so every upload and download below goes to the
     * slow store itself, which is the regime under test. */
    char s3url[128];
    snprintf(s3url, sizeof s3url, "http+s3://AKIDEXAMPLE:SECRETEXAMPLEKEY@127.0.0.1:%d/slowbucket", g_s3.port);
    setenv("OPENCHIME_S3", s3url, 1);
    char datadir[600];
    sbpath(datadir, sizeof datadir, "data");
    mkdir(datadir, 0700);
    setenv("OPENCHIME_DATA_DIR", datadir, 1);
    oc_netloop_set_blob_cache_floor(UINT64_MAX / 4);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    oc_dbwriter *dbw = oc_dbwriter_start(dbp);
    if (dbw) oc_dbwriter_set_pw_mem(dbw, 2048);   /* a cheap Argon2id for tests */
    CHECK(dbw != NULL);
    if (!dbw) { slow_s3_stop(&g_s3); oc_test_rmtree(g_sbdir); return failures; }
    CHECK(oc_dbwriter_register_local(dbw, "sb-alice", "pw", OC_ROLE_OWNER, 2048) != 0);
    CHECK(oc_dbwriter_register_local(dbw, "sb-bob",   "pw", OC_ROLE_MEMBER, 2048) != 0);

    struct sb_loop_arg arg;
    arg.port = 0;                                 /* the kernel picks (sb_loop_start) */
    arg.srv = &srv; arg.dbw = dbw; arg.stop = 0;
    pthread_t th;
    int loop_up = sb_loop_start(&arg, &th) == 0;
    CHECK(loop_up);
    oc_client *alice = NULL, *bob = NULL;
    if (!loop_up) goto done;

    alice = sb_start(arg.port, "sb-alice", "pw");
    bob   = sb_start(arg.port, "sb-bob", "pw");
    CHECK(alice && bob);
    if (!alice || !bob) goto done;
    CHECK(SB_WAIT(alice, m->authed, 8000));
    CHECK(SB_WAIT(bob,   m->authed, 8000));
    CHECK(SB_WAIT(alice, m->n_channels > 0, 5000));
    CHECK(SB_WAIT(bob,   m->n_channels > 0, 5000));

    /* --- storage pressure is not measured on S3 ------------------------------
     * The attachments are in S3, so the daemon samples no filesystem: the
     * owner's storage report carries no disk size and no pressure, whatever
     * the local disk under the data directory holds -- which is what keeps a
     * low local disk from evicting out of S3 or refusing uploads (netloop.c). */
    oc_client_storage_status(alice);
    CHECK(SB_WAIT(alice, m->storage_have, 5000));
    CHECK(oc_client_model(alice)->storage.total_bytes == 0);
    CHECK(oc_client_model(alice)->storage.avail_bytes == 0);
    CHECK(oc_client_model(alice)->storage.under_pressure == 0);

    /* --- baseline: bob's messages go through with nothing else happening --- */
    for (int i = 0; i < 5; i++) {
        char body[32]; snprintf(body, sizeof body, "base-%d", i);
        oc_client_send(bob, oc_client_model(bob)->channels[0].channel_id, body);
        CHECK(SB_WAIT(bob, has_body(m, body), 8000));
    }

    /* --- now: bob sends while alice pushes an upload through the slow S3 --- */
    const char *upath = upath_b;
    {
        FILE *f = fopen(upath, "wb");
        CHECK(f != NULL);
        if (f) {
            /* Several chunks, so the upload spans many slow backend ops. */
            uint8_t *blk = malloc(64 * 1024);
            CHECK(blk != NULL);
            if (blk) {
                memset(blk, 0xAB, 64 * 1024);
                /* 128 KB: the fake backend dribbles a download in 16 KB
                 * segments, each a stalled blob call (the stall is the
                 * backend's, not a socket's), so this is eight of them --
                 * enough for bob's rounds below, which hold four. */
                for (int i = 0; i < 2; i++) fwrite(blk, 1, 64 * 1024, f);
                free(blk);
            }
            fclose(f);
        }
    }

    /* Upload first (fast — the fake backend only dribbles on GET), then let the
     * stalled download be what runs concurrently with bob's messages. */
    uint64_t ch = oc_client_model(alice)->channels[0].channel_id;
    oc_client_upload(alice, ch, upath);
    CHECK(SB_WAIT(alice, g_s3.obj_len > 0, 20000));
    uint64_t aid = 0;
    CHECK(SB_WAIT(alice, ({
              for (size_t c2 = 0; c2 < m->n_channels && !aid; c2++)
                  for (size_t j = 0; j < m->channels[c2].n_msgs && !aid; j++)
                      if (m->channels[c2].msgs[j].n_attach)
                          aid = m->channels[c2].msgs[j].attach[0].id;
              aid != 0; }), 4000));
    g_s3.holds = 0;
    g_s3.released = 0;
    g_s3.step = 1;                          /* the download's segments wait on the test */
    if (aid) oc_client_download(alice, aid, dlpath);

    g_alice = alice;
    g_alice_run = 1;
    pthread_t alice_th;
    CHECK(pthread_create(&alice_th, NULL, alice_thread, NULL) == 0);
    int segs_before = g_s3.requests;
    uint64_t loop_ops_before = oc_blobstore_loop_ops();

    /* Four rounds. In each, the backend holds a segment of alice's download
     * until the test releases it, so the transfer is stalled in a blob read;
     * bob sends while it is held, and his message must be delivered then. Were
     * the read on the event loop's thread, the loop would be blocked in it
     * until the release, and bob's message would not arrive. */
    enum { ROUNDS = 4 };
    int rounds_ok = 0;
    for (int i = 0; i < ROUNDS; i++) {
        int held = WAIT_UNTIL(g_s3.holds == i + 1 && g_s3.waiting, 20000);
        CHECK(held);
        if (!held) break;
        CHECK(g_s3.requests - segs_before == i);  /* the earlier segments, each released, were served */
        char body[32]; snprintf(body, sizeof body, "during-%d", i);
        oc_client_send(bob, oc_client_model(bob)->channels[0].channel_id, body);
        int ok = SB_WAIT(bob, has_body(m, body), 8000);
        CHECK(ok);
        /* Delivered while the segment was still held: only the test releases it. */
        CHECK(g_s3.waiting && g_s3.holds == i + 1);
        if (ok) rounds_ok++;
        g_s3.released = i + 1;
    }
    int segs_during = g_s3.requests - segs_before;
    uint64_t loop_ops_during = oc_blobstore_loop_ops() - loop_ops_before;
    g_s3.step = 0;                          /* the rest of the download streams */
    g_alice_run = 0;
    pthread_join(alice_th, NULL);
    CHECK(rounds_ok == ROUNDS);

    printf("  bob: %d message(s) delivered, each while a backend read was held; "
           "%d segment(s) served during the rounds\n", rounds_ok, segs_during);

    /* And the mechanism, by construction: not one blob operation ran on the
     * event loop's thread while the stalled transfer streamed (blobstore.h).
     * Counted, not timed -- a single operation moved onto the loop makes it
     * non-zero. */
    printf("  blob operations on the loop thread during the transfer: %llu\n",
           (unsigned long long)loop_ops_during);
    CHECK(loop_ops_during == 0);

    /* The download was genuinely in flight across bob's rounds, segment after
     * segment, or the rounds proved nothing. */
    CHECK(segs_during >= ROUNDS - 1);

    /* Cancelling a post of files (REQ-140), by construction rather than by
     * timing. With the gate shut, an upload that has sent its last byte waits on
     * the backend's reply to its commit, so it cannot finish however long this
     * takes — cancelling it then is cancelling a RUNNING upload. It also holds the
     * connection's one transfer slot, so a post queued behind it cannot start —
     * cancelling that is cancelling a QUEUED one. Neither posts anything; the post
     * queued after both does, once the gate opens. Bob sees posts in the order
     * they were made, so seeing the last one means he would already have seen
     * either of the others had it gone. */
    {
        CHECK(SB_WAIT(alice, ({ const oc_xfer_state *d = oc_model_xfer(m, aid); d && d->phase != 0; }), 30000));
        char pa_b[600], pb_b[600], pc_b[600];
        const char *pa = sbpath(pa_b, sizeof pa_b, "itest_slowblob_post_a.txt"),
                   *pb = sbpath(pb_b, sizeof pb_b, "itest_slowblob_post_b.txt"),
                   *pc = sbpath(pc_b, sizeof pc_b, "itest_slowblob_post_c.txt");
        const char *files[3] = { pa, pb, pc };
        for (int i = 0; i < 3; i++) {
            FILE *f = fopen(files[i], "wb");
            CHECK(f != NULL);
            if (f) { fprintf(f, "post %d\n", i); fclose(f); }
        }
        g_s3.held = 0;
        g_s3.gate = 1;
        uint64_t ta = oc_client_post_files(alice, ch, 0, &pa, 1, "running, then cancelled");
        CHECK(ta != 0);
        CHECK(SB_WAIT(alice, g_s3.held == 1, 10000));
        uint64_t tb = oc_client_post_files(alice, ch, 0, &pb, 1, "queued, then cancelled");
        uint64_t tc = oc_client_post_files(alice, ch, 0, &pc, 1, "queued after both");
        CHECK(tb != 0 && tc != 0);
        oc_client_cancel_transfer(alice, tb);
        oc_client_cancel_transfer(alice, ta);
        CHECK(SB_WAIT(alice, ({ const oc_xfer_state *x = oc_model_xfer(m, ta); x && x->phase == 2; }), 5000));
        CHECK(g_s3.held == 1);                  /* nothing else reached the backend */
        g_s3.gate = 0;
        CHECK(SB_WAIT(alice, ({ const oc_xfer_state *x = oc_model_xfer(m, tc); x && x->phase == 1; }), 10000));
        CHECK(SB_WAIT(bob, has_body(m, "queued after both"), 8000));
        CHECK(oc_model_xfer(oc_client_model(alice), tb) == NULL);   /* it never started */
        CHECK(!has_body(oc_client_model(bob), "running, then cancelled"));
        CHECK(!has_body(oc_client_model(bob), "queued, then cancelled"));
        for (int i = 0; i < 3; i++) unlink(files[i]);
    }

done:
    if (alice) oc_client_stop(alice);
    if (bob) oc_client_stop(bob);
    if (loop_up) {
        __atomic_store_n(&arg.stop, 1, __ATOMIC_RELEASE);
        pthread_join(th, NULL);
    }
    oc_dbwriter_stop(dbw);
    oc_tls_server_free(&srv);
    slow_s3_stop(&g_s3);

    unsetenv("OPENCHIME_S3");
    unsetenv("OPENCHIME_DATA_DIR");
    oc_netloop_set_blob_cache_floor(0);
    oc_test_rmtree(g_sbdir);
    return failures;
}
