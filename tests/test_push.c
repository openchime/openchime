/* Outbound push emitter (ARCH-85): the DND-window evaluator, the recipient
 * collector (level/DND/membership gating), the CP-12 request signature (verified
 * with the public key — the exact check central runs), the contentless body
 * builder, device-token register/unregister/prune through the writer, and a full
 * notify->relay->prune round-trip against a fake HTTP gateway. */

#include "check.h"
#include "push.h"
#include "notify.h"
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

/* Every collect below states the moment as BOTH a minute-of-day and the instant
 * that minute is — the schedule is evaluated from the instant (it has to know the
 * weekday and the recipient's offset), so passing 0 for it silently asked "is
 * 01:40 on 1 January 1970 allowed" for every case.
 *
 * The SCHEDULE, and its window is the ALLOWED range (REQ-136) — so a caller
 * that wants somebody quiet from 10:00 to 11:00 allows the complement. Named for
 * what it does to the test rather than for the column it writes. */
static void set_quiet(oc_dbwriter *w, uint64_t user_id, int quiet_start, int quiet_end) {
    oc_job *j = oc_job_new(OC_JOB_SET_SCHEDULE, 0);
    j->user_id = user_id; j->sched_mode = OC_DND_EVERY_DAY;
    /* allowed = the complement of quiet */
    j->sched_start_min = (uint16_t)quiet_end; j->sched_end_min = (uint16_t)quiet_start;
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
    set_quiet(w, bob, 600, 660);                /* bob quiet 10:00–11:00 */

    sqlite3 *rdb = NULL;
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);

    /* At 10:30 (630): author excluded, carol muted, bob in DND → only dave. */
    oc_push_target t[8];
    int n = oc_push_collect(rdb, 1, alice, 0, 0, 630, (uint64_t)630 * 60000ull, t, 8);
    CHECK(n == 1);
    if (n == 1) CHECK(strcmp(t[0].token, "tok-dave") == 0);

    /* At 01:40 (100): bob no longer in DND → bob + dave (carol still muted). */
    n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
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

    /* --- the MENTIONS level (REQ-221) — the deferred half of ARCH-72 ---------
     * Level MENTIONS must deliver only when the message actually names the
     * recipient. Before REQ-221 there was no way to answer that, so the level
     * was accepted and then silently behaved as NONE. */
    set_level(w, dave, 1, OC_NOTIFY_MENTIONS);
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_FCM, "tok-carol"));  /* flush */

    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    /* A message that mentions nobody: dave is on MENTIONS, so he is out. */
    n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    int only_bob = (n == 1) && strcmp(t[0].token, "tok-bob") == 0;
    CHECK(only_bob);

    /* Now record a mention of dave on a message and collect against it. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                 " VALUES(4242,1,%llu,0,0,5,1);", (unsigned long long)dave);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 4242, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    int saw_d = 0, saw_b = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].token, "tok-dave") == 0) saw_d = 1;
        if (strcmp(t[i].token, "tok-bob") == 0)  saw_b = 1;
    }
    CHECK(saw_d && saw_b);          /* mentioned + level-ALL */

    /* A DIFFERENT message still leaves him out: the gate is per message, not a
     * sticky "dave gets mentions" flag. */
    n = oc_push_collect(rdb, 1, alice, 4243, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0;
    for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d = 1;
    CHECK(!saw_d);

    /* A broadcast reaches him without naming him; carol stays muted, because
     * NONE outranks a broadcast. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        CHECK(sqlite3_exec(wdb,
            "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
            " VALUES(4244,1,NULL,2,0,8,1);", NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 4244, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0; int saw_c = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].token, "tok-dave") == 0)  saw_d = 1;
        if (strcmp(t[i].token, "tok-carol") == 0) saw_c = 1;
    }
    CHECK(saw_d);
    CHECK(!saw_c);

    /* --- a REPLY notifies the thread's PARTICIPANTS (REQ-061, ARCH-104) -----
     *
     * dave is on MENTIONS, so an ordinary channel message leaves him out. A
     * reply in a thread he is IN must reach him anyway: participants are
     * notified per their level, "independent of whether they were @mentioned".
     *
     * Participation is derived, never stored — you are in a thread if you wrote
     * its root or any reply — with thread_follows carrying only overrides. All
     * four of those rules are exercised below against the same user, so a change
     * to any one of them fails here. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[512];
        /* A root dave wrote, and a reply from alice under it. */
        snprintf(sql, sizeof sql,
                 "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms)"
                 " VALUES(5000,1,%llu,'root',1);"
                 "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms,parent_id)"
                 " VALUES(5001,1,%llu,'reply',2,5000);",
                 (unsigned long long)dave, (unsigned long long)alice);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }

    /* He wrote the root, so the reply reaches him. */
    n = oc_push_collect(rdb, 1, alice, 5001, 5000, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0; saw_c = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].token, "tok-dave") == 0)  saw_d = 1;
        if (strcmp(t[i].token, "tok-carol") == 0) saw_c = 1;
    }
    CHECK(saw_d);
    /* And mute still wins over participation: carol is in the channel and
     * muted, and nothing pierces mute (REQ-137). */
    CHECK(!saw_c);

    /* The SAME message with no root is an ordinary send, and he is out again —
     * so this is the thread branch doing the work, not the message. */
    n = oc_push_collect(rdb, 1, alice, 5001, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0;
    for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d = 1;
    CHECK(!saw_d);

    /* AN EXPLICIT UNFOLLOW OUTRANKS HAVING WRITTEN IN IT. This is the rule most
     * easily lost by writing the predicate as a plain OR, and it is the whole
     * meaning of "turn off replies" for the person most likely to press it. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO thread_follows(user_id,root_id,channel_id,state)"
                 " VALUES(%llu,5000,1,0);", (unsigned long long)dave);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 5001, 5000, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0;
    for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d = 1;
    CHECK(!saw_d);

    /* And an explicit FOLLOW reaches someone who never wrote in it: the root is
     * reassigned to alice so dave is no longer a participant by derivation, and
     * only the override is left to carry him. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "UPDATE thread_follows SET state=1 WHERE user_id=%llu AND root_id=5000;"
                 "UPDATE messages SET author_id=%llu WHERE id=5000;",
                 (unsigned long long)dave, (unsigned long long)alice);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 5001, 5000, 100, (uint64_t)100 * 60000ull, t, 8);
    saw_d = 0;
    for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d = 1;
    CHECK(saw_d);

    sqlite3_close(rdb);

    /* --- a PAUSE silences regardless of the window (REQ-278) --------
     * The window is periodic; a pause is one instant. At 01:40 bob is outside
     * his quiet hours and would be pushed — a pause has to reach him anyway,
     * which is the whole reason the two mechanisms both exist. */
    {
        oc_job *sj = oc_job_new(OC_JOB_SET_SNOOZE, 0);
        sj->user_id = bob; sj->snooze_minutes = 60;
        oc_dbwriter_submit(w, sj);
        oc_dbres *sr = wait_result(w);
        CHECK(sr && sr->type == OC_RES_SNOOZE && sr->snooze_until_ms > 0);
        uint64_t until = sr ? sr->snooze_until_ms : 0;
        oc_dbres_free(sr);

        CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        n = oc_push_collect(rdb, 1, alice, 0, 0, 100, until - 1000, t, 8);
        int saw_b2 = 0;
        for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-bob") == 0) saw_b2 = 1;
        CHECK(!saw_b2);                       /* paused: no push, window or not */

        /* One millisecond past the end it is over, with nothing having cleared
         * it — the stamp is enforced on READ, so there is no sweep to wait for. */
        n = oc_push_collect(rdb, 1, alice, 0, 0, 100, until + 1, t, 8);
        saw_b2 = 0;
        for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-bob") == 0) saw_b2 = 1;
        CHECK(saw_b2);
        sqlite3_close(rdb);

        /* Ending early is the same op with 0 minutes, not a second one. */
        sj = oc_job_new(OC_JOB_SET_SNOOZE, 0);
        sj->user_id = bob; sj->snooze_minutes = 0;
        oc_dbwriter_submit(w, sj);
        sr = wait_result(w);
        CHECK(sr && sr->type == OC_RES_SNOOZE && sr->snooze_until_ms == 0);
        oc_dbres_free(sr);
        CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        n = oc_push_collect(rdb, 1, alice, 0, 0, 100, until - 1000, t, 8);
        saw_b2 = 0;
        for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-bob") == 0) saw_b2 = 1;
        CHECK(saw_b2);
        sqlite3_close(rdb);
    }

    /* --- the SCHEDULE evaluates the recipient's LOCAL day (REQ-136) ---
     * The unit under test is the predicate, because the day boundary is where
     * this goes wrong: a UTC weekday puts a large part of the world's Friday
     * evening on Saturday, and no fixture would notice. */
    {
        /* Mode off: never quiet, whatever the hour. */
        CHECK(oc_notify_quiet(OC_DND_OFF, 480, 1320, 0, 0, 0, 0, 30, 3) == 0);
        /* Every day, allowed 08:00-22:00: 09:00 notifies, 23:00 does not. */
        CHECK(oc_notify_quiet(OC_DND_EVERY_DAY, 480, 1320, 0, 0, 0, 0, 540, 3) == 0);
        CHECK(oc_notify_quiet(OC_DND_EVERY_DAY, 480, 1320, 0, 0, 0, 0, 1380, 3) == 1);
        /* Weekdays: the same window Monday to Friday, and silence at the weekend
         * whatever the hour — which is the case a single daily window could not
         * express and REQ-136 exists for. */
        CHECK(oc_notify_quiet(OC_DND_WEEKDAYS, 480, 1320, 0, 0, 0, 0, 540, 1) == 0);
        CHECK(oc_notify_quiet(OC_DND_WEEKDAYS, 480, 1320, 0, 0, 0, 0, 540, 0) == 1);  /* Sunday */
        CHECK(oc_notify_quiet(OC_DND_WEEKDAYS, 480, 1320, 0, 0, 0, 0, 540, 6) == 1);  /* Saturday */
        /* Custom: this day's own window wins. */
        CHECK(oc_notify_quiet(OC_DND_CUSTOM, 480, 1320, 1, 1, 600, 660, 630, 2) == 0);
        CHECK(oc_notify_quiet(OC_DND_CUSTOM, 480, 1320, 1, 1, 600, 660, 700, 2) == 1);
        /* A day switched off is quiet all day, and a day with NO ROW is quiet
         * too: a custom schedule listing Monday to Friday is a statement about
         * the weekend as well, not an omission. */
        CHECK(oc_notify_quiet(OC_DND_CUSTOM, 480, 1320, 1, 0, 600, 660, 630, 2) == 1);
        CHECK(oc_notify_quiet(OC_DND_CUSTOM, 480, 1320, 0, 0, 0, 0, 630, 2) == 1);
        /* A window that wraps midnight (a night shift) still reads as one range. */
        CHECK(oc_notify_quiet(OC_DND_EVERY_DAY, 1320, 480, 0, 0, 0, 0, 60, 3) == 0);
        CHECK(oc_notify_quiet(OC_DND_EVERY_DAY, 1320, 480, 0, 0, 0, 0, 720, 3) == 1);
    }

    /* --- the notify decision is ONE function, and these pin its ORDER ---
     * (REQ-135, REQ-137, ARCH-89.) The push query below exercises the same
     * precedence against real rows; this exercises the shared statement of it
     * that a client's toast gate feeds, where the order is exactly the part
     * that had drifted. Arguments read:
     *   (own, muted, vip, level, mentioned, keyword, thread_reply, quiet,
     *    paused). */
    {
        /* Plain traffic: ALL notifies, NONE does not, MENTIONS needs a reason. */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_ALL, 0, 0, 0, 0, 0) == 1);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_NONE, 0, 0, 0, 0, 0) == 0);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 0, 0, 0, 0, 0) == 0);
        /* At MENTIONS an @-mention and a keyword hit are the same event:
         * REQ-135 makes keywords part of the level, not a separate switch. */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 1, 0, 0, 0, 0) == 1);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 0, 1, 0, 0, 0) == 1);
        /* And so is a reply in a thread you are in (REQ-061): participants are
         * notified per their level, "independent of whether they were
         * @mentioned" — the same shape keywords have, one input further. */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 0, 0, 1, 0, 0) == 1);
        /* But it satisfies the LEVEL rather than bypassing anything: NONE still
         * passes nothing, mute still wins, and the schedule still silences. */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_NONE,     0, 0, 1, 0, 0) == 0);
        CHECK(oc_notify_decide(0, 1, 0, OC_NOTIFY_MENTIONS, 0, 0, 1, 0, 0) == 0);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 0, 0, 1, 1, 0) == 0);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 0, 0, 1, 0, 1) == 0);
        /* Your own reply is still not news. */
        CHECK(oc_notify_decide(1, 0, 0, OC_NOTIFY_MENTIONS, 0, 0, 1, 0, 0) == 0);
        /* NONE means none: a mention does not pierce it. Only a person does. */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_NONE, 1, 1, 0, 0, 0) == 0);
        CHECK(oc_notify_decide(0, 0, 1, OC_NOTIFY_NONE, 0, 0, 0, 0, 0) == 1);
        /* Your own message is not news, whoever you are to yourself. */
        CHECK(oc_notify_decide(1, 0, 1, OC_NOTIFY_ALL, 1, 1, 0, 0, 0) == 0);
        /* Mute is absolute (REQ-137): not even a priority person pierces it. */
        CHECK(oc_notify_decide(0, 1, 1, OC_NOTIFY_ALL, 1, 1, 0, 0, 0) == 0);
        /* A priority person pierces the schedule and the pause alike:
         * both say WHEN, and a priority person is a WHO. */
        CHECK(oc_notify_decide(0, 0, 1, OC_NOTIFY_MENTIONS, 0, 0, 0, 1, 1) == 1);
        /* For everyone else the schedule and the pause each silence
         * everything, a mention included (REQ-136, REQ-278). */
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_ALL, 0, 0, 0, 1, 0) == 0);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_ALL, 0, 0, 0, 0, 1) == 0);
        CHECK(oc_notify_decide(0, 0, 0, OC_NOTIFY_MENTIONS, 1, 0, 0, 0, 1) == 0);
    }

    /* --- a PRIORITY person pierces the level and the pause (REQ-135) --------
     * dave is set to mentions-only below; a message naming nobody would not
     * reach him. Making alice a priority person must, because a level says WHEN
     * and this says WHO — while a MUTE still wins, which is the deliberate limit
     * and the reason carol stays silent throughout.
     */
    set_level(w, dave, 1, OC_NOTIFY_MENTIONS);
    {
        oc_job *pj = oc_job_new(OC_JOB_SET_PRIORITY, 0);
        pj->user_id = dave;
        pj->pri_people = calloc(1, sizeof *pj->pri_people);
        pj->pri_people[0] = alice; pj->n_pri_people = 1;
        oc_dbwriter_submit(w, pj);
        oc_dbres *pr = wait_result(w);
        CHECK(pr && pr->type == OC_RES_ALERT_PREFS && pr->al_n_people == 1);
        oc_dbres_free(pr);
    }
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    { int saw_d3 = 0;
      for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d3 = 1;
      CHECK(saw_d3); }
    sqlite3_close(rdb);
    /* And through a pause, which is the other half of the same claim. */
    {
        oc_job *sj = oc_job_new(OC_JOB_SET_SNOOZE, 0);
        sj->user_id = dave; sj->snooze_minutes = 60;
        oc_dbwriter_submit(w, sj);
        oc_dbres *sr = wait_result(w);
        uint64_t until = sr ? sr->snooze_until_ms : 0;
        oc_dbres_free(sr);
        CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        n = oc_push_collect(rdb, 1, alice, 0, 0, 100, until - 1000, t, 8);
        int saw_d4 = 0;
        for (int i = 0; i < n; i++) if (strcmp(t[i].token, "tok-dave") == 0) saw_d4 = 1;
        CHECK(saw_d4);
        sqlite3_close(rdb);
        sj = oc_job_new(OC_JOB_SET_SNOOZE, 0);
        sj->user_id = dave; sj->snooze_minutes = 0;
        oc_dbwriter_submit(w, sj);
        oc_dbres_free(wait_result(w));
    }
    /* Take the priority back, or the checks below inherit it. */
    {
        oc_job *pj = oc_job_new(OC_JOB_SET_PRIORITY, 0);
        pj->user_id = dave; pj->n_pri_people = 0;
        oc_dbwriter_submit(w, pj);
        oc_dbres_free(wait_result(w));
    }

    set_level(w, dave, 1, OC_NOTIFY_ALL);        /* restore for the checks below */
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_FCM, "tok-carol"));

    /* Prune dave's token; re-registering carol's (synchronous) flushes the FIFO
     * so the fire-and-forget prune has definitely applied. */
    oc_dbwriter_prune_device_token(w, "tok-dave");
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_FCM, "tok-carol"));
    CHECK(token_count(path, "tok-dave") == 0);

    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
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

