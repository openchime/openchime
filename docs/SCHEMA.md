# OpenChime — Database Schema

The SQLite schema (ARCH-2) and how it evolves. The migration *mechanism* is
ARCH-27; the *content* below is applied by migration 0001. New tables/columns
arrive as later numbered migrations, never as edits to an existing one.

**Status.** Migration 0001 establishes the foundational tables for the core
messaging path — the frames the v1 codec already supports (send / broadcast /
ack / backfill). Roles (REQ-030), reactions (REQ-070), threads (REQ-060),
full-text search (REQ-080/FTS5), presence, notification config, and attachments
are intentionally **not** here yet; each is a future migration once its own
requirement is settled. What is present is grounded in already-decided
requirements, so 0001 makes only one genuinely new storage decision (membership,
REQ-031).

---

## 1. The migration mechanism (ARCH-27)

- Migrations are an ordered, embedded C array (`OC_MIGRATIONS` in
  `src/migrate.c`): `{ version, sql }`, versions strictly increasing from 1.
- On startup the daemon calls `oc_migrate_default(db, …)`, which:
  1. ensures a `schema_version` table exists;
  2. reads the highest applied version;
  3. for each embedded migration newer than that, runs it **in its own
     transaction** and records the version in `schema_version` on commit.
- **Atomicity and resume:** each migration is one transaction, so a failure
  rolls back that step and leaves the database at the last good version; a
  restart resumes from exactly there. This is why each step is independently
  committed rather than all-pending-in-one transaction.
- **Forward-only:** there are no down-migrations. Recovery from a bad migration
  is roll-forward (a new migration) or restore from the Litestream replica
  (ARCH-24), consistent with the single-box model.

`schema_version`:

| column       | type    | notes                                        |
|--------------|---------|----------------------------------------------|
| `version`    | INTEGER | primary key; the migration number.           |
| `applied_at` | TEXT    | ISO-8601 UTC timestamp, defaulted on insert. |

---

## 2. Migration 0001 — core messaging tables

All timestamps are `INTEGER` milliseconds since the Unix epoch UTC (`*_ms`),
matching the wire protocol's `u64` time fields ([PROTOCOL.md](./PROTOCOL.md) §7).
Foreign keys are declared for integrity; the daemon runs with
`PRAGMA foreign_keys = ON`.

### `users`
Identity resolved from the OIDC token (REQ-023). `id` is the tenant-local
`user_id` carried in `AUTH_OK` and `BROADCAST.author_id`.

| column          | type    | notes                                            |
|-----------------|---------|--------------------------------------------------|
| `id`            | INTEGER | primary key (tenant-local user id).              |
| `subject`       | TEXT    | unique; the OIDC `issuer\|subject` pair.         |
| `email`         | TEXT    | from the token; display/convenience.             |
| `display_name`  | TEXT    | optional.                                        |
| `created_at_ms` | INTEGER | first seen.                                      |

### `channels`
A channel or a direct-message conversation (REQ-050). `is_public` distinguishes
open channels from private ones for the read/post gate in REQ-031.

| column          | type    | notes                                            |
|-----------------|---------|--------------------------------------------------|
| `id`            | INTEGER | primary key (the wire `channel_id`).             |
| `kind`          | TEXT    | `CHECK (kind IN ('channel','dm'))`.              |
| `name`          | TEXT    | null for DMs.                                     |
| `is_public`     | INTEGER | `0`/`1`; private channels are members-only.       |
| `created_at_ms` | INTEGER |                                                  |

### `channel_members`  — resolves REQ-031
Membership is independent of any tenant role. Only members may read/post in a
non-public channel; this table is that membership set.

| column         | type    | notes                                    |
|----------------|---------|------------------------------------------|
| `channel_id`   | INTEGER | → `channels(id)`.                        |
| `user_id`      | INTEGER | → `users(id)`.                           |
| `joined_at_ms` | INTEGER |                                          |
|                |         | primary key `(channel_id, user_id)`.     |

### `messages`
`id` is the server-assigned, tenant-monotonic message id (ARCH-43) — declared
`AUTOINCREMENT` so ids never decrease even if a row were ever removed, which is
what makes ascending `id` equal accept order within a channel (REQ-092) and lets
backfill replay `id > cursor` (REQ-101). Deletion is a **tombstone**, not a row
removal (REQ-052): the body is nulled while author, timestamps, and id are kept
so thread links and reply counts don't break. `deleted_by` distinguishes a
self-delete from a moderator delete (REQ-032). Edits keep the original
`created_at_ms` and set `edited_at_ms` (REQ-051).

| column          | type    | notes                                                  |
|-----------------|---------|--------------------------------------------------------|
| `id`            | INTEGER | primary key `AUTOINCREMENT` (wire `message_id`).       |
| `channel_id`    | INTEGER | → `channels(id)`.                                      |
| `author_id`     | INTEGER | → `users(id)`.                                         |
| `body`          | TEXT    | `NULL` once tombstoned.                                |
| `created_at_ms` | INTEGER | server send time (REQ-050); unchanged by edits.        |
| `edited_at_ms`  | INTEGER | null unless edited (REQ-051).                          |
| `deleted_at_ms` | INTEGER | null unless tombstoned (REQ-052).                     |
| `deleted_by`    | INTEGER | → `users(id)`; author vs moderator (REQ-032).          |

Index `idx_messages_channel (channel_id, id)` serves per-channel ordered reads
and backfill (`WHERE channel_id = ? AND id > ? ORDER BY id`).

### `sent_messages`  — idempotency map (ARCH-44)
The dedup mapping for safe send-retry (REQ-093). A `SEND`'s
`(channel_id, idempotency_token)` is looked up here; a hit re-returns the
existing `message_id` instead of inserting a duplicate. Pruned after a bounded
window at runtime (target 24h, ARCH-44) — pruning is a delete job, not schema.

| column              | type    | notes                                        |
|---------------------|---------|----------------------------------------------|
| `channel_id`        | INTEGER |                                              |
| `idempotency_token` | BLOB    | the 16-byte client token (PROTOCOL.md §7).   |
| `message_id`        | INTEGER | → `messages(id)`.                            |
| `created_at_ms`     | INTEGER | for the pruning window.                      |
|                     |         | primary key `(channel_id, idempotency_token)`.|

---

## 3. Deferred to later migrations

Tracked here so the omissions are deliberate, not forgotten:

- **Roles** (owner/admin/member) and their enforcement point — REQ-030,
  REQ-032's authorization half, REQ-033. Still `[needs ARCH decision]`.
- **Reactions** (REQ-070/071), **threads** (parent linkage, REQ-060/061).
- **FTS5** full-text index over `messages.body` (REQ-080, ARCH-15).
- **Presence / typing** (REQ-120/121) — likely in-memory, not schema.
- **Notification settings** and DND (REQ-130/131).
- **Attachments** metadata (REQ-140) — object-storage pointers, not blobs.
- **Sessions / revocation** bookkeeping (REQ-182) if a local session table is
  needed alongside JWT validation.
