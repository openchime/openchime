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

/* 0005: message threads (REQ-060). `parent_id` links a reply to the top-level
 * message it threads under (NULL for a top-level message). Threads are flat: a
 * reply to a reply resolves to the same root, so parent_id always names a
 * top-level message. The default-NULL column keeps the ADD COLUMN a metadata-
 * only change (existing rows become top-level). */
static const char MIGRATION_0005[] =
    "ALTER TABLE messages ADD COLUMN parent_id INTEGER REFERENCES messages(id);"
    "CREATE INDEX idx_messages_parent ON messages(parent_id, id);";

/* 0006: full-text search over message bodies (REQ-080, ARCH-15). An external-
 * content FTS5 table indexes messages.body without duplicating it; the three
 * triggers keep it in sync (insert/tombstone/edit), and the seed indexes rows
 * that predate this migration. Requires an SQLite built with FTS5 (Alpine's
 * sqlite-libs and the vendored/system libs used here all enable it). */
static const char MIGRATION_0006[] =
    "CREATE VIRTUAL TABLE messages_fts USING fts5("
    "  body, content='messages', content_rowid='id');"

    "INSERT INTO messages_fts(rowid, body) SELECT id, body FROM messages;"

    "CREATE TRIGGER messages_fts_ai AFTER INSERT ON messages BEGIN"
    "  INSERT INTO messages_fts(rowid, body) VALUES (new.id, new.body);"
    "END;"
    "CREATE TRIGGER messages_fts_ad AFTER DELETE ON messages BEGIN"
    "  INSERT INTO messages_fts(messages_fts, rowid, body) VALUES('delete', old.id, old.body);"
    "END;"
    "CREATE TRIGGER messages_fts_au AFTER UPDATE ON messages BEGIN"
    "  INSERT INTO messages_fts(messages_fts, rowid, body) VALUES('delete', old.id, old.body);"
    "  INSERT INTO messages_fts(rowid, body) VALUES (new.id, new.body);"
    "END;";

/* 0007: server-side delivery accounting (REQ-090). A per-(user, channel) cursor
 * of the highest contiguously-acked message id (CLIENT_ACK). Lets the daemon
 * catch a returning client up from where it left off when it backfills without
 * supplying its own cursors, and records at-least-once delivery state. */
static const char MIGRATION_0007[] =
    "CREATE TABLE delivery_cursors ("
    "  user_id       INTEGER NOT NULL REFERENCES users(id),"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  message_id    INTEGER NOT NULL,"           /* highest cumulatively acked */
    "  updated_at_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (user_id, channel_id)"
    ");";

/* 0008: persist the daemon's self-signed TLS identity (ARCH-66b) so the TOFU
 * fingerprint (ARCH-10) survives the database being restored onto a new box. The cert+key live in
 * the replicated database; on a cold restore the daemon reloads the same
 * identity instead of generating a new one (which would trip every client's
 * pin). Single row (id=1). */
static const char MIGRATION_0008[] =
    "CREATE TABLE server_identity ("
    "  id            INTEGER PRIMARY KEY CHECK (id = 1),"
    "  cert_pem      TEXT NOT NULL,"
    "  key_pem       TEXT NOT NULL,"
    "  created_at_ms INTEGER NOT NULL"
    ");";

/* 0009: file attachments (REQ-140/141, ARCH-69/70). Blob bytes live in object
 * storage, never here — this table holds only the pointer (`storage_key`) and
 * metadata. `message_id` is NULL while an attachment is pending (uploaded but not
 * yet referenced by a message); it is set when a SEND links it, at which point
 * access control follows the message's channel (REQ-141). The two indexes serve
 * delivery (fetch a message's attachments) and the orphan sweep of pending rows
 * whose upload was abandoned (time-gated, like sent_messages pruning). */
