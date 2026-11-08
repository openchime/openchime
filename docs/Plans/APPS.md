# Apps — the app platform

A proposal, for review, of the app platform: how an app is defined, signed,
installed and trusted, what it may call and receive, how it shows UI, and how
an app that runs inside the workspace is sandboxed. It answers REQ-172,
REQ-173 and REQ-175, and the requirements of its own, REQ-376 to REQ-381. The
decisions, and what each rejected, are ARCH-130 to ARCH-133. The Web API an app
calls is REQ-176 and REQ-350 to REQ-354 ([HTTP-API.md](./HTTP-API.md)); the
shapes an app written with Bolt expects are recorded in [BOLT.md](./BOLT.md).
Nothing here is built yet.

What is decided is stated as decided; everything else is a proposal, and the
points left open for the reviewer are marked **Open** and gathered in §14.

## 1. Who it is for, and the two app types

An **app** is a third party's code acting in a workspace under its own
identity: posting, answering commands and buttons, reacting to what happens,
and showing UI in the clients. A workspace's owners and admins decide which
apps it has and what each may do.

Decided: there are two types, defined by the same manifest (§3).

- **An external app** is a manifest plus code on the developer's own server.
  It calls the Web API over HTTPS and receives events either as signed HTTPS
  POSTs to an endpoint it declares, or over a connection it opens itself
  (Socket Mode, §5.4). An app written with Bolt runs unchanged, pointed at the
  workspace's address by configuration ([BOLT.md](./BOLT.md) §6).
- **An in-workspace app** is a manifest plus JavaScript that the daemon runs
  itself (§10). It needs no server of its own. It calls the same Web API
  methods as direct function calls and receives the same events as direct
  calls, through a Bolt-like listener API.

Both see the same methods, the same events and the same payloads, so an app
can move from one type to the other without being rewritten.

Agents (MCP.md §3.1) are not apps, but they share the principal (§2), the
scopes (§4) and the Web API.

## 2. The principal

Decided:

- **A bot is a `users` row of kind `bot`**, beside the `agent` kind of the MCP
  design (ARCH-120, ARCH-123). The `users.kind` column already allows both
  (SCHEMA.md, migration 0071). A bot is not a person: it takes no seat in the
  registered-user cap, cannot sign in to a client, and is never in a DM it was
  not added to by its install.
- **One hashed token store for both kinds.** A token is 32 random bytes, shown
  once, stored only as its SHA-256 and found by one indexed probe, as session
  and webhook tokens are (ARCH-58, ARCH-71). Each token row carries the scopes
  granted to it (§4).
- **Each external install gets**, each shown once:
  - a **bot token**, for the Web API;
  - a **per-install signing secret**, which signs every event and interaction
    sent to the app (§5.3);
  - an **app-level token**, if the manifest chooses Socket Mode, which can do
    one thing only: open a connection (§5.4).
- **Agents use the same store and the same checks.** An agent's token carries
  scopes from the same catalog, chosen by the owner when the token is created.
  The agent design records this change to its requirements (REQ-321).
- **Posts are attributed to the app**: its name and icon, marked as an app,
  wherever a person's name would appear, in every client and in exports.

**Ids.** Decided: ids are strings of a kind letter followed by the numeric id:
`U` for a person, bot or agent (`U42`), `C` for a channel (`C7`), `D` for a DM
(`D9`). A message's id is its `ts`, and a reply's parent is its `thread_ts`.
The same form is used in the Web API, in every event payload, and inside
`<@ID>` mentions (`<@U42>`, MARKDOWN.md).

## 3. The manifest

Decided: the manifest is **OpenChime's own schema**, as JSON, parsed with the
vendored jsmn. It is the **only definition of an app**: the developer keeps it
in their repository, and no console, catalog or daemon keeps a separate copy
that could drift from it. A console that offers forms edits the same file.

One validator, in `shared/`, checks it for the daemon at install and for the
command line tools (REQ-376), with the same plain-language messages in both.

