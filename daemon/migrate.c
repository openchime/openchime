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

static const char MIGRATION_0026[] =
    /* Mute (REQ-137) is NOT the same as notification level "none", which is
     * why it needs a column of its own rather than a fourth level. `level` decides
     * whether the daemon NOTIFIES you; mute additionally de-emphasises the row and
     * suppresses its unread badge. Slack keeps them separate for the same reason: you
     * can want a channel quiet but still countable, or countable but silent. */
    "ALTER TABLE notification_prefs ADD COLUMN muted INTEGER NOT NULL DEFAULT 0 "
    "  CHECK (muted IN (0,1));"

    /* Mark-unread (REQ-235). The read cursor is monotonic BY CONSTRUCTION —
     * process_client_ack upserts MAX(message_id, excluded.message_id) — because a
     * replayed ack must never be able to move it backwards. That is a correctness
     * property worth keeping, so marking unread cannot reuse the ack path; it gets
     * its own op which sets the cursor deliberately. No schema change is needed for
     * the cursor itself; this index is what makes "unread from here" cheap to
     * recompute afterwards. */
    "CREATE INDEX IF NOT EXISTS idx_cursors_user ON delivery_cursors(user_id, channel_id);";

static const char MIGRATION_0027[] =
    /* Custom status (REQ-241/122) and the richer profile fields
     * (REQ-240). All on `users`, because they are facts about a person and
     * there is exactly one row per person — a side table would buy nothing but a
     * join.
     *
     * `status_expires_ms` is why status is not just two text columns: "in a meeting
     * until 3pm" has to stop being true on its own, and a client cannot be trusted
     * to clear it (it may not be running). 0 = no expiry.
     *
     * The avatar is an ATTACHMENT id, not a blob column: the attachment store
     * already handles upload, size limits, dedup by hash and reclamation (REQ-215,
     * ARCH-78), and a second image path would have to reimplement all of it. */
    "ALTER TABLE users ADD COLUMN status_emoji TEXT;"
    "ALTER TABLE users ADD COLUMN status_text TEXT;"
    "ALTER TABLE users ADD COLUMN status_expires_ms INTEGER NOT NULL DEFAULT 0;"
    "ALTER TABLE users ADD COLUMN title TEXT;"
    "ALTER TABLE users ADD COLUMN timezone TEXT;"
    "ALTER TABLE users ADD COLUMN avatar_attachment_id INTEGER;";

static const char MIGRATION_0028[] =
    /* Global notification default (REQ-134). `notification_prefs` is a
     * PER-CHANNEL override, so there was no answer for "a channel I have never
     * touched" other than a compiled-in constant — which meant the user could not
     * change it. One column on `users`, and the per-channel rows keep overriding it. */
    "ALTER TABLE users ADD COLUMN notify_default INTEGER NOT NULL DEFAULT 0 "
    "  CHECK (notify_default IN (0,1,2));";

/* 0029: custom emoji (REQ-072). The image is an ATTACHMENT id, not a blob column —
 * the attachment store already does upload, size caps, dedup and reclamation, and a
 * second binary store in SQLite would be a second thing to back up and a second
 * thing to get wrong. The name is the primary key because a shortcode IS the
 * identity: `:shipit:` must mean one image workspace-wide or every message
 * containing it becomes ambiguous. */
static const char MIGRATION_0029[] =
    "CREATE TABLE custom_emoji ("
    "  name          TEXT PRIMARY KEY,"           /* lowercase, no colons */
    "  attachment_id INTEGER NOT NULL REFERENCES attachments(id),"
    "  created_by    INTEGER NOT NULL REFERENCES users(id),"
    "  created_at_ms INTEGER NOT NULL"
    ");";

