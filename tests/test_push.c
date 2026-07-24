/* Outbound push emitter (ARCH-85): the DND-window evaluator, the recipient
 * collector (level/DND/membership gating), the CP-12 request signature (verified
 * with the public key — the exact check central runs), the contentless body
 * builder, device-token register/unregister/prune through the writer, and a full
 * notify->relay->prune round-trip against a fake HTTP gateway. */

#include "check.h"
#include "push.h"
#include "dbwriter.h"
#include "enroll.h"
#include "migrate.h"
#include "protocol.h"

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- helpers --------------------------------------------------------------- */

static void cleanup_db(const char *path) {
    unlink(path);
    char wal[256], shm[256];
    snprintf(wal, sizeof wal, "%s-wal", path);
    snprintf(shm, sizeof shm, "%s-shm", path);
    unlink(wal); unlink(shm);
}

static oc_dbres *wait_result(oc_dbwriter *w) {
    for (int i = 0; i < 500; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) return r;
        usleep(2000);
    }
    return NULL;
}

/* Set a user's per-channel notification level via the writer's SET_NOTIFY_PREF
 * job (drains the resulting snapshot). */
static void set_level(oc_dbwriter *w, uint64_t user_id, uint64_t channel_id, uint8_t level) {
    oc_job *j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 0);
    j->user_id = user_id; j->channel_id = channel_id; j->notify_level = level;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    oc_dbres_free(r);
}

static void set_dnd(oc_dbwriter *w, uint64_t user_id, int start_min, int end_min) {
    oc_job *j = oc_job_new(OC_JOB_SET_DND, 0);
    j->user_id = user_id; j->dnd_enabled = 1;
    j->dnd_start_min = (uint16_t)start_min; j->dnd_end_min = (uint16_t)end_min;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    oc_dbres_free(r);
}

static int token_count(const char *path, const char *token) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM device_tokens WHERE token=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* --- DND evaluator --------------------------------------------------------- */

static void test_dnd(void) {
    CHECK(oc_push_dnd_active(0, 600, 660, 630) == 0);        /* disabled */
    CHECK(oc_push_dnd_active(1, 600, 660, 630) == 1);        /* inside a normal window */
    CHECK(oc_push_dnd_active(1, 600, 660, 600) == 1);        /* inclusive start */
    CHECK(oc_push_dnd_active(1, 600, 660, 660) == 0);        /* exclusive end */
    CHECK(oc_push_dnd_active(1, 600, 660, 500) == 0);        /* before */
    CHECK(oc_push_dnd_active(1, 600, 660, 700) == 0);        /* after */
    CHECK(oc_push_dnd_active(1, 300, 300, 300) == 0);        /* empty window */
    /* Wrap-around (e.g. 22:00 -> 07:00). */
    CHECK(oc_push_dnd_active(1, 1320, 420, 1400) == 1);      /* late night */
    CHECK(oc_push_dnd_active(1, 1320, 420, 60) == 1);        /* early morning */
    CHECK(oc_push_dnd_active(1, 1320, 420, 700) == 0);       /* midday, outside */
    CHECK(oc_push_dnd_active(1, 1320, 420, 420) == 0);       /* exclusive end across wrap */
}

/* --- body builder ---------------------------------------------------------- */

static void test_build_body(void) {
    oc_push_target t[2];
    t[0].platform = OC_PUSH_APNS; snprintf(t[0].token, sizeof t[0].token, "tokA");
    t[1].platform = OC_PUSH_FCM;  snprintf(t[1].token, sizeof t[1].token, "tokB");
    char body[512];
    CHECK(oc_push_build_body(42, t, 2, body, sizeof body) == 0);
    CHECK(strstr(body, "\"notifications\"") != NULL);
    CHECK(strstr(body, "\"platform\":\"apns\"") != NULL);
    CHECK(strstr(body, "\"platform\":\"fcm\"") != NULL);
    CHECK(strstr(body, "\"token\":\"tokA\"") != NULL);
    CHECK(strstr(body, "\"channelId\":\"42\"") != NULL);
    /* Contentless: no message body/title/sender leaks into the wire. */
    CHECK(strstr(body, "title") == NULL);
    CHECK(strstr(body, "body") == NULL);
}

/* --- CP-12 signature ------------------------------------------------------- */

