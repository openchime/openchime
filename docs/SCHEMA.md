# OpenChime — Database Schema

The SQLite schema (ARCH-2) and how it evolves. The migration *mechanism* is
ARCH-27; the *content* below is applied by migration 0001. New tables/columns
arrive as later numbered migrations, never as edits to an existing one.

**Status.** **Migrations 0001–0018 are applied.** 0001 establishes the core
messaging tables; **0002** (§3) the authentication data model (sessions, local
credentials, invites, `users` role/avatar, [AUTH.md](./AUTH.md)); **0003** (§3a) the
`users.disabled` lockout flag; **0004** (§3b) reactions; **0005** (§3c) threads;
**0006** (§3d) FTS5 search; **0007** (§3e) delivery cursors; **0008** (§3f) the
persisted server TLS identity; **0009** (§3g) attachments; **0010–0011** (§3h/§3i)
incoming webhooks and the message display-name override; **0012** (§3j)
notification preferences and the DND window; **0013** (§3k) synced client
settings; **0014–0015** attachment tombstones and reclaim reason; **0016** (§3l)
the audit log; **0017** (§3m) federated enrollment; **0018** (§3n) push device
tokens.

*Corrected: an earlier revision of this line said presence, notification config,
and attachments were "intentionally not here yet." Notification config landed in
0012 and attachments in 0009. **Presence and typing remain deliberately
schema-less** — they are ephemeral in-memory net-thread state by design
(ARCH-67/68) and will never get a table.*

