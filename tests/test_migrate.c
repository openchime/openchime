/* Unit tests for the schema migrations runner (ARCH-27, docs/TESTING.md §2.2).
 * Includes migrate.c directly per the openblocks convention; links libsqlite3.
 * All tests run against in-memory databases — no files touched. */

#include "migrate.h"
#include "check.h"

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
    CHECK(oc_schema_version(db) == 18);   /* + reactions/threads/FTS/cursors/identity/attachments/webhooks/notify/client_settings/enrollment */

    const char *tables[] = { "users", "channels", "channel_members",
                             "messages", "sent_messages",
                             "sessions", "local_credentials", "invites", "reactions",
                             "messages_fts", "delivery_cursors", "server_identity",
                             "attachments", "webhooks", "notification_prefs",
                             "client_settings", "audit_log" };
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

    /* the kind CHECK constraint rejects an invalid channel kind */
    CHECK(sqlite3_exec(db,
        "INSERT INTO channels(id,kind,created_at_ms) VALUES(2,'bogus',0);",
        NULL, NULL, NULL) != SQLITE_OK);
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
    return failures;
}