static void test_sign_verify(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);

    const char *body = "{\"notifications\":[]}";
    long ts = 1750000000L;
    char sig_b64[512];
    CHECK(oc_push_sign(pk, aud, body, ts, sig_b64, sizeof sig_b64) == 0);

    uint8_t sig[160];
    size_t siglen = 0;
    CHECK(mbedtls_base64_decode(sig, sizeof sig, &siglen,
                                (const unsigned char *)sig_b64, strlen(sig_b64)) == 0);

    /* Reconstruct central's canonical string and verify with the public key. */
    mbedtls_pk_context kp;
    mbedtls_pk_init(&kp);
    CHECK(mbedtls_pk_parse_key(&kp, (const unsigned char *)pk, strlen(pk) + 1, NULL, 0, NULL, NULL) == 0);

    uint8_t bh[32];
    mbedtls_sha256((const unsigned char *)body, strlen(body), bh, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", bh[i]);
    char canon[640];
    int cn = snprintf(canon, sizeof canon, "openchime-machine-v1|%s|%ld|%s", aud, ts, hex);
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)canon, (size_t)cn, h, 0);
    CHECK(mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen) == 0);

    /* A tampered body (different hash) must not verify against the same signature. */
    uint8_t bh2[32];
    mbedtls_sha256((const unsigned char *)"{\"notifications\":[{\"x\":1}]}", 26, bh2, 0);
    char hex2[65];
    for (int i = 0; i < 32; i++) snprintf(hex2 + i * 2, 3, "%02x", bh2[i]);
    char canon2[640];
    int cn2 = snprintf(canon2, sizeof canon2, "openchime-machine-v1|%s|%ld|%s", aud, ts, hex2);
    uint8_t h2[32];
    mbedtls_sha256((const unsigned char *)canon2, (size_t)cn2, h2, 0);
    CHECK(mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, h2, sizeof h2, sig, siglen) != 0);

    mbedtls_pk_free(&kp);
}

/* --- recipient collector + register/prune ---------------------------------- */