static const char MIGRATION_0009[] =
    "CREATE TABLE attachments ("
    "  id            INTEGER PRIMARY KEY,"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  message_id    INTEGER REFERENCES messages(id),"   /* NULL while pending */
    "  uploader_id   INTEGER NOT NULL REFERENCES users(id),"
    "  storage_key   TEXT NOT NULL,"
    "  filename      TEXT NOT NULL,"
    "  mime          TEXT NOT NULL,"
    "  size          INTEGER NOT NULL,"
    "  sha256        BLOB,"
    "  created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE INDEX idx_attachments_message ON attachments(message_id);"
    "CREATE INDEX idx_attachments_pending ON attachments(uploader_id, created_at_ms);";

/* 0010: incoming webhooks (REQ-170, ARCH-32). A webhook is a per-channel secret
 * that lets an uncontrolled third party post a message without a user session.
 * The token is stored hashed (SHA-256), like sessions — the raw value is shown
 * once at creation. Posts are attributed to the creating user (`creator_id`).
 * The unique token_hash index makes the HTTP-side lookup a single indexed probe. */
static const char MIGRATION_0010[] =
    "CREATE TABLE webhooks ("
    "  id            INTEGER PRIMARY KEY,"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  creator_id    INTEGER NOT NULL REFERENCES users(id),"
    "  token_hash    BLOB NOT NULL,"
    "  label         TEXT NOT NULL DEFAULT '',"
    "  disabled      INTEGER NOT NULL DEFAULT 0 CHECK (disabled IN (0,1)),"
    "  created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE UNIQUE INDEX idx_webhooks_token ON webhooks(token_hash);";

/* 0011: an optional per-message display-name override (REQ-170). NULL for an
 * ordinary message (the client renders the author's own name); set to the
 * webhook's label for a webhook post, so it shows as e.g. "GitHub CI" rather
 * than the human who created the webhook. A metadata-only default-NULL add. */
static const char MIGRATION_0011[] =
    "ALTER TABLE messages ADD COLUMN author_name TEXT;";

/* 0012: notification preferences (REQ-130/131). Per-channel notification level
 * (0=all, 1=mentions, 2=none) — an absent row means the default (all). Plus a
 * per-user do-not-disturb window as columns on `users`: a daily [start,end)
 * range in minutes-of-day (UTC; the client converts from local), wrapping past
 * midnight when start > end. These are the server-authoritative settings that
 * clients honor and the future push gateway (REQ-132/133) will consult; DND
 * suppresses push, not in-app unread (REQ-131). */
static const char MIGRATION_0012[] =
    "CREATE TABLE notification_prefs ("
    "  user_id    INTEGER NOT NULL REFERENCES users(id),"
    "  channel_id INTEGER NOT NULL REFERENCES channels(id),"
    "  level      INTEGER NOT NULL DEFAULT 0 CHECK (level IN (0,1,2)),"
    "  PRIMARY KEY (user_id, channel_id)"
    ");"
    "ALTER TABLE users ADD COLUMN dnd_enabled INTEGER NOT NULL DEFAULT 0 CHECK (dnd_enabled IN (0,1));"
    "ALTER TABLE users ADD COLUMN dnd_start_min INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE users ADD COLUMN dnd_end_min INTEGER NOT NULL DEFAULT 0;";

/* Portable, device-synced client settings (the daemon half of the layered client
 * config). A row is one (user, client_type, key) -> value: the client_type
 * partitions the store into per-frontend buckets ("tui", a future "gui", …) so a
 * TUI's synced prefs don't collide with a GUI's. The daemon is opaque about the
 * keys/values — it stores and fans them back; the frontend decides their meaning.
 * Machine-local prefs stay in the client's home config file (never synced here). */
static const char MIGRATION_0013[] =
    "CREATE TABLE client_settings ("
    "  user_id     INTEGER NOT NULL REFERENCES users(id),"
    "  client_type TEXT NOT NULL,"
    "  key         TEXT NOT NULL,"
    "  value       TEXT NOT NULL,"
    "  updated_ms  INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (user_id, client_type, key)"
    ");";

/* 0014: attachment tombstones (REQ-215/217, ARCH-77/78). When storage pressure
 * evicts a blob or it ages past the configured maximum, the BYTES go but the row
 * stays, marked with the time it was reclaimed. A client then renders "no longer
 * available" instead of failing an opaque download, so the conversation stays
 * intelligible and message history (REQ-053) is untouched. Zero means the blob is
 * present; the index serves the maintenance pass's oldest-first scan. */
