/*
 * OpenChime schema migrations runner + embedded migration set (ARCH-27).
 * See migrate.h and docs/SCHEMA.md.
 */

#include "migrate.h"

#include <stddef.h>

/* --- Embedded migrations ------------------------------------------------ */

/* 0001: foundational tables for the core messaging path (docs/SCHEMA.md).
 * Roles (REQ-030), reactions, threads, FTS5, presence, and attachments are
 * deferred to later migrations. */
static const char MIGRATION_0001[] =
    "CREATE TABLE users ("
    "  id            INTEGER PRIMARY KEY,"
    "  subject       TEXT NOT NULL UNIQUE,"          /* OIDC issuer|subject */
    "  email         TEXT,"
    "  display_name  TEXT,"
    "  created_at_ms INTEGER NOT NULL"
    ");"

    "CREATE TABLE channels ("
    "  id            INTEGER PRIMARY KEY,"
    "  kind          TEXT NOT NULL CHECK (kind IN ('channel','dm')),"
    "  name          TEXT,"
    "  is_public     INTEGER NOT NULL DEFAULT 0 CHECK (is_public IN (0,1)),"
    "  created_at_ms INTEGER NOT NULL"
    ");"

    "CREATE TABLE channel_members ("            /* membership storage (REQ-031) */
    "  channel_id   INTEGER NOT NULL REFERENCES channels(id),"
    "  user_id      INTEGER NOT NULL REFERENCES users(id),"
    "  joined_at_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (channel_id, user_id)"
    ");"

    "CREATE TABLE messages ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"  /* tenant-monotonic id (ARCH-43) */
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  author_id     INTEGER NOT NULL REFERENCES users(id),"
    "  body          TEXT,"                              /* NULL once tombstoned (REQ-052) */
    "  created_at_ms INTEGER NOT NULL,"                  /* server send time (REQ-050) */
    "  edited_at_ms  INTEGER,"                           /* set on edit; original kept (REQ-051) */
    "  deleted_at_ms INTEGER,"                           /* tombstone time (REQ-052) */
    "  deleted_by    INTEGER REFERENCES users(id)"       /* self vs moderator delete (REQ-032) */
    ");"
    "CREATE INDEX idx_messages_channel ON messages(channel_id, id);"

    "CREATE TABLE sent_messages ("             /* idempotency map (ARCH-44) */
    "  channel_id        INTEGER NOT NULL,"
    "  idempotency_token BLOB NOT NULL,"                 /* 16-byte client token */
    "  message_id        INTEGER NOT NULL REFERENCES messages(id),"
    "  created_at_ms     INTEGER NOT NULL,"
    "  PRIMARY KEY (channel_id, idempotency_token)"
    ");";

/* 0002: authentication data model (docs/AUTH.md, SCHEMA.md §3, ARCH-58/59/60).
 * Adds sessions, local passwords, invites, and a role/avatar on users. */
static const char MIGRATION_0002[] =
    "ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'member' "
    "  CHECK (role IN ('owner','admin','member'));"        /* tenant role (ARCH-60) */
    "ALTER TABLE users ADD COLUMN avatar_key TEXT;"        /* object-storage key (ARCH-17) */

    "CREATE TABLE sessions ("                              /* daemon-issued sessions (ARCH-58) */
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  token_hash    BLOB NOT NULL UNIQUE,"                /* SHA-256 of the 32-byte token */
    "  user_id       INTEGER NOT NULL REFERENCES users(id),"
    "  created_at_ms INTEGER NOT NULL,"
    "  expires_at_ms INTEGER NOT NULL,"                    /* daemon-set lifetime (REQ-181) */
    "  last_seen_ms  INTEGER,"
    "  device_label  TEXT"
    ");"
    "CREATE INDEX idx_sessions_user ON sessions(user_id);"

    "CREATE TABLE local_credentials ("                     /* local-mode passwords (ARCH-59) */
    "  user_id       INTEGER PRIMARY KEY REFERENCES users(id),"
    "  salt          BLOB NOT NULL,"
    "  iterations    INTEGER NOT NULL,"                    /* PBKDF2 count (raisable) */
    "  hash          BLOB NOT NULL,"                       /* derived key; never a plaintext pw */
    "  updated_at_ms INTEGER NOT NULL"
    ");"

    "CREATE TABLE invites ("                               /* local account creation (REQ-033) */
    "  token_hash     BLOB PRIMARY KEY,"                   /* SHA-256 of the invite token */
    "  created_by     INTEGER REFERENCES users(id),"
    "  role           TEXT NOT NULL DEFAULT 'member' CHECK (role IN ('owner','admin','member')),"
    "  expires_at_ms  INTEGER NOT NULL,"
    "  consumed_at_ms INTEGER"                             /* null until used; single-use */
    ");";

