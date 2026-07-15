# OpenChime — Database Schema

The SQLite schema (ARCH-2) and how it evolves. The migration *mechanism* is
ARCH-27; the *content* below is applied by migration 0001. New tables/columns
arrive as later numbered migrations, never as edits to an existing one.

**Status.** Migration 0001 establishes the foundational tables for the core
messaging path — the frames the v1 codec already supports (send / broadcast /
ack / backfill). **Migration 0002** (§3) adds the authentication data model
(sessions, local credentials, invites, and `users` role/avatar) for the two-mode
auth design ([AUTH.md](./AUTH.md)). **Migration 0003** (§3a) adds a
`users.disabled` lockout flag for tenant member removal (REQ-033). **Migration
0004** (§3b) adds emoji reactions (REQ-070/071) and **migration 0005** (§3c) adds
message threads (REQ-060) and **migration 0006** (§3d) adds FTS5 full-text search
(REQ-080). Presence, notification config, and attachments are intentionally
**not** here yet; each is a future migration once its own requirement is settled.

---

## 1. The migration mechanism (ARCH-27)

- Migrations are an ordered, embedded C array (`OC_MIGRATIONS` in
  `daemon/migrate.c`): `{ version, sql }`, versions strictly increasing from 1.
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
open channels from private ones for the read/post gate in REQ-031. A **DM** is a
`kind='dm'` row with `name=NULL`, `is_public=0`, and exactly two members
(`OPEN_DM`, PROTOCOL.md §5.12) — so messaging/backfill/search reuse the ordinary
membership path. The channel-management ops act only on `kind='channel'`.

| column          | type    | notes                                            |
|-----------------|---------|--------------------------------------------------|
| `id`            | INTEGER | primary key (the wire `channel_id`).             |
| `kind`          | TEXT    | `CHECK (kind IN ('channel','dm'))`.              |
| `name`          | TEXT    | null for DMs.                                     |
| `is_public`     | INTEGER | `0`/`1`; private channels are members-only.       |
| `created_at_ms` | INTEGER |                                                  |

### `channel_members`  — resolves REQ-031
Membership is independent of any tenant role. Only members may read/post in a
non-public channel; this table is that membership set. The channel-management
frames (PROTOCOL.md §5.7) maintain it: create/join/invite insert rows,
leave/remove delete them, and posting to a *public* channel auto-joins the
sender. Public channels are readable/postable by any tenant user; private
channels are members-only, reachable only via `INVITE_TO_CHANNEL` (REQ-033).

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

## 3. Migration 0002 — authentication, sessions, roles

Adds the tables the real auth design ([AUTH.md](./AUTH.md), ARCH-55–60) needs,
and extends `users`. Appended as `MIGRATION_0002` + a `{ 2, ... }` entry in
`OC_MIGRATIONS` — the runner is unchanged (§1). `users` gains columns via
`ALTER TABLE ADD COLUMN` (SQLite-supported; existing rows default).

### `users` (added columns)
`subject` remains the unique identity key, namespaced by source
(`oidc:<issuer>|<sub>` or `local:<username>`, ARCH-19).

| column       | type    | notes                                                     |
|--------------|---------|-----------------------------------------------------------|
| `role`       | TEXT    | `NOT NULL DEFAULT 'member' CHECK (role IN ('owner','admin','member'))` — tenant role (ARCH-60, REQ-030). |
| `avatar_key` | TEXT    | object-storage key for the profile image (ARCH-17); null if none. |

### `sessions`  — daemon-issued sessions (ARCH-58)
The convergence point of both auth modes. Stores only the **hash** of the
session token, so a DB leak does not expose live sessions.

| column         | type    | notes                                                  |
|----------------|---------|--------------------------------------------------------|
| `id`           | INTEGER | primary key.                                           |
| `token_hash`   | BLOB    | `NOT NULL UNIQUE` — SHA-256 of the 32-byte session token. |
| `user_id`      | INTEGER | → `users(id)`.                                         |
| `created_at_ms`| INTEGER |                                                        |
| `expires_at_ms`| INTEGER | daemon-set lifetime (REQ-181).                         |
| `last_seen_ms` | INTEGER | updated on use.                                        |
| `device_label` | TEXT    | optional, for "log out other devices" (REQ-182).       |

Reconnect hashes the presented token and looks it up here; logout/revoke deletes
the row(s) (REQ-182).

### `local_credentials`  — local-mode passwords (ARCH-59)
Only local users have a row. PBKDF2-HMAC-SHA256; never a plaintext password.

| column        | type    | notes                                                   |
|---------------|---------|---------------------------------------------------------|
| `user_id`     | INTEGER | primary key, → `users(id)`.                             |
| `salt`        | BLOB    | per-user random salt.                                   |
| `iterations`  | INTEGER | PBKDF2 iteration count (recorded so it can be raised).  |
| `hash`        | BLOB    | the derived key.                                        |
| `updated_at_ms`| INTEGER |                                                        |

