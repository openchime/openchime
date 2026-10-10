/* Unit tests for the schema migrations runner (ARCH-27, docs/TESTING.md §2.2).
 * Includes migrate.c directly per the openblocks convention; links libsqlite3.
 * All tests run against in-memory databases — no files touched. */

#include "migrate.h"
#include "check.h"
#include "action.h"

#include <string.h>

static sqlite3 *open_mem(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        printf("  FAIL could not open :memory: db\n");
        failures++;
    }
    return db;
}

static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    int v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* A controlled two-step set for exercising the runner itself. */
static const oc_migration TWO[] = {
    { 1, "CREATE TABLE t1 (x);" },
    { 2, "CREATE TABLE t2 (y);" },
};

static void test_fresh_apply(void) {
    sqlite3 *db = open_mem();
    CHECK(oc_schema_version(db) == 0);              /* no schema_version table yet */

    char *err = NULL;
    CHECK(oc_migrate(db, TWO, 2, &err) == SQLITE_OK);
    CHECK(err == NULL);
    CHECK(oc_schema_version(db) == 2);
    CHECK(table_exists(db, "t1"));
    CHECK(table_exists(db, "t2"));
    sqlite3_close(db);
}

static void test_idempotent_rerun(void) {
    sqlite3 *db = open_mem();
    char *err = NULL;
    CHECK(oc_migrate(db, TWO, 2, &err) == SQLITE_OK);
    /* Re-running applies nothing and does not error on the existing tables. */
    CHECK(oc_migrate(db, TWO, 2, &err) == SQLITE_OK);
    CHECK(err == NULL);
    CHECK(oc_schema_version(db) == 2);
    CHECK(scalar(db, "SELECT COUNT(*) FROM schema_version;") == 2);
    sqlite3_close(db);
}

static void test_resume_partial(void) {
    sqlite3 *db = open_mem();
    char *err = NULL;
    /* Apply only step 1... */
    CHECK(oc_migrate(db, TWO, 1, &err) == SQLITE_OK);
    CHECK(oc_schema_version(db) == 1);
    CHECK(table_exists(db, "t1") && !table_exists(db, "t2"));
    /* ...then the full set applies only the remaining step 2. */
    CHECK(oc_migrate(db, TWO, 2, &err) == SQLITE_OK);
    CHECK(oc_schema_version(db) == 2);
    CHECK(table_exists(db, "t2"));
    sqlite3_close(db);
}

