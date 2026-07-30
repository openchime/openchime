/* Storage pressure policy + the maintenance pass (REQ-212..218, ARCH-77/78).
 *
 * Two halves, both hermetic:
 *   - the policy/measurement helpers in daemon/storage.c, driven through the
 *     environment exactly as the daemon loads them;
 *   - the writer-side maintenance job, driven against a real SQLite database so
 *     the three reclamation tiers, the grace window, and the tombstone are
 *     exercised as SQL rather than asserted in prose.
 *
 * What is deliberately NOT covered here: actually deleting blob bytes. That runs
 * on the transfer pool (ARCH-69) as fire-and-forget jobs; this test asserts the
 * pass *selects* the right rows and tombstones them, which is the part where a
 * mistake destroys data. */

#include "storage.h"
#include "dbwriter.h"
#include "migrate.h"
#include "check.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#define DBPATH "build/oc-storage-test.db"

/* --- policy ---------------------------------------------------------------- */

static void test_policy_defaults(void) {
    unsetenv("OPENCHIME_MAINT_INTERVAL_MS");
    unsetenv("OPENCHIME_ATTACH_MAX_AGE_DAYS");
    unsetenv("OPENCHIME_EVICT_GRACE_HOURS");
    unsetenv("OPENCHIME_DB_RESERVE_MB");
    unsetenv("OPENCHIME_PRESSURE_MB");
    unsetenv("OPENCHIME_RECOVER_MB");
    unsetenv("OPENCHIME_MAINT_BATCH");
    unsetenv("OPENCHIME_EVICT");

    oc_storage_policy p;
    oc_storage_policy_load(&p);
    CHECK(p.interval_ms == 5 * 60 * 1000);
    CHECK(p.max_age_ms == 0);                 /* keep attachments forever by default */
    CHECK(p.evict_enabled == 1);              /* default-on, by decision */
    CHECK(p.reserve_bytes > 0);
    /* The watermarks must be ordered or the pass would oscillate or never
     * protect the database. */
    CHECK(p.recover_bytes >= p.pressure_bytes);
    CHECK(p.pressure_bytes >= p.reserve_bytes);
}

static void test_policy_env(void) {
    setenv("OPENCHIME_ATTACH_MAX_AGE_DAYS", "30", 1);
    setenv("OPENCHIME_EVICT", "off", 1);
    setenv("OPENCHIME_MAINT_INTERVAL_MS", "1000", 1);
    setenv("OPENCHIME_DB_RESERVE_MB", "10", 1);
    setenv("OPENCHIME_PRESSURE_MB", "20", 1);
    setenv("OPENCHIME_RECOVER_MB", "40", 1);

    oc_storage_policy p;
    oc_storage_policy_load(&p);
    CHECK(p.max_age_ms == 30ull * 24 * 60 * 60 * 1000);
    CHECK(p.evict_enabled == 0);
    CHECK(p.interval_ms == 1000);
    CHECK(p.reserve_bytes == 10ull * 1024 * 1024);

    /* A nonsensical ordering is repaired rather than obeyed. */
    setenv("OPENCHIME_DB_RESERVE_MB", "100", 1);
    setenv("OPENCHIME_PRESSURE_MB", "10", 1);
    setenv("OPENCHIME_RECOVER_MB", "5", 1);
    oc_storage_policy_load(&p);
    CHECK(p.pressure_bytes >= p.reserve_bytes);
    CHECK(p.recover_bytes >= p.pressure_bytes);

    /* An interval of zero would busy-loop the maintenance tick. */
    setenv("OPENCHIME_MAINT_INTERVAL_MS", "0", 1);
    oc_storage_policy_load(&p);
    CHECK(p.interval_ms >= 1000);

    unsetenv("OPENCHIME_ATTACH_MAX_AGE_DAYS");
    unsetenv("OPENCHIME_EVICT");
    unsetenv("OPENCHIME_MAINT_INTERVAL_MS");
    unsetenv("OPENCHIME_DB_RESERVE_MB");
    unsetenv("OPENCHIME_PRESSURE_MB");
    unsetenv("OPENCHIME_RECOVER_MB");
}