### 3.1 Fields

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schema` | integer | yes | The manifest schema's version. This document is version 1. |
| `id` | string | yes | The app's identifier: lowercase letters, digits and `-`, 2 to 32 characters. It is the namespace of its commands (§6). |
| `name` | string | yes | Shown to people, at most 64 characters. |
| `description` | string | yes | One paragraph, shown at install, at most 500 characters. |
| `icon` | string | no | A path inside the package to a square PNG, 512 × 512 at most. |
| `version` | string | yes | The app's version, `MAJOR.MINOR.PATCH`. |
| `developer` | object | yes | `name`, and `url` or `email`, shown at install. |
| `type` | string | yes | `external` or `in-workspace`. |
| `scopes` | array of strings | yes | The scopes the app asks for (§4). May be empty. |
| `commands` | array of objects | no | Each `{name, description, usage_hint, scope}`: a slash command, with `scope` either `workspace` or `channel` (§6). |
| `shortcuts` | array of objects | no | Each `{callback_id, name, description, type}`, `type` being `global` or `message` (§6). |
| `events` | array of strings | no | The event names subscribed to (§5.1). Each needs the scope §4 names for it. |
| `delivery` | string | external: yes | `http` or `socket` (§5.4). |
| `urls` | object | external: as needed | `events` (the event endpoint), `interactivity` (buttons, menus, modal submissions), `commands` (slash commands), and `redirect` (an array of allowed OAuth redirect URLs, §9.2). Each is an `https` URL. Not used with `socket` delivery, except `redirect`. |
| `entry` | string | in-workspace: yes | The path inside the package to the JavaScript module the runtime loads (§10). |
| `outbound_hosts` | array of strings | no | In-workspace only: the hosts the app may call over HTTPS (§10.4). Exact names, or `*.` plus a domain. |
| `secrets` | array of objects | no | Each `{name, description}`: a value an owner or admin fills in at or after install (§10.4). |
| `schedules` | array of objects | no | In-workspace only: each `{name, every}` or `{name, cron}`, in the workspace's zone (§10.4). |
| `inbound_webhook` | boolean | no | In-workspace only: the install gets an address of its own (§11). |
| `auth_providers` | array of objects | no | Each `{id, name, authorize_url, token_url, scopes}`: a third-party service the app acts on for a person (§10.4). |
| `unfurl_domains` | array of strings | no | Domains whose links the app previews (§5.1, `link_shared`). |
| `blocks` | string | no | The Block Kit spec version the app's UI is written against (§7). |

A field the schema does not know is refused, not ignored: a manifest that says
something the platform cannot honour is better refused at install than
silently narrowed.

### 3.2 An external app

```json
{
  "schema": 1,
  "id": "standup",
  "name": "Standup",
  "description": "Asks the team for a daily update and posts a summary.",
  "icon": "icon.png",
  "version": "1.4.0",
  "developer": {"name": "Example Tools", "url": "https://tools.example"},
  "type": "external",
  "scopes": ["chat:write", "commands", "channels:history", "users:read"],
  "commands": [
    {"name": "standup", "description": "Post your update", "usage_hint": "[text]", "scope": "channel"}
  ],
  "events": ["app_mention", "member_joined_channel"],
  "delivery": "http",
  "urls": {
    "events": "https://tools.example/standup/events",
    "interactivity": "https://tools.example/standup/interactive",
    "commands": "https://tools.example/standup/commands",
    "redirect": ["https://tools.example/standup/oauth/callback"]
  },
  "blocks": "2026-10"
}
```

### 3.3 An in-workspace app

```json
{
  "schema": 1,
  "id": "oncall",
  "name": "On call",
  "description": "Pages whoever is on call when a channel asks for help.",
  "version": "0.3.1",
  "developer": {"name": "Ops team", "email": "ops@example.org"},
  "type": "in-workspace",
  "scopes": ["chat:write", "app_mentions:read", "users:read"],
  "events": ["app_mention"],
  "entry": "main.js",
  "outbound_hosts": ["api.pager.example"],
  "secrets": [{"name": "PAGER_KEY", "description": "The paging service's API key"}],
  "schedules": [{"name": "handover", "cron": "0 9 * * 1"}]
}
```

## 4. Scopes

Decided:

- An app asks for named **scopes** in its manifest. An owner or admin approves
  them at install, each shown with its plain-language description. Nothing an
  app may call or receive is outside them.
- **Least privilege, narrowed by membership.** A scope grants a kind of
  access; channel membership grants where. A bot reads and posts only in the
  channels it is a member of, and receives events only from them. A DM is
  reachable only when the bot is one of its members.
- Every Web API call is checked against the calling token's scopes, then
  against membership. Every event is filtered the same way before delivery: an
  event whose scope was not approved is not delivered.
- **An update that asks for a new scope is not applied** until an owner or
  admin approves it again (§9.3).

The catalog for the first release. A method or event not listed under any
scope is not available to apps in the first release.

| Scope | What an owner or admin is told | Methods | Events |
|---|---|---|---|
| `chat:write` | Post and edit its own messages in channels it is in | `chat.postMessage`, `chat.update`, `chat.delete`, `chat.postEphemeral` | — |
| `channels:read` | See the names and members of public channels it is in | `conversations.list`, `conversations.info`, `conversations.members` (public) | `member_joined_channel`, `member_left_channel`, `channel_rename`, `channel_archive`, `channel_unarchive` (public) |
| `channels:history` | Read messages in public channels it is in | `conversations.history`, `conversations.replies` (public) | `message` (public) |
| `groups:read` | See the names and members of private channels it is in | as `channels:read`, private | as `channels:read`, private |
| `groups:history` | Read messages in private channels it is in | as `channels:history`, private | `message` (private) |
| `im:history` | Read direct messages it is part of | as `channels:history`, DMs and group DMs | `message` (DMs) |
| `users:read` | See people's names, titles and time zones | `users.info`, `users.list` | `user_change` |
| `users:read.email` | See people's email addresses | the `email` field of `users.info` and `users.list` | — |
| `reactions:read` | See reactions in channels it is in | `reactions.get` | `reaction_added`, `reaction_removed` |
| `reactions:write` | Add and remove its own reactions | `reactions.add`, `reactions.remove` | — |
| `app_mentions:read` | See messages that mention it | — | `app_mention` |
| `commands` | Add slash commands and shortcuts | `views.open`, `views.update`, `views.push` (from a trigger) | slash commands, shortcuts, interactions |
| `links:read` | See links to its own domains | — | `link_shared` |
| `links:write` | Show previews of links to its own domains | `chat.unfurl` | — |

`auth.test`, `apps.connections.open` (with an app-level token) and
`oauth.v2.access` need no scope. `app_uninstalled` and `tokens_revoked` are
always delivered.

Encrypted DMs (PRIVATE-DM.md) are never readable by an app, under any scope:
the daemon cannot read them.

## 5. Events

Decided: one **event catalog** and one **signing scheme** serve both installed
apps and the plain outgoing webhooks an owner or admin sets up (REQ-173). An
outgoing webhook is the simpler form of the same delivery: an endpoint and a
list of events, with no install, no bot and no commands.

### 5.1 The catalog and the envelope

Event names and payloads follow the widely used chat-platform convention, so
an app's handlers need no translation. The first release:

| Event | When |
|---|---|
| `message` | A message is posted in a conversation the bot is in. Subtypes: `message_changed`, `message_deleted`, `bot_message`, `me_message`. |
| `app_mention` | A message mentions the bot, by `<@ID>` or by name. |
| `reaction_added`, `reaction_removed` | A reaction on a message in a conversation the bot is in. |
| `member_joined_channel`, `member_left_channel` | Someone joins or leaves a conversation the bot is in. |
| `channel_rename`, `channel_archive`, `channel_unarchive` | A conversation the bot is in changes. |
| `user_change` | A person's profile changes. |
| `link_shared` | A message links to one of the app's `unfurl_domains`. |
| `app_uninstalled`, `tokens_revoked` | The app's install or a token ends. |

The envelope:

```json
{
  "type": "event_callback",
  "team_id": "T1",
  "api_app_id": "Astandup",
  "event_id": "Ev1c9a2f04",
  "event_time": 1730295731,
  "authorizations": [
    {"team_id": "T1", "user_id": "U57", "is_bot": true, "is_enterprise_install": false}
  ],
  "event": {"type": "app_mention", "user": "U42", "text": "<@U57> status",
            "ts": "90211", "channel": "C7", "event_ts": "90211"}
}
```

- `team_id` is the workspace's id, `T` plus its audience id's short form.
- `api_app_id` is `A` plus the manifest's `id`.
- Message text gives every mention as `<@ID>`, whichever form its author
  typed, so an app sees one form.
- `event_id` is unique per event and stable across redeliveries, so a receiver
  can drop duplicates.

### 5.2 The endpoint check

When an app is installed, or an outgoing webhook created, with an HTTP event
endpoint, the daemon sends
`{"type": "url_verification", "challenge": "<random>"}`, signed as any event
is. The endpoint must answer with the `challenge`, or the install or webhook is
refused with the reason.

### 5.3 Delivery and signing

Decided:

- **Signing.** Each request carries `X-OpenChime-Request-Timestamp` (Unix
  seconds) and `X-OpenChime-Signature`, which is `v0=` followed by the hex
  HMAC-SHA-256, keyed with the install's signing secret (or the webhook's), of
  `v0:{timestamp}:{raw body}`. A receiver compares in constant time and rejects
  a timestamp more than five minutes away from its own clock. Bolt's request
  verification accepts this scheme ([BOLT.md](./BOLT.md) §1.3); the header
  names a Bolt app reads are configured once in its receiver.
- **At least once.** A delivery is done when the endpoint answers 2xx within
  three seconds. Anything else is retried with backoff (doubling from 1 s, up
  to eight attempts over about a day), each retry carrying
  `X-OpenChime-Retry-Num` and `X-OpenChime-Retry-Reason`
  (`http_timeout`, `http_error`, `connection_failed`).
- **Off the net loop.** Delivery runs on its own worker, beside the outbound
  HTTPS client's other callers, with the queue in the database, so a restart
  loses nothing.
- **Disabled after continued failure.** An endpoint failing for 24 hours is
  disabled, and owners and admins are told; the change is audited.
- **In-workspace apps** receive the same payload as a direct call to their
  handler (§10). There is nothing to sign and nothing to retry over a network;
  a handler that throws or runs past its deadline is retried as a failed
  delivery would be.

### 5.4 Socket Mode

Decided: an external app may receive everything over a **connection it opens
itself**, so it can run behind a firewall or on a developer's laptop with no
public address. This reverses the earlier choice of no app socket: many Bolt
apps and most tutorials use it ([BOLT.md](./BOLT.md) §1.5).

- The manifest's `delivery` is `socket`. The install gets an **app-level
  token**, which can call only `apps.connections.open`.
- `apps.connections.open` returns a single-use `wss://` URL on the shared
  listener (ARCH-32, ARCH-54).