### `invites`  — local-mode account creation (ARCH-59, REQ-033)
An owner/admin issues an invite; the invitee sets a password by presenting it.

| column        | type    | notes                                                   |
|---------------|---------|---------------------------------------------------------|
| `token_hash`  | BLOB    | primary key — SHA-256 of the invite token.              |
| `created_by`  | INTEGER | → `users(id)`.                                          |
| `role`        | TEXT    | role the invitee will receive.                          |
| `expires_at_ms`| INTEGER |                                                        |
| `consumed_at_ms`| INTEGER | null until used; single-use.                          |

The `invites` and `local_credentials` tables are exercised by the tenant-admin
wire ops (PROTOCOL.md §5.8): `INVITE_USER` inserts an `invites` row and
`REDEEM_INVITE` consumes it while creating the account.

---

## 3a. Migration 0003 — member removal

`REMOVE_USER` (REQ-033) locks a member out of the tenant rather than deleting
their row (which authored messages/tombstones still reference). Appended as
`MIGRATION_0003` + a `{ 3, ... }` entry in `OC_MIGRATIONS`.

### `users` (added column)

| column     | type    | notes                                                        |
|------------|---------|--------------------------------------------------------------|
| `disabled` | INTEGER | `NOT NULL DEFAULT 0 CHECK (disabled IN (0,1))` — `1` once removed; every auth path (local/session/oidc) refuses a disabled user. Removal also deletes the user's `sessions`, `channel_members`, and `local_credentials`. |

---

## 3b. Migration 0004 — reactions

Emoji reactions (REQ-070/071), added as `MIGRATION_0004`.

### `reactions`
The composite primary key enforces REQ-070's "one reaction of a given emoji per
user per message": an add is `INSERT OR IGNORE` (no stacking), a remove is a
delete (toggle), and the aggregate per emoji is a `COUNT(*)`. A message tombstone
(REQ-052) deletes its reaction rows.

| column          | type    | notes                                            |
|-----------------|---------|--------------------------------------------------|
| `message_id`    | INTEGER | → `messages(id)`.                                |
| `user_id`       | INTEGER | → `users(id)`.                                   |
| `emoji`         | TEXT    | the reacted emoji (≤ 32 bytes on the wire).       |
| `created_at_ms` | INTEGER |                                                  |
|                 |         | primary key `(message_id, user_id, emoji)`.      |

Index `idx_reactions_message (message_id)` serves per-message aggregation and the
`LIST_REACTIONS` inspection query (PROTOCOL.md §5.9).

---

## 3c. Migration 0005 — threads

Message threads (REQ-060), added as `MIGRATION_0005`.

### `messages` (added column)

| column      | type    | notes                                                     |
|-------------|---------|-----------------------------------------------------------|
| `parent_id` | INTEGER | → `messages(id)`; `NULL` for a top-level message. A reply threads under a **top-level root** (replying to a reply flattens to that reply's root), so `parent_id` always names a top-level message. Existing rows default to `NULL`. |

Index `idx_messages_parent (parent_id, id)` serves the per-thread reply query
(`LIST_THREAD`) and the reply-count subqueries. The main-scroll backfill filters
`parent_id IS NULL` so replies never appear inline (PROTOCOL.md §5.10).

---

## 3d. Migration 0006 — full-text search

Full-text search over message bodies (REQ-080, ARCH-15), added as
`MIGRATION_0006`. Requires an SQLite built with FTS5 (Alpine's `sqlite-libs` and
the system/vendored libs used here enable it).

### `messages_fts`  (FTS5 virtual table)
An **external-content** FTS5 index (`content='messages', content_rowid='id'`)
over `messages.body` — it stores only the inverted index, not a copy of the
bodies. Three triggers keep it in sync:

- `messages_fts_ai` (after insert) indexes a new message;
- `messages_fts_au` (after update) re-indexes on edit and drops the text on
  tombstone (body → NULL);
- `messages_fts_ad` (after delete) removes the entry.

The migration seeds the index from existing rows. `SEARCH` (PROTOCOL.md §5.11)
joins `messages_fts MATCH ?` back to `messages`/`channels`, filters tombstones
and to the user's readable channels (REQ-031), and returns `snippet()` excerpts
newest-first.

---

## 4. Deferred to later migrations

Tracked here so the omissions are deliberate, not forgotten:

- **Thread notifications** (REQ-061) — needs notification config (REQ-130).
- **Presence / typing** (REQ-120/121) — likely in-memory, not schema.
- **Notification settings** and DND (REQ-130/131).
- **Attachments** metadata (REQ-140) — object-storage pointers, not blobs.

(Roles, sessions/revocation, and local credentials — previously deferred — are
now defined in migration 0002 above.)
