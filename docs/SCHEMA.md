# OpenChime — Database Schema

The SQLite schema (ARCH-2) and how it evolves. The migration *mechanism* is
ARCH-27; the *content* below is applied by migration 0001. New tables/columns
arrive as later numbered migrations, never as edits to an existing one.

**Status.** **Migrations 0001–0040 are applied** (`daemon/migrate.c`,
`OC_MIGRATIONS`). 0001 establishes the core
messaging tables; **0002** (§3) the authentication data model (sessions, local
credentials, invites, `users` role/avatar, [AUTH.md](./AUTH.md)); **0003** (§3a) the
`users.disabled` lockout flag; **0004** (§3b) reactions; **0005** (§3c) threads;
**0006** (§3d) FTS5 search; **0007** (§3e) delivery cursors; **0008** (§3f) the
persisted server TLS identity; **0009** (§3g) attachments; **0010–0011** (§3h/§3i)
incoming webhooks and the message display-name override; **0012** (§3j)
notification preferences and the DND window; **0013** (§3k) synced client
settings; **0014–0015** attachment tombstones and reclaim reason; **0016** (§3l)
the audit log; **0017** (§3m) federated enrollment; **0018** (§3n) push device
tokens; **0019** (§3o) the DM participant-set key; **0020** (§3p) the
unique channel name; **0021** (§3q) resolved @mentions; **0022** (§3r) pinned messages; **0023** (§3s) the channel-files index; **0024** (§3t) channel topic + archive; **0025** (§3u) saved items;
**0026** (§3v) channel mute + the delivery-cursor index; **0027** (§3w) profile
depth — custom status with expiry, title, timezone, avatar; **0028** (§3x) the
global notification default; **0029** (§3y) custom emoji; **0030–0031** (§3z)
drafts, then unaddressed drafts; **0032** (§3aa) scheduled messages;
**0033–0035** (§3j) the notification pause, the per-weekday schedule that
replaces the DND window, and keywords + priority people; **0036** (§3ab) thread
follows and per-thread read cursors; **0037** the attachment idempotency token;
**0038** (§3ac) link unfurls; **0039** (§3ad) the rest of the profile;
**0040** (§3ae) what a forward points at.

*Presence and typing are deliberately
schema-less — ephemeral in-memory net-thread state by design
(ARCH-67/68) — and will never get a table.*

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
`oidc:<issuer>|<sub>` (ARCH-19). `id` is the tenant-local
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
— one for a self-DM (notes to self, REQ-055), two for a 1:1, or **three to nine
for a group DM** (REQ-056) — via `OPEN_DM` / `OPEN_GROUP_DM`, so
messaging/backfill/search reuse the ordinary membership path. A group DM is not a
second kind: it is the same `kind='dm'` row under the same `dm_key`, which is why
no migration and no second membership path were needed. The channel-management ops act only on `kind='channel'`.

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

### `users` — read path (REQ-289)
`title`, `timezone`, `status_emoji` and `status_text` travel on `USER_LIST`
as well as `PROFILE_INFO` — `PROFILE_INFO` is sent only to the
user who edited them, so the roster is the one place every client learns
everyone else's. Expired status reads as absent
here too, applying `build_profile`'s rule in the second place that needs it.

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
REQ-090. A `delivery_cursors` row per `(user, channel)` advanced by
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
- `idem_token` (BLOB, nullable, migration 0037) — the 16-byte token from
  `UPLOAD_BEGIN`, with a **partial** `UNIQUE (channel_id, uploader_id,
  idem_token) WHERE idem_token IS NOT NULL`, which is what makes a retried
  declaration return the first row instead of minting a second. The **uploader
  is in the key**, where `sent_messages` needs only `(channel, token)`: replaying
  a SEND token returns a message id, while replaying this one returns a storage
  key the net loop opens for *writing*, so a lookup that ignored the uploader
  would let one member stream over another's attachment. The row already records
  its owner as the only user allowed to link it (above), and the index matches
  the lookup exactly so the two cannot disagree about what "already declared"
  means. Held here rather than in a
  `sent_messages`-style side map because an attachment is *reclaimable*
  (REQ-215/217): a map row would outlive the attachment the storage sweep
  deleted and hand a client back an id for something gone. NULL on every row
  predating the migration, and NULLs do not collide in a UNIQUE index.
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
- `(user_id, channel_id)` (PK) — one row per user per channel.
- `level` (INTEGER 0/1/2) — 0 all, 1 mentions-only, 2 none. An **absent row falls
  back to `users.notify_default`** (migration 0028, REQ-134), not to a hardcoded
  "all", so the table stays sparse and the per-user default is what fills the
  gap: the push query reads `COALESCE(np.level, u.notify_default)`.