Section ordering note: §3 onward is numbered by the migration's *documentation*
section rather than strictly by migration number, so the attachment-tombstone
entries (0014/0015) appear before §3's 0002. The list above is the authoritative
order.

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
A tenant user, provisioned by **either** auth mode (REQ-023) — `subject` is the
unique identity key, namespaced by source: `local:<username>` or
`oidc:<issuer>|<sub>` (ARCH-19; an earlier revision of this line said "resolved
from the OIDC token," which predates local mode). `id` is the tenant-local
`user_id` carried in `AUTH_OK` and `BROADCAST.author_id`. Columns added by later
migrations — `role`/`avatar_key` (0002), `disabled` (0003), and the DND window
(0012) — are documented in their own sections below.

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


### Migration 0014 — attachment tombstones

`attachments.reclaimed_at_ms` (0 = the blob is present) plus an index on
`(reclaimed_at_ms, created_at_ms)` for the maintenance pass's oldest-first scan
(REQ-215/217, ARCH-77/78).

When storage pressure evicts a blob or it ages past the configured maximum, the
**bytes** go but the **row stays**, stamped with when it was reclaimed. A
client then renders "no longer available" instead of failing an opaque
download, and message history (REQ-053) is untouched — only attachment bytes
are ever removed, never messages.

Migration 0015 adds `reclaim_reason` (1 orphan, 2 aged out, 3 evicted under
pressure). The table already recorded *when* a blob went; this records *which
tier took it*, which makes REQ-215's audit trail a query against a table we
already keep rather than a second, ever-growing log — and eviction is
default-on and destroys data nobody approved individually, so being able to ask
exactly what it took matters.

Ordering note: the row is tombstoned by the writer *before* the transfer pool
deletes the bytes. A crash between the two leaves an orphaned blob, which the
next orphan sweep collects. That asymmetry is deliberate — dangling metadata
pointing at bytes that are gone is a visible bug, whereas a stray blob is
merely wasted space.

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

### `server_identity`
- `id` (INTEGER PK, `CHECK (id = 1)`) — a single-row table; the daemon has one
  identity.
- `cert_pem` / `key_pem` (TEXT) — the self-signed certificate and its private
  key, PEM-encoded. This is the one private key in the daemon's database; it is
  what makes the pin survive a restore onto a new box (ARCH-66b).
- `created_at_ms` (INTEGER) — when the identity was generated.

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
- `created_at_ms` (INTEGER) — upload time; drives the orphan sweep of pending rows
  whose blob was never finalized/linked (time-gated, like `sent_messages`
  pruning, ARCH-44/70).

Indexed by `message_id` (fetch a message's attachments for delivery/backfill)
and by `(uploader_id, created_at_ms)` (sweep abandoned pending uploads). A committed
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
  per key. `client_type` (`tui` and `gui` today) partitions the store so
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

## 3l. Migration 0016 — audit log (REQ-251, ARCH-79)

A bounded, queryable record of security-sensitive events (auth failures, admin and
moderation actions, account changes) for owner/admin review — never message content.

### `audit_log`
- `id` (INTEGER PK, AUTOINCREMENT); `at_ms` (event time).
- `family` (INTEGER) — 1 admin, 2 account, 3 security, 4 moderation.
- `action` (TEXT) — e.g. `auth.failed`, `webhook.create`.
- `actor_id` / `actor_name` — the actor (NULL id for an unauthenticated event; the name
  is denormalized so a later-removed actor is still legible).
- `target_id` / `target`; `outcome` (1 ok, 0 denied/failed); `detail` (TEXT).
- Indexes on `(family, at_ms)` and `(at_ms)`. Per-family flood cap + paging are applied
  by the query path, not the schema.

## 3m. Migration 0017 — federated enrollment (ARCH-84)

A single-row table holding the box's enrollment identity, so an enrolled/federated box
keeps its audience + key across restarts (the same key-survival pattern as the TLS
identity, ARCH-66b).

### `enrollment`
- `id` (INTEGER PK, `CHECK (id = 1)`) — one row.
- `privkey_pem` (TEXT) — the daemon's ECDSA-P256 enrollment private key (the public half
  lives only in the control plane).
- `audience` (TEXT) — the opaque `ws_…` audience id (also the OIDC `aud`).
- `state` (TEXT) — `pending` | `active`.
- `activated_at_ms` (nullable) / `created_at_ms`.

## 3n. Migration 0018 — push device tokens (REQ-132/133, ARCH-85)

The daemon owns the mobile-push device registry — the control-plane gateway is a
stateless relay that stores nothing (REQ-041). A client registers its APNs/FCM token
over its authenticated connection (`REGISTER_DEVICE_TOKEN`), the emitter reads it to
decide who to notify, and prunes it when the gateway reports it stale.

### `device_tokens`
- `id` (INTEGER PK, AUTOINCREMENT).
- `user_id` (INTEGER → `users.id`) — the owner; a removed member's rows go with them.
- `platform` (TEXT) — `apns` | `fcm`.
- `token` (TEXT) — the provider device token (an opaque device handle, not identity).
- `created_at_ms` / `last_seen_ms` (INTEGER) — first registration / last refresh.
- `UNIQUE(user_id, token)` — re-registering the same token upserts `last_seen_ms`;
  index on `user_id` for the recipient join.

---

## 4. Deferred to later migrations

Tracked here so the omissions are deliberate, not forgotten:

- **Thread notifications** (REQ-061) — still deferred; needs @mentions (REQ-221),
  which is the same dependency blocking the `MENTIONS` notification level
  (ARCH-72/85).
- **Rich text, pins, saved items, permalinks, profiles, retention policy**
  (REQ-220–234, REQ-240/241, REQ-250) — forward scope, none yet backed by an ARCH
  decision.
- **Screenshare** (REQ-161) — needs no schema; it is ephemeral media state on the
  same relay path as audio (ARCH-86, [VIDEO.md](./VIDEO.md)).

(Previously listed here and **now built**: roles, sessions/revocation, local
credentials, delivery cursors, server identity, and attachment metadata
(migrations 0002, 0007–0009); **push device tokens** for APNs/FCM (migration 0018,
ARCH-85) — the notification *settings* they are gated by are migration 0012.
**Audio** call state (REQ-150–152) was correctly predicted to need no schema: the
roster is ephemeral net-thread state (ARCH-73). **Presence/typing** (REQ-120/121)
likewise stayed in-memory by design, ARCH-67/68.)