static void test_failure_rolls_back(void) {
    sqlite3 *db = open_mem();
    char *err = NULL;
    const oc_migration bad[] = {
        { 1, "CREATE TABLE ok (x);" },
        { 2, "CREATE TABLE ok (x);" },   /* fails: table already exists */
    };
    int rc = oc_migrate(db, bad, 2, &err);
    CHECK(rc != SQLITE_OK);
    CHECK(err != NULL);                  /* a message was produced */
    sqlite3_free(err);
    /* Step 1 committed; the failed step 2 left the version at 1. */
    CHECK(oc_schema_version(db) == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM schema_version;") == 1);
    sqlite3_close(db);
}

static void test_embedded_schema(void) {
    sqlite3 *db = open_mem();
    char *err = NULL;
    CHECK(oc_migrate_default(db, &err) == SQLITE_OK);
    CHECK(err == NULL);
    CHECK(oc_schema_version(db) == 60);   /* + reactions/threads/FTS/cursors/identity/attachments/webhooks/notify/client_settings/enrollment/mute/drafts/scheduled/snooze/schedule/keywords/threads/upload-idempotency/forwards/video-media/read-aloud/call-events/identities/invite-address/channel-description/credential-version/user-groups/tls-certificate/local-issuer/alerts/delete-holds/second-step/credential-resets/passkeys/actions/summaries/summary-queue/summary-notices/mentions-indexed */

    const char *tables[] = { "drafts", "scheduled_messages", "users", "channels", "channel_members",
                             "messages", "sent_messages",
                             "sessions", "local_credentials", "invites", "reactions",
                             "messages_fts", "delivery_cursors", "server_identity",
                             "attachments", "webhooks", "notification_prefs",
                             "client_settings", "audit_log", "rendered_audio",
                             "user_identities", "actions" };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        CHECK(table_exists(db, tables[i]));
    }

    /* 0002 added users.role, defaulting to 'member' (ARCH-60); 0003 added
     * users.disabled, defaulting to 0 (REQ-033). */
    CHECK(sqlite3_exec(db, "INSERT INTO users(id,subject,created_at_ms) VALUES(9,'s9',0);",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar(db, "SELECT COUNT(*) FROM users WHERE id=9 AND role='member' AND disabled=0;") == 1);

    /* message ids are strictly increasing (ARCH-43): seed a user + channel,
     * insert two messages, check the second id exceeds the first. */
    CHECK(sqlite3_exec(db,
        "INSERT INTO users(id,subject,created_at_ms) VALUES(1,'iss|sub',0);"
        "INSERT INTO channels(id,kind,created_at_ms) VALUES(1,'channel',0);"
        "INSERT INTO messages(channel_id,author_id,body,created_at_ms) VALUES(1,1,'a',1);"
        "INSERT INTO messages(channel_id,author_id,body,created_at_ms) VALUES(1,1,'b',2);",
        NULL, NULL, &err) == SQLITE_OK);
    CHECK(scalar(db, "SELECT MAX(id) > MIN(id) FROM messages;") == 1);

    /* 0042: a rendering is cached per (handle, model version), so the same text
     * under a new model is another row rather than an overwrite, and the same
     * pair twice is refused (REQ-293, ARCH-111). */
    CHECK(sqlite3_exec(db,
        "INSERT INTO rendered_audio(handle,model_version,blob_key,bytes,duration_ms,created_at_ms,last_used_ms)"
        " VALUES(x'0102',    'kitten-1', 'k1', 10, 100, 1, 1),"
        "       (x'0102',    'kitten-2', 'k2', 10, 100, 1, 1),"
        "       (x'0203',    'kitten-1', 'k3', 10, 100, 1, 1);",
        NULL, NULL, &err) == SQLITE_OK);
    CHECK(scalar(db, "SELECT COUNT(*) FROM rendered_audio;") == 3);
    CHECK(sqlite3_exec(db,
        "INSERT INTO rendered_audio(handle,model_version,blob_key,bytes,duration_ms,created_at_ms,last_used_ms)"
        " VALUES(x'0102','kitten-1','again',10,100,1,1);",
        NULL, NULL, NULL) != SQLITE_OK);
    /* 0042 also gave users a voice, absent until the daemon picks one. */
    CHECK(scalar(db, "SELECT COUNT(*) FROM users WHERE id=9 AND voice_id IS NULL;") == 1);

    /* the kind CHECK constraint rejects an invalid channel kind */
    CHECK(sqlite3_exec(db,
        "INSERT INTO channels(id,kind,created_at_ms) VALUES(2,'bogus',0);",
        NULL, NULL, NULL) != SQLITE_OK);
    sqlite3_close(db);
}

/* 0044 files existing OIDC accounts as (issuer, subject) rows, and leaves alone
 * what is not one: a local account, and a string with no second bar. */
static void test_identity_backfill(void) {
    sqlite3 *db = open_mem();
    char *err = NULL;
    CHECK(oc_migrate(db, OC_MIGRATIONS, 43, &err) == SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO users(id,subject,email,created_at_ms) VALUES"
        "(1,'oidc:https://auth.openchime.io|https://accounts.google.com|1234','a@x.example',7),"
        "(2,'oidc:https://auth.openchime.io|https://login.microsoftonline.com/t/v2.0|ab|cd',NULL,8),"
        "(3,'local:dana',NULL,9),"
        "(4,'oidc:https://auth.openchime.io|nobar',NULL,10),"
        "(5,'oidc:https://auth.openchime.io|trailing|',NULL,11);", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(oc_migrate(db, OC_MIGRATIONS, OC_MIGRATIONS_COUNT, &err) == SQLITE_OK);
    CHECK(scalar(db, "SELECT COUNT(*) FROM user_identities;") == 2);
    CHECK(scalar(db, "SELECT COUNT(*) FROM user_identities WHERE user_id=1 AND "
                     "issuer='https://accounts.google.com' AND subject='1234' AND "
                     "email='a@x.example' AND email_verified=0 AND first_seen_ms=7;") == 1);
    /* The issuer ends at the FIRST bar after central's: a subject may hold one. */
    CHECK(scalar(db, "SELECT COUNT(*) FROM user_identities WHERE user_id=2 AND "
                     "issuer='https://login.microsoftonline.com/t/v2.0' AND subject='ab|cd';") == 1);
    /* One person, one row. */
    CHECK(sqlite3_exec(db, "INSERT INTO user_identities(user_id,issuer,subject,first_seen_ms,"
                           "last_login_ms) VALUES(3,'https://accounts.google.com','1234',0,0);",
                       NULL, NULL, NULL) != SQLITE_OK);
    sqlite3_close(db);
}

/* 0056 states shared/action.c's rule in SQL to backfill existing messages; the
 * two must give the same answer for every body, so each fixture is asked of
 * both. A call event and a tombstone are never actions; a webhook's label is
 * kept; a forward's excerpt is asked the same question. */
static void test_action_backfill(void) {
    static const char *const bodies[] = {
        "/me is away", "/me    waves", "/me waves  ", "/me waves\nand leaves", "/me /me",
        "/me \xC3\xA9tudie", "\xC3\xA9 /me x", "/me", "/me ", "/me    ", "/me \nwaves", "/me \twaves",
        "/me \r\nwaves", "/me\twaves", "/mewaves", "/mex waves", " /me waves", "/ME waves",
        "me waves", "", "plain text",
    };
    const int n = (int)(sizeof bodies / sizeof bodies[0]);
    sqlite3 *db = open_mem();
    char *err = NULL;
    CHECK(oc_migrate(db, OC_MIGRATIONS, 55, &err) == SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO users(id,subject,created_at_ms) VALUES(1,'local:ada',1);"
        "INSERT INTO channels(id,kind,name,is_public,created_at_ms) VALUES(1,'channel','general',1,1);",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    /* Each body twice: as a blob, the way the send path stores it, and as text,
     * the way a restore does. A blob never equals a text literal, so a rule
     * that forgot the first would pass on the second alone. */
    sqlite3_prepare_v2(db, "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms) VALUES(?,1,1,?,?);",
                       -1, &st, NULL);
    for (int i = 0; i < 2 * n; i++) {
        const char *b = bodies[i % n];
        sqlite3_bind_int(st, 1, i + 1);
        if (i < n) sqlite3_bind_blob(st, 2, b, (int)strlen(b), SQLITE_STATIC);
        else       sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, 100 + i);
        CHECK(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    CHECK(sqlite3_exec(db,
        "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms,author_name) "
        "  VALUES(901,1,1,'/me deploys',5,'GitHub CI');"
        "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms,kind) VALUES(902,1,1,'/me called',6,1);"
        "INSERT INTO messages(id,channel_id,author_id,body,created_at_ms,deleted_at_ms) VALUES(903,1,1,NULL,7,8);"
        "INSERT INTO forwards(message_id,src_channel,src_message,src_author,excerpt,n_attach) VALUES"
        "  (1,1,901,1,'/me   deploys',0),(2,1,902,1,'/me',0),(3,1,903,1,'plain',0),"
        "  (4,1,901,1,CAST('/me ships' AS BLOB),0);",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(oc_migrate(db, OC_MIGRATIONS, OC_MIGRATIONS_COUNT, &err) == SQLITE_OK);

    sqlite3_prepare_v2(db, "SELECT text_start, text_len, actor_id, actor_name IS NULL, created_at_ms "
                           "FROM actions WHERE message_id=?;", -1, &st, NULL);
    for (int i = 0; i < 2 * n; i++) {
        uint32_t ws = 0, wl = 0;
        int want = oc_action_parse(bodies[i % n], strlen(bodies[i % n]), &ws, &wl);
        sqlite3_bind_int(st, 1, i + 1);
        int got = sqlite3_step(st) == SQLITE_ROW;
        int same = got == want && (!got || ((uint32_t)sqlite3_column_int(st, 0) == ws &&
                                            (uint32_t)sqlite3_column_int(st, 1) == wl &&
                                            sqlite3_column_int(st, 2) == 1 && sqlite3_column_int(st, 3) == 1 &&
                                            sqlite3_column_int(st, 4) == 100 + i));
        if (!same) printf("    backfill disagrees with the rule on body %d\n", i);
        CHECK(same);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    CHECK(scalar(db, "SELECT COUNT(*) FROM actions WHERE message_id=901 AND actor_name='GitHub CI' "
                     "AND text_start=4 AND text_len=7;") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM actions WHERE message_id IN (902,903);") == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM forwards WHERE message_id=1 AND src_action=1 AND excerpt='deploys';") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM forwards WHERE message_id=2 AND src_action=0 AND excerpt='/me';") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM forwards WHERE message_id=3 AND src_action=0 AND excerpt='plain';") == 1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM forwards WHERE message_id=4 AND src_action=1 AND excerpt='ships';") == 1);
    /* One row per message. */
    CHECK(sqlite3_exec(db, "INSERT INTO actions(message_id,channel_id,actor_id,text_start,text_len,created_at_ms) "
                           "VALUES(1,1,1,4,1,0);", NULL, NULL, NULL) != SQLITE_OK);
    sqlite3_close(db);
}

int run_migrate_tests(void) {
    printf("test_migrate: fresh apply, idempotent rerun, resume, rollback,\n");
    printf("              embedded core schema\n");
    test_fresh_apply();
    test_idempotent_rerun();
    test_resume_partial();
    test_failure_rolls_back();
    test_embedded_schema();
    test_identity_backfill();
    test_action_backfill();
    return failures;
}