static const char MIGRATION_0014[] =
    "ALTER TABLE attachments ADD COLUMN reclaimed_at_ms INTEGER NOT NULL DEFAULT 0;"
    "CREATE INDEX idx_attachments_reclaim ON attachments(reclaimed_at_ms, created_at_ms);";

/* 0015: why a blob was reclaimed (REQ-215 auditability). The attachments table
 * already records WHEN via reclaimed_at_ms; this records WHICH TIER did it —
 * 1 orphan, 2 aged out, 3 evicted under pressure. That makes the audit trail a
 * query against a table we already keep rather than a second, ever-growing log:
 * an operator can ask exactly what eviction took and when, which matters
 * because eviction is default-on and destroys data nobody approved
 * individually. */
static const char MIGRATION_0015[] =
    "ALTER TABLE attachments ADD COLUMN reclaim_reason INTEGER NOT NULL DEFAULT 0;";

/* 0016: the audit log (REQ-251, ARCH-79). Its own table, sharing no schema or
 * query surface with messages. Append-only as a write policy — the daemon emits
 * only INSERT, and the single age-based prune of ARCH-78.
 *
 * `family` is what makes the cap safe: the catalog deliberately includes failed
 * authentication, whose volume an attacker controls, so each family ages out
 * against its own budget and a flood of security noise cannot evict
 * administrative history (REQ-251b). The index serves both the newest-first read
 * and that per-family prune.
 *
 * `detail` is a short human string. It must never carry the secret involved:
 * that a password changed, never the password. */
static const char MIGRATION_0016[] =
    "CREATE TABLE audit_log ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  at_ms      INTEGER NOT NULL,"
    "  family     INTEGER NOT NULL,"   /* 1 admin, 2 account, 3 security, 4 moderation */
    "  action     TEXT    NOT NULL,"
    "  actor_id   INTEGER,"            /* NULL when unauthenticated (a failed login) */
    "  actor_name TEXT,"               /* denormalized: the actor may later be removed */
    "  target_id  INTEGER,"
    "  target     TEXT,"
    "  outcome    INTEGER NOT NULL DEFAULT 1,"  /* 1 ok, 0 denied/failed */
    "  detail     TEXT"
    ");"
    "CREATE INDEX idx_audit_family_time ON audit_log(family, at_ms);"
    "CREATE INDEX idx_audit_time ON audit_log(at_ms);";

/* 0017: the daemon's federated-enrollment identity (CP-8). Generated once on
 * first boot and persisted so the keypair + audience survive restarts (like
 * server_identity, migration 0008): a new key would need a fresh operator courier
 * + re-activation. Single row (id=1). `privkey_pem` is the ECDSA P-256 private
 * key; `audience` is the opaque id central ratifies; `state` is 'pending' until
 * central acks activation, then 'active'. */
static const char MIGRATION_0017[] =
    "CREATE TABLE enrollment ("
    "  id              INTEGER PRIMARY KEY CHECK (id = 1),"
    "  privkey_pem     TEXT NOT NULL,"
    "  audience        TEXT NOT NULL,"
    "  state           TEXT NOT NULL,"
    "  activated_at_ms INTEGER,"
    "  created_at_ms   INTEGER NOT NULL"
    ");";

/* Push device tokens (ARCH-85, REQ-132/133). The daemon owns the device registry
 * — the control-plane push gateway is a stateless relay that stores nothing
 * (CP-13). One row per (user, provider device token); a removed member's rows go
 * with the user. `platform` is 'apns' | 'fcm'. */
static const char MIGRATION_0018[] =
    "CREATE TABLE device_tokens ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  user_id       INTEGER NOT NULL REFERENCES users(id),"
    "  platform      TEXT NOT NULL,"
    "  token         TEXT NOT NULL,"
    "  created_at_ms INTEGER NOT NULL,"
    "  last_seen_ms  INTEGER NOT NULL,"
    "  UNIQUE(user_id, token)"
    ");"
    "CREATE INDEX idx_device_tokens_user ON device_tokens(user_id);";