- `muted` (INTEGER 0/1, migration 0026) — distinct from `level`. Level decides
  whether the daemon notifies; mute additionally de-emphasises the sidebar row
  and drops the conversation from the unread badge (REQ-137). Push excludes a
  muted channel unconditionally, so mute outranks both the level and a priority
  person.

### `users` (added columns)
- `dnd_mode` (INTEGER 0-3, migration 0034) — the notification schedule (REQ-136):
  `0` off, `1` every day, `2` weekdays, `3` custom. It **replaced** `dnd_enabled`,
  which is dropped.
- `allow_start_min`, `allow_end_min` (INTEGER) — the daily window as minutes of
  day, wrapping past midnight when `start > end`. Renamed from `dnd_start_min`/
  `dnd_end_min` in migration 0034 **because the sense flipped**: this is the range
  in which notifications are **allowed**, the complement of the quiet window it
  replaced. Existing values were swapped as they were carried forward. Suppresses
  push, not in-app unread.
- `tz_offset_min` (INTEGER) — minutes east of UTC. A per-weekday window is only
  meaningful on the user's local calendar day, and the daemon cannot resolve an
  IANA zone per push (ARCH-103). **The app-core refreshes it on every connect**
  (`SET_TZ_OFFSET`, sent straight after `AUTH_OK` so no frontend has to
  remember to), and it also rides `SET_SCHEDULE` — so travel and
  daylight-saving turnover correct themselves at the next connect.

### `notify_schedule` (migration 0034)
- `(user_id, weekday)` (PK), `enabled`, `start_min`, `end_min` — used by
  **custom** mode only. Sparse on purpose: no row for a day means that day is
  quiet, so a schedule listing Monday to Friday is a statement about the weekend
  too rather than an omission.

### `thread_follows`, `thread_reads` (migration 0036)
- `thread_follows (user_id, root_id)` + `channel_id`, `state` — **overrides only**
  (REQ-062, ARCH-104). Participation is derived from `messages.parent_id`: you are
  in a thread if you wrote its root or a reply. `state` 1 is an explicit follow of
  one you did not write in, `state` 0 an explicit unfollow of one you did — and
  the unfollow outranks participation, which is what "turn off replies" means.
- `thread_reads (user_id, root_id, last_read_reply_id)` — a per-thread cursor,
  advancing only. It cannot be derived from `delivery_cursors`: that advances when
  the CHANNEL is read, and thread replies are deliberately not in the main scroll
  (REQ-060), so the channel cursor sweeps past replies nobody has seen.

### `notify_keywords`, `priority_people` (migration 0035)
- `notify_keywords (user_id, term)` — one term per row, stored lowercased.
  Matching is case-insensitive and **exact** ("deploy" does not match
  "deployment"), done by `shared/mention.c` and never by SQL, because the client
  has to highlight precisely what the server notified on (REQ-135, ARCH-103).
- `priority_people (user_id, person_id)` — a relation, not a level: every other
  notification setting says *where*, this one says *who*. Pierces a level and a
  pause, never a mute.
- A keyword hit is written into **`mentions`** with `kind = 4`, so the push
  query, the activity feed and the reader's highlight all keep working with no
  further change.
- `dnd_until_ms` (INTEGER, migration 0033) — a **pause** (REQ-278): the absolute
  instant it ends, `0` for none. A different type from the window above, not a
  longer version of it — the window is periodic by construction, this is one
  moment, and neither can express the other, which is why "do not disturb until
  5pm" was unbuildable with the window alone.

  **Enforced on read**, the pattern migration 0027 proved for status expiry: a
  stamp that has passed reads as absent everywhere (the push worker, the auth
  result, the prefs snapshot), so a client that is not running to clear its own
  state costs nothing and there is no sweep to keep correct. Ending a pause early
  writes `0` — the same op with 0 minutes, not a second one.

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