- On that connection: `hello` once it is ready, pings both ways, and
  `disconnect` before the daemon closes it (a restart, say). The app then
  reconnects, and nothing is lost: undelivered envelopes wait in the queue.
- Each message is an envelope with `envelope_id`, `type` (`events_api`,
  `slash_commands` or `interactive`), `payload` (exactly the body HTTP
  delivery would carry), `accepts_response_payload`, `retry_attempt` and
  `retry_reason`.
- The app acknowledges with `{"envelope_id": ..., "payload": {...}}`, under
  the same three-second rule, and the optional `payload` stands in for an HTTP
  response body.

An agent may open a Socket Mode connection, or register an event URL, and then
receives interactions with its own messages' buttons and modals as an app does.

## 6. Slash commands and shortcuts

Decided:

- **Namespaced from the first release.** A command is `/<app>/<command>`,
  where `<app>` is the manifest's `id`. An unqualified alias, `/<command>`,
  works only where exactly one installed app declares that command; otherwise
  the client offers the qualified forms. Two apps cannot fight over `/deploy`.
- **Channel-scoped commands** (`scope: "channel"`) resolve only in a
  conversation the app is a member of; workspace-scoped ones resolve anywhere
  the person can post.
- **Delivery** is form-encoded, with `command`, `text`, `trigger_id`,
  `response_url`, `channel_id`, `user_id` and `team_id`. `command` carries the
  name the app registered (`/standup`), however the person qualified it, so
  Bolt's `app.command('/standup')` matches.