static void test_thresholds(void) {
    oc_storage_policy p;
    memset(&p, 0, sizeof p);
    p.reserve_bytes  = 100;
    p.pressure_bytes = 200;

    oc_storage_stats s;
    memset(&s, 0, sizeof s);
    s.valid = 1;

    s.avail_bytes = 500;
    CHECK(!oc_storage_under_pressure(&s, &p));
    CHECK(!oc_storage_must_refuse(&s, &p));

    s.avail_bytes = 150;                       /* under pressure, above reserve */
    CHECK(oc_storage_under_pressure(&s, &p));
    CHECK(!oc_storage_must_refuse(&s, &p));

    s.avail_bytes = 50;                        /* into the database's reserve */
    CHECK(oc_storage_under_pressure(&s, &p));
    CHECK(oc_storage_must_refuse(&s, &p));

    /* An unmeasurable filesystem must read as unconstrained: evicting user data
     * because statvfs failed would be far worse than doing nothing. */
    s.valid = 0;
    s.avail_bytes = 0;
    CHECK(!oc_storage_under_pressure(&s, &p));
    CHECK(!oc_storage_must_refuse(&s, &p));
}

static void test_sample(void) {
    oc_storage_stats s;
    oc_storage_sample("build", 12345, &s);
    CHECK(s.valid == 1);                       /* build/ exists */
    CHECK(s.total_bytes > 0);
    CHECK(s.sampled_ms == 12345);

    oc_storage_sample("/no/such/path/at/all", 999, &s);
    CHECK(s.valid == 0);                       /* reported, never fatal */
}

/* --- the maintenance pass -------------------------------------------------- */

/* Insert an attachment row directly. `age_ms` is how long ago it was created;
 * `linked` controls whether a message references it (an unlinked row is the
 * orphan an abandoned upload leaves behind). */
static void insert_attachment(sqlite3 *db, uint64_t id, uint64_t age_ms,
                              int linked, uint64_t now) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO attachments(id, channel_id, message_id, uploader_id, storage_key,"
        " filename, mime, size, sha256, created_at_ms) "
        "VALUES(?1, 1, ?2, 1, ?3, 'f.bin', 'application/octet-stream', 10, x'00', ?4);",
        -1, &st, NULL);
    char key[32];
    snprintf(key, sizeof key, "%08llx", (unsigned long long)id);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    if (linked) sqlite3_bind_int64(st, 2, 1); else sqlite3_bind_null(st, 2);
    sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)(now - age_ms));
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

