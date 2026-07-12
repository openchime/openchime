/*
 * OpenChime schema migrations (ARCH-27).
 *
 * Sequential migrations are embedded in the daemon binary as an ordered array,
 * applied automatically against a `schema_version` table on startup — each in
 * its own transaction — before the daemon serves traffic. No separate
 * migration tool or manual operator step. See docs/SCHEMA.md for the schema
 * itself.
 */

#ifndef OPENCHIME_MIGRATE_H
#define OPENCHIME_MIGRATE_H

#include <sqlite3.h>

typedef struct {
    int         version; /* strictly increasing, starting at 1 */
    const char *sql;     /* one or more statements defining this step */
} oc_migration;

/* The daemon's embedded, ascending migration set. */
extern const oc_migration OC_MIGRATIONS[];
extern const int OC_MIGRATIONS_COUNT;

/* Highest applied migration version, or 0 if the database has none. */
int oc_schema_version(sqlite3 *db);

/* Apply every migration in `set` (ascending `version`, `n` entries) whose
 * version exceeds the current schema version, each inside its own transaction.
 * Returns SQLITE_OK on success. On failure returns the SQLite error code, sets
 * *errmsg (caller frees with sqlite3_free) if non-NULL, and leaves the database
 * at the last successfully-applied version — the partially-applied migration is
 * rolled back, so a restart resumes cleanly from that point. */
int oc_migrate(sqlite3 *db, const oc_migration *set, int n, char **errmsg);

/* Apply the daemon's embedded OC_MIGRATIONS. */
int oc_migrate_default(sqlite3 *db, char **errmsg);

#endif /* OPENCHIME_MIGRATE_H */
