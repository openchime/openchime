/*
 * OpenChime client — local SQLite store. See store.h.
 */

#include "store.h"

#include "migrate.h"    /* oc_migrate — the daemon's runner, reused with our set */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct oc_store { sqlite3 *db; };

/* The client migration set (independent of the daemon's OC_MIGRATIONS). One row
 * per server instance holds the reconnect state. */
static const oc_migration CLIENT_MIGRATIONS[] = {
    { 1,
      "CREATE TABLE instance_state ("
      "  instance       TEXT PRIMARY KEY,"
      "  session_token  BLOB,"
      "  session_expiry INTEGER,"
      "  tls_pin        BLOB"
      ");" },
};
static const int CLIENT_MIGRATIONS_COUNT =
    (int)(sizeof CLIENT_MIGRATIONS / sizeof CLIENT_MIGRATIONS[0]);

oc_store *oc_store_open(const char *path) {
    if (!path || !path[0]) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return NULL; }
    /* WAL keeps a crash from corrupting the file; busy timeout tolerates a
     * momentary lock if two client processes share a store. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
    char *err = NULL;
    if (oc_migrate(db, CLIENT_MIGRATIONS, CLIENT_MIGRATIONS_COUNT, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    oc_store *s = calloc(1, sizeof *s);
    if (!s) { sqlite3_close(db); return NULL; }
    s->db = db;
    return s;
}

void oc_store_close(oc_store *s) {
    if (!s) return;
    if (s->db) sqlite3_close(s->db);
    free(s);
}

/* Upsert one blob/int column pair for `instance`, preserving the other columns
 * (INSERT ... ON CONFLICT DO UPDATE). Used by both save paths. */
static void upsert(oc_store *s, const char *instance, const char *sql,
                   const void *blob, int blob_len, int64_t num, int has_num) {
    if (!s || !s->db || !instance) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, instance, -1, SQLITE_TRANSIENT);
    int col = 2;
    if (blob) sqlite3_bind_blob(st, col++, blob, blob_len, SQLITE_TRANSIENT);
    if (has_num) sqlite3_bind_int64(st, col++, num);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int oc_store_load_session(oc_store *s, const char *instance,
                          uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t *expiry,
                          uint64_t now_ms) {
    if (!s || !s->db || !instance) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT session_token, session_expiry FROM instance_state WHERE instance=?1;",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, instance, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_BLOB &&
        sqlite3_column_bytes(st, 0) == OC_SESSION_TOKEN_LEN) {
        uint64_t exp = (uint64_t)sqlite3_column_int64(st, 1);
        if (now_ms == 0 || exp == 0 || exp > now_ms) {
            memcpy(token, sqlite3_column_blob(st, 0), OC_SESSION_TOKEN_LEN);
            if (expiry) *expiry = exp;
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found;
}

void oc_store_save_session(oc_store *s, const char *instance,
                           const uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t expiry) {
    upsert(s, instance,
           "INSERT INTO instance_state (instance, session_token, session_expiry) VALUES (?1, ?2, ?3) "
           "ON CONFLICT(instance) DO UPDATE SET session_token=?2, session_expiry=?3;",
           token, OC_SESSION_TOKEN_LEN, (int64_t)expiry, 1);
}

void oc_store_clear_session(oc_store *s, const char *instance) {
    if (!s || !s->db || !instance) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE instance_state SET session_token=NULL, session_expiry=0 WHERE instance=?1;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, instance, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int oc_store_load_pin(oc_store *s, const char *instance,
                      uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    if (!s || !s->db || !instance) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT tls_pin FROM instance_state WHERE instance=?1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, instance, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_BLOB &&
        sqlite3_column_bytes(st, 0) == OC_TLS_FINGERPRINT_LEN) {
        memcpy(pin, sqlite3_column_blob(st, 0), OC_TLS_FINGERPRINT_LEN);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

void oc_store_save_pin(oc_store *s, const char *instance,
                       const uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    upsert(s, instance,
           "INSERT INTO instance_state (instance, tls_pin) VALUES (?1, ?2) "
           "ON CONFLICT(instance) DO UPDATE SET tls_pin=?2;",
           pin, OC_TLS_FINGERPRINT_LEN, 0, 0);
}