/* 0030: drafts (REQ-223, ARCH-101). Its own table with its own ops, NOT the
 * client_settings bucket: that bucket is keyed (user, client_type, key) and
 * partitioned per frontend by design, so a draft written in the GUI would be
 * invisible in the TUI — the opposite of "synced across that user's devices".
 * Its own schema notes also call its contents low-contention *prefs*, and a
 * draft is the one thing there that the user typed as a message.
 *
 * `thread_root` is in the key from the start, 0 meaning the channel itself. No
 * client can write anything else yet — the thread pane shares the one composer
 * — but the costs are asymmetric: one column now, against a migration on a
 * table of user content plus a change to two wire ops that shipped clients
 * already speak. Thread drafts then cost a client change and no server work. */
static const char MIGRATION_0030[] =
    "CREATE TABLE drafts ("
    "  user_id     INTEGER NOT NULL REFERENCES users(id),"
    "  channel_id  INTEGER NOT NULL REFERENCES channels(id),"
    "  thread_root INTEGER NOT NULL DEFAULT 0,"
    "  body        TEXT    NOT NULL,"
    "  updated_ms  INTEGER NOT NULL,"
    "  PRIMARY KEY (user_id, channel_id, thread_root)"
    ");";

/* 0031: a draft may be UNADDRESSED (REQ-229, ARCH-101 as amended). Slack's New
 * Message pane autosaves before a recipient is chosen, and the original key
 * cannot express that.
 *
 * SQLite cannot drop a NOT NULL or change a primary key in place, so the table
 * is rebuilt — which is also the only way to keep the ADDRESSED case exactly as
 * it was: a surrogate id, plus a UNIQUE INDEX on (user_id, channel_id,
 * thread_root) that only applies WHERE channel_id IS NOT NULL. Addressed drafts
 * keep their one-per-conversation guarantee; unaddressed ones are free rows,
 * because "one unaddressed draft per user" is a rule nobody asked for. */
static const char MIGRATION_0031[] =
    "CREATE TABLE drafts_new ("
    "  id          INTEGER PRIMARY KEY,"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),"
    "  channel_id  INTEGER REFERENCES channels(id),"   /* NULL = not addressed yet */
    "  thread_root INTEGER NOT NULL DEFAULT 0,"
    "  recipients  TEXT    NOT NULL DEFAULT '',"       /* comma-separated user ids */
    "  body        TEXT    NOT NULL,"
    "  updated_ms  INTEGER NOT NULL"
    ");"
    "INSERT INTO drafts_new (user_id, channel_id, thread_root, body, updated_ms) "
    "  SELECT user_id, channel_id, thread_root, body, updated_ms FROM drafts;"
    "DROP TABLE drafts;"
    "ALTER TABLE drafts_new RENAME TO drafts;"
    "CREATE UNIQUE INDEX idx_drafts_conv ON drafts(user_id, channel_id, thread_root) "
    "  WHERE channel_id IS NOT NULL;"
    "CREATE INDEX idx_drafts_user ON drafts(user_id, updated_ms DESC);";

/* 0032: scheduled messages (REQ-224, ARCH-102). Its own table and NOT a flag on
 * `messages`: a scheduled message is not a message yet — no id anyone can link
 * to, invisible to every member but its author, and it may never exist at all —
 * so a pending flag would make history, search, backfill, unread and mentions
 * each responsible for excluding it, and the first query that forgot would leak
 * an unsent message into somebody else's transcript.
 *
 * `state` carries the outcome rather than the row being deleted on failure: a
 * message that was promised and could not be sent is exactly what its author
 * needs to see (ARCH-102). */
