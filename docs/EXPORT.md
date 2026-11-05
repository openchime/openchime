# OpenChime: The Export Format

How a workspace's history leaves a daemon as files, and the shape another
daemon reads back in (REQ-360). This document is the format, version 1; the
decision, the alternatives rejected and how an import maps people are
ARCH-124. The commands that write and read it are not built yet.

An export is for **moving a team**: out of OpenChime, into it from another
product by way of a converter, or from one daemon to another. It is not
compliance capture (REQ-276), which is continuous, keeps edits and deletions,
and is delivered to an archive the tenant names; and it is not a legal hold
(REQ-252). It is written by the workspace's own daemon from its own database, to
a folder the operator chooses, and nothing in it passes through a service the
project operates (REQ-040/041).

---

## 1. The folder

```
manifest.json
users.jsonl
channels.jsonl
messages.jsonl
reactions.jsonl
pins.jsonl
attachments.jsonl
attachments/
```

Every file is present in every export, empty where there are no records, so a
reader never has to tell a missing file from a missing kind.

**The `.jsonl` files** are UTF-8, one JSON object per line, each line ended by a
line feed. A file can be read a line at a time, so a workspace of any size is
read in bounded memory, and two exports of the same workspace compare line by
line.

**Ids are strings** (`"1042"`), in every file and wherever one record names
another. A daemon's ids are 64-bit integers, and a reader in a language whose
numbers are doubles would round one above 2^53 without saying so. An id
identifies a record **within this export**: a reader maps it to whatever id its
own store gives the record, and never assumes the two are the same.

**Times are integers**, milliseconds since the Unix epoch, UTC, named `*_ms`:
the daemon's own unit (SCHEMA.md §2) and the wire's, so nothing is rounded on
the way out. **Absent values are `null`**, not an empty string and not a
missing key, so a field that is there and empty can be told from one that was
never set.

**Records are in id order.** In `messages.jsonl` that is the order the daemon
accepted them (ARCH-43), so a thread's root always comes before its replies and
a reader can resolve each reference as it goes.

## 2. `manifest.json`

One JSON object:

| Field | Meaning | Source |
|---|---|---|
| `version` | The format version, the integer `1`. | constant |
| `workspace` | The workspace's display name, or `null` when the operator has not set one. | derived: the daemon's configuration, `OPENCHIME_WORKSPACE_NAME`; the database holds no name |
| `exported_ms` | When the export began. | derived: the clock |
| `daemon` | The build id of the daemon that wrote it, as `openchimed --version` prints it. | derived: the binary |
| `files` | One entry per `.jsonl` file, keyed by its name: `{"count": N, "sha256": "<hex>"}`, the number of records and the SHA-256 of the file's bytes. | derived: computed over the files written |

A reader checks each file against its count and digest before it trusts any of
it. The attachments' bytes are checked by their own digests (§3.6), not here.

## 3. The records

The **Source** column names the column in the daemon's database each field is
read from (SCHEMA.md), or says how it is derived where nothing stores it.

### 3.1 `users.jsonl`

Every account, deactivated ones included: a message keeps its author.

| Field | Meaning | Source |
|---|---|---|
| `id` | The user's id. | `users.id` |
| `subject` | The identity key: `local:<username>`, or `oidc:` and the provider's issuer and subject. | `users.subject` |
| `username` | A local account's username; `null` for an account that signs in through a provider. | derived: `subject` after `local:` |
| `display_name` | What a transcript calls them. | `users.display_name` |
| `full_name` | Their full name. | `users.full_name` |
| `email` | Their address; `null` where none is known. | `users.email` |
| `role` | `owner`, `admin` or `member`. | `users.role` |
| `deactivated` | `true` once the account has been removed from the workspace. | `users.disabled` |
| `created_ms` | When the account was first seen. | `users.created_at_ms` |

### 3.2 `channels.jsonl`

Every channel and every DM, archived ones included, but encrypted DMs (§4).

| Field | Meaning | Source |
|---|---|---|
| `id` | The channel's id. | `channels.id` |
| `kind` | `channel`, `dm` or `group_dm`. | derived: `channels.kind`, and a `dm` with three or more members is a `group_dm`; the database has no third kind (SCHEMA.md §2). A DM with one member is a self-DM. |
| `name` | The channel's name, without `#`; `null` for a DM. | `channels.name` |
| `topic` | The one-line topic, or `null`. | `channels.topic` |
| `description` | The long-form description, or `null`. | `channels.description` |
| `visibility` | `public` or `private`; a DM is always `private`. | derived: `channels.is_public` |
| `archived_ms` | When it was archived; `null` when it is not. | `channels.archived_at_ms` |
| `members` | The member user ids. | derived: the channel's `channel_members` rows |
| `created_ms` | When it was made. | `channels.created_at_ms` |

