/*
 * OpenChime client — local SQLite store. See store.h.
 */

#include "store.h"

#include "migrate.h"    /* oc_migrate — the daemon's runner, reused with our set */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct oc_store { sqlite3 *db; oc_secret *secret; };

void oc_store_set_secret(oc_store *s, oc_secret *secret) {
    if (s) s->secret = secret;
}

/* The client migration set (independent of the daemon's OC_MIGRATIONS). One row
 * per workspace holds the reconnect state.
 *
 * Migrations are forward-only, so 1-3 keep the historical `instance` spelling
 * they shipped with; migration 4 renames it to `workspace` everywhere (the term
 * the whole client now uses). A fresh store runs 1-3 then 4 and lands on the
 * same schema an upgraded store does. */
static const oc_migration CLIENT_MIGRATIONS[] = {
    { 1,
      "CREATE TABLE instance_state ("
      "  instance       TEXT PRIMARY KEY,"
      "  session_token  BLOB,"
      "  session_expiry INTEGER,"
      "  tls_pin        BLOB"
      ");" },
    { 2,
      "CREATE TABLE cached_message ("
      "  instance     TEXT    NOT NULL,"
      "  channel_id   INTEGER NOT NULL,"
      "  message_id   INTEGER NOT NULL,"
      "  author_id    INTEGER,"
      "  author_name  TEXT,"
      "  server_time  INTEGER,"
      "  body         TEXT,"
      "  edited       INTEGER NOT NULL DEFAULT 0,"
      "  deleted      INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY (instance, message_id)"
      ");" },
    { 3,
      "CREATE TABLE outbox ("
      "  instance     TEXT NOT NULL,"
      "  idem         BLOB NOT NULL,"
      "  channel_id   INTEGER NOT NULL,"
      "  body         TEXT,"
      "  created      INTEGER,"
      "  PRIMARY KEY (instance, idem)"
      ");" },
    { 4,
      "ALTER TABLE instance_state RENAME TO workspace_state;"
      "ALTER TABLE workspace_state RENAME COLUMN instance TO workspace;"
      "ALTER TABLE cached_message  RENAME COLUMN instance TO workspace;"
      "ALTER TABLE outbox          RENAME COLUMN instance TO workspace;" },
    { 5,
      "CREATE TABLE workspace_book ("
      "  workspace    TEXT PRIMARY KEY,"
      "  label        TEXT,"
      "  username     TEXT,"
      "  last_used_ms INTEGER NOT NULL DEFAULT 0"
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

/* Upsert one blob/int column pair for `workspace`, preserving the other columns
 * (INSERT ... ON CONFLICT DO UPDATE). Used by both save paths. */
static void upsert(oc_store *s, const char *workspace, const char *sql,
                   const void *blob, int blob_len, int64_t num, int has_num) {
    if (!s || !s->db || !workspace) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    int col = 2;
    if (blob) sqlite3_bind_blob(st, col++, blob, blob_len, SQLITE_TRANSIENT);
    if (has_num) sqlite3_bind_int64(st, col++, num);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Pack/unpack a token + 8-byte little-endian expiry as one keyring blob. */
#define OC_SECRET_BLOB (OC_SESSION_TOKEN_LEN + 8)
static void secret_pack(const uint8_t *token, uint64_t expiry, uint8_t *out) {
    memcpy(out, token, OC_SESSION_TOKEN_LEN);
    for (int i = 0; i < 8; i++) out[OC_SESSION_TOKEN_LEN + i] = (uint8_t)(expiry >> (8 * i));
}
static uint64_t secret_unpack_expiry(const uint8_t *blob) {
    uint64_t e = 0;
    for (int i = 0; i < 8; i++) e |= (uint64_t)blob[OC_SESSION_TOKEN_LEN + i] << (8 * i);
    return e;
}

int oc_store_load_session(oc_store *s, const char *workspace,
                          uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t *expiry,
                          uint64_t now_ms) {
    if (!s || !workspace) return 0;
    if (s->secret) {   /* keyring-backed: one token+expiry blob per workspace */
        uint8_t blob[OC_SECRET_BLOB]; size_t got = 0;
        if (!oc_secret_get(s->secret, workspace, blob, sizeof blob, &got) || got != OC_SECRET_BLOB)
            return 0;
        uint64_t exp = secret_unpack_expiry(blob);
        if (now_ms != 0 && exp != 0 && exp <= now_ms) return 0;   /* expired */
        memcpy(token, blob, OC_SESSION_TOKEN_LEN);
        if (expiry) *expiry = exp;
        return 1;
    }
    if (!s->db) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT session_token, session_expiry FROM workspace_state WHERE workspace=?1;",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
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

void oc_store_save_session(oc_store *s, const char *workspace,
                           const uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t expiry) {
    if (s && s->secret) {
        uint8_t blob[OC_SECRET_BLOB];
        secret_pack(token, expiry, blob);
        oc_secret_put(s->secret, workspace, blob, sizeof blob);
        return;
    }
    upsert(s, workspace,
           "INSERT INTO workspace_state (workspace, session_token, session_expiry) VALUES (?1, ?2, ?3) "
           "ON CONFLICT(workspace) DO UPDATE SET session_token=?2, session_expiry=?3;",
           token, OC_SESSION_TOKEN_LEN, (int64_t)expiry, 1);
}

void oc_store_clear_session(oc_store *s, const char *workspace) {
    if (!s || !workspace) return;
    if (s->secret) { oc_secret_del(s->secret, workspace); return; }
    if (!s->db) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE workspace_state SET session_token=NULL, session_expiry=0 WHERE workspace=?1;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int oc_store_load_pin(oc_store *s, const char *workspace,
                      uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    if (!s || !s->db || !workspace) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT tls_pin FROM workspace_state WHERE workspace=?1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_BLOB &&
        sqlite3_column_bytes(st, 0) == OC_TLS_FINGERPRINT_LEN) {
        memcpy(pin, sqlite3_column_blob(st, 0), OC_TLS_FINGERPRINT_LEN);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

void oc_store_save_pin(oc_store *s, const char *workspace,
                       const uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    upsert(s, workspace,
           "INSERT INTO workspace_state (workspace, tls_pin) VALUES (?1, ?2) "
           "ON CONFLICT(workspace) DO UPDATE SET tls_pin=?2;",
           pin, OC_TLS_FINGERPRINT_LEN, 0, 0);
}

void oc_store_save_message(oc_store *s, const char *workspace, uint64_t channel_id,
                           uint64_t message_id, uint64_t author_id,
                           const char *author_name, uint64_t server_time,
                           const char *body, int edited, int deleted) {
    if (!s || !s->db || !workspace || !message_id) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO cached_message "
            "(workspace, channel_id, message_id, author_id, author_name, server_time, body, edited, deleted) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
            "ON CONFLICT(workspace, message_id) DO UPDATE SET "
            "  channel_id=?2, author_id=?4, author_name=?5, server_time=?6, body=?7, edited=?8, deleted=?9;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)channel_id);
    sqlite3_bind_int64(st, 3, (int64_t)message_id);
    sqlite3_bind_int64(st, 4, (int64_t)author_id);
    sqlite3_bind_text(st, 5, author_name ? author_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (int64_t)server_time);
    if (body) sqlite3_bind_text(st, 7, body, -1, SQLITE_TRANSIENT);
    else      sqlite3_bind_null(st, 7);
    sqlite3_bind_int(st, 8, edited ? 1 : 0);
    sqlite3_bind_int(st, 9, deleted ? 1 : 0);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void oc_store_edit_message(oc_store *s, const char *workspace, uint64_t message_id,
                           const char *body) {
    if (!s || !s->db || !workspace) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE cached_message SET body=?3, edited=1 WHERE workspace=?1 AND message_id=?2;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)message_id);
    if (body) sqlite3_bind_text(st, 3, body, -1, SQLITE_TRANSIENT);
    else      sqlite3_bind_null(st, 3);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void oc_store_delete_message(oc_store *s, const char *workspace, uint64_t message_id) {
    if (!s || !s->db || !workspace) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE cached_message SET body=NULL, deleted=1 WHERE workspace=?1 AND message_id=?2;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (int64_t)message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void oc_store_each_message(oc_store *s, const char *workspace,
                           oc_store_msg_cb cb, void *ctx) {
    if (!s || !s->db || !workspace || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT channel_id, message_id, author_id, author_name, server_time, body, edited, deleted "
            "FROM cached_message WHERE workspace=?1 ORDER BY message_id ASC;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *body = (const char *)sqlite3_column_text(st, 5);   /* NULL if deleted */
        cb(ctx,
           (uint64_t)sqlite3_column_int64(st, 0),
           (uint64_t)sqlite3_column_int64(st, 1),
           (uint64_t)sqlite3_column_int64(st, 2),
           (const char *)sqlite3_column_text(st, 3),
           (uint64_t)sqlite3_column_int64(st, 4),
           body,
           sqlite3_column_int(st, 6),
           sqlite3_column_int(st, 7));
    }
    sqlite3_finalize(st);
}

void oc_store_outbox_add(oc_store *s, const char *workspace,
                         const uint8_t idem[OC_IDEM_SIZE], uint64_t channel_id,
                         const char *body) {
    if (!s || !s->db || !workspace) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO outbox (workspace, idem, channel_id, body, created) "
            "VALUES (?1, ?2, ?3, ?4, strftime('%s','now')) "
            "ON CONFLICT(workspace, idem) DO NOTHING;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, idem, OC_IDEM_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (int64_t)channel_id);
    sqlite3_bind_text(st, 4, body ? body : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void oc_store_outbox_remove(oc_store *s, const char *workspace,
                            const uint8_t idem[OC_IDEM_SIZE]) {
    if (!s || !s->db || !workspace) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "DELETE FROM outbox WHERE workspace=?1 AND idem=?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, idem, OC_IDEM_SIZE, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* --- the workspace book (REQ-012) ---------------------------------------- */

void oc_store_workspace_remember(oc_store *s, const char *workspace,
                                 const char *label, const char *username,
                                 uint64_t now_ms) {
    if (!s || !s->db || !workspace || !workspace[0]) return;
    sqlite3_stmt *st = NULL;
    /* A re-login keeps the existing label/username when the caller passes NULL,
     * so re-signing-in never blanks out what the switcher shows. */
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO workspace_book (workspace, label, username, last_used_ms) "
            "VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(workspace) DO UPDATE SET "
            "  label        = COALESCE(?2, label),"
            "  username     = COALESCE(?3, username),"
            "  last_used_ms = ?4;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    if (label)    sqlite3_bind_text(st, 2, label, -1, SQLITE_TRANSIENT);
    else          sqlite3_bind_null(st, 2);
    if (username) sqlite3_bind_text(st, 3, username, -1, SQLITE_TRANSIENT);
    else          sqlite3_bind_null(st, 3);
    sqlite3_bind_int64(st, 4, (int64_t)now_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void oc_store_workspace_forget(oc_store *s, const char *workspace) {
    if (!s || !s->db || !workspace) return;
    /* Drop the book entry AND every trace of the workspace: leaving the session
     * token or cached history behind would mean "forget" silently kept the
     * user's messages on disk. */
    static const char *const SQL[] = {
        "DELETE FROM workspace_book  WHERE workspace=?1;",
        "DELETE FROM workspace_state WHERE workspace=?1;",
        "DELETE FROM cached_message  WHERE workspace=?1;",
        "DELETE FROM outbox          WHERE workspace=?1;",
    };
    for (size_t i = 0; i < sizeof SQL / sizeof SQL[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db, SQL[i], -1, &st, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    /* The token may live in the OS keyring rather than the column above. */
    if (s->secret) oc_secret_del(s->secret, workspace);
}

void oc_store_workspace_each(oc_store *s, oc_store_workspace_cb cb, void *ctx) {
    if (!s || !s->db || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT workspace, label, username, last_used_ms FROM workspace_book "
            "ORDER BY last_used_ms DESC, workspace ASC;",
            -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        cb(ctx,
           (const char *)sqlite3_column_text(st, 0),
           (const char *)sqlite3_column_text(st, 1),
           (const char *)sqlite3_column_text(st, 2),
           (uint64_t)sqlite3_column_int64(st, 3));
    }
    sqlite3_finalize(st);
}

void oc_store_outbox_each(oc_store *s, const char *workspace,
                          oc_store_outbox_cb cb, void *ctx) {
    if (!s || !s->db || !workspace || !cb) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db,
            "SELECT idem, channel_id, body FROM outbox WHERE workspace=?1 ORDER BY created ASC, rowid ASC;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, workspace, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_bytes(st, 0) != OC_IDEM_SIZE) continue;
        cb(ctx, (const uint8_t *)sqlite3_column_blob(st, 0),
           (uint64_t)sqlite3_column_int64(st, 1),
           (const char *)sqlite3_column_text(st, 2));
    }
    sqlite3_finalize(st);
}
