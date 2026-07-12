/* Test for the DB-writer thread lifecycle (dbwriter.c): start migrates the
 * database on boot, stop tears down cleanly, and the on-disk schema is present
 * afterward. Includes the code under test directly; links sqlite + pthread. */

#include "dbwriter.c"
#include "migrate.c"

#include <stdio.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

static void test_start_migrates_and_stops(void) {
    const char *path = "build/test_dbwriter.db";
    unlink(path);
    unlink("build/test_dbwriter.db-wal");
    unlink("build/test_dbwriter.db-shm");

    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    oc_dbwriter_stop(w);

    /* Reopen independently and confirm migrate-on-boot took effect. */
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(oc_schema_version(db) == 1);
    CHECK(table_exists(db, "messages"));
    CHECK(table_exists(db, "channel_members"));
    sqlite3_close(db);

    unlink(path);
    unlink("build/test_dbwriter.db-wal");
    unlink("build/test_dbwriter.db-shm");
}

int main(void) {
    printf("test_dbwriter: start migrates on boot, clean stop, on-disk schema\n");
    test_start_migrates_and_stops();
    if (failures == 0) { printf("OK: all checks passed\n"); return 0; }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