- **The three-second acknowledgement.** The HTTP response (or the Socket Mode
  ack) is the acknowledgement. Without one in time, the person is told the app
  did not answer.
- **Shortcuts** are declared in the manifest: global ones from the composer's
  menu, message ones from a message's menu. They are delivered as interactions
  (§7.2).
- **Completion** in the GUI and the TUI lists the commands the person can use
  where they are, with each one's description and usage hint.

## 7. UI

### 7.1 Blocks

Decided:

- **UI is Block Kit**, adopted as published and tracked by its spec version,
  not forked. An app sends a description of its UI, never code; the client
  draws it with its own controls. A manifest's `blocks` names the version its
  UI is written against.
- **Every block has a defined text form**, so the TUI loses no app function: a
  button is a numbered choice, a select a list to pick from, an input a
  prompt. The forms are written down in TUIKIT.md.
- **An unsupported block falls back to its text**: its `text` field, or a line
  saying what it is.
- One parser and validator in `shared/` reads blocks for the daemon and every
  client, as the rich-text scanner does.

**Open:** the launch subset. Proposed:

| Block | Elements |
|---|---|
| `section` | `text` (`mrkdwn` or `plain_text`), `fields`, and an `accessory` of `button`, `static_select`, `overflow` or `image` |
| `header` | `plain_text` |
| `divider` | — |
| `context` | `mrkdwn`, `plain_text`, `image` |
| `actions` | `button`, `static_select`, `users_select`, `conversations_select`, `overflow`, `datepicker` |
| `input` | `plain_text_input`, `static_select`, `users_select`, `checkboxes`, `radio_buttons`, `datepicker` |
| `image` | an `https` URL and alt text, fetched through the link-preview path, never by the client directly |