/* 0019: make a DM's participant set its IDENTITY (REQ-050/055). Matching a DM by
 * membership was correct but fragile — anything that deleted a membership row
 * (remove_user did, for DMs as well as channels) left a half-membered channel
 * that the lookup could no longer match, so the next OPEN_DM created a DUPLICATE
 * conversation. A `dm_key` of the sorted participant ids under a UNIQUE index
 * makes that impossible to represent, and turns the lookup into one indexed
 * probe instead of a count-join.
 *
 * The backfill uses MIN/MAX rather than group_concat because SQLite does not
 * guarantee group_concat's ordering, and this key must be byte-identical to the
 * one the daemon computes. DMs have one or two participants (group DMs are
 * REQ-056, unbuilt), so MIN/MAX covers every case.
 *
 * Pre-existing violations are DELETED, not merged: this ships before any release
 * (the operator decision on record), and merging conversations is a judgement no
 * migration should make silently. Member-less DM channels go, and where a
 * participant set has duplicates only the oldest channel survives. */
static const char MIGRATION_0019[] =
    "ALTER TABLE channels ADD COLUMN dm_key TEXT;"
    /* Key every DM that has a sane participant set. */
    "UPDATE channels SET dm_key = ("
    "  SELECT CASE WHEN COUNT(*)=1 THEN CAST(MIN(user_id) AS TEXT)"
    "              ELSE CAST(MIN(user_id) AS TEXT)||','||CAST(MAX(user_id) AS TEXT) END"
    "  FROM channel_members WHERE channel_id=channels.id)"
    " WHERE kind='dm'"
    "   AND (SELECT COUNT(*) FROM channel_members WHERE channel_id=channels.id) BETWEEN 1 AND 2;"
    /* Drop DMs with no usable participant set (nothing can reach them). */
    "DELETE FROM messages WHERE channel_id IN"
    "  (SELECT id FROM channels WHERE kind='dm' AND dm_key IS NULL);"
    "DELETE FROM channel_members WHERE channel_id IN"
    "  (SELECT id FROM channels WHERE kind='dm' AND dm_key IS NULL);"
    "DELETE FROM channels WHERE kind='dm' AND dm_key IS NULL;"
    /* Of each duplicated participant set, keep only the oldest channel. */
    "DELETE FROM messages WHERE channel_id IN"
    "  (SELECT id FROM channels WHERE kind='dm' AND id NOT IN"
    "     (SELECT MIN(id) FROM channels WHERE kind='dm' GROUP BY dm_key));"
    "DELETE FROM channel_members WHERE channel_id IN"
    "  (SELECT id FROM channels WHERE kind='dm' AND id NOT IN"
    "     (SELECT MIN(id) FROM channels WHERE kind='dm' GROUP BY dm_key));"
    "DELETE FROM channels WHERE kind='dm' AND id NOT IN"
    "  (SELECT MIN(id) FROM channels WHERE kind='dm' GROUP BY dm_key);"
    /* Partial index: only DMs carry a key, so named channels stay unconstrained. */
    "CREATE UNIQUE INDEX idx_channels_dm_key ON channels(dm_key) WHERE dm_key IS NOT NULL;";

/* 0020: a named channel's NAME is unique within the tenant, case-insensitively
 * (REQ-031/036). CREATE_CHANNEL previously inserted unconditionally, so two
 * channels could share a name — indistinguishable in every client's sidebar, and
 * a coin-flip which one a member joined. Same shape as the DM fix (0019): make
 * the duplicate unrepresentable rather than trusting each write path to check.
 *
 * The index is over lower(name) so "Test" cannot shadow "test", and partial on
 * kind='channel' because DMs legitimately have a NULL name.
 *
 * As with 0019, pre-existing violations are deleted rather than merged (nothing
 * is released yet): of each duplicated name only the oldest channel survives. */