## 3o. Migration 0019 — a DM's participant set is its identity (REQ-050/055)

Adds `channels.dm_key` and a **partial unique index** over it
(`WHERE dm_key IS NOT NULL`, so named channels are unconstrained).

- `dm_key` (TEXT) — the participant ids, sorted, as `"1"` for a self-DM or
  `"1,2"` for a pair. Written by `process_open_dm` and matched with one indexed
  probe.

*Why.* A DM identified only by its membership rows — a count-join asserting
"exactly these participants" — is fragile: **anything that deletes a membership
row leaves a DM the lookup can no longer match**, and the next
`OPEN_DM` creates a *duplicate conversation*. Making the participant set a
unique key means the duplicate state cannot be represented at all, rather than
being prevented by every write path remembering to.

The backfill uses `MIN`/`MAX` rather than `group_concat`, whose ordering SQLite
does not guarantee — the key must be byte-identical to the one the daemon
computes. A DM holds one participant (self-DM), two, or up to nine for a group DM
(`OC_MAX_GROUP_DM` is 8 others plus the caller) — the same key over a longer
sorted list, which is exactly why group DMs (REQ-056) needed no migration.

*Pre-existing violations are deleted, not merged* — this shipped before any
release, and merging two conversations is a judgement no migration should make
silently. Member-less DM channels go; where a participant set is duplicated only
the oldest channel survives, with the others' messages and membership removed.

---

## 3p. Migration 0020 — a channel name is unique (REQ-040)

Adds a **partial unique index** on `lower(name)` `WHERE kind='channel'`, so two
`#test` channels cannot coexist and `#Test` cannot shadow `#test`. DMs are
excluded because their `name` is legitimately NULL.

*Why.* Without the index, same-named channels accumulate and are
indistinguishable in the sidebar. This is the same defect class as
§3o: the constraint belongs in the schema, where it cannot be forgotten, rather
than in the one write path that happens to remember it. The daemon also
pre-checks and returns `CHANNEL_EXISTS` (3017) so the client gets a usable
message instead of a constraint failure.

*Pre-existing duplicates are deleted, keeping the lowest id* — the same
pre-release reasoning as §3o. Deletion walks every table that references
`channels` or the affected `messages` (`sent_messages`, `reactions`,
`attachments`, `webhooks`, `notification_prefs`, `delivery_cursors`,
`channel_members`) in dependency order: a missed reference aborts the migration
on a foreign-key constraint, which fails the daemon at boot rather than
degrading.

---

## 3q. Migration 0021 — resolved @mentions (REQ-221, ARCH-89)

```sql
CREATE TABLE mentions (
  message_id INTEGER NOT NULL REFERENCES messages(id),
  channel_id INTEGER NOT NULL REFERENCES channels(id),
  user_id    INTEGER REFERENCES users(id),   -- NULL for a broadcast audience
  kind       INTEGER NOT NULL,               -- 0 user, 1 here, 2 channel, 3 everyone
  span_start INTEGER NOT NULL,               -- byte offset of '@' in the body
  span_len   INTEGER NOT NULL,
  created_at_ms INTEGER NOT NULL);
```