static const char MIGRATION_0032[] =
    "CREATE TABLE scheduled_messages ("
    "  id          INTEGER PRIMARY KEY,"
    "  user_id     INTEGER NOT NULL REFERENCES users(id),"
    "  channel_id  INTEGER REFERENCES channels(id),"   /* NULL until recipients resolve */
    "  thread_root INTEGER NOT NULL DEFAULT 0,"
    "  recipients  TEXT    NOT NULL DEFAULT '',"
    "  body        TEXT    NOT NULL,"
    "  send_at_ms  INTEGER NOT NULL,"
    "  created_ms  INTEGER NOT NULL,"
    "  state       TEXT    NOT NULL DEFAULT 'pending'"
    "    CHECK (state IN ('pending','sent','failed')),"
    "  fail_reason TEXT,"
    "  message_id  INTEGER"                            /* what it became, once sent */
    ");"
    /* The sweep's query, and the only one that runs on a timer. */
    "CREATE INDEX idx_sched_due ON scheduled_messages(send_at_ms) WHERE state='pending';"
    "CREATE INDEX idx_sched_user ON scheduled_messages(user_id, send_at_ms);";

static const char MIGRATION_0033[] =
    /* Pausing notifications (REQ-278). An ABSOLUTE instant, not a
     * duration: the wire takes minutes-from-now because that is what the presets
     * are, and the daemon resolves them once, here, so a pause cannot drift with
     * the reader's clock.
     *
     * Enforced ON READ, the pattern migration 0027 proved for status expiry: a
     * client that is not running cannot clear its own state, and a stamp that
     * reads as absent once it has passed needs no sweep and no timer. 0 means
     * "not paused", which is also what ending one early writes — there is no
     * second op (REQ-278).
     *
     * Beside the ARCH-72 window rather than replacing it: they are different
     * types. The window is a pair of minutes-of-day and is periodic by
     * construction; this is one instant. Neither can express the other, which is
     * why Slack ships both and why the window alone left "until 5pm" unbuildable. */
    "ALTER TABLE users ADD COLUMN dnd_until_ms INTEGER NOT NULL DEFAULT 0;";

static const char MIGRATION_0034[] =
    /* The notification SCHEDULE (REQ-136, ARCH-103). It REPLACES the
     * single daily window rather than joining it: Slack has exactly one
     * recurring mechanism, and two things able to disagree about "am I quiet
     * now" would need a precedence rule nobody could remember.
     *
     * The sense flips with it. REQ-131's window was the QUIET hours ("do not
     * disturb from 22:00 to 08:00"); Slack's schedule — and REQ-136 — states the
     * ALLOWED hours and suppresses everything outside them. Same two integers,
     * opposite meaning, so the columns are renamed rather than reused silently,
     * and the existing values are swapped as they are carried forward. A column
     * whose meaning changed under its old name is the kind of thing that reads
     * correctly for a year and is wrong the whole time.
     *
     * `dnd_mode`: 0 off, 1 every day, 2 weekdays, 3 custom. The first three need
     * no rows anywhere — only *custom* uses the table below, so the common cases
     * stay two integers.
     *
     * `tz_offset_min` is what makes a per-weekday window answerable: the day has
     * to be the user's LOCAL one, and the daemon cannot resolve an IANA zone per
     * push without carrying tzdata into the push thread. The client refreshes the
     * offset on connect — the same division of labour the pause uses. */
    "ALTER TABLE users RENAME COLUMN dnd_start_min TO allow_start_min;"
    "ALTER TABLE users RENAME COLUMN dnd_end_min TO allow_end_min;"
    "ALTER TABLE users ADD COLUMN dnd_mode INTEGER NOT NULL DEFAULT 0 "
    "  CHECK (dnd_mode IN (0,1,2,3));"
    "ALTER TABLE users ADD COLUMN tz_offset_min INTEGER NOT NULL DEFAULT 0;"
    /* Quiet [start,end) becomes allowed [end,start): the complement of the same
     * pair. An enabled window carries forward as "every day", which is what it
     * meant, and SQLite evaluates both right-hand sides against the original row,
     * so this is a real swap rather than an assignment through itself. */
    "UPDATE users SET dnd_mode = 1, allow_start_min = allow_end_min, "
    "                 allow_end_min = allow_start_min WHERE dnd_enabled = 1;"
    "ALTER TABLE users DROP COLUMN dnd_enabled;"
    "CREATE TABLE notify_schedule ("
    "  user_id   INTEGER NOT NULL REFERENCES users(id),"
    "  weekday   INTEGER NOT NULL,"          /* 0 = Sunday, as strftime('%w') */
    "  enabled   INTEGER NOT NULL DEFAULT 1,"
    "  start_min INTEGER NOT NULL DEFAULT 0,"
    "  end_min   INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (user_id, weekday)"
    ") WITHOUT ROWID;";

