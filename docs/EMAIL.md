# Email

The email the daemon sends: a mention email and a digest for the people who
want them, and the invitation (REQ-280, REQ-365–370). This document is the
design; the decisions and what was rejected are ARCH-125 (the three levels at
which a feature is switched on) and ARCH-126 (the providers and the outbox).
Where a detail below is marked **proposed**, it is a starting point for review
rather than a decision, and the build may settle it differently.

## 1. What is mailed

| Kind | Sent when | Default |
|---|---|---|
| Mention email | an @mention or a DM is still unread on every one of the person's devices after their delay: 10 min, 30 min, 1 h or 4 h | 30 min delay; on or off as the workspace's default for members says (§2) |
| Digest | on the person's days, at their time, in their timezone | Monday to Friday, 08:00; **off** |
| Invitation | an invite bound to an address is made, with invitation mail turned on (ARCH-85) | as today |

Nothing else is mailed (REQ-280). A mention email is sent **at most once per
conversation per delay window**: a burst of mentions in `#ops` is one email,
and the next one about `#ops` waits a full delay after it. Muted conversations
and people are left out by the one notification evaluator (`shared/notify.c`,
REQ-281), asked the same question push asks.

## 2. Who decides: three levels

Each kind works only where all three levels allow it (ARCH-125):

| Level | Who | Where it is held |
|---|---|---|
| System | the operator | the email provider, chosen in the environment (§3); fixed to `openchime` by central on a managed box |
| Workspace | an owner or admin | the workspace's own settings in the database: a switch per kind, and the defaults members start from |
| Person | the person | their notification settings, synced to every device (ARCH-72): each kind on or off, the delay, the digest's days and time |

A person's choice overrides the workspace's default, never its switch. The
daemon answers "is this on for this person" in one function, and tells a
client the state of each level, so the settings screen can say which one is
off ("this server sends no email", "your workspace has turned this off")
rather than hide the control.

## 3. Providers

The daemon hands every message to one provider, chosen by the operator:

| Provider | What it does | Where |
|---|---|---|
| `none` | sends nothing; the features that need mail say so (§2) | any box; **proposed** as the default off a managed box |
| `null` | accepts every message and drops it | any box; for building and testing everything else without a mail system |
| `openchime` | relays each message to central, signed with the enrollment key over the transport push already uses (ARCH-84/85) | a managed or federated box with an active enrollment; the only provider on a managed box |

A provider that delivers without central — the operator's own relay — is a
later addition behind the same interface; until there is one, a stand-alone box
sends no notification email (REQ-280).

**The interface (proposed).** A provider is a small table of functions:
`send` takes one message (§5) and answers *sent*, *retry later* or *refused*;
`name` says which provider it is, for the journal. The outbox (§4) is the
only caller, so a provider never sees the event loop or a database connection.
`null` answers *sent* and counts what it was given, which is what the tests
read.

## 4. The outbox

A trigger (§6) never talks to a provider. It writes a row into an outbox table
on the DB-writer thread (ARCH-5), in the same transaction that records why the
mail was due — so a mail is either owed and recorded, or neither. A worker
thread of its own takes rows to the provider, so a slow or absent provider
delays no frame and no send (ARCH-23).

- *Sent:* the row is done.
- *Retry later* (for `openchime`, a 5xx or no answer): tried again with
  backoff, as invitation mail is today.
- *Refused* (a 4xx): final, and logged with the kind and the reason, never the
  address.
- A row survives a restart, and the worker picks up where it was.

**Proposed:** the backoff doubles from 30 seconds to an hour, a row older than
a day is given up and logged, and done rows are pruned by the maintenance pass
(ARCH-78) after a week. A mention email that waited an hour in the outbox
because central was down is still worth sending; one a day late is not.

Invitation mail moves onto the outbox (ARCH-126): an invitation is one more
message kind, and today's dedicated worker and its five-attempt retry go.
Invitation mail keeps its own switch (`OPENCHIME_INVITE_MAIL`); how that switch
and the system level's provider combine for invitations is for the build to
settle, and invitations must keep working as they do today for a workspace that
has invitation mail on.

