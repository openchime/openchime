# Compliance capture

How a workspace's history reaches an archiving or eDiscovery system without
anyone exporting files by hand (REQ-276, REQ-340–345). The decision and what was
rejected are ARCH-122. This document is the design, **proposed for review**:
nothing in it is built yet, and the field names, limits and paths below are
proposals to settle in review before the first build change starts.

What is already decided, and not reopened here: email (RFC 5322/2045) is the
interchange, because Global Relay ingests it and Microsoft Purview's
third-party connectors and Proofpoint reach it too; the **file drop comes
first**, then **our own pull API**, then **SMTP journaling**; the pull API is
ours, not a copy of Slack's Discovery API; paging is a keyset cursor, never an
offset; a vendor is called supported only once its ingest has been tested.

## 1. One log, three ways out

The tables hold only the present. An edit overwrites a message's body
(REQ-051); a deletion nulls it, keeping the row as a tombstone (REQ-052); a
draft is one row per conversation, replaced on every save (REQ-223). Reading
the tables at export time would therefore capture the last text of an edited
message and nothing of a deleted one.

So capture is a **log the writer keeps**. When capture is on, every change in
scope is appended, in the same transaction as the change itself, as the record
stood at that moment. Each row has a **capture sequence number** (`seq`),
assigned by the writer and strictly increasing, as message ids are (ARCH-43).
The change and its record commit together or not at all.

Each mechanism keeps one cursor: the last `seq` it delivered. The file drop and
journaling keep theirs in the database; a pull-API reader keeps its own and
presents it. Because the cursor is a position in an append-only log, a record
written mid-read lands after it and is neither repeated nor skipped.

**The baseline.** Turning capture on first writes a baseline: every user, every
channel with its members, and every stored message and file as they stand,
each marked `baseline`. What changed before capture was on was never recorded
and cannot be reconstructed; the baseline says so rather than implying a
complete history.

**How long the log is kept (proposal).** A record stays until the file drop's
and journaling's cursors have both passed it (where they are on) and it is
older than a fixed window, proposed as 30 days, which is how far back a
pull-API reader can resume. A reader whose cursor is older than the oldest
record is told so with a distinct error (§6) rather than silently given a gap,
and re-reads from a new baseline. The log lives in the tenant's database, so it
sits above the database reserve (ARCH-77) and is never evicted to make room for
attachments; a cursor that has stopped moving is shown to owners and admins as a
failure that needs a person (REQ-263).

## 2. The extract schema

Every record carries a common envelope, then the fields of its kind. A field is
the same in both encodings: a JSON member in the pull API, and an
`X-OpenChime-` header in the EML, named by capitalising each word of the member
name (`channel_id` becomes `X-OpenChime-Channel-Id`). Times are milliseconds
since the epoch in UTC, server-assigned (REQ-050); ids are the daemon's own.

**The envelope**

| Field | Meaning |
|---|---|
| `seq` | The capture sequence number; the cursor. |
| `kind` | `message`, `edit`, `deletion`, `draft`, `file`, `channel` or `user`. |
| `op` | What happened to it (per kind, below), or `baseline`. |
| `at_ms` | When the change happened. |
| `actor_id` | Who made the change; absent for the daemon's own (an eviction, a baseline). |
| `workspace` | The workspace address. |
| `scope` | `public` or `private` (§4). |
| `held` | Present and true on an original a legal hold kept (§5). |

**Per kind**