static const char MIGRATION_0035[] =
    /* Keywords and priority people (REQ-135, ARCH-103). One term per row: a
     * comma-joined string would make a term containing a comma unrepresentable
     * and every read a parser.
     *
     * Terms are stored lowercased; matching is case-insensitive and exact, so
     * "deploy" does not match "deployment" — done by shared/mention.c and never
     * by SQL, because the CLIENT has to highlight precisely what the server
     * notified on and a predicate cannot be linked into a client. */
    "CREATE TABLE notify_keywords ("
    "  user_id INTEGER NOT NULL REFERENCES users(id),"
    "  term    TEXT    NOT NULL,"
    "  PRIMARY KEY (user_id, term)"
    ") WITHOUT ROWID;"
    /* A relation, not a level: every other notification setting says WHERE, this
     * one says WHO, and it is evaluated against the author. */
    "CREATE TABLE priority_people ("
    "  user_id   INTEGER NOT NULL REFERENCES users(id),"
    "  person_id INTEGER NOT NULL REFERENCES users(id),"
    "  PRIMARY KEY (user_id, person_id)"
    ") WITHOUT ROWID;";

static const char MIGRATION_0036[] =
    /* The aggregated Threads view (REQ-062, ARCH-104).
     *
     * Participation is DERIVED, never stored: you are in a thread if you wrote
     * its root or any reply, which the messages table already knows. So this
     * table holds only OVERRIDES — an explicit follow of a thread you never
     * replied to, and an explicit unfollow of one you did. `state` 1 follow,
     * 0 unfollow. Storing participation instead would mean a write on every
     * reply and a second source of truth for a question the first one answers.
     *
     * `thread_reads` is a per-thread cursor, and it has to be its own: the
     * channel cursor (delivery_cursors, REQ-090) advances when you read the
     * CHANNEL, which says nothing about whether you read a thread inside it —
     * and unread-reply counts are the whole point of the view. */
    "CREATE TABLE thread_follows ("
    "  user_id    INTEGER NOT NULL REFERENCES users(id),"
    "  root_id    INTEGER NOT NULL REFERENCES messages(id),"
    "  channel_id INTEGER NOT NULL REFERENCES channels(id),"
    "  state      INTEGER NOT NULL DEFAULT 1 CHECK (state IN (0,1)),"
    "  updated_ms INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (user_id, root_id)"
    ") WITHOUT ROWID;"
    "CREATE TABLE thread_reads ("
    "  user_id             INTEGER NOT NULL REFERENCES users(id),"
    "  root_id             INTEGER NOT NULL REFERENCES messages(id),"
    "  last_read_reply_id  INTEGER NOT NULL DEFAULT 0,"
    "  updated_ms          INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (user_id, root_id)"
    ") WITHOUT ROWID;";

