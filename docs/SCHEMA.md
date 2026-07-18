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
  is roll-forward (a new migration) or restore from the operator's own backup
  (ARCH-3), consistent with the island model (ARCH-4).

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
`kind='dm'` row with `name=NULL`, `is_public=0`, and its participants as members
— two for a normal DM, or one for a self-DM (notes to self, REQ-055) — via
`OPEN_DM` (PROTOCOL.md §5.12), so messaging/backfill/search reuse the ordinary
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

## 3e. Migration 0007 — delivery cursors

Server-side delivery accounting (REQ-090); see the server-robustness notes in
STATUS.md. A `delivery_cursors` row per `(user, channel)` advanced by
`CLIENT_ACK` so a cursorless backfill can resume where the user left off.

---

## 3f. Migration 0008 — server identity

Persists the daemon's TLS identity across restart/restore so the TOFU
fingerprint clients pinned (ARCH-10) survives a redeploy.

---

## 3g. Migration 0009 — attachments (REQ-140, ARCH-17/69/70)

Attachment **bytes live in object storage, never in SQLite** — this table holds
only the pointer and metadata. Bytes are proxied through the daemon in chunks
(PROTOCOL.md §5.14); this row is the durable record the transfer produces.

### `attachments`
- `id` (PK) — the `attachment_id` on the wire.
- `channel_id` (FK `channels`) — the channel/DM the upload targeted; access
  control derives from this via the message it is linked to (REQ-141).
- `message_id` (FK `messages`, nullable) — the message that published the
  attachment. **NULL while pending** (uploaded but not yet referenced); set when
  a `SEND`/`SEND_REPLY` references the id.
- `uploader_id` (FK `users`) — who uploaded it; the only user allowed to link a
  pending attachment.
- `storage_key` (TEXT) — opaque object-storage key (never exposed on the wire).
- `filename`, `mime` (TEXT) — original name and declared content type.
- `size` (INTEGER) — byte length, verified against the streamed total on
  `UPLOAD_END`.
- `sha256` (BLOB) — content digest, returned in `UPLOAD_OK` / `DOWNLOAD_INFO`.
- `created_at` (INTEGER) — upload time; drives the orphan sweep of pending rows
  whose blob was never finalized/linked (time-gated, like `sent_messages`
  pruning, ARCH-44/70).

Indexed by `message_id` (fetch a message's attachments for delivery/backfill)
and by `(uploader_id, created_at)` (sweep abandoned pending uploads). A committed
attachment's lifetime is tied to its message: a tombstoned/deleted message's
attachment blob is eligible for removal (REQ-141, "as long as the message
exists").

---

## 3h. Migration 0010 — incoming webhooks (REQ-170, ARCH-32/71)

Lets an uncontrolled third party post a message into one channel over the HTTP
endpoint without a user session or JWT.

### `webhooks`
- `id` (PK) — the webhook id (returned to the creating client).
- `channel_id` (FK `channels`) — the single channel this token may post to.
- `creator_id` (FK `users`) — the member who minted it; posts are attributed to
  this user.
- `token_hash` (BLOB) — **SHA-256 of the 32-byte token**, never the token
  itself (the raw value is shown once at creation, like a session — ARCH-58).
- `label` (TEXT) — a human note (e.g. "GitHub CI").
- `disabled` (INTEGER 0/1) — a soft off switch; a disabled token 404s.
- `created_at_ms` (INTEGER).

A **unique index on `token_hash`** makes the per-request HTTP-side lookup a
single indexed probe and forbids duplicate tokens.

---

## 3i. Migration 0011 — message display-name override (REQ-170)

`ALTER TABLE messages ADD COLUMN author_name TEXT;` — NULL for an ordinary
message (the client renders the `author_id` user's own name), set to the
webhook's label for a webhook post so it shows as e.g. "GitHub CI". Persisting it
on the message (not just the live broadcast) keeps the name consistent through
backfill. A metadata-only default-NULL add.

---

## 3j. Migration 0012 — notification preferences (REQ-130/131, ARCH-72)

Server-authoritative notification settings, synced across a user's devices.

### `notification_prefs`
- `(user_id, channel_id)` (PK) — one level per user per channel.
- `level` (INTEGER 0/1/2) — 0 all, 1 mentions-only, 2 none. An **absent row means
  the default** (all), so the table stays sparse.

### `users` (added columns)
- `dnd_enabled` (INTEGER 0/1) — do-not-disturb on/off.
- `dnd_start_min`, `dnd_end_min` (INTEGER) — the daily DND window as minutes of
  day (UTC; the client converts from local), wrapping past midnight when
  `start > end`. DND suppresses push, not in-app unread (REQ-131).

---

## 3k. Migration 0013 — synced client settings

The daemon-side layer of the client's config: portable, cross-device UI prefs a
frontend chooses to sync (the machine-local `~/.config/openchime/config` still
holds terminal-bound defaults; the synced value, when present, wins). Opaque
key/value storage — the daemon stores and fans it back, the frontend owns the
meaning (PROTOCOL.md: `SET_CLIENT_SETTING` / `LIST_CLIENT_SETTINGS` /
`CLIENT_SETTINGS`).

### `client_settings`
- `(user_id, client_type, key)` (PK) — one value per user, per frontend bucket,
  per key. `client_type` (e.g. `tui`, a future `gui`) partitions the store so
  one frontend's prefs never collide with another's.
- `value` (TEXT) — the setting value; an empty value on `SET` deletes the row, so
  the bucket stays sparse and a deleted key falls back to the client's file
  default.
- `updated_ms` (INTEGER) — last-write timestamp (last-writer-wins; no vector
  clock — these are single-user, low-contention prefs).

A `SET` returns a fresh bucket snapshot, which the daemon fans to **all** of the
user's live connections; each client folds only the snapshot whose `client_type`
matches its own, so a change on one device reaches the user's other same-type
devices.

---

## 4. Deferred to later migrations

Tracked here so the omissions are deliberate, not forgotten:

- **Thread notifications** (REQ-061) — needs notification config (REQ-130).
- **Push delivery** state (REQ-132/133) — device tokens for APNs/FCM; the
  notification *settings* (REQ-130/131) are now in migration 0012 above.
- **Audio** call state (REQ-150–152) — likely a UDP sidecar, not schema (ARCH-18).

(Roles, sessions/revocation, local credentials, delivery cursors, server
identity, and attachment metadata — previously deferred — are now defined in
migrations 0002 and 0007–0009 above. Presence/typing (REQ-120/121) stayed
in-memory by design, ARCH-67/68, and needs no schema.)