static void test_collect(void) {
    const char *path = "build/test_push_collect.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) return;

    /* register_local auto-joins each user to the default channel (id 1). */
    uint64_t alice = oc_dbwriter_register_local(w, "alice", "pw", OC_ROLE_OWNER, 2048);
    uint64_t bob   = oc_dbwriter_register_local(w, "bob",   "pw", OC_ROLE_MEMBER, 2048);
    uint64_t carol = oc_dbwriter_register_local(w, "carol", "pw", OC_ROLE_MEMBER, 2048);
    uint64_t dave  = oc_dbwriter_register_local(w, "dave",  "pw", OC_ROLE_MEMBER, 2048);
    CHECK(alice && bob && carol && dave);

    /* Everyone gets a device token, including the author. */
    CHECK(oc_dbwriter_register_device_token(w, alice, OC_PUSH_APNS, "tok-alice"));
    CHECK(oc_dbwriter_register_device_token(w, bob,   OC_PUSH_APNS, "tok-bob"));
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_FCM,  "tok-carol"));
    CHECK(oc_dbwriter_register_device_token(w, dave,  OC_PUSH_APNS, "tok-dave"));

    set_level(w, carol, 1, OC_NOTIFY_NONE);   /* carol muted this channel */
    set_dnd(w, bob, 600, 660);                /* bob quiet 10:00–11:00 */

    sqlite3 *rdb = NULL;
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);

    /* At 10:30 (630): author excluded, carol muted, bob in DND → only dave. */
    oc_push_target t[8];
    int n = oc_push_collect(rdb, 1, alice, 630, t, 8);
    CHECK(n == 1);
    if (n == 1) CHECK(strcmp(t[0].token, "tok-dave") == 0);

    /* At 01:40 (100): bob no longer in DND → bob + dave (carol still muted). */
    n = oc_push_collect(rdb, 1, alice, 100, t, 8);
    CHECK(n == 2);
    int saw_bob = 0, saw_dave = 0, saw_carol = 0, saw_alice = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].token, "tok-bob") == 0) saw_bob = 1;
        if (strcmp(t[i].token, "tok-dave") == 0) saw_dave = 1;
        if (strcmp(t[i].token, "tok-carol") == 0) saw_carol = 1;
        if (strcmp(t[i].token, "tok-alice") == 0) saw_alice = 1;
    }
    CHECK(saw_bob && saw_dave);
    CHECK(!saw_carol && !saw_alice);   /* muted / author never notified */
    sqlite3_close(rdb);

    /* Prune dave's token; re-registering carol's (synchronous) flushes the FIFO
     * so the fire-and-forget prune has definitely applied. */
    oc_dbwriter_prune_device_token(w, "tok-dave");
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_FCM, "tok-carol"));
    CHECK(token_count(path, "tok-dave") == 0);

    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    n = oc_push_collect(rdb, 1, alice, 100, t, 8);
    CHECK(n == 1);   /* dave pruned → only bob */
    if (n == 1) CHECK(strcmp(t[0].token, "tok-bob") == 0);
    sqlite3_close(rdb);

    /* Unregister bob's own token. */
    oc_job *j = oc_job_new(OC_JOB_UNREGISTER_DEVICE_TOKEN, 0);
    j->user_id = bob; j->device_token = strdup("tok-bob");
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_DEVICE_TOKEN_OK);
    oc_dbres_free(r);
    CHECK(token_count(path, "tok-bob") == 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- fake gateway: full notify -> relay -> prune round-trip ----------------- */

typedef struct { int fd; int got_request; int had_signature; } fake_gw;

static void *fake_gw_thread(void *arg) {
    fake_gw *g = arg;
    int c = accept(g->fd, NULL, NULL);
    if (c < 0) return NULL;
    /* Drain the whole request (headers + Content-Length body) before responding —
     * a real server does, and responding/closing mid-write would EPIPE the client. */
    char buf[8192];
    size_t total = 0, headers_end = 0;
    long content_len = -1;
    for (;;) {
        if (total >= sizeof buf - 1) break;
        ssize_t n = read(c, buf + total, sizeof buf - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (!headers_end) {
            char *he = strstr(buf, "\r\n\r\n");
            if (he) {
                headers_end = (size_t)(he - buf) + 4;
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl) content_len = strtol(cl + 15, NULL, 10);
            }
        }
        if (headers_end && content_len >= 0 && total >= headers_end + (size_t)content_len) break;
    }
    if (total > 0) {
        g->got_request = 1;
        if (strcasestr(buf, "X-OpenChime-Signature:")) g->had_signature = 1;
    }
    /* Report bob's token stale so the emitter prunes it. */
    const char *body = "{\"staleTokens\":[\"tok-bob-rt\"]}";
    char resp[256];
    int rn = snprintf(resp, sizeof resp,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s", strlen(body), body);
    ssize_t wr = write(c, resp, (size_t)rn);
    (void)wr;
    close(c);
    return NULL;
}

static void test_notify_roundtrip(void) {
    const char *path = "build/test_push_rt.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) { return; }

    uint64_t alice = oc_dbwriter_register_local(w, "alice", "pw", OC_ROLE_OWNER, 2048);
    uint64_t bob   = oc_dbwriter_register_local(w, "bob",   "pw", OC_ROLE_MEMBER, 2048);
    CHECK(alice && bob);
    CHECK(oc_dbwriter_register_device_token(w, bob, OC_PUSH_APNS, "tok-bob-rt"));

    /* Bind a fake gateway on an ephemeral port. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    CHECK(bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0);
    CHECK(listen(lfd, 1) == 0);
    socklen_t sl = sizeof sa;
    getsockname(lfd, (struct sockaddr *)&sa, &sl);
    int port = ntohs(sa.sin_port);

    fake_gw gw = { lfd, 0, 0 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, fake_gw_thread, &gw) == 0);

    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);

    oc_push *p = oc_push_start(path, w, url, NULL, aud, pk);
    CHECK(p != NULL);
    if (p) {
        oc_push_notify(p, 1, alice);   /* alice sent → bob should be notified */
        /* stop drains the in-flight notify: the worker completes do_notify (POST +
         * the fire-and-forget prune submit) before it exits — deterministic, no poll. */
        oc_push_stop(p);
    }

    pthread_join(th, NULL);
    close(lfd);
    CHECK(gw.got_request);
    CHECK(gw.had_signature);

    /* A synchronous writer op flushes the FIFO, so the prune (submitted earlier on
     * the same writer) has definitely applied. */
    if (p) {
        oc_dbwriter_register_device_token(w, alice, OC_PUSH_APNS, "flush-tok");
        CHECK(token_count(path, "tok-bob-rt") == 0);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

int run_push_tests(void) {
    printf("test_push: DND window (incl wrap-around), recipient collect "
           "(level/DND/author gating), CP-12 sign+verify, contentless body, "
           "device-token register/unregister/prune, notify->relay->prune round-trip\n");
    test_dnd();
    test_build_body();
    test_sign_verify();
    test_collect();
    test_notify_roundtrip();
    return failures;
}