static const char MIGRATION_0037[] =
    /* Make UPLOAD_BEGIN idempotent.
     *
     * The frame has always carried a 16-byte idempotency token and the daemon
     * has always copied it into the job — and then never read it, so a retry
     * after a dropped connection minted a second pending row for the same
     * upload. SEND solves this with a `sent_messages` map; attachments do not
     * need a second table, because unlike a message an attachment row is
     * RECLAIMABLE (REQ-215/217): a map keyed to a row the storage sweep later
     * deletes would hand a client back an id for something that no longer
     * exists. Holding the token on the row itself means it dies with the row.
     *
     * NULL for every existing row, and NULLs do not collide in a UNIQUE index —
     * so the constraint applies only to rows minted from here on.
     *
     * Keyed on the UPLOADER as well as the channel, which `sent_messages` does
     * not need to be: replaying a SEND token returns a message id, and replaying
     * this one returns a WRITABLE handle — the net loop opens the row's storage
     * key and lets the caller stream over it. Without the uploader in the key,
     * one member replaying another's token in a channel they share is handed
     * back that person's attachment. Guessing 128 random bits is not a threat,
     * but the row already records who owns it and is the only one allowed to
     * link it (§3g), so scoping the lookup the same way costs one column and
     * removes the question. The index matches the lookup exactly, so the two
     * cannot disagree about what "already declared" means. */
    "ALTER TABLE attachments ADD COLUMN idem_token BLOB;"
    "CREATE UNIQUE INDEX idx_attachments_idem"
    "  ON attachments(channel_id, uploader_id, idem_token)"
    "  WHERE idem_token IS NOT NULL;";

static const char MIGRATION_0038[] =
    /* Link unfurls (REQ-222, ARCH-105): the fetched title + description of a
     * URL in a message, keyed per (message, url) so pinning twice cannot
     * happen and an EDIT that keeps a URL keeps its preview.
     *
     * Rows exist only for COMPLETED fetches — there is no pending state,
     * because a fetch that never finishes should leave nothing to clean up
     * and nothing to replay. `channel_id` is denormalised from the message
     * exactly as `mentions` and `pins` do it: the backfill replay reads by
     * message and must not join per row.
     *
     * No thumbnail column: og:image is deferred (ARCH-105), and adding the
     * column now would be pretending otherwise. */
    "CREATE TABLE unfurls ("
    "  message_id    INTEGER NOT NULL REFERENCES messages(id),"
    "  channel_id    INTEGER NOT NULL REFERENCES channels(id),"
    "  url           TEXT    NOT NULL,"
    "  title         TEXT,"
    "  descr         TEXT,"
    "  created_at_ms INTEGER NOT NULL,"
    "  PRIMARY KEY (message_id, url)"
    ");";

static const char MIGRATION_0039[] =
    /* The rest of the profile (REQ-240). On `users` for the same reason 0027's
     * fields are: they are facts about a person, there is exactly one row per
     * person, and a side table would buy nothing but a join.
     *
     * `full_name` beside `display_name` is the split the reference product
     * makes, and the two answer different questions — what you are called on
     * paper, and what a transcript calls you. Neither substitutes for the
     * other, which is why one column could not serve both.
     *
     * `pronouns` completes REQ-240. It is read beside a name, so it rides
     * USER_LIST as well as PROFILE_INFO.
     *
     * `phone` does NOT ride USER_LIST. It is contact detail rather than a name
     * decoration, and every member's number in every roster fan-out is a size
     * and privacy cost with nothing asking for it — PROFILE_INFO carries it to
     * the one person who opened the card.
     *
     * All nullable, no defaults: an absent value means the person has not said,
     * which is a different thing from an empty string they typed. */
    "ALTER TABLE users ADD COLUMN full_name TEXT;"
    "ALTER TABLE users ADD COLUMN phone TEXT;"
    "ALTER TABLE users ADD COLUMN pronouns TEXT;";

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
    { 26, MIGRATION_0026 },
    { 27, MIGRATION_0027 },
    { 28, MIGRATION_0028 },
    { 29, MIGRATION_0029 },
    { 30, MIGRATION_0030 },
    { 31, MIGRATION_0031 },
    { 32, MIGRATION_0032 },
    { 33, MIGRATION_0033 },
    { 34, MIGRATION_0034 },
    { 35, MIGRATION_0035 },
    { 36, MIGRATION_0036 },
    { 37, MIGRATION_0037 },
    { 38, MIGRATION_0038 },
    { 39, MIGRATION_0039 },
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