static int reclaimed(sqlite3 *db, uint64_t id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT reclaimed_at_ms FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    int v = (sqlite3_step(st) == SQLITE_ROW) ? (sqlite3_column_int64(st, 0) != 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* Does the row still exist at all? Tombstoning must never delete it — the
 * message has to stay readable (REQ-053/215). */
static int row_exists(sqlite3 *db, uint64_t id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    int v = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return v;
}

static const uint64_t HOUR = 60ull * 60 * 1000;
static const uint64_t DAY  = 24ull * 60 * 60 * 1000;


/* --- audit log (REQ-251) -------------------------------------------------- */

static int audit_count(sqlite3 *db, int family) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM audit_log WHERE family=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, family);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void audit_insert(sqlite3 *db, int family, const char *action, uint64_t at_ms) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO audit_log(at_ms, family, action, outcome) VALUES(?1,?2,?3,1);",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)at_ms);
    sqlite3_bind_int(st, 2, family);
    sqlite3_bind_text(st, 3, action, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

/* The property REQ-251b exists for: a flood of attacker-controlled SECURITY
 * entries must not age out ADMIN history. With one global cap it would; with a
 * per-family cap it cannot. This is the test that would catch a regression
 * turning the audit log back into an evidence-shredder. */
static void test_audit_partitioned_cap(oc_dbwriter *w, sqlite3 *db, uint64_t now) {
    const uint64_t DAYms = 24ull * 60 * 60 * 1000;

    /* One old admin entry we must keep, and a flood of even older security ones. */
    audit_insert(db, OC_AUDIT_ADMIN, "role.change", now - 10 * DAYms);
    for (int i = 0; i < 200; i++)
        audit_insert(db, OC_AUDIT_SECURITY, "auth.failed", now - 40 * DAYms - i);

    CHECK(audit_count(db, OC_AUDIT_ADMIN) == 1);
    CHECK(audit_count(db, OC_AUDIT_SECURITY) == 200);

    /* Prune at 30 days: the security flood is older and goes; the 10-day-old
     * admin entry is younger than the cutoff and survives. */
    oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
    CHECK(j != NULL);
    if (!j) return;
    j->maint_grace_ms = 1 * HOUR;
    j->maint_batch = 8;
    j->audit_max_age_ms = 30 * DAYms;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = NULL;
    for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
    CHECK(r != NULL);
    if (r) oc_dbres_free(r);

    CHECK(audit_count(db, OC_AUDIT_SECURITY) == 0);   /* aged out */
    CHECK(audit_count(db, OC_AUDIT_ADMIN) == 1);      /* SURVIVED the flood */
}

/* Reading a page: newest first, gated to owner/admin. */
static void test_audit_query(oc_dbwriter *w, sqlite3 *db, uint64_t now, uint64_t member_id) {
    audit_insert(db, OC_AUDIT_ADMIN, "webhook.create", now - 3000);
    audit_insert(db, OC_AUDIT_ADMIN, "user.invite",    now - 2000);
    audit_insert(db, OC_AUDIT_ACCOUNT, "password.change", now - 1000);

    oc_job *j = oc_job_new(OC_JOB_AUDIT_QUERY, 0);
    CHECK(j != NULL);
    if (!j) return;
    j->user_id = 1;                 /* owner */
    j->audit_limit = 10;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = NULL;
    for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
    CHECK(r != NULL);
    if (r) {
        CHECK(r->type == OC_RES_AUDIT_PAGE);
        CHECK(r->n_audit >= 3);
        /* Newest first. */
        if (r->n_audit >= 2) CHECK(r->audit[0].at_ms >= r->audit[1].at_ms);
        int saw_pw = 0;
        for (size_t i = 0; i < r->n_audit; i++)
            if (r->audit[i].action && strcmp(r->audit[i].action, "password.change") == 0) saw_pw = 1;
        CHECK(saw_pw);
        oc_dbres_free(r);
    }

    /* A member is refused, and gets no entries with the refusal. */
    j = oc_job_new(OC_JOB_AUDIT_QUERY, 0);
    CHECK(j != NULL);
    if (!j) return;
    j->user_id = member_id;
    j->audit_limit = 10;
    oc_dbwriter_submit(w, j);
    r = NULL;
    for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
    CHECK(r != NULL);
    if (r) {
        CHECK(r->type == OC_RES_AUDIT_ERR);
        CHECK(r->err_code == OC_ERR_FORBIDDEN);
        CHECK(r->n_audit == 0);
        oc_dbres_free(r);
    }
}

int run_storage_tests(void) {
    printf("test_storage: policy defaults + env override, watermark ordering, "
           "unmeasurable-fs safety, statvfs, and the maintenance pass "
           "(orphans, age expiry, pressure eviction, grace window, tombstones), "
           "plus the usage report and its owner/admin gate, and the audit log "
           "(per-family cap defeating the flood attack, paging, admin gate)\n");

    test_policy_defaults();
    test_policy_env();
    test_thresholds();
    test_sample();

    /* --- the pass, against a real database --------------------------------- */
    unlink(DBPATH); unlink(DBPATH "-wal"); unlink(DBPATH "-shm");
    oc_dbwriter *w = oc_dbwriter_start(DBPATH);
    CHECK(w != NULL);
    if (!w) return failures;

    /* A user and a message for the linked rows to reference. */
    CHECK(oc_dbwriter_register_local(w, "sa", "pw-sa", OC_ROLE_OWNER, 1000) != 0);

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(DBPATH, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "INSERT OR IGNORE INTO messages(id, channel_id, author_id, body, created_at_ms) "
                     "VALUES(1, 1, 1, 'm', 1);", NULL, NULL, NULL);

    /* Real wall-clock ms: the writer stamps reclamations with its own clock, so
     * ages have to be expressed against the same one. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = (uint64_t)tv.tv_sec * 1000 + (uint64_t)(tv.tv_usec / 1000);

    /*  id  age      linked   what it should be
     *  10  3 days   no       orphan            -> reclaimed (tier 1)
     *  11  1 min    no       orphan, but fresh -> KEPT (inside the grace window,
     *                                             an upload may be in flight)
     *  12  90 days  yes      old, linked       -> reclaimed by age (tier 2a)
     *  13  2 days   yes      recent, linked    -> KEPT unless evicting
     */
    insert_attachment(db, 10, 3 * DAY,  0, now);
    insert_attachment(db, 11, 60000,    0, now);
    insert_attachment(db, 12, 90 * DAY, 1, now);
    insert_attachment(db, 13, 2 * DAY,  1, now);

    /* Pass 1: no age policy, no pressure. Only the orphan sweep should fire —
     * and it must leave the FRESH orphan alone. */
    {
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 24 * HOUR;
        j->maint_batch = 64;
        j->maint_max_age_ms = 0;
        j->maint_evict = 0;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) {
            CHECK(r->type == OC_RES_STORAGE_MAINT);
            CHECK(r->maint_orphans == 1);      /* id 10 only */
            CHECK(r->maint_expired == 0);
            CHECK(r->maint_evicted == 0);
            CHECK(r->n_reclaim == 1);
            oc_dbres_free(r);
        }
        CHECK(reclaimed(db, 10) == 1);
        CHECK(reclaimed(db, 11) == 0);         /* grace window protected it */
        CHECK(reclaimed(db, 12) == 0);
        CHECK(reclaimed(db, 13) == 0);
        /* Tombstone, not deletion. */
        CHECK(row_exists(db, 10) == 1);
    }

    /* Pass 2: a 30-day maximum age. The old linked attachment expires; the
     * two-day-old one does not. */
    {
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 24 * HOUR;
        j->maint_batch = 64;
        j->maint_max_age_ms = 30 * DAY;
        j->maint_evict = 0;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) {
            CHECK(r->maint_expired == 1);      /* id 12 */
            CHECK(r->maint_evicted == 0);
            oc_dbres_free(r);
        }
        CHECK(reclaimed(db, 12) == 1);
        CHECK(reclaimed(db, 13) == 0);
        CHECK(row_exists(db, 12) == 1);
    }

    /* Pass 3: pressure eviction, no age policy. The last live attachment goes
     * even though nothing is old — that is the durability-for-uptime trade. */
    {
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 1 * HOUR;          /* id 13 is 2 days old, so eligible */
        j->maint_batch = 64;
        j->maint_max_age_ms = 0;
        j->maint_evict = 1;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) {
            CHECK(r->maint_evicted == 1);      /* id 13 */
            oc_dbres_free(r);
        }
        CHECK(reclaimed(db, 13) == 1);
        CHECK(row_exists(db, 13) == 1);
        /* id 11 is still inside the grace window and must have survived every
         * pass — the guarantee that a file shared into a live conversation
         * cannot vanish mid-discussion. */
        CHECK(reclaimed(db, 11) == 0);
    }

    /* An AVATAR is an attachment no message references (WIN-47), so it looks exactly
     * like an orphan to tier 1 and like any other old file to tiers 2a and 2b. It
     * must survive all three, or every profile picture in the workspace goes blank an
     * hour after being set. */
    {
        insert_attachment(db, 200, 10 * DAY, 0, now);      /* orphan-shaped, and old */
        sqlite3_exec(db, "UPDATE users SET avatar_attachment_id=200 WHERE id=1;",
                     NULL, NULL, NULL);
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 1 * HOUR;
        j->maint_batch = 64;
        j->maint_max_age_ms = 1 * DAY;      /* tier 2a would take it on age alone */
        j->maint_evict = 1;                 /* and tier 2b on pressure */
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) oc_dbres_free(r);
        CHECK(reclaimed(db, 200) == 0);
        /* And once it is no longer anyone's avatar it becomes collectable again —
         * the exclusion is about being IN USE, not about being special forever. */
        sqlite3_exec(db, "UPDATE users SET avatar_attachment_id=NULL WHERE id=1;",
                     NULL, NULL, NULL);
        j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        j->maint_grace_ms = 1 * HOUR;
        j->maint_batch = 64;
        j->maint_max_age_ms = 0;
        j->maint_evict = 0;
        oc_dbwriter_submit(w, j);
        r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) oc_dbres_free(r);
        CHECK(reclaimed(db, 200) == 1);

        /* A CUSTOM EMOJI's image is in use for the same non-obvious reason: no
         * message references it (REQ-072). */
        insert_attachment(db, 201, 10 * DAY, 0, now);
        sqlite3_exec(db, "INSERT INTO custom_emoji(name,attachment_id,created_by,created_at_ms) "
                         "VALUES('shipit',201,1,1);", NULL, NULL, NULL);
        oc_job *j2 = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        j2->maint_grace_ms = 1 * HOUR;
        j2->maint_batch = 64;
        j2->maint_max_age_ms = 1 * DAY;
        j2->maint_evict = 1;
        oc_dbwriter_submit(w, j2);
        r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) oc_dbres_free(r);
        CHECK(reclaimed(db, 201) == 0);
    }

    /* Pass 4: idempotence. Everything reclaimable is already tombstoned, so a
     * further pass must find nothing rather than re-reclaiming (which would
     * re-queue blob deletes forever). */
    {
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 1 * HOUR;
        j->maint_batch = 64;
        j->maint_max_age_ms = 30 * DAY;
        j->maint_evict = 1;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) { CHECK(r->n_reclaim == 0); oc_dbres_free(r); }
    }

    /* The batch cap bounds one pass, so a badly over-limit box recovers across
     * several rather than stalling the daemon in one sweep (REQ-212/218). */
    {
        for (uint64_t id = 100; id < 120; id++) insert_attachment(db, id, 5 * DAY, 0, now);
        oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
        CHECK(j != NULL);
        j->maint_grace_ms = 1 * HOUR;
        j->maint_batch = 5;                    /* far fewer than the 20 eligible */
        j->maint_evict = 0;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) { CHECK(r->n_reclaim == 5); oc_dbres_free(r); }
    }


    /* --- the report, and its authorization gate (REQ-214) ------------------ */
    /* The gate matters: free space and what eviction has taken are operational
     * details, and the check reads the user's CURRENT role rather than trusting
     * the connection, so a demotion takes effect at once. */
    {
        uint64_t member = oc_dbwriter_register_local(w, "plain", "pw-plain", OC_ROLE_MEMBER, 1000);
        CHECK(member != 0);

        /* An owner gets the report. */
        oc_job *j = oc_job_new(OC_JOB_STORAGE_STATUS, 0);
        CHECK(j != NULL);
        j->user_id = 1;                       /* "sa", registered as owner above */
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) {
            CHECK(r->type == OC_RES_STORAGE_STATUS);
            /* Reclamation counts come from reclaim_reason, which is what makes
             * eviction auditable after the fact. Earlier passes reclaimed one
             * of each kind. */
            /* 7 orphans: one from the first pass, the 5 the batch-cap step
             * reclaimed, and the ex-avatar collected once it stopped being one.
             * One expired and one evicted from their passes. */
            CHECK(r->st_rec_orphan == 7);
            CHECK(r->st_rec_expired == 1);
            CHECK(r->st_rec_evicted == 1);
            CHECK(r->st_last_reclaim_ms > 0);
            oc_dbres_free(r);
        }

        /* A plain member is refused. */
        j = oc_job_new(OC_JOB_STORAGE_STATUS, 0);
        CHECK(j != NULL);
        j->user_id = member;
        oc_dbwriter_submit(w, j);
        r = NULL;
        for (int i = 0; i < 200 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) {
            CHECK(r->type == OC_RES_STORAGE_ERR);
            CHECK(r->err_code == OC_ERR_FORBIDDEN);
            /* And it must not leak the numbers alongside the refusal. */
            CHECK(r->st_attach_bytes == 0 && r->st_rec_evicted == 0);
            oc_dbres_free(r);
        }
    }

    test_audit_partitioned_cap(w, db, now);
    {
        uint64_t mid = oc_dbwriter_register_local(w, "plain2", "pw-p2", OC_ROLE_MEMBER, 1000);
        CHECK(mid != 0);
        test_audit_query(w, db, now, mid);
    }

    sqlite3_close(db);
    oc_dbwriter_stop(w);
    unlink(DBPATH); unlink(DBPATH "-wal"); unlink(DBPATH "-shm");
    return failures;
}