Views: the `modal` view, with `title`, `submit`, `close`, `blocks` and
`private_metadata`. The `home` tab is not in the first release.

### 7.2 Interactions

- A click, a selection or a modal submission becomes an interaction delivered
  to the app, form-encoded in a single `payload` field holding JSON: a
  `block_actions`, `view_submission`, `view_closed`, `shortcut` or
  `message_action`.
- Each carries a `trigger_id`, valid for three seconds and once, which lets
  the app open a modal (`views.open`) or push one (`views.push`), and a
  `response_url`: unauthenticated, valid for 30 minutes and five uses, and
  specific to that interaction, to which the app may POST a message.
- The app acknowledges within three seconds. For a modal submission the
  acknowledgement may carry `response_action`: `errors` (shown against the
  fields), `update`, `push` or `clear`.

## 8. Signing and trust

Decided:

- **A package** is the manifest, the code (for an in-workspace app), the icon,
  and an Ed25519 signature over all of them. An external app's package is its
  manifest and icon.
- **Developers sign** with keys registered in the developer console that
  central runs. **The project countersigns** an app that passes review for
  Public listing (§12).
- **The daemon trusts** three kinds of key:
  - the project's key, built into the daemon;
  - its own `server_identity` key (SCHEMA.md, migration 0008), through a
    signing command in the daemon binary, so possession of the host is the
    authority to sign for it, as with an SSH host key;
  - **delegate keys** an admin adds, for sideloaded apps signed by a build
    system. A delegate key can sign an app that runs with whatever its
    manifest asks for, so adding one grants authority over the workspace:
    it is written to the audit log (REQ-251) and takes effect only after a
    restart.
- **Nothing loads unsigned**, in any deployment model (ARCH-76). Verification
  is the same whether an app came from a catalog or from the operator's
  laptop; only the key differs.

Proposed: the signing command is `openchimed --sign-app <package>`, run on the
host; and delegate keys are a daemon setting listing Ed25519 public keys in
base64, added to CONFIG.md's registry when it is built.

## 9. Install, update and uninstall

### 9.1 Install

Decided:

- **Owners and admins only** (REQ-172). The install screen says in plain
  language what the app asks for: each scope's description, each outbound
  host, each secret, whether it has an inbound webhook address, and which
  third-party services it connects to.
- A package whose manifest is invalid, or whose signature does not verify
  against a trusted key, is refused, with the validator's message.
- **External apps** are installed by uploading the package or by OAuth (§9.2);
  **in-workspace apps** by uploading the package, by dropping it in a watched
  directory on the host, or from a catalog (§12).
- Install and every later change to an install are audited.

### 9.2 Install by OAuth