/* 0003: tenant member removal (REQ-033). A removed member is locked out rather
 * than deleted, so their authored messages and tombstones keep a valid author.
 * The flag is checked in every auth path (local/session/oidc). */
static const char MIGRATION_0003[] =
    "ALTER TABLE users ADD COLUMN disabled INTEGER NOT NULL DEFAULT 0 "
    "  CHECK (disabled IN (0,1));";

/* 0004: emoji reactions (REQ-070/071). The composite primary key enforces
 * "one reaction of a given emoji per user per message" — a repeat add is a
 * silent no-op (toggle off is a delete), never a stacked duplicate. */
static const char MIGRATION_0004[] =
    "CREATE TABLE reactions ("
    "  message_id    INTEGER NOT NULL REFERENCES messages(id),"
    "  user_id       INTEGER NOT NULL REFERENCES users(id),"
    "  emoji         TEXT NOT NULL,"
    "  created_at_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (message_id, user_id, emoji)"
    ");"
    "CREATE INDEX idx_reactions_message ON reactions(message_id);";

const oc_migration OC_MIGRATIONS[] = {
    { 1, MIGRATION_0001 },
    { 2, MIGRATION_0002 },
    { 3, MIGRATION_0003 },
    { 4, MIGRATION_0004 },
};
const int OC_MIGRATIONS_COUNT = (int)(sizeof OC_MIGRATIONS / sizeof OC_MIGRATIONS[0]);

/* --- Runner ------------------------------------------------------------- */

int oc_schema_version(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    /* Prepare fails cleanly if the table doesn't exist yet — treat as v0. */
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(version), 0) FROM schema_version;",
                           -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return 0;
    }
    int ver = 0;
    if (sqlite3_step(st) == SQLITE_ROW) ver = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return ver;
}

int oc_migrate(sqlite3 *db, const oc_migration *set, int n, char **errmsg) {
    if (errmsg) *errmsg = NULL;

    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "  version    INTEGER PRIMARY KEY,"
        "  applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))"
        ");", NULL, NULL, errmsg);
    if (rc != SQLITE_OK) return rc;

    int cur = oc_schema_version(db);

    for (int i = 0; i < n; i++) {
        if (set[i].version <= cur) continue;

        rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, errmsg);
        if (rc != SQLITE_OK) return rc;

        rc = sqlite3_exec(db, set[i].sql, NULL, NULL, errmsg);
        if (rc == SQLITE_OK) {
            char *ins = sqlite3_mprintf(
                "INSERT INTO schema_version(version) VALUES(%d);", set[i].version);
            rc = sqlite3_exec(db, ins, NULL, NULL, errmsg);
            sqlite3_free(ins);
        }

        if (rc != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL); /* leave db at `cur` */
            return rc;
        }

        rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, errmsg);
        if (rc != SQLITE_OK) return rc;
        cur = set[i].version;
    }
    return SQLITE_OK;
}

int oc_migrate_default(sqlite3 *db, char **errmsg) {
    return oc_migrate(db, OC_MIGRATIONS, OC_MIGRATIONS_COUNT, errmsg);
}