## 5. Messages

A message is a kind and its fields; the provider renders it — for
`openchime`, central does, so no HTML is built in the daemon. **No field is
ever message text**: not a body, a preview, an attachment's name or a quoted
line (REQ-369). A field is a name, a count, a time or a link destination.

| Kind | Fields |
|---|---|
| Mention email | recipient address; workspace id and name; conversation kind (channel or DM), id and name (for a DM, the other people's names); the names of who mentioned or wrote, and how many unread; the destination of the first unread one |
| Digest | recipient address; workspace id and name; the period it covers; per conversation with unread mentions or DMs: its kind, id, name, who, how many, and its destination; per starred channel with unread messages: its name, the count and its destination |
| Invitation | as today (ARCH-85): the invitation's id, the address and when it expires |

A name is the one the product shows at the moment the message is made: a
person's display name, a channel's name, the workspace's display name.

**Proposed:** the daemon sends each message as one JSON object, `kind` plus the
fields above, so a new kind is a new object and central can refuse one it does
not know.

## 6. Triggers and timing

Both notification kinds are found by the daemon on the **scheduled-message
tick** — the ~15 s connectionless sweep that fires scheduled messages
(ARCH-102) — rather than a timer per mention or a scheduler of their own.
Fifteen seconds is well inside the shortest delay a person can choose.

**Mention email.** On each tick the daemon looks for people with mention email
on, at all three levels, who have an @mention (any `mentions` row naming them,
ARCH-89/103) or a DM message that is past their read cursor (REQ-090/095) and
older than their delay. A message read on any device in the meantime has moved
the cursor and is no longer found. For each such (person, conversation) not
mailed within the last delay window, it writes one outbox row and records the
time, which is what holds the next email back.

**Digest.** On each tick the daemon looks for people with the digest on whose
chosen time has passed today, on one of their chosen days, by their stored
UTC offset (`users.tz_offset_min`, ARCH-103), and who have had no digest for
that slot. The digest covers what is unread since the last one (or since it was
switched on), and a digest with nothing in it is not sent (**proposed**).

## 7. Links

Every link in a notification email goes through central:

1. The daemon reports the **destination**: the workspace's id, the
   conversation and, where there is one, the message (a permalink's id pair,
   ARCH-96).
2. Central keeps a **permanent map** from an opaque random id to that
   destination and puts a link to the id, on OpenChime's own domain, in the
   email. Nothing in the link says which workspace, conversation or message it
   points at.
3. The page at that link opens the **installed app** at the workspace, found by
   its id, and failing that the **web client** at the workspace's address. A
   federated workspace's address is the one central recorded at enrollment,
   after the daemon proved it holds the workspace's key; a managed one's is
   central's own record. So a link can only open the workspace that sent it.

The client half — opening a workspace, conversation and message from that
page — is the client's (CLIENT.md).

## 8. What central does

Central renders and sends the mail, and it alone:

- **confirms every recipient address once** before sending it notification
  mail, so a daemon cannot make central mail an address that never agreed
  (REQ-370);
- **honours unsubscribe**, whatever a daemon asks afterwards; a person turns
  mail back on from their own settings;
- keeps the link map (§7).

Central's mail endpoint, its confirmation, unsubscribe and caps are specified
in the control plane's own documents, not here.

## 9. Elsewhere

The wire, the storage, the settings and the screens are filled in as they are
built, each in its own document:

- [CONFIG.md](./CONFIG.md): the provider setting (§3);
- [SCHEMA.md](./SCHEMA.md): the outbox (§4), each person's email settings and
  the workspace's switches and defaults (§2), and what holds back the next
  mention email and digest (§6);
- [PROTOCOL.md](./PROTOCOL.md): the frames that read and set the person's and
  the workspace's settings, and that tell a client each level's state (§2);
- [CLIENT.md](./CLIENT.md): the settings screens, and opening a link (§7).