static const char MIGRATION_0020[] =
    /* Deleting a channel means deleting everything that REFERENCES it, in
     * dependency order — channels are pointed at by channel_members, messages,
     * delivery_cursors, attachments, webhooks and notification_prefs, and
     * messages in turn by sent_messages, reactions and attachments. Miss one and
     * the whole migration aborts on a FOREIGN KEY constraint, which takes the
     * daemon down at boot rather than degrading. */
    "CREATE TEMP TABLE dup_ch AS SELECT id FROM channels WHERE kind='channel' AND id NOT IN"
    "  (SELECT MIN(id) FROM channels WHERE kind='channel' GROUP BY lower(name));"
    "CREATE TEMP TABLE dup_msg AS SELECT id FROM messages WHERE channel_id IN (SELECT id FROM dup_ch);"
    /* message dependents first */
    "DELETE FROM sent_messages WHERE message_id IN (SELECT id FROM dup_msg);"
    "DELETE FROM reactions     WHERE message_id IN (SELECT id FROM dup_msg);"
    "DELETE FROM attachments   WHERE message_id IN (SELECT id FROM dup_msg);"
    /* then channel dependents */
    "DELETE FROM attachments        WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM webhooks           WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM notification_prefs WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM delivery_cursors   WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM messages           WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM channel_members    WHERE channel_id IN (SELECT id FROM dup_ch);"
    "DELETE FROM channels           WHERE id IN (SELECT id FROM dup_ch);"
    "DROP TABLE dup_msg;"
    "DROP TABLE dup_ch;"
    /* Over lower(name) so "Test" cannot shadow "test"; partial on kind='channel'
     * because a DM legitimately has a NULL name. */
    "CREATE UNIQUE INDEX idx_channels_name ON channels(lower(name)) WHERE kind='channel';";

static const char MIGRATION_0021[] =
    /* Resolved @mentions, one row per (message, target). The BODY still holds
     * the literal "@alice" — plain UTF-8 (REQ-054), so FTS5 still matches a
     * search for the name and a webhook or log line stays readable. This table
     * is the STABLE reference REQ-221 asks for: an id that survives a display
     * name change, alongside the byte span so a client can re-render the
     * current name over the original text.
     *
     * channel_id is denormalised from the message on purpose: the mentions
     * feed (REQ-139) filters by what the reader can see, and doing that without
     * joining messages keeps the hot query to one index. */
    "CREATE TABLE mentions ("
    "  message_id    INTEGER NOT NULL REFERENCES messages(id),"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  user_id       INTEGER REFERENCES users(id),"   /* NULL for a broadcast */
    "  kind          INTEGER NOT NULL,"               /* 0 user, 1 here, 2 channel, 3 everyone */
    "  span_start    INTEGER NOT NULL,"
    "  span_len      INTEGER NOT NULL,"
    "  created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE INDEX idx_mentions_user ON mentions(user_id, message_id);"
    "CREATE INDEX idx_mentions_msg  ON mentions(message_id);"
    "CREATE INDEX idx_mentions_chan ON mentions(channel_id, message_id);";

static const char MIGRATION_0022[] =
    /* Pinned messages (REQ-230, ARCH-90). A pin belongs to the CHANNEL, not to
     * the person who pinned it: every member sees the same set, so message_id
     * is the primary key and pinning twice is a no-op rather than a second row.
     * That is precisely what separates a pin from a saved item (REQ-231), which
     * will be per-user and private when it is built.
     *
     * pinned_by is kept for attribution ("pinned by alice") and is nullable so
     * that removing a user (REQ-025) does not have to delete the channel's
     * pins — the pin outlives whoever placed it, because it belongs to the
     * channel.
     *
     * channel_id is denormalised from the message for the same reason as
     * mentions (§3q): listing a channel's pins is the hot path and should not
     * need a join to know what it can see. */
    "CREATE TABLE pins ("
    "  message_id    INTEGER PRIMARY KEY REFERENCES messages(id),"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  pinned_by     INTEGER REFERENCES users(id),"
    "  created_at_ms INTEGER NOT NULL"
    ");"
    "CREATE INDEX idx_pins_channel ON pins(channel_id, created_at_ms DESC);";

