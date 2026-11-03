# Moderation — the build plan

The design is REQ-371 to REQ-374 and ARCH-128. This document plans the build:
where the moderator role sits in the code, the reports table and frames, the
audit family, and the three permissions. It ends with the build issues to open
once it merges.

## 1. The moderator role (REQ-371)

**The value.** `OC_ROLE_MODERATOR = 3` in `shared/protocol.h`, after
`OC_ROLE_OWNER = 2`. It is appended rather than slotted between member and
admin, so the bytes already on the wire and in the database (`users.role`
holds `'member'`, `'admin'`, `'owner'`; the role byte on `AUTH_OK`,
`USER_LIST`, `SET_ROLE`) keep their meaning. The database stores it as
`'moderator'`. `role_to_u8` and `u8_to_role` in `daemon/dbwriter.c` learn it,
and the `users.role` CHECK widens in a migration.

**The order is a function, not the numbers.** With the moderator appended, its
number no longer says where it ranks, so every `role >= OC_ROLE_ADMIN` in the
daemon and the clients becomes a call to one new predicate,
`oc_role_rank(uint8_t)` (member 0, moderator 1, admin 2, owner 3), in
`daemon/roles.c`, with a copy for the clients in `client/core`. The build
changes these comparisons in one step, before anything grants the role.

**The predicates** in `daemon/roles.c`:

| Predicate | Today | With a moderator |
|---|---|---|
| `oc_role_can_moderate` | owner, admin | owner, admin, **moderator**: delete another's message, remove someone from a channel, see and resolve reports |
| `oc_role_can_manage_members` | owner, admin | unchanged: invite to and remove from the workspace, reset credentials |
| `oc_role_can_set_role` | owner any; admin over members | the same, and an admin may make a member a moderator or a moderator a member; a moderator sets no role |

The places that today ask `oc_role_can_moderate` for something that is
administration move to `oc_role_can_manage_members`: renaming, archiving and
changing a channel's visibility (`process_update_channel`) stay with owners
and admins. Only message deletion, channel removal and reports widen.

**Clients** draw the role as "moderator" wherever they draw "admin", offer
"Make moderator" and "Make member" to owners and admins in the member menu, and
show a moderator the moderation actions (Delete on another's message, Remove
from channel, the reports view) and nothing else an admin sees.

## 2. Reports (REQ-372, REQ-373)

**The table** (a new migration):

```sql
CREATE TABLE reports (
  id             INTEGER PRIMARY KEY,
  message_id     INTEGER NOT NULL REFERENCES messages(id),
  channel_id     INTEGER NOT NULL REFERENCES channels(id),
  reporter_id    INTEGER NOT NULL REFERENCES users(id),
  reason         INTEGER NOT NULL CHECK (reason IN (1,2,3,4)),  -- spam, harassment, inappropriate, other
  note           TEXT,
  copy_author_id INTEGER,          -- private channel or DM: the message's author
  copy_body      BLOB,             -- and its body, as it was when reported
  created_at_ms  INTEGER NOT NULL,
  resolved_by    INTEGER REFERENCES users(id),
  resolution     INTEGER,          -- 1 action taken, 2 dismissed
  resolution_note TEXT,
  resolved_at_ms INTEGER);
CREATE UNIQUE INDEX reports_once ON reports(message_id, reporter_id) WHERE resolved_at_ms IS NULL;
```

One open report per reporter per message; a second one replaces the note. The
copy is taken only when the channel is private or a DM, and it is never the
conversation, only the one message. A report in an encrypted DM is refused,
because the daemon holds no body to copy.

**The frames** (the next free opcodes when built):

- `REPORT_MESSAGE` (C → S) `{ channel_id, message_id, reason: u8, note: str }`,
  answered `REPORT_OK` or `ERROR`. It is refused for one's own message, a
  message the reporter cannot read, an unknown reason, or a note over 500
  bytes.
- `LIST_REPORTS` (C → S) `{ open_only: u8, cursor: u64 }`, from a moderator,
  admin or owner, answered with `REPORT_ENTRY` frames and `REPORTS_END`. Each
  entry carries the message reference and, for a private conversation, the
  copy. It names the reporter only to these roles, never on anything the
  reported person receives.
- `RESOLVE_REPORT` (C → S) `{ report_id, resolution: u8, note: str }`,
  answered with the updated `REPORT_ENTRY` to every connected moderator,
  admin and owner.
- `REPORTS_WAITING` (S → C) `{ open: u32 }`, to moderators, admins and owners at
  sign-in and whenever the count changes, for a badge. It follows the pattern
  of `ALERTS_SUMMARY`.

A moderator who acts on a report deletes the message or removes the person
from the channel through the existing frames. The report records the
outcome; it does not perform it.

**The audit family.** `OC_AUDIT_MODERATION` already exists and names a
moderator deleting another's message and removing a member from a channel. It
gains `report.filed` (actor: the reporter, target: the reported author) and
`report.resolved` (actor: the resolver, with the resolution). The audit view
already shows the family to owners and admins. The reporter's identity on
`report.filed` is visible there to owners and admins only, as every audit row
is.

## 3. The three permissions (REQ-374)

Configuration variables in the registry (`daemon/config_table.c`), each
client-visible on `WORKSPACE_SETTINGS`, as the other workspace settings are.
Named here without their `OPENCHIME_` prefix, since they are not settings until
they are built:

| Variable, after `OPENCHIME_` | Client key | Values | Default |
|---|---|---|---|
| `CREATE_PUBLIC_CHANNELS` | `create_public_channels` | `everyone`, `moderators`, `admins` | `everyone` |
| `CREATE_PRIVATE_CHANNELS` | `create_private_channels` | `everyone`, `moderators`, `admins` | `everyone` |
| `INVITE_PEOPLE` | `invite_people` | `everyone`, `moderators`, `admins` | `admins` |

`moderators` admits moderators, admins and owners; `admins`, admins and owners.
The daemon enforces them in `process_create_channel` (by the requested
visibility) and in the invitation handlers, which then ask this setting
instead of `oc_role_can_manage_members`. A refusal is `FORBIDDEN`. Clients hide
the New channel choice, or its public or private half, and Invite people
when the person may not.

`INVITE_PEOPLE` set to `everyone` lets a member invite people to the
workspace. Inviting someone at an elevated role stays an owner's
(`oc_role_can_set_role`), whoever may invite.

## 4. Build issues to open

To open once this document merges, in this order:

1. **The moderator role.** `OC_ROLE_MODERATOR`, the migration widening
   `users.role`, `oc_role_rank` replacing ordered comparisons in the daemon and
   clients, the predicates of §1, and granting it from the member menu. Tests:
   `tests/test_roles.c` for every predicate and actor; `tests/test_dbwriter.c`
   for a moderator deleting another's message and removing someone from a
   channel, and being refused a role change, an invitation and a channel
   rename.
2. **Reports.** The table, frames and audit rows of §2, and the copy for a
   private conversation. Tests: filing (refused for one's own message and for
   one the reporter cannot read), listing (moderators, admins and owners only),
   resolving, the copy present for a private channel and a DM and absent for a
   public channel, and the reporter never on a frame the reported person
   receives.
3. **The three permissions.** The variables, enforcement and client hiding of
   §3. Tests: `tests/test_config.c` for the values; `tests/test_dbwriter.c`
   for each setting refusing and admitting each role.
4. **The clients.** The report action on a message, the reports view with its
   badge and the resolve dialog, and the moderator's menus, in the GUI and the
   terminal client.