`members` is who is in the channel when the export is taken, however they came
to be: added directly, or through a user group the channel has. Groups are not
in version 1, so a reader gets the people and not the reason.

### 3.3 `messages.jsonl`

Every message in an exported channel, thread replies included.

| Field | Meaning | Source |
|---|---|---|
| `id` | The message's id. | `messages.id` |
| `channel` | The channel it is in. | `messages.channel_id` |
| `author` | Who sent it. | `messages.author_id` |
| `created_ms` | When the daemon accepted it; an edit does not change it. | `messages.created_at_ms` |
| `body` | The text as typed, its formatting marks and `@names` as written (MARKDOWN.md), or `null` for a deleted message. | `messages.body` |
| `edited_ms` | When it was last edited; `null` if never. | `messages.edited_at_ms` |
| `thread_root` | The top-level message it replies to; `null` for a top-level message. | `messages.parent_id` |
| `kind` | `message`, or `call` for the record the daemon writes of a missed call. | derived: `messages.kind` (`0`, `1`) |
| `attachments` | The ids of its files, in `attachments.jsonl`; `[]` for none. | derived: the `attachments` rows whose `message_id` is this message |

**A deleted message is kept, with a `null` body**, as the daemon keeps it
(REQ-052), so the replies of a deleted root still have a root. A thread is one
level deep: `thread_root` always names a top-level message.

A reader that meets a `kind` it does not know shows the message as an ordinary
one: the body of every kind is written to be read.

### 3.4 `reactions.jsonl`

| Field | Meaning | Source |
|---|---|---|
| `message` | The message reacted to. | `reactions.message_id` |
| `user` | Who reacted. | `reactions.user_id` |
| `emoji` | The emoji, as the daemon stores it. | `reactions.emoji` |

One record per person per emoji per message (REQ-070).

### 3.5 `pins.jsonl`

| Field | Meaning | Source |
|---|---|---|
| `message` | The pinned message. A message is pinned or not, so it appears once. | `pins.message_id` |
| `by` | Who pinned it, or `null` where the daemon did not record one. | `pins.pinned_by` |
| `at_ms` | When it was pinned. | `pins.created_at_ms` |

### 3.6 `attachments.jsonl`

Every file a message in the export carries, and those files' bytes in
`attachments/`.

| Field | Meaning | Source |
|---|---|---|
| `id` | The attachment's id. | `attachments.id` |
| `filename` | The name it was uploaded under. | `attachments.filename` |
| `mime` | The content type the uploader declared. | `attachments.mime` |
| `size` | Its length in bytes. | `attachments.size` |
| `sha256` | The SHA-256 of its bytes, as lower-case hex; `null` where the daemon recorded none. | `attachments.sha256`, as hex |
| `path` | Where its bytes are, relative to the folder: `attachments/<id>`. `null` when the bytes are gone. | derived: from `id`; `null` when `attachments.reclaimed_at_ms` is set |

**The bytes are named by id, not by filename**, so two files called
`report.pdf` cannot collide and no name can reach outside the folder. A reader
refuses a `path` that is not `attachments/` followed by a name with no slash.

**A file whose bytes the daemon reclaimed** (REQ-215) keeps its record with a
`null` `path`, as it keeps its row, so the message still says a file was there.

Files no message carries are not exported: uploads never sent, and the images
behind avatars and custom emoji.

## 4. What is never exported

- **Sessions** and **push device tokens**: they are someone's live access, and
  meaningless to another daemon.
- **Credentials**: passwords, second steps (TOTP secrets, recovery codes,
  passkeys), provider identities, invites, webhook tokens. A person imported
  into another workspace signs in there afresh.
- **Encrypted DMs** and everything in them. Their bodies are ciphertext the
  daemon cannot read, and the keys that would open them are on the two devices
  and nowhere else (PRIVATE-DM.md); the daemon has none to export.
- **Drafts**: what someone has typed and not sent is theirs, not the
  workspace's history.

Also not in version 1, and free to arrive in a later export as new fields or
new files under §5's rule: user groups, profiles beyond the names (title,
pronouns, status, avatar), custom emoji, polls, forwards, saved items,
scheduled messages, a webhook post's display name, notification settings and
read state.

## 5. Versions

**A reader ignores a field it does not know**, in the manifest and in every
record, and ignores a file it does not know. A later export may therefore add a
field or a file and keep the same version, and a version 1 reader reads it.

**A change that a version 1 reader would misread is a new version, and a new
number**: removing or renaming a field, changing what one means or its type, or
changing the folder's layout. `version` is a single integer, so there is no
minor version to reason about: a reader refuses a version it does not know
rather than guessing at it.