| Kind | `op` | Fields |
|---|---|---|
| message | `created`, `restored` | `message_id`, `channel_id`, `thread_root` (the top-level message, absent at top level), `author_id`, `created_at_ms`, `body` (the text as stored, markup as written), `label` (a webhook's display name, where one posted it), `forward_of` (the message forwarded, where it is a forward), `file_ids`, `encrypted` |
| edit | `edited` | `message_id`, `channel_id`, `author_id`, `edited_at_ms`, `body` (the new text), `previous_seq` (the record holding the text it replaced) |
| deletion | `deleted` | `message_id`, `channel_id`, `author_id`, `deleted_at_ms`, `deleted_by`, `by_moderator`, `previous_seq` |
| draft | `saved`, `cleared` | `draft_id`, `user_id`, `channel_id` (absent while unaddressed), `recipients` (while unaddressed), `thread_root`, `body`, `updated_ms` |
| file | `stored`, `unavailable` | `file_id`, `message_id`, `channel_id`, `uploader_id`, `filename`, `content_type`, `size`, `sha256`, and the content (§3, §6) |
| channel | `created`, `changed`, `archived`, `unarchived`, `member_added`, `member_removed` | `channel_id`, `channel_kind` (`channel` or `dm`), `visibility` (`public` or `private`), `name`, `topic`, `description`, `created_at_ms`, `encrypted`, `members` (user ids, on `created` and `baseline`), `member_id` (on a membership change) |
| user | `created`, `changed`, `removed` | `user_id`, `username` (a local account's), `display_name`, `role`, `address`, `address_source` (§4), `created_at_ms`, `disabled` |

**What each kind means in practice.**

- A **reply** is a message with `thread_root`; a thread is the root and the
  records naming it.
- An **edit**'s `previous_seq` chains back through earlier edits to the
  message's `created` record, so an archive holds every version that existed
  while capture was on, though the daemon keeps only the last.
- A **deletion** carries no body; the body is in the record `previous_seq`
  names. A **restore** within REQ-052's two minutes is a `message` record with
  `op` `restored` and the body put back.
- A **draft** is captured each time its stored text changes and when it is
  cleared, by sending or by discarding. That is the daemon's stored state,
  which clients save on a pause in typing, not every keystroke.
- A **file** whose bytes the storage sweep evicts (ARCH-77) gets a record with
  `op` `unavailable`; the content went out with its `stored` record.
- A message in an **encrypted DM** (REQ-314) is captured with `encrypted` true
  and no `body` or file content: the daemon holds only ciphertext it cannot
  read. Its conversation, participants, ids, authors and times are captured.
- What a **DLP webhook** (REQ-277) rewrote is captured as stored. The original
  was never stored, so there is nothing else to capture.

**Not records in this design (proposal):** reactions, pins, saved items,
mentions, read state, polls' votes and calls. REQ-276 names messages, threads,
files, channels and users; these can be added as kinds later without changing
the envelope.

## 3. Address mapping

Every email-based archive files a record by its addresses, and a person with
no address is a record it cannot file. Each person is given one by the same
rule everywhere (REQ-342):

1. The address a sign-in provider gave as **verified**, from the person's most
   recent sign-in that carried one (`address_source` `verified`).
2. Else the **address on the profile** (`address_source` `profile`).
3. Else `<username>@<workspace address>` (`address_source` `synthetic`), where
   the username is a local account's and, for an account with none, `user-`
   followed by its id.

A synthetic address is marked in every record that carries it: the
`address_source` member in JSON, and in the EML the display name ends
`(no email address)` and the record carries `X-OpenChime-Address-Source:
synthetic`. The `user_id` travels beside every address, so a person stays one
person when their address changes; the change itself is a `user` record.

## 4. The capture credential and the two scopes

**Two scopes.** `public` covers public channels, their messages, threads and
files, and the workspace's users and channels list. `private` covers private
channels, DMs, group DMs and drafts. A record's `scope` is decided by its
channel's visibility at the time; a draft is always `private`. A channel made
private, or public, takes its later records with it; earlier ones keep the
scope they were captured under. A reader outside a record's scope never sees
it: it is left out, not blanked.

**The credential (REQ-341)**, for the pull API:

- **Minted by an owner**, in the administration console (REQ-043), with a
  label. It is 32 random bytes, shown once, and stored as its SHA-256, as a
  session token is (ARCH-58).
- **Scoped.** A new credential holds `public`. `private` is granted
  afterwards, to that credential, by an owner, as a separate act with its own
  audit entry (REQ-343). It is never part of minting.
- **Revocable** by any owner, effective on the next request. Removing the owner
  who minted it does not revoke it; it belongs to the workspace.
- **Rate-limited** per credential, by the daemon's existing limiter (REQ-190),
  with a limit to be settled in review; a request over it is refused with a
  wait (§6).
- **Audited in its own family**, a fifth beside REQ-251's four, retained the
  same 365 days (REQ-251b): mint, grant, withdrawal, revocation, a refused
  request, and each read, as the credential's label, the resource, the cursor
  range returned and the count. Never the credential.
- **Never a user session** (REQ-182). It opens no session, cannot send, holds
  no role, and authenticates nothing but the pull API.

**The file drop and journaling** have no credential to present: the operator
configures them, and with them which scopes they carry. Including `private`
is a configuration of its own, not implied by turning capture on. The daemon
records in the same audit family, at start and on every change, which
mechanisms are on and with which scopes, so "was private content leaving, and
since when" is answered by the log.

## 5. Legal hold

A legal hold (REQ-252) is not built; this is how capture meets it when it is
(REQ-345). A hold only ever adds:

- Where a hold keeps an original that would otherwise have gone — a body that
  retention (REQ-250) would have removed, or the prior text of an edited or
  deleted message — capture includes it as a record with `held` true, and
  captures the release when the hold lifts.
- Capture runs whether or not a hold is on; a hold changes nothing about what
  is captured otherwise, and lifting one removes nothing an archive holds.
- Capture is not itself a hold: it records what changed, and the daemon may
  still remove what retention removes once it has been captured.

## 6. The mechanisms

### The file drop (first)

- **Format.** One record is one RFC 5322 message with RFC 2045 MIME, UTF-8
  throughout (RFC 2047 encoded words in headers where needed):
  - `From`: the actor's address (§3); `To`: the channel's members at the time,
    by address (for a draft, its author); `Date`: `at_ms`; `Subject`: the kind,
    the operation and the channel's name, for a person reading the archive;
  - `Message-ID`: `<seq.kind.id@workspace address>`; `In-Reply-To`: the record
    `previous_seq` names, or a reply's thread root, so a mail client threads
    them;
  - the record's fields as `X-OpenChime-` headers (§2);
  - the body as a `text/plain` part, and each file's bytes as an attachment
    part named and typed as uploaded, with its SHA-256 in a header.
- **Spool.** A directory the operator names. Each record is written to a
  `tmp/` subdirectory, flushed to disk, then renamed into `new/` as
  `<seq>.eml`, so a collector never sees a partial file. The daemon never
  deletes from `new/`; the collector takes files away.
- **Manifest.** Beside the files, a manifest per day, appended as each file is
  renamed: one line per file with its `seq`, kind, file name, size and SHA-256.
  A collector checks what it took against it.
- **Restart-safe cursor.** The cursor advances only after the rename is on
  disk. After a crash the daemon rewrites from the cursor on; a record written
  twice has the same name and the same bytes, so the rewrite replaces it
  rather than duplicating it.
- **When the spool is full or unwritable** the drop stops and waits, the
  cursor does not advance, and owners and admins are shown the failure
  (REQ-263). It never skips a record.
- **Size (proposal).** A file over a set size, proposed as 25 MB, which is
  what most archive ingest accepts, goes as a manifest line and an
  `X-OpenChime-File-Omitted` header rather than as a part, and is recorded as
  omitted.

### The pull API (second)

- **On the existing listener**, over HTTPS on port 443 (ARCH-32, ARCH-54),
  authenticated by `Authorization: Bearer <credential>`.
- **Resources by kind**, each keyset-paged by `seq`:
  `GET /compliance/v1/<kind>?after=<seq>&limit=<n>` for `messages` (message,
  edit and deletion records), `drafts`, `files`, `channels` and `users`, and
  `GET /compliance/v1/records?after=<seq>` for every kind in one stream.
  A page is `{"records": [...], "next": <seq>}`; an empty page at the head of
  the log returns the same `next`. `limit` defaults to and is capped at a
  number to be settled in review.
- **File content** is `GET /compliance/v1/files/<id>/content`, in the
  credential's scope, answering only while the bytes exist.
- **Errors.** 401 for a missing, unknown or revoked credential; 403 for a
  resource outside its scope; 429 with `Retry-After` over its rate; 410 for a
  cursor older than the oldest record (§1), with the `seq` to re-baseline from.
- **Specified in the repository** as a document of its own, written with the
  build, which is what "documented and published" means.

### SMTP journaling (later)

The same EML, submitted by authenticated SMTP to a per-customer journal
address the vendor issues, reading the log from its own cursor as the file
drop does. It needs:

- an SMTP client: implicit TLS or STARTTLS on the TLS stack the daemon already
  carries (ARCH-51), `AUTH PLAIN` or `LOGIN`;
- the vendor-issued credentials — user, password, journal address, and a
  server and port where the vendor names one — held as a secret file the
  operator provides, readable only by the daemon, never in the audit log or a
  log line;
- retry with backoff on a temporary failure; the cursor is the queue, so
  nothing is lost across a restart and nothing waits in memory;
- a permanent refusal stops it and is shown to owners and admins (REQ-263);
- a test account from each vendor. Until its ingest has been tested a vendor
  is not named as supported (REQ-276).

## 7. The build changes

Each is one change, opened once this design merges:

1. **File-drop EML export.** The capture log and its baseline, the extract
   schema, the address rule, the two scopes in configuration and their audit
   entries, the EML encoder, the spool with its manifest and restart-safe
   cursor. Its tests parse every record with a standard mail parser.
2. **The pull API.** The capture credential (mint, grant, revoke, hash,
   rate limit, its audit family), the console surface for it, the HTTP
   resources and their keyset paging, and the API's published specification.
3. **SMTP journaling.** The SMTP client, the vendor credential file, retry and
   failure handling, tested against at least one vendor's ingest before that
   vendor is named as supported.
