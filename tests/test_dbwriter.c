/* Tests for the DB-writer thread (dbwriter.c): migrate-on-boot lifecycle, and
 * the AUTH / SEND job processing (idempotency, membership, broadcast fan-out
 * list) exercised directly through the job queue — no network involved.
 * Includes the code under test directly; links sqlite + pthread. */

#include "dbwriter.h"
#include "migrate.h"
#include "protocol.h"
#include "check.h"

#include <string.h>
#include <unistd.h>

static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* Block until a result is available (bounded), draining the eventfd. */
static oc_dbres *wait_result(oc_dbwriter *w) {
    for (int i = 0; i < 500; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) return r;
        usleep(2000);
    }
    return NULL;
}

static void cleanup_db(const char *path) {
    unlink(path);
    char wal[256], shm[256];
    snprintf(wal, sizeof wal, "%s-wal", path);
    snprintf(shm, sizeof shm, "%s-shm", path);
    unlink(wal); unlink(shm);
}

static void test_start_migrates_and_stops(void) {
    const char *path = "build/test_dbwriter1.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    oc_dbwriter_stop(w);

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(oc_schema_version(db) == 2);
    CHECK(table_exists(db, "messages"));
    CHECK(table_exists(db, "sessions"));
    sqlite3_close(db);
    cleanup_db(path);
}

static uint64_t do_auth(oc_dbwriter *w, uint64_t conn_id, const char *token) {
    oc_job *j = oc_job_new(OC_JOB_AUTH, conn_id);
    oc_job_set_token(j, token, strlen(token));
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t uid = 0;
    if (r && r->type == OC_RES_AUTH_OK && r->conn_id == conn_id) uid = r->user_id;
    oc_dbres_free(r);
    return uid;
}

static void test_auth_and_send(void) {
    const char *path = "build/test_dbwriter2.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    /* Two users authenticate; each gets a distinct id and joins the channel. */
    uint64_t a = do_auth(w, 10, "user-a");
    uint64_t b = do_auth(w, 11, "user-b");
    CHECK(a != 0 && b != 0 && a != b);
    /* Re-auth of the same subject is idempotent (same id). */
    CHECK(do_auth(w, 12, "user-a") == a);

    /* user-a sends to the default channel: gets an id and both members back. */
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0x11, sizeof idem);
    oc_job *j = oc_job_new(OC_JOB_SEND, 10);
    j->user_id = a; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, sizeof idem);
    oc_job_set_body(j, "hello all", 9);
    oc_dbwriter_submit(w, j);

    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK);
    CHECK(r->message_id > 0);
    CHECK(r->duplicate == 0);
    CHECK(r->body_len == 9 && memcmp(r->body, "hello all", 9) == 0);
    int saw_a = 0, saw_b = 0;
    for (size_t i = 0; i < r->n_members; i++) {
        if (r->members[i] == a) saw_a = 1;
        if (r->members[i] == b) saw_b = 1;
    }
    CHECK(saw_a && saw_b);
    uint64_t first_id = r->message_id;
    oc_dbres_free(r);

    /* Same idempotency token -> replay of the original id, no new broadcast. */
    j = oc_job_new(OC_JOB_SEND, 10);
    j->user_id = a; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, sizeof idem);
    oc_job_set_body(j, "hello all", 9);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->duplicate == 1);
    CHECK(r->message_id == first_id);
    oc_dbres_free(r);

    /* A different token allocates a strictly greater id (monotonic, ARCH-43). */
    j = oc_job_new(OC_JOB_SEND, 11);
    j->user_id = b; j->channel_id = OC_DEFAULT_CHANNEL;
    memset(j->idem, 0x22, OC_IDEM_LEN);
    oc_job_set_body(j, "hi", 2);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->message_id > first_id);
    oc_dbres_free(r);

    /* A non-member author is rejected. */
    j = oc_job_new(OC_JOB_SEND, 99);
    j->user_id = 9999; j->channel_id = OC_DEFAULT_CHANNEL;
    memset(j->idem, 0x33, OC_IDEM_LEN);
    oc_job_set_body(j, "x", 1);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static uint64_t send_msg(oc_dbwriter *w, uint64_t uid, const uint8_t idem[OC_IDEM_LEN], const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SEND, 1);
    j->user_id = uid; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t id = (r && r->type == OC_RES_SEND_OK) ? r->message_id : 0;
    oc_dbres_free(r);
    return id;
}

static oc_dbres *backfill(oc_dbwriter *w, uint64_t uid, uint64_t channel, uint64_t after) {
    oc_job *j = oc_job_new(OC_JOB_BACKFILL, 1);
    j->user_id = uid;
    j->cursors = malloc(sizeof(oc_bf_cursor));
    j->cursors[0].channel_id = channel;
    j->cursors[0].after_message_id = after;
    j->n_cursors = 1;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_backfill(void) {
    const char *path = "build/test_dbwriter3.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t u = do_auth(w, 1, "bf-user");
    CHECK(u != 0);
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 1, sizeof idem); uint64_t m1 = send_msg(w, u, idem, "one");
    memset(idem, 2, sizeof idem); uint64_t m2 = send_msg(w, u, idem, "two");
    memset(idem, 3, sizeof idem); uint64_t m3 = send_msg(w, u, idem, "three");
    CHECK(m1 && m2 > m1 && m3 > m2);

    /* From m1: replay m2, m3 in ascending order; high-water is m3. */
    oc_dbres *r = backfill(w, u, OC_DEFAULT_CHANNEL, m1);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 2);
    CHECK(r->replay[0].message_id == m2 && r->replay[1].message_id == m3);
    CHECK(r->replay[0].body_len == 3 && memcmp(r->replay[0].body, "two", 3) == 0);
    CHECK(r->high_water == m3);
    oc_dbres_free(r);

    /* From 0: all three. */
    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_replay == 3 && r->replay[0].message_id == m1);
    oc_dbres_free(r);

    /* A channel the user isn't a member of replays nothing. */
    r = backfill(w, u, 999, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

int run_dbwriter_tests(void) {
    printf("test_dbwriter: migrate-on-boot, AUTH upsert, SEND persist/idempotency/members, backfill\n");
    test_start_migrates_and_stops();
    test_auth_and_send();
    test_backfill();
    return failures;
}