with indexes on `(user_id, message_id)` (a user's mention feed, REQ-139),
`(message_id)` (the push gate) and `(channel_id, message_id)`.

*Why a table and not a body encoding.* The body stays plain UTF-8 (REQ-054) with
the literal `@alice`, so it remains readable in the database, in a webhook
payload, in a log line, and to FTS5 (§3d). The resolved id — the part that must
survive a display-name change — lives here beside it, which is what ARCH-89
records. `user_id` is NULL for `@here`/`@channel`/`@everyone`: the audience is
the channel's membership, which is already a table.

*Rows are written by the daemon at send time*, resolving each name
case-insensitively against `users JOIN channel_members` for that channel — a name
that is not a member of the channel is not a mention, so nobody is notified about
a message they cannot open. `remove_user` (REQ-033) deletes a departing user's
mention rows before their messages, in the dependency order §3p established.

---

## 3r. Migration 0022 — pinned messages (REQ-230, ARCH-90)

```sql
CREATE TABLE pins (
  message_id    INTEGER PRIMARY KEY REFERENCES messages(id),
  channel_id    INTEGER NOT NULL REFERENCES channels(id),
  pinned_by     INTEGER REFERENCES users(id),   -- nullable: the pin outlives the pinner
  created_at_ms INTEGER NOT NULL);
CREATE INDEX idx_pins_channel ON pins(channel_id, created_at_ms DESC);
```

*Why `message_id` is the primary key.* A pin belongs to the **channel**, not to
the person who placed it — every member sees the same set — so a message is
pinned or it is not. Pinning twice is a no-op rather than a second row. This is
precisely what will separate a pin from a **saved item** (REQ-231), which is
per-user and private; the two read alike in a menu and are opposite in the
schema.

*`pinned_by` is nullable* for the same reason: it is attribution, not ownership,
and the pin must survive the account that placed it. Removing a user (REQ-033)
disables rather than deletes the row (§3), so this does not dangle in practice —
but a DM's pins are deleted along with its messages, in the dependency order
§3p established.

*`channel_id` is denormalised* from the message exactly as in §3q: listing a
channel's pins is the hot path and should not need a join to know what it can
see. The index is descending on time because the list is newest-pin-first.

*The 100-per-channel cap lives in the daemon*, not in a constraint: SQLite
cannot express "at most N rows per group" without a trigger, and the check has
to distinguish a new pin from a re-pin — which is application logic, and is
tested as such.

---

## 3s. Migration 0023 — reading a channel's files (REQ-143, ARCH-91)

```sql
CREATE INDEX idx_attachments_channel ON attachments(channel_id, created_at_ms DESC)
  WHERE message_id IS NOT NULL;
```

No new columns: §3g's `attachments` has carried `channel_id` since it was
created. What was missing is an index for reading them *that* way — 0009 indexes
by message and by uploader, so "this channel's files, newest first" was a full
scan of every attachment in the workspace.

*Partial on `message_id`*: a pending upload has no message yet and is not a
shared file, so keeping those rows out of the index keeps it to exactly what the
query reads.

The member roster (REQ-031) needed no migration at all — `channel_members` is
keyed `(channel_id, user_id)`, so listing one channel's members already reads a
prefix of the primary key.

---

## 3t. Migration 0024 — channel topic and archive (REQ-034/035/036, ARCH-93)

```sql
ALTER TABLE channels ADD COLUMN topic TEXT;
ALTER TABLE channels ADD COLUMN archived_at_ms INTEGER;
CREATE INDEX idx_channels_archived ON channels(archived_at_ms);
```

*Two columns for three features.* **Rename needs no column at all** — the name
was always mutable and everything durable keys on `channels.id`, so a rename is
an `UPDATE`. There is deliberately **no name-history table**: ARCH-93 records why
(permalinks must key on ids).

*`archived_at_ms` non-NULL is the flag*, rather than a boolean beside a date. One
column cannot disagree with itself, "when was this archived" comes free, and
unarchive is a single NULL write.

*`topic`* is NULL or `''` for "none" — the writer stores NULL when given an empty
string, so the two spellings do not both end up in the table.

The index is on `archived_at_ms` because the sidebar's default query is "channels
I can see that are **not** archived".

---

## 3z. Migrations 0030–0031 — drafts, addressed and unaddressed (REQ-223/229, ARCH-101)

0030 created the table keyed on the conversation. **0031 rebuilt it** so a draft
may exist before it has a recipient (REQ-229's New Message pane autosaves before
you choose one), which the original primary key could not express:

```sql
CREATE TABLE drafts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  channel_id  INTEGER REFERENCES channels(id),   -- NULL = unaddressed
  thread_root INTEGER NOT NULL DEFAULT 0,
  recipients  TEXT,
  body        TEXT    NOT NULL,
  updated_ms  INTEGER NOT NULL);
CREATE UNIQUE INDEX idx_drafts_conv ON drafts(user_id, channel_id, thread_root)
  WHERE channel_id IS NOT NULL;
CREATE INDEX idx_drafts_user ON drafts(user_id, updated_ms DESC);
```

The addressed case keeps exactly its old shape through the **partial unique
index** rather than through the primary key, so one conversation still holds at
most one draft while an unaddressed draft — `channel_id` NULL, its `recipients`
beside it — is representable and repeatable. The surrogate `id` is what the wire
returns so a client can address a specific unaddressed draft.

*Its own table, not the `client_settings` bucket.* That bucket was the cheap
answer and is wrong twice: it is keyed `(user, client_type, key)` and
**partitioned per frontend by design**, so a draft written in the GUI would be
invisible in the TUI — the opposite of REQ-223's "synced across that user's
devices" — and its own notes here describe its contents as low-contention
*prefs*, while a draft is the one thing we would put there that the user typed as
a **message**.

*Two indexes, each with a job.* `idx_drafts_conv` enforces one draft per
conversation (and only for addressed drafts, which is what the `WHERE` clause
buys); `idx_drafts_user` orders "all my drafts, newest first", which is what the
Drafts pane asks for.

*`thread_root` is in the key from the start*, 0 meaning the channel itself. No
client can write another value yet — the thread pane shares the one composer —
but the costs are asymmetric: one column now, against a migration on a table of
**user content** plus a change to two wire ops that shipped clients already
speak.

*Deleted only when the thing it belongs to is gone*: on send (in the send's own
transaction), and with the user on removal. **Not** on archive and not on
leaving, both of which are reversible (REQ-035) — discarding typed content for an
undoable act is a permanent consequence of a temporary one. A draft for a channel
you are not in is simply not listed until you return.

---

## 3u. Migration 0025 — saved items, and the activity watermark (REQ-231/139, ARCH-95)

```sql
CREATE TABLE saved_items (
  user_id       INTEGER NOT NULL REFERENCES users(id),
  message_id    INTEGER NOT NULL REFERENCES messages(id),
  created_at_ms INTEGER NOT NULL,
  PRIMARY KEY (user_id, message_id));
CREATE INDEX idx_saved_user ON saved_items(user_id, created_at_ms DESC);

ALTER TABLE users ADD COLUMN activity_seen_ms INTEGER NOT NULL DEFAULT 0;
```

*The mirror of pins.* §3r keys a pin on the **message alone** because a pin
belongs to the channel; a saved item is keyed on **(user, message)** because it
belongs to a person. Same gesture, opposite ownership — and the reason ARCH-90
wrote the distinction down before this table existed.

*The activity feed has no table.* It is a query over rows that already exist and
are already indexed (`idx_mentions_user` was built by §3q for exactly this read).
What a query cannot cheaply carry is read state, so `activity_seen_ms` is the
whole of it: one watermark per user, stamped when the feed is opened, enough to
mark what is new. Per-item read/dismiss is the point at which a table would earn
its place — see ARCH-95.

---

## 3v. Migration 0026 — mute, and the read-cursor index (REQ-137/235)

```sql
ALTER TABLE notification_prefs ADD COLUMN muted INTEGER NOT NULL DEFAULT 0
  CHECK (muted IN (0,1));
CREATE INDEX IF NOT EXISTS idx_cursors_user ON delivery_cursors(user_id, channel_id);
```

*Mute is a column, not a fourth level.* `level` decides whether the daemon
notifies; mute additionally de-emphasises the sidebar row and drops the unread
badge. The two are independent because a user can want a channel quiet but still
countable, or countable but silent.

*The index serves mark-unread.* `process_client_ack` upserts
`MAX(message_id, excluded.message_id)`, so the read cursor is monotonic by
construction and a replayed ack can never move it backwards — a correctness
property worth keeping, which is why marking unread gets its own op
(`SET_READ_CURSOR`) rather than reusing the ack. The cursor needs no schema
change; this index is what makes recomputing "unread from here" cheap.

## 3w. Migration 0027 — profile depth and custom status (REQ-240/241/122)

```sql
ALTER TABLE users ADD COLUMN status_emoji TEXT;
ALTER TABLE users ADD COLUMN status_text TEXT;
ALTER TABLE users ADD COLUMN status_expires_ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN title TEXT;
ALTER TABLE users ADD COLUMN timezone TEXT;
ALTER TABLE users ADD COLUMN avatar_attachment_id INTEGER;
```

All on `users`: they are facts about a person and there is exactly one row per
person, so a side table would buy nothing but a join.

`status_expires_ms` is why a status is not two text columns. "In a meeting until
3pm" has to stop being true on its own, and a client cannot be trusted to clear
it because it may not be running — so the daemon **enforces expiry on read**, and
`0` means no expiry. That pattern is reused for the notification pause (§3j,
migration 0033).

The avatar is an **attachment id**, not a blob column: the attachment store
already handles upload, the size cap, dedup by hash and reclamation (REQ-215,
ARCH-78), and a second image path would reimplement all of it. Reclamation
excludes avatars, which building them exposed.

## 3x. Migration 0028 — the global notification default (REQ-134)

```sql
ALTER TABLE users ADD COLUMN notify_default INTEGER NOT NULL DEFAULT 0
  CHECK (notify_default IN (0,1,2));
```

`notification_prefs` is a per-channel **override**, so a channel the user has
never touched had no answer but a compiled-in constant — which the user could not
change. One column on `users` supplies it, per-channel rows keep overriding it,
and the push query reads `COALESCE(np.level, u.notify_default)`. Storage is on
`users` rather than a sentinel row in `notification_prefs`: a default is a
property of the person, and a `channel_id` of 0 in a table keyed by channel is a
trap for every later query.

## 3y. Migration 0029 — custom emoji (REQ-072)

```sql
CREATE TABLE custom_emoji (
  name          TEXT PRIMARY KEY,
  attachment_id INTEGER NOT NULL REFERENCES attachments(id),
  created_by    INTEGER NOT NULL REFERENCES users(id),
  created_at_ms INTEGER NOT NULL);
```

**The name is the identity** and the namespace is flat and workspace-wide, so
`:shipit:` means one image or every message containing it is ambiguous. A
duplicate is refused rather than replacing the image existing messages already
refer to. The image is an attachment id for the same reason an avatar is (§3w).
Any member may add one; deleting is the creator or an admin, because deletion
breaks every message that used the shortcode.

## 3aa. Migration 0032 — scheduled messages (REQ-224, ARCH-102)

```sql
CREATE TABLE scheduled_messages (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  channel_id  INTEGER REFERENCES channels(id),
  thread_root INTEGER NOT NULL DEFAULT 0,
  recipients  TEXT,
  body        TEXT NOT NULL,
  send_at_ms  INTEGER NOT NULL,
  created_ms  INTEGER NOT NULL,
  state       TEXT NOT NULL DEFAULT 'pending'
              CHECK (state IN ('pending','sent','failed')),
  fail_reason TEXT,
  message_id  INTEGER);
CREATE INDEX idx_sched_due  ON scheduled_messages(send_at_ms) WHERE state='pending';
CREATE INDEX idx_sched_user ON scheduled_messages(user_id, send_at_ms);
```

**Its own table, not a flag on `messages`.** A scheduled message is not a message
yet: nothing can link to it, it is invisible to search and to every member but its
author, and it may never exist at all. A "pending" flag would make every history,
search, backfill, unread and mention query responsible for excluding it, and the
first one that forgot would leak unsent text into somebody else's transcript.

The partial `idx_sched_due` is what the ~15 s firing sweep reads
(`OPENCHIME_SCHED_TICK_MS`); `state` carries the outcome, and `fail_reason` is
shown to the author, because a message promised and not sent is the one case
where silence is indefensible.

## 3ac. Migration 0038 — link unfurls (REQ-222, ARCH-105)

```sql
CREATE TABLE unfurls (
  message_id    INTEGER NOT NULL REFERENCES messages(id),
  channel_id    INTEGER NOT NULL REFERENCES channels(id),
  url           TEXT    NOT NULL,
  title         TEXT,
  descr         TEXT,
  created_at_ms INTEGER NOT NULL,
  PRIMARY KEY (message_id, url));
```

**Completed fetches only — there is no pending state.** A fetch that fails or
never finishes leaves nothing to clean up, nothing to sweep, and nothing to
replay. The primary key makes a re-fetch (after an edit re-adds a URL, or on the
`INSERT OR REPLACE` a duplicate store performs) an upsert rather than a second
row.

*`channel_id` is denormalised* from the message exactly as `mentions` (§3q) and
`pins` (§3r) do it: the backfill replay reads unfurls by message and must not
join per row. *No thumbnail column* — og:image is deferred (ARCH-105), and a
column now would pretend otherwise. Rows are deleted with the message's other
body-attached state on tombstone (§3r's reasoning) and wholesale on edit, since
they describe the old body; the net thread re-fetches from the new one.

## 3ad. Migration 0039 — the rest of the profile (REQ-240)

```sql
ALTER TABLE users ADD COLUMN full_name TEXT;
ALTER TABLE users ADD COLUMN phone TEXT;
ALTER TABLE users ADD COLUMN pronouns TEXT;
```

All on `users`, for the reason 0027's fields are: they are facts about a person,
there is exactly one row per person, and a side table would buy nothing but a
join.

*`full_name` beside `display_name`* is the split the reference product makes,
and the two answer different questions — what you are called on paper, and what
a transcript calls you. Neither substitutes for the other, which is why one
column could not serve both.

*Two of the three ride the roster, one does not.* `full_name` and `pronouns` are
read beside a name, so they travel on `USER_LIST` as well as `PROFILE_INFO` —
the People directory (REQ-289) can learn them nowhere else, since `PROFILE_INFO`
reaches only the person who edited it. **`phone` stays off the roster**: it is
contact detail, and every member's number in every fan-out is a size and privacy
cost nobody asked for.

*All nullable, no defaults.* An absent value means the person has not said,
which is a different thing from an empty string they typed and cleared.

*`timezone` is unaffected and stays what it was* — the human-readable fact for
the profile. Quiet hours run on `users.tz_offset_min`, which the client
refreshes from the OS on every connect (ARCH-103); a zone chosen on the profile
screen says where somebody is, not when to stop notifying them.

## 3ae. Migration 0040 — what a forward points at (REQ-057, ARCH-108)

```sql
CREATE TABLE forwards (
  message_id  INTEGER PRIMARY KEY REFERENCES messages(id),
  src_channel INTEGER NOT NULL,
  src_message INTEGER NOT NULL,
  src_author  INTEGER NOT NULL,
  excerpt     TEXT    NOT NULL,
  n_attach    INTEGER NOT NULL,
  attach_name TEXT    NOT NULL DEFAULT ''
);
```

*A side table, not columns on `messages`*, following `unfurls` (§3ac): almost no
message is a forward, and five sparse columns on the hot table would be paid for
on every row to serve a rare one. One row per message, so the primary key is the
message.

*`src_channel` and `src_message` are deliberately NOT foreign keys.* The source
may be tombstoned (REQ-052) or its channel archived, and a forward has to outlive
both — it records what was forwarded, it is not a live pointer. A foreign key
would make deleting the original either fail or silently erase the record that
the forward ever happened.

*`src_author`, `excerpt` and `n_attach` are a snapshot* taken when the forward
was sent, and all three are resolved **by the daemon** from the source row rather
than taken from the client: what the destination is told about the original is
what the daemon read, not what the sender claimed. Editing the original
afterwards does not rewrite what was forwarded.

*`n_attach` counts the source's files and `attach_name` holds the first one's
name.* That is what a card can show — five filenames is a wall, "report.txt and
4 more" is a sentence. The files themselves stay on the source message
(`attachments.message_id`, §3g, is one row, one message), so the card **names**
them and the card itself opens the original. Copying would mean either a second
row sharing a `storage_key`, which breaks the orphan model reclamation counts on
(ARCH-77/78), or duplicating the bytes.

## 3ab. Migration 0036 — thread follows and per-thread reads (REQ-062, ARCH-104)

Documented with the notification tables in §3j, since they arrived together.

---

## 4. Deferred to later migrations

Tracked here so the omissions are deliberate, not forgotten:

- **Thread notifications** (REQ-061) — still deferred. Its dependency, @mentions
  (REQ-221), is now built (§3q), so what remains is deciding *when a reply
  notifies a thread's participants*, not the mention machinery underneath it.
- **Polls** (REQ-225) — forward scope, no ARCH decision and no schema.
  **Snippets** (REQ-226) have half of what they need:
  fenced code blocks parse and render (REQ-220), but a titled snippet *object*
  does not exist.
- **Retention policy** (REQ-250) — opt-in message ageing, distinct from REQ-217's
  attachment max age (built). No schema, no ARCH decision.
- **Legal hold** (REQ-252), **compliance capture** (REQ-276) and **DLP**
  (REQ-277) — scoped in REQUIREMENTS.md, no schema.

- **Screenshare** (REQ-161) — needs no schema; it is ephemeral media state on the
  same relay path as audio (ARCH-86, [VIDEO.md](./VIDEO.md)).