static const char MIGRATION_0023[] =
    /* Listing a channel's shared files (REQ-143, ARCH-91). No new columns: 0009
     * already puts channel_id on the attachment. What was missing is the index
     * for reading them that way — 0009's indexes are by message and by
     * uploader, so "this channel's files, newest first" was a full scan of
     * every attachment in the workspace.
     *
     * Partial on message_id: a pending upload has no message yet and is not a
     * shared file, so keeping those out of the index keeps it to what the query
     * actually reads. */
    "CREATE INDEX idx_attachments_channel ON attachments(channel_id, created_at_ms DESC) "
    "  WHERE message_id IS NOT NULL;";

static const char MIGRATION_0024[] =
    /* Channel mutability (REQ-034/035/036, ARCH-93). Two columns, because the
     * three verbs are one row's state:
     *
     *   topic           — REQ-034. Metadata, distinct from the name; any member
     *                     may set it. NULL and '' both mean "no topic".
     *   archived_at_ms  — REQ-035. Non-NULL IS the archived flag, which makes
     *                     "when was it archived" free and unarchive a single
     *                     NULL write. A boolean column plus a date column would
     *                     have let the two disagree.
     *
     * Rename (REQ-036) needs no column at all: the name is already mutable and
     * everything durable keys on channels.id, so a rename is an UPDATE. There is
     * deliberately no name-history table — see ARCH-93. */
    "ALTER TABLE channels ADD COLUMN topic TEXT;"
    "ALTER TABLE channels ADD COLUMN archived_at_ms INTEGER;"
    /* The sidebar's default query is "channels I can see that are not archived",
     * so that is the shape worth indexing. */
    "CREATE INDEX idx_channels_archived ON channels(archived_at_ms);";

static const char MIGRATION_0025[] =
    /* Saved items (REQ-231, ARCH-95) — the deliberate MIRROR of pins (§3r).
     * A pin is keyed on the message alone because it belongs to the channel;
     * a saved item is keyed on (user, message) because it belongs to a person.
     * Same gesture, opposite ownership, and the reason ARCH-90 wrote the
     * distinction down before this table existed. */
    "CREATE TABLE saved_items ("
    "  user_id       INTEGER NOT NULL REFERENCES users(id),"
    "  message_id    INTEGER NOT NULL REFERENCES messages(id),"
    "  created_at_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (user_id, message_id)"
    ");"
    "CREATE INDEX idx_saved_user ON saved_items(user_id, created_at_ms DESC);"

    /* The activity feed (REQ-139) is a QUERY, not a table (ARCH-95) — the rows
     * it reads are already stored and already indexed. What a query cannot give
     * cheaply is read state, so this one column is the whole of it: a watermark
     * stamped when the feed is opened, enough to badge "something new" and
     * nothing more. */
    "ALTER TABLE users ADD COLUMN activity_seen_ms INTEGER NOT NULL DEFAULT 0;";

const oc_migration OC_MIGRATIONS[] = {
    { 1, MIGRATION_0001 },
    { 2, MIGRATION_0002 },
    { 3, MIGRATION_0003 },
    { 4, MIGRATION_0004 },
    { 5, MIGRATION_0005 },
    { 6, MIGRATION_0006 },
    { 7, MIGRATION_0007 },
    { 8, MIGRATION_0008 },
    { 9, MIGRATION_0009 },
    { 10, MIGRATION_0010 },
    { 11, MIGRATION_0011 },
    { 12, MIGRATION_0012 },
    { 13, MIGRATION_0013 },
    { 14, MIGRATION_0014 },
    { 15, MIGRATION_0015 },
    { 16, MIGRATION_0016 },
    { 17, MIGRATION_0017 },
    { 18, MIGRATION_0018 },
    { 19, MIGRATION_0019 },
    { 20, MIGRATION_0020 },
    { 21, MIGRATION_0021 },
    { 22, MIGRATION_0022 },
    { 23, MIGRATION_0023 },
    { 24, MIGRATION_0024 },
    { 25, MIGRATION_0025 },
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