/* The GLOBAL default (REQ-134) and mute (REQ-137) as PUSH gates.
 *
 * Both were settings the daemon stored and then ignored when it decided whether to
 * ring a phone: the fallback level was hardcoded to ALL, so "only mention me"
 * applied to nothing but the channels you had touched individually, and a muted
 * channel at level ALL still pushed. */
static void test_default_and_mute(void) {
    const char *path = "build/test_push_default.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) return;

    uint64_t alice = oc_dbwriter_register_local(w, "d-alice", "pw", OC_ROLE_OWNER, 2048);
    uint64_t bob   = oc_dbwriter_register_local(w, "d-bob",   "pw", OC_ROLE_MEMBER, 2048);
    uint64_t carol = oc_dbwriter_register_local(w, "d-carol", "pw", OC_ROLE_MEMBER, 2048);
    CHECK(alice && bob && carol);
    CHECK(oc_dbwriter_register_device_token(w, bob,   OC_PUSH_APNS, "tok-b"));
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_APNS, "tok-c"));

    oc_push_target t[8];
    sqlite3 *rdb = NULL;
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8) == 2);   /* the default default is ALL */
    sqlite3_close(rdb);

    /* Bob sets his global default to MENTIONS, having no row for channel 1. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_NOTIFY_DEFAULT, 0);
        j->user_id = bob; j->notify_level = OC_NOTIFY_MENTIONS;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_NOTIFY_PREFS);
        oc_dbres_free(r);
    }
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    int n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    CHECK(n == 1 && strcmp(t[0].token, "tok-c") == 0);          /* bob is out */

    /* ... and a message that names him is in, through the same default. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                 " VALUES(9001,1,%llu,0,0,5,1);", (unsigned long long)bob);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    CHECK(oc_push_collect(rdb, 1, alice, 9001, 0, 100, (uint64_t)100 * 60000ull, t, 8) == 2);
    sqlite3_close(rdb);

    /* An explicit per-channel row still OVERRIDES the default, in both directions. */
    set_level(w, bob, 1, OC_NOTIFY_ALL);
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8) == 2);
    sqlite3_close(rdb);

    /* Mute wins over the level: carol is at ALL and hears nothing. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_MUTE, 0);
        j->user_id = carol; j->channel_id = 1; j->hook_disabled = 1;   /* the mute flag */
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        oc_dbres_free(r);
    }
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    n = oc_push_collect(rdb, 1, alice, 0, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    CHECK(n == 1 && strcmp(t[0].token, "tok-b") == 0);
    /* ... not even for a message that names her: mute means mute. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                 " VALUES(9002,1,%llu,0,0,5,1);", (unsigned long long)carol);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 9002, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    CHECK(n == 1 && strcmp(t[0].token, "tok-b") == 0);
    sqlite3_close(rdb);

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
        oc_push_notify(p, 1, alice, 0, 0);   /* alice sent → bob should be notified */
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

/* --- a keyword hit is one person's, not an audience --------------------------
 *
 * REQ-135 puts keywords inside the MENTIONS level and stores a hit in the
 * `mentions` table with kind 4 (ARCH-103, SCHEMA §3q), so the push gate, the
 * activity feed and the reader's highlight all keep working unchanged. The gate
 * used to read "kind <> 0" as "a broadcast audience", which made one person's
 * private term an @channel: every MENTIONS-level member of the channel was
 * pushed because somebody else's word appeared in the message.
 *
 * Nothing in the tree could see it. The row is real, the level is real, and the
 * query is self-consistent — it just answers a question nobody asked. */
static void test_keyword_is_not_a_broadcast(void) {
    const char *path = "build/test_push_keyword.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) return;

    uint64_t alice = oc_dbwriter_register_local(w, "alice", "pw", OC_ROLE_OWNER,  2048);
    uint64_t bob   = oc_dbwriter_register_local(w, "bob",   "pw", OC_ROLE_MEMBER, 2048);
    uint64_t carol = oc_dbwriter_register_local(w, "carol", "pw", OC_ROLE_MEMBER, 2048);
    CHECK(alice && bob && carol);
    CHECK(oc_dbwriter_register_device_token(w, bob,   OC_PUSH_APNS, "tok-bob"));
    CHECK(oc_dbwriter_register_device_token(w, carol, OC_PUSH_APNS, "tok-carol"));

    /* Both on MENTIONS: neither wants the channel's ordinary traffic. */
    set_level(w, bob,   1, OC_NOTIFY_MENTIONS);
    set_level(w, carol, 1, OC_NOTIFY_MENTIONS);

    /* CAROL's keyword matched this message. Bob's did not, and he is not named. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        char sql[256];
        snprintf(sql, sizeof sql,
                 "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                 " VALUES(7001,1,%llu,4,0,6,1);", (unsigned long long)carol);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }

    sqlite3 *rdb = NULL;
    CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    oc_push_target t[8];
    int n = oc_push_collect(rdb, 1, alice, 7001, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    int saw_bob = 0, saw_carol = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].token, "tok-bob")   == 0) saw_bob = 1;
        if (strcmp(t[i].token, "tok-carol") == 0) saw_carol = 1;
    }
    CHECK(saw_carol);       /* her term, her notification */
    CHECK(!saw_bob);        /* not his, and not the channel's */
    CHECK(n == 1);

    /* The real broadcasts still are broadcasts — the fix names kinds 1/2/3
     * rather than widening to "not 0", so this must not have been narrowed
     * into silence. */
    {
        sqlite3 *wdb = NULL;
        CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);
        CHECK(sqlite3_exec(wdb,
            "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
            " VALUES(7002,1,NULL,2,0,8,1);", NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(wdb);
    }
    n = oc_push_collect(rdb, 1, alice, 7002, 0, 100, (uint64_t)100 * 60000ull, t, 8);
    CHECK(n == 2);          /* @channel reaches both */

    sqlite3_close(rdb);
    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- the daemon and the evaluator, swept against each other ------------------
 *
 * The point of the single evaluator is that ONE function decides, so the way to
 * prove it is not a list of cases somebody thought of — that is how the two copies drifted in
 * the first place, each passing its own tests. This sweeps every combination of
 * the seven inputs the push path can express (mute x level x priority x mention
 * x keyword x schedule x pause: 2*3*2*2*2*2 = 192 states), sets each one up in
 * the database, and requires oc_push_collect's answer to equal
 * oc_notify_decide's for the same inputs.
 *
 * `thread_reply` is the eighth input and is not swept here: it is derived from
 * message rows rather than set as a flag, and test_collect covers it directly.
 * It is passed as 0 to both sides, which is what a non-threaded send means.
 *
 * A BYSTANDER sits in the channel for every one of those states: on MENTIONS,
 * never named, never a priority person, with no term of their own. Nothing in
 * the sweep is about them, which is the point — every state is also a test that
 * one recipient's settings do not become somebody else's notification. Without
 * them the sweep passed against the query that read a keyword row as an
 * audience, because with a single recipient a row addressed to that recipient
 * answers the same either way. That is the shape of the bug this whole change
 * is about, and a sweep that cannot see it is agreement by coincidence. */
static void test_collect_matches_evaluator(void) {
    const char *path = "build/test_push_parity.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    if (!w) return;

    uint64_t alice = oc_dbwriter_register_local(w, "alice", "pw", OC_ROLE_OWNER,  2048);
    uint64_t bob   = oc_dbwriter_register_local(w, "bob",   "pw", OC_ROLE_MEMBER, 2048);
    uint64_t zoe   = oc_dbwriter_register_local(w, "zoe",   "pw", OC_ROLE_MEMBER, 2048);
    CHECK(alice && bob && zoe);
    CHECK(oc_dbwriter_register_device_token(w, bob, OC_PUSH_APNS, "tok-bob"));
    CHECK(oc_dbwriter_register_device_token(w, zoe, OC_PUSH_APNS, "tok-zoe"));
    set_level(w, zoe, 1, OC_NOTIFY_MENTIONS);       /* the bystander, permanently */
    oc_dbwriter_stop(w);        /* the rest is set directly, one state per pass */

    /* tz_offset_min 0 and this instant put the recipient's local clock at
     * 00:40 on a Thursday, which the allow-window below excludes. */
    const uint64_t now_ms = (uint64_t)100 * 60000ull;
    const int allow_start = 600, allow_end = 660;   /* 10:00-11:00 */

    sqlite3 *wdb = NULL;
    CHECK(sqlite3_open(path, &wdb) == SQLITE_OK);

    int idx = 0, mismatches = 0, notified = 0;
    for (int muted = 0; muted < 2; muted++)
    for (unsigned level = 0; level < 3; level++)
    for (int vip = 0; vip < 2; vip++)
    for (int mentioned = 0; mentioned < 2; mentioned++)
    for (int keyword = 0; keyword < 2; keyword++)
    for (int quiet = 0; quiet < 2; quiet++)
    for (int paused = 0; paused < 2; paused++) {
        uint64_t msg = 900000ull + (uint64_t)idx++;
        char sql[1024];
        snprintf(sql, sizeof sql,
            "DELETE FROM notification_prefs WHERE user_id=%llu AND channel_id=1;"
            "INSERT INTO notification_prefs(user_id,channel_id,level,muted) VALUES(%llu,1,%u,%d);"
            "DELETE FROM priority_people WHERE user_id=%llu;"
            "UPDATE users SET dnd_mode=%d, allow_start_min=%d, allow_end_min=%d,"
            "                 dnd_until_ms=%llu, tz_offset_min=0 WHERE id=%llu;",
            (unsigned long long)bob,
            (unsigned long long)bob, level, muted,
            (unsigned long long)bob,
            quiet ? OC_DND_EVERY_DAY : OC_DND_OFF, allow_start, allow_end,
            (unsigned long long)(paused ? now_ms + 60000ull : 0ull),
            (unsigned long long)bob);
        CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        if (vip) {
            snprintf(sql, sizeof sql,
                "INSERT INTO priority_people(user_id,person_id) VALUES(%llu,%llu);",
                (unsigned long long)bob, (unsigned long long)alice);
            CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        }
        if (mentioned) {
            snprintf(sql, sizeof sql,
                "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                " VALUES(%llu,1,%llu,0,0,4,1);",
                (unsigned long long)msg, (unsigned long long)bob);
            CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        }
        if (keyword) {
            snprintf(sql, sizeof sql,
                "INSERT INTO mentions(message_id,channel_id,user_id,kind,span_start,span_len,created_at_ms)"
                " VALUES(%llu,1,%llu,4,0,6,1);",
                (unsigned long long)msg, (unsigned long long)bob);
            CHECK(sqlite3_exec(wdb, sql, NULL, NULL, NULL) == SQLITE_OK);
        }

        sqlite3 *rdb = NULL;
        CHECK(sqlite3_open_v2(path, &rdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        oc_push_target t[8];
        int n = oc_push_collect(rdb, 1, alice, msg, 0, 100, now_ms, t, 8);
        sqlite3_close(rdb);

        /* What the shared rule says, from the same inputs. The schedule is
         * asked of the shared evaluator too, rather than restated as `quiet`,
         * so this pins the daemon's clock arithmetic as well as its order. */
        int want_quiet = oc_notify_quiet(quiet ? OC_DND_EVERY_DAY : OC_DND_OFF,
                                         allow_start, allow_end,
                                         0, 0, 0, 0,
                                         (int)((now_ms / 60000) % 1440),
                                         (int)((((now_ms / 60000) / 1440) + 4) % 7));
        int want = oc_notify_decide(0, muted, vip, level,
                                    mentioned, keyword, 0, want_quiet, paused);
        /* Bob if and only if the rule says so, and the bystander never. */
        int got_bob = 0, got_zoe = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(t[i].token, "tok-bob") == 0) got_bob = 1;
            if (strcmp(t[i].token, "tok-zoe") == 0) got_zoe = 1;
        }
        if (got_bob != want || got_zoe || n != want) {
            mismatches++;
            printf("  parity: muted=%d level=%u vip=%d men=%d kw=%d quiet=%d paused=%d"
                   " -> bob %d (rule says %d), bystander %d, total %d\n",
                   muted, level, vip, mentioned, keyword, quiet, paused,
                   got_bob, want, got_zoe, n);
        }
        notified += want;
    }
    sqlite3_close(wdb);

    CHECK(idx == 192);
    CHECK(mismatches == 0);
    /* Both answers occur across the sweep: a rule that says no to everything
     * would otherwise agree with a collector that returns nothing. */
    CHECK(notified > 0 && notified < idx);
    cleanup_db(path);
}

int run_push_tests(void) {
    printf("test_push: DND window (incl wrap-around), recipient collect "
           "(level/DND/author gating), CP-12 sign+verify, contentless body, "
           "global default + mute as push gates, "
           "a keyword hit that is one person's and not an audience, "
           "collect swept against the shared evaluator over all 192 states, "
           "device-token register/unregister/prune, notify->relay->prune round-trip\n");
    test_dnd();
    test_build_body();
    test_sign_verify();
    test_collect();
    test_default_and_mute();
    test_keyword_is_not_a_broadcast();
    test_collect_matches_evaluator();
    test_notify_roundtrip();
    return failures;
}