Decided, for a service that hosts the app itself (an automation platform, say):
the service sends an owner or admin to the workspace's authorize page; they
sign in as they always do (AUTH.md §8.10), see what the app asks for, and
approve; the service exchanges the code at `oauth.v2.access` for the install's
bot token. The answer carries `access_token`, `token_type`, `scope`,
`bot_user_id`, `app_id`, `team.id`, `enterprise`, `is_enterprise_install` and
`authed_user`, plus `refresh_token` and `expires_in` when token rotation is on
([BOLT.md](./BOLT.md) §1.6). PKCE, single-use codes and a short expiry; only
a redirect URL in the manifest's `urls.redirect` is accepted; refusal is a
clean redirect carrying the error.

### 9.3 Updates

Decided:

- Every update is recorded with its version, and an admin can roll back to the
  previous version and its data.
- **An update that asks for a new scope, outbound host, secret, auth provider
  or inbound webhook waits** until an owner or admin approves it again.

**Open:** updates that change code only. Owners and admins choose, per
workspace and per app, between applying them automatically and approving each.
What the default is remains open; proposed: approve each, until the app has
been installed for 30 days without a quarantine.

### 9.4 Uninstall

Uninstalling revokes every token of the install, ends its Socket Mode
connection, removes the bot from every conversation, deletes the app's data
(§10.4), files and secrets, and sends `app_uninstalled` to an external app. The
bot's past messages stay, attributed to the app.

### 9.5 Visibility

An app in the developer console is **Draft** (only its developer's test
workspaces), **Private** (the workspaces its developer is linked to), or
**Public** (every catalog, after review). Visibility is central's record;
the daemon sees only what a catalog offers it (§12).

## 10. The in-workspace runtime

### 10.1 Where it runs

Decided: in-workspace apps run **inside the daemon's process**, on QuickJS
(quickjs-ng, pinned at 0.18.0, recorded in VENDORS.md), as one binary and one
process (ARCH-28) with no containers (ARCH-36). The protection comes from the
sandbox (§10.2), not from a separate process. The measurements behind this are
in `docs/Plans/QUICKJS.md`.

- **One QuickJS runtime per app**, since the memory cap, the stack cap and the
  interrupt handler are set per runtime, and contexts in one runtime share a
  heap. A runtime costs about 160 KB resident.
- **Apps run on a small pool of worker threads**, never on the net loop or the
  database writer. A native built-in that runs past a deadline delays only that
  app.
- **The daemon compiles; packages carry source.** Source is compiled at
  install, and the bytecode is hashed and stored. Bytecode from a package is
  never loaded.

### 10.2 The sandbox

Decided:

- **No ambient authority.** An app's context is built with `JS_NewContextRaw`
  and given only the intrinsics the runtime's API needs and the generated host
  bindings: no file system, environment, sockets, process, timers beyond the
  host's, or state shared with another app. `quickjs-libc` is never linked.
- **Limits per app**, by default: a 16 MB heap, a 256 KB stack, a CPU deadline
  of 100 ms per invocation (counted only while JavaScript runs), and a CPU
  budget of 1 s a minute. An app that exceeds a deadline or its budget three
  times in an hour is **quarantined** until an admin re-enables it, and admins
  are told.
- **A combined heap budget** for all apps, through the allocator hook: 48 MB
  in the lean profile (REQ-210), 160 MB in the standard one (REQ-211).
- **Every binding validates every argument** at the boundary, and every call
  goes through the same scope and membership checks as the Web API.
- **Fuzzing** of the bindings and the app-loading path, in the same gate as
  the daemon's other parsers.
- **Switches**: one per workspace that turns the runtime off, and one per app.

### 10.3 One interface description

The host interface — every Web API method an app may call, every event it may
receive, and every platform service (§10.4) — is written once, as a
description from which the runtime's bindings, the type definitions developers
use, and the generated reference are all produced. A later runtime (WASM, say)
gets the same interface from the same description.

On top of the bindings, a **Bolt-like listener API**: `app.message`,
`app.event`, `app.command`, `app.action`, `app.view`, `app.shortcut`, with
`ack`, `say`, `respond` and `client.<method>` in each handler. A listener's
body moves between an external Bolt app and an in-workspace app almost
unchanged ([BOLT.md](./BOLT.md) §4).

### 10.4 Platform services

| Service | Decided |
|---|---|
| **Key-value storage** | Per install, in the workspace's database, written through the single writer: get, set, delete, list by prefix. Quota-capped, in backups, deleted on uninstall. |
| **Files** | Through the blob store, tagged to the install, under a hard quota and exempt from attachment eviction. Private to the app. **Open:** whether posting one in a message attaches the app's file or a copy; proposed: a copy, through the ordinary attachment path. |
| **Secrets** | Declared by name; owners and admins fill in the values; stored encrypted; never shown again, returned by any API, logged, exported or visible to the developer. Changes are audited. |
| **Outbound HTTPS** | Through the daemon's outbound client and its checks, only to the manifest's `outbound_hosts` as approved, never to a private address. No raw sockets. Off the event loop, with timeouts and limits per app. |
| **Schedules** | Interval or calendar expressions in the workspace's zone, delivered as events. A run missed while the daemon was down runs once at start, never twice. |
| **Inbound webhook** | An address of the install's own (§11). |
| **Acting for a person** | Per-person OAuth to a third-party provider the manifest declares, run on the daemon's own browser pages; refresh tokens stored encrypted per person and per install; the app gets an access token when it asks, and nothing more. A person sees and revokes their connections; an admin can revoke anyone's. |

### 10.5 The memory budget

The lean profile (REQ-210, about 256 MB) holds about 150 installed apps beside
200 connections at the defaults of §10.2; the standard profile (REQ-211)
proportionally more. The combined heap budget is what keeps the daemon inside
the profile however many apps are installed.

## 11. Inbound webhooks and reachability

Decided:

- An install that declares `inbound_webhook` gets a unique address,
  `https://<workspace>/hooks/<random token>`: the token identifies the install
  and is its credential. A request to it wakes the app's handler with the
  request, and the handler's answer is returned. Where the sending service
  signs its payloads, the admin enters the shared secret (§10.4) and the
  handler verifies it. A wrong token is a 404; the address can be rotated; it
  is rate-limited per install.
- The incoming webhooks of ARCH-71 stay as they are.
- **A workspace that is not publicly reachable** — behind a firewall — receives
  these only if it is connected to the OpenChime services: central gives it a public address and hands
  each request to the workspace over a connection the workspace holds open to
  central. The payload passes through central and is not kept, as with push. A
  self-hosted workspace that is neither reachable nor connected goes without inbound webhooks,
  and the install screen and the documentation say so.

## 12. The two catalogs

Decided (REQ-175):

- **A local catalog** for every deployment: the apps registered with this
  daemon, by upload or the watched directory, from which an owner or admin
  installs. A self-hosted workspace that is not connected has only this one and
  makes no outbound catalog call.
- **The curated catalog**, which central serves to deployments enrolled with it
  (connected self-hosted and managed): Public apps, and the Private apps of developers
  linked to the workspace. The daemon fetches it and **verifies every entry's
  signature** before offering it; an entry that does not verify is not
  installable and is shown as such.
- **One package format in both.** Moving an app from the local catalog to the
  curated one is a publishing step, not a rewrite.

## 13. What each deployment model gets

| | Self-hosted, not connected | Self-hosted, connected | Managed |
|---|---|---|---|
| External apps, by upload | yes | yes | yes |
| External apps, by OAuth install | yes | yes | yes |
| In-workspace apps | yes | yes | yes |
| Local catalog | yes | yes | yes |
| Curated catalog | no | yes | yes |
| Developer-signed and project-countersigned apps | install from a package | yes | yes |
| Inbound webhooks, publicly reachable | yes | yes | yes |
| Inbound webhooks, not publicly reachable | no | through central | not applicable |
| Third-party sign-in for apps | the operator's own provider clients | the operator's, or central's redirect | the project's clients |

Everything in the daemon works with no OpenChime-operated service; only the
curated catalog, forwarding and central's provider clients are OpenChime
services (ARCH-76).

## 14. Open points

1. **The Block Kit launch subset** (§7.1). The proposed list stands unless the
   review narrows or widens it.
2. **The default for code-only updates** (§9.3). Proposed: approve each, until
   30 days without a quarantine.
3. **App files in messages** (§10.4). Proposed: a copy, through the ordinary
   attachment path.
4. **The signing command's name, and the delegate-key setting** (§8), as
   proposed.
