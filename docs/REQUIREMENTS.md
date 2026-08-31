# OpenChime — Requirements

**Status: target-state specification, partly implemented.** Written in
descriptive present-perfect ("the system has supported X") as a *contract* — the
form the finished system is required to take.

**Each requirement carries its own status marker.** A requirement with no marker
is **built** for the daemon and the Win32 client. Anything else says so
explicitly, in one of four forms:

| Marker | Meaning |
|---|---|
| *(Not built)* | No implementation exists. |
| *(Partly built)* | Some of it exists; the requirement's text overstates what ships. |
| *(Built in the daemon; no client reaches it)* | The server half works and nothing can use it. |
| *(Built in the Win32 client only)* | Present in the GUI, absent elsewhere. |

That marker is what keeps this document honest: the present-perfect voice states
what the finished system does, so without it a reader cannot tell a shipped
guarantee from an intention. **Every marked requirement has a corresponding
issue** in the [tracker](https://github.com/openchime/openchime/issues), which is the project's only issue list and the
place priority is expressed — this document says *what*, an issue says *what is
missing and what it costs*. The markers are assigned by checking the code.

The daemon is a feature-complete v1 chat core: messaging, auth, roles, channels,
DMs, reactions, threads, search, presence and typing, notification settings with
the schedule and pause, attachments, webhooks, drafts, scheduled send, the
storage-maintenance tiers, the audit log, federated enrollment and the push
emitter. Section 15 collects the cross-client UI-parity requirements a
competitive analysis against Slack and Pumble surfaced; Section 16 records what
the product deliberately excludes.

This document is technical scope only. **Deployment topology is an
architectural concern and is tracked here** — the three deployment models
(self-hosted stand-alone, self-hosted federated, hosted; ARCH-76) determine
what a given deployment does and does not do, so requirements state which
models they apply to. Pricing, licensing, and go-to-market decisions remain
out of scope and are not tracked in any document in this repository. Technical design decisions —
*how* a requirement below has been implemented — live in
[ARCHITECTURE.md](./ARCHITECTURE.md) and are cross-referenced by `ARCH-N` id
where one exists. Where no architecture decision yet covers a requirement,
the requirement says so explicitly rather than implying one.

**Conventions.**
- Each requirement carries a stable identifier of the form `REQ-NNN`.
  Identifiers are assigned in increments of 10 at the start of each
  subsection and increment by 1 within it, leaving room to insert later
  requirements into a subsection without renumbering anything else.
- Requirements are written in present-perfect tense, as a factual statement
  of what the finished system does — not as a to-do or a design rationale.
  Rationale, when it matters, is a separate sentence after the requirement.
- "The system" refers to the daemon and a client jointly. "The daemon" and
  "the client" are used when a requirement applies to only one side.
- "Tenant" refers to one organization's workspace — one daemon, one SQLite
  database (ARCH-4) — in any of the three deployment models (ARCH-76).
  "Organization" and "tenant" are used interchangeably.
- "Deployment model" means one of the three shapes defined in ARCH-76:
  **self-hosted stand-alone** (no dependency on any OpenChime-operated
  service), **self-hosted federated** (operator runs the daemon and owns all
  message data, opting in to OpenChime-operated services for OIDC, push, the
  app directory, SCIM, an optional DNS name, and packages), and **hosted**
  (OpenChime operates the daemon and those services). A requirement that
  applies to only some models says so.
- Cross-references cite the implementing decision as `(ARCH-N)`. A
  requirement with no such citation is not yet backed by an architecture
  decision and is flagged inline as **[needs ARCH decision]**.

---

## Table of contents

1. [Tenancy, Identity, and Access](#1-tenancy-identity-and-access)
2. [Messaging](#2-messaging)
3. [Delivery and Reliability](#3-delivery-and-reliability)
4. [Presence and Real-Time State](#4-presence-and-real-time-state)
5. [Notifications](#5-notifications)
6. [Attachments and Media](#6-attachments-and-media)
7. [Integrations](#7-integrations)
8. [Security Posture](#8-security-posture)
9. [Client and Platform Support](#9-client-and-platform-support)
10. [Infrastructure and Resource Constraints](#10-infrastructure-and-resource-constraints)
11. [Rich Text and Message Composition](#11-rich-text-and-message-composition)
12. [Message Organization and Retrieval](#12-message-organization-and-retrieval)
13. [User Profiles](#13-user-profiles)
14. [Compliance and Data Governance](#14-compliance-and-data-governance)
15. [Client Experience and UI Shell](#15-client-experience-and-ui-shell)
16. [Explicitly Out of Scope](#16-explicitly-out-of-scope)

---

## 1. Tenancy, Identity, and Access

### 1.1 Tenant Discovery and Resolution

- **REQ-010.** *(Partly built)* The client has collected a **workspace** (the tenant's
  address) and the user's **email** at sign-in, and has resolved the workspace
  to a daemon address by plain DNS before opening a connection — with no
  resolution *service* involved in any deployment model (ARCH-14/76). A
  workspace given as a full domain (e.g. `chat.acme.com`) — the stand-alone
  case, and the federated case where the operator keeps their own DNS — has
  resolved via SRV records plus optional `.well-known` metadata; a workspace
  given as a bare name (e.g. `acme`) — a hosted tenant, or a federated
  self-hoster who took a vanity name under the service suffix — has had the
  service's known DNS suffix appended client-side (`acme` →
  `acme.openchime.example`) and then resolved by ordinary DNS. In every case
  the mapping has lived in static DNS records, so no lookup has reached an
  OpenChime-operated service. The
  email has been used only to drive the OIDC login (REQ-020), not to derive
  the workspace.
- **REQ-011.** Resolution failure (the workspace name does not resolve in DNS,
  no SRV record, malformed `.well-known` metadata) has produced a distinct,
  user-facing error rather than being conflated with an authentication or
  network failure, since the user has needed to know which of "this org
  doesn't exist," "the org is unreachable," and "your login failed" applies.

### 1.1a Multiple Workspaces

- **REQ-012.** A client has remembered every workspace the user has signed into
  on that device — the address they typed, the account they used, and when they
  last used it — so that returning to a workspace has never required retyping
  its address or re-resolving it by hand. Forgetting a workspace has removed its
  stored credentials and cached history along with the entry, so that "forget"
  has left nothing of that workspace on disk.
- **REQ-013.** Every client has offered a **workspace switcher** with the same
  shape across platforms: the remembered workspaces, an indication of unread
  activity and connection state for each, and an always-present **"Log in to new
  workspace"** entry. Switching has been possible at any time without restarting
  the client.
- **REQ-014.** A client has held sessions to **several workspaces at once**, so
  that activity in a workspace the user is not currently looking at has still
  arrived and been counted as unread — the switcher has therefore shown live
  unread state rather than state as of the last visit. Each workspace's
  connection, credentials, model, and cached history have stayed isolated from
  every other's; nothing has crossed between them (REQ-040).
- **REQ-015.** *(Partly built)* Per-workspace view state — the focused channel, scroll position,
  and a partly-typed message — has survived switching away and back, so that a
  switch has not discarded work in progress.

### 1.2 Authentication

- **REQ-020.** *(Built in the daemon; no client reaches it)* The system has authenticated a user in one of two deployment-selected
  modes (ARCH-19, ARCH-55, [AUTH.md](./AUTH.md)): **local** (daemon-managed
  username+password) or **OIDC** (social login via the project's central
  service). Because OIDC is a federated function (ARCH-76), self-hosted
  stand-alone deployments have used **local** mode; self-hosted federated
  deployments have used either, choosing OIDC precisely by opting in; hosted
  deployments have used OIDC. In OIDC mode the login has been a client-driven browser flow using
  platform-native auth session APIs — `ASWebAuthenticationSession` on iOS/macOS,
  a loopback redirect on desktop — with PKCE. The daemon has advertised its mode
  to the client before authentication.
- **REQ-021.** *(Not built)* The system has supported OIDC login against Microsoft Entra
  ID and Google Workspace as identity providers. Provider integration has lived
  in the central service (ARCH-56), not the daemon.
- **REQ-022.** *(Not built)* The system has supported Apple Sign-In, required for iOS App
  Store compliance for any app offering third-party login. The system has
  not supported Facebook login. (Also brokered by the central service.)
- **REQ-023.** The daemon has established a session only against a verified
  credential appropriate to its mode, rejecting a connection outright on any
  mismatch without partial trust (ARCH-19): in **OIDC mode** an ES256 JWT
  re-issued by the central service, verified against a pinned key with the
  algorithm pinned to `ES256` plus `iss`/`aud`/`exp` checks (ARCH-56/57) — the
  daemon has not validated raw *provider* JWTs or fetched provider JWKS itself;
  in **local mode** a username+password checked against the stored PBKDF2 hash
  (ARCH-59).
- **REQ-024.** In local mode the daemon has managed accounts itself: passwords
  hashed with PBKDF2-HMAC-SHA256 and never stored in the clear, the first owner
  bootstrapped from a one-time setup token, further users created by invite
  token, and repeated failed attempts rate-limited (ARCH-59). This mode has
  required no external identity provider and has functioned air-gapped.
- **REQ-025.** *(Not built)* In OIDC mode the maintainer's central service has held the
  provider app credentials and re-issued a workspace-scoped identity token that
  the daemon trusts; self-hosted deployments have reached it through a relay so
  their users get social login without registering provider apps, and the client
  (not the central service) has carried the token to the daemon (ARCH-56).
- **REQ-026.** *(Partly built)* An owner/admin has been able to create a **shareable invite link**
  — a multi-use, expirable invite URL — in addition to the single-use per-user
  invite token (REQ-024/033), and has managed outstanding invites: listing
  pending ones, setting an expiry and a target role, and revoking one before it
  is redeemed. The link has resolved to the same account-creation path as a token
  (ARCH-59), so it has granted no capability a token does not. **[needs ARCH
  decision — invite-link token model (multi-use vs. per-redeem), expiry, and
  revocation storage.]**
- **REQ-027.** *(Excluded by decision)* The system's only single-sign-on has been **OIDC brokered through
  the project's central service** (REQ-020, ARCH-55/56); it has supported neither
  **SAML 2.0** nor a **bring-your-own-IdP** mode pointing the daemon directly at
  an organization's identity provider. This is a deliberate exclusion, not a
  deferred feature: ARCH-55 routes all OIDC through central to keep the daemon
  free of JWKS fetching and multi-provider handling, and SAML has never been
  built, designed, or scheduled — so an organization whose procurement mandates
  SAML is unserved. The ARCH-55-consistent path, were it ever wanted, is central
  terminating SAML and re-issuing the same ES256 JWT (a control-plane concern in
  `openchime-saas`, not a daemon one).

  **The exact position, stated so it is never inferred from a tick in a table:**

  | | Slack | Pumble | **OpenChime** |
  |---|---|---|---|
  | SAML 2.0 | from Business+ | top tier only | **not supported** — not built, not designed, not scheduled |
  | OIDC / social login | yes | OAuth2, top tier | **designed, daemon side built**, brokered by the central relay (ARCH-56/57) |
  | Bring-your-own IdP, direct to the server | yes | yes | **excluded by design** (ARCH-55) — OIDC always routes through central |
  | Providers reachable | any IdP | any IdP | **Google only**; Entra and Apple sit behind the same seam |
  | End-to-end login working today | yes | yes | **no** — the client courier half does not exist |

  Three consequences follow, and each is a fact about the product rather than a
  plan. An RFP requiring SAML 2.0 disqualifies this system outright. Our OIDC is
  not equivalent to their SSO: it costs a self-hoster a login-time dependency on
  the project and gives the project visibility into who signs in to which
  workspace (AUTH.md §3.4), so a stand-alone deployment declining that runs local
  accounts only. And **nobody completes an OIDC login in any deployment model**,
  because the daemon verifies and the control plane mints while nothing carries
  the token between them — `scripts/demo-oidc.sh` proves the mint↔verify contract
  with a dev endpoint that deliberately bypasses the browser flow. That last one
  is the open item, and is tracked; the first two are settled.

### 1.3 Authorization and Roles

- **REQ-030.** Every user in a tenant has held exactly one tenant-level role
  at a time: **owner**, **admin**, or **member**, stored as a `role` column on
  `users` and enforced in the DB-writer handlers (ARCH-60). A tenant has had at
  least one owner at all times; the system has refused an action that would
  remove or demote the last owner.
- **REQ-031.** Channel membership has been independent of tenant role: a
  member has belonged to zero or more channels, and only channel members
  have been able to read or post in a channel that is not public. Membership
  has been stored in the `channel_members` table, with `channels.is_public`
  gating read/post on non-public channels (ARCH-50).
- **REQ-032.** A user has been able to edit or delete only their own
  messages, with one exception: a tenant admin or owner has been able to
  delete (not edit) any message in a channel they belong to, for moderation
  purposes. Deletion by a non-author has been distinguishable in the message
  record from self-deletion via the `messages.deleted_by` column (REQ-052), with
  the admin/owner gate enforced in the delete handler (ARCH-60).
- **REQ-033.** Only an owner or admin has been able to invite or remove a
  member from the tenant. Channel-level invite/remove for private channels
  has been available to any existing member of that channel, not gated to
  admins. Both gates have been enforced in the DB-writer handlers (ARCH-60).
- **REQ-034.** Each channel has carried an optional human-set **topic/
  description** — a short line shown in the channel header — set by a channel
  member. It is metadata on the channel, distinct from the channel name.
  **Built (ARCH-93, migration 0024):** a `topic` column, set with
  `UPDATE_CHANNEL`; **any member may set it** — it is already visible to the
  channel and a wrong one is corrected in seconds. An empty value clears it.
  Capped at 250 bytes and shown on the channel header's second line.
- **REQ-035.** A channel has been **archivable** by an owner or admin: an
  archived channel has become read-only and hidden from the default channel list
  while its history remained searchable and retrievable (REQ-031/080), and it has
  been restorable. Archiving (reversible) is distinct from deletion, which is not
  offered for channels holding history. **Built (ARCH-93, migration 0024):**
  `archived_at_ms` non-NULL *is* the flag, so "when" is free and unarchive is one
  NULL write. Read-only holds for **every** writer. Send, threaded reply
  and attachment upload inherit it from `channel_post_access` and return
  `CHANNEL_ARCHIVED`; the incoming webhook is the one writer with no user behind
  it, so it cannot use that check — going through it would also re-test the
  creator's membership — and tests the same archived helper directly, answering
  `403`. The webhook itself is untouched, so unarchiving restores it. Hidden from the channel list for non-members; a
  member keeps it (and the way back). Owner/admin only.
- **REQ-036a.** A channel's **visibility has been changeable** by an owner/admin —
  public to private and back — without touching its membership or its history.
  **Built (ARCH-93):** two ops (`OC_CHUP_PRIVATE` / `OC_CHUP_PUBLIC`)
  rather than one toggle, so the request names a target state and two admins acting
  at once cannot flip it twice. No membership surgery in either direction: read
  access is already `is_public=1 OR is_member`, so **private** pins the audience to
  the people who actually joined (browsers lose access, and it leaves the directory),
  and **public** adds nobody — membership is a subscription, not permission.
  **The directions are not symmetric, and the product says so.** Private only
  narrows; public retroactively exposes everything said while the channel was
  private, which flipping it back does not undo. The client confirms that in those
  words, and the daemon audits the two as distinct actions so an audit reader can see
  which way it went without opening the row.
- **REQ-036.** A channel has been **renamable** by an owner/admin (or the
  channel's creator per a deployment setting), the rename applying everywhere the
  channel is shown without breaking membership, history, or permalinks (REQ-232).
  The name is distinct from the topic (REQ-034). **Built (ARCH-93):** owner/admin
  only, and the channel **id is untouched**, so membership, history and delivery
  cursors follow the rename with no work — verified against a live daemon by
  renaming a channel with 13 messages and 3 members and finding both intact.
  Migration 0020's unique-name index applies exactly as it does to a create, so a
  collision returns `CHANNEL_EXISTS`. **Deliberately no name-history table** — a
  permalink (REQ-232) must key on the id, not on a name that was always mutable
  (ARCH-93).
- **REQ-037.** *(Not built)* A deployment has been able to admit **guest accounts** — users
  restricted to one channel (single-channel guest) or an explicit set
  (multi-channel guest) rather than the whole tenant — created and scoped by an
  owner/admin. A guest has been an ordinary user (REQ-030) whose channel
  membership (REQ-031) is the whole of their reach: they have seen no channel they
  were not added to and have not browsed the directory (REQ-038). **[needs ARCH
  decision — guest as a distinct role vs. a membership-scoped flag, and its
  interaction with the role model (REQ-030).]**
- **REQ-038.** A user has been able to **browse the tenant's public channels** —
  a searchable directory of joinable channels they are not yet a member of — and
  join one directly, so discovering a channel has not required an invitation.
  Private channels (REQ-031) have never appeared in the directory. **[needs ARCH
  decision — directory listing query + join authorization for public channels.]**

### 1.4 Multi-Tenant Data Isolation

- **REQ-040.** No query path in the daemon has been able to return data
  belonging to a different tenant, because each tenant has run as a
  separate daemon process against a separate SQLite database file with no
  shared connection or shared query surface (ARCH-4, ARCH-7). Isolation has
  therefore been a property of the deployment topology, not of a
  tenant-ID filter inside shared queries.
- **REQ-041.** **No shared, always-on runtime component has existed in the
  message/data path — in any deployment model (ARCH-76).** Every message a
  tenant sends, stores, backfills, or searches has been handled solely by that
  tenant's own daemon and database (REQ-040); no OpenChime-operated service has
  ever received, relayed, stored, or been able to read message content, whether
  the deployment was stand-alone, federated, or hosted. Workspace resolution has
  been plain DNS (REQ-010, ARCH-14), so the name-to-daemon-address mapping has
  lived in static DNS records rather than in a resolution service, and no lookup
  traffic has reached the project.

  A **self-hosted federated** deployment (ARCH-76) has additionally depended on
  OpenChime-operated services for functions it opted into — OIDC login
  (ARCH-56), push delivery (ARCH-16), the app directory (REQ-175), SCIM
  (REQ-253), an optional DNS name and the workspace/audience registry (ARCH-14),
  and package distribution (ARCH-20). Each of these has brokered only
  **identity, notification, discovery, or provisioning metadata**: which user
  signed in to which workspace, that a notification is due and for whom, which
  integrations exist, which accounts to provision. **None has carried message
  content, and none has sat in the message path**, so a federated deployment has
  held exactly the same data-sovereignty guarantee as a stand-alone one — it has
  traded independence of *availability* (those functions stop when the project's
  services are unreachable) for capability, never confidentiality of messages.

  A **self-hosted stand-alone** deployment has depended on no OpenChime-operated
  service at all at runtime, at the cost of the federated-only features — most
  visibly mobile push (REQ-133). A **hosted** deployment has run the same daemon
  with the same per-tenant isolation, operated by the project.

  So multi-tenant *data* isolation has been unconditional across all three
  models; what has varied between them is only which non-message functions a
  deployment has delegated.

### 1.5 Workspace Administration

- **REQ-042.** *(Not built)* An owner/admin has been able to set **workspace-level settings and
  branding** — the workspace display name and icon, a set of default channels new
  members auto-join, and the tenant's join/invite policy — stored on the tenant
  and shown wherever the workspace is presented (the switcher, REQ-013; the
  client header). **[needs ARCH decision — workspace-settings storage + which
  settings are owner-only vs. admin-settable.]**
- **REQ-043.** *(Partly built)* An owner/admin has had an **administration console** distinct from
  the ordinary chat surface: a sortable member table with bulk actions (role
  change, remove — REQ-030/033), a channel-management view (archive/rename/delete
  — REQ-035/036), and workspace usage figures building on the storage and audit
  surfaces already present (REQ-214/251). **[needs ARCH decision — admin-console
  surface + which operations are bulk-capable.]**

---

## 2. Messaging

### 2.1 Core Messaging

- **REQ-050.** Every message has belonged to exactly one channel or one
  direct-message conversation, has recorded its author, and has recorded a
  server-assigned timestamp distinct from any client-supplied send time.
- **REQ-051.** A user has been able to edit their own message after sending
  it. An edited message has carried a visible "edited" marker and has
  retained its original send timestamp and position in history.
- **REQ-052.** A user has been able to delete their own message after
  sending it. Deletion has produced a tombstone record (author, timestamp,
  and thread linkage preserved; body removed) rather than removing the row,
  so that thread reply counts and quoted references have not broken.
  A tombstone has also dropped everything that hung off the body — its
  reactions, its pins, and **its attachments**. **Built:** reactions and pins are
  deleted outright; an attachment is *detached* (`message_id` set NULL), which is
  the orphan state the storage-maintenance sweep (ARCH-78) already reclaims — so
  it leaves the message immediately and its blob is collected by an existing,
  tested path rather than a second deletion mechanism.

- **REQ-053.** Full message history has had no retention cutoff or paid-tier
  history cap of the kind Slack's free tier imposes.
- **REQ-054.** A message body has been capped at approximately 64KB
  (ARCH-30). Attachment bytes (REQ-140) are exempt from this cap: they are
  chunked across many frames (ARCH-69) and bounded instead by
  `MAX_ATTACHMENT_SIZE`; each individual chunk still fits one frame.
- **REQ-055.** A user has been able to open a direct-message conversation with
  **themselves** — a private personal space ("notes to self" / saved messages) —
  realized as a single-participant DM. Like any DM it has been idempotent (a
  second open returned the existing self-DM), members-only, and reached through
  the ordinary messaging path; the daemon has not treated a self-target as an
  error (PROTOCOL.md §5.12, ARCH-50).
- **REQ-056.** A **group direct message** among three or more users has been
  supported as a first-class conversation distinct from a named channel — a
  participant-defined, unnamed DM any participant could post to but not rename or
  govern membership on. It has ridden the same message model as a 1:1 or self-DM
  (REQ-050/055), extended to N participants. The decision:
  **a group DM is a DM with more than two participants** — the same `channels.kind`,
  identified by the same `dm_key` (the sorted participant ids under a unique index)
  the 1:1 case has always used. No migration, no second code path for membership or
  read access, and no way for the two to disagree about what a DM is. Reopening the
  same set — in any order, by any participant — returns the same conversation.
  Capped at 9 people: a group DM with fifty in it is a channel, and pretending
  otherwise gives you a conversation nobody can name, govern or leave. The
  participant list rides `CHANNEL_INFO` and `CHANNEL_LIST`, so a client titles it by
  its people on first paint rather than after a roster fetch per group.
- **REQ-057.** *(Built in the Win32 client only)* A user has been able to **forward (share) a message** to another
  channel or DM they can post to, carrying a reference to the original — its
  author and a quoted excerpt — rather than copying the text opaquely, so a
  forwarded message has stayed attributable to its source.
  The reference is **structured, not prose** (ARCH-108): the client sends the two
  source ids on `SEND` and the daemon resolves the author, the excerpt and the
  attachment count from the row it holds, so a client cannot claim someone said
  something they did not, and the recipient renders a card it recognises rather
  than string-matching a sentence anyone could have typed by hand. The excerpt is
  a **snapshot** taken when the forward was sent — editing the original later does
  not rewrite what was forwarded.
  The forwarder must be able to **read the source**, or the message sends with no
  reference at all: without that gate, forwarding is a way to read a channel you
  were never in by asking the daemon to quote it at you. A source that is gone or
  unreadable degrades to an ordinary message rather than an error, because a stale
  permalink is not a failure the sender can act on.
  **The original's attachments do not travel.** The card names them — the first
  by filename, the rest as a count — and clicking the card opens the original; one attachment belongs to one message, so copying would
  mean either a second row sharing a `storage_key` — which breaks the orphan model
  reclamation counts on (ARCH-77/78) — or duplicating the bytes. Naming rather
  than offering is also what a recipient who cannot read the source needs: a
  download button would fail for exactly the people most likely to press it.

### 2.2 Threads

- **REQ-060.** *(Partly built)* Every message has been eligible to be replied to as a
  thread. A thread reply has not appeared inline in the parent channel's
  main scroll; the parent message has displayed a reply count and the most
  recent repliers.
- **REQ-061.** Replying in a thread has notified the thread's participants
  (message author plus prior repliers) according to their per-channel
  notification setting (REQ-130), independent of whether they were
  `@mentioned`.

  **Participation is the audience, and it is derived rather than stored**
  (ARCH-104): you are in a thread if you wrote its root or any reply, with
  `thread_follows` carrying only overrides — an explicit follow of one you never
  wrote in, and an explicit unfollow that **outranks having replied**, because
  otherwise "turn off replies" does nothing for the person most likely to want
  it. The push decision asks the same predicate the cross-channel thread list
  asks, so the view and the notification cannot disagree about who is in a
  thread.

  Being a participant satisfies the **mentions** level, exactly as a keyword hit
  does (REQ-135) — it is another way to pass the level, not a way around it, so
  mute, the schedule and the pause all still silence a reply and `none` still
  passes nothing. An `@mention` in a reply is an ordinary mention, stored and
  resolved as one anywhere else.
- **REQ-062.** *(Partly built)* A user has been able to **follow or unfollow a thread**
  independently of having replied to it, and has had a **followed-threads view**
  aggregating every thread they participate in or follow across channels, with
  unread reply counts — so keeping up with threads has not required revisiting
  each parent channel. A reply has optionally also been **posted to the channel's
  main scroll** rather than only the thread. **[needs ARCH decision —
  thread-follow storage + the aggregated cross-channel thread query.]**
- **REQ-282.** *(Not built)* A user has been able to **follow every thread in a chosen channel
  or DM** — a per-conversation setting distinct from following one thread
  (REQ-062) — so that every new thread started there, and every reply within it,
  has notified them under that conversation's level (REQ-130). This is the setting
  a person responsible for a channel actually wants: following threads one at a
  time requires already having seen the thread, which is the thing being missed.
  It has composed with mute (REQ-137) in the obvious direction — a muted
  conversation has stayed silent regardless — and that ordering is fixed by
  REQ-281's precedence, not decided per client. **[needs ARCH decision — storage
  is naturally a column on `notification_prefs` beside `level` and `muted`, since
  it is per (user, channel) exactly as they are.]**

### 2.3 Reactions

- **REQ-070.** A user has been able to attach one or more emoji reactions to
  any message they can read, including their own. A given user has been
  limited to one reaction of the same emoji per message (toggled, not
  stacked).
- **REQ-071.** A message has displayed an aggregate count per distinct
  emoji reacted with, and the identities of the reacting users have been
  available on inspection (hover/tap).
- **REQ-072.** *(Partly built)* A tenant has been able to register **custom emoji** (a named
  image usable in reactions and message text) beyond the built-in Unicode set,
  under admin control of who may add them. A reaction (REQ-070) references an
  emoji by a stable shortcode that resolves to either a Unicode sequence or a
  tenant custom-emoji asset; a text-only client (the TUI) shows the shortcode
  rather than the image, with one accepted limitation.
  Storage: the image is an **attachment id** (migration 0029's `custom_emoji`), so the
  existing store handles upload, the size cap, dedup and reclamation — a second binary
  store in SQLite would be a second thing to back up and a second thing to get wrong.
  Namespace: **flat and workspace-wide**, name as primary key, lowercased and
  restricted to `[a-z0-9_+-]`. `:shipit:` has to mean one image or every message
  containing it is ambiguous, so a duplicate is REFUSED rather than replacing the
  image every existing message already refers to. Who may add: any member (adding one
  is additive and cheap); **deleting** is the creator or an admin, because deletion
  breaks every message that used the shortcode. The TUI shows the shortcode, as this
  requirement always said it would.
  **Limitation:** in the Win32 client the image is drawn over its hidden shortcode, so
  the run keeps the shortcode's WIDTH — a short emoji leaves symmetric spacing around
  it. Closing that gap needs an `IDWriteInlineObject` (a COM object per emoji);
  recorded rather than pretended away.
- **REQ-073.** A user has had **one-click quick reactions** — a small,
  per-user-configurable set of frequently-used emoji offered inline on a message
  as a shortcut over the full picker (REQ-070/265). The set has been a per-user
  preference synced across their devices. **[needs ARCH decision — quick-reaction
  set storage (per-user) + the default set.]**

### 2.4 Search

- **REQ-080.** Full-text search has covered the complete message history
  visible to the searching user (i.e., channels they are a member of, per
  REQ-031), with no retention cutoff, implemented via SQLite FTS5 (ARCH-15).
- **REQ-081.** *(Partly built)* Search has accepted **structured operators** that scope a query —
  at minimum `from:<user>`, `in:<channel>`, `has:<attachment|link>`, and a date
  range (`before:`/`after:`/`on:`) — combined with free-text terms, all still
  bounded to history the searcher may read (REQ-031). **[needs ARCH decision —
  operator grammar + mapping onto the FTS5 query.]**

---

## 3. Delivery and Reliability

### 3.1 Message Delivery Guarantees

- **REQ-090.** The daemon has delivered every accepted message to every
  currently-connected, authorized client at least once. The wire protocol's
  `msg_type` enum has included an explicit ack message type used by the
  client to confirm receipt (ARCH-8).
- **REQ-091.** A client has treated redelivery of a message it has already
  acked as a duplicate and has suppressed it rather than rendering it twice,
  keyed on the server-assigned message id (REQ-050), using a per-channel
  high-water mark of the highest id seen (ARCH-45).
- **REQ-092.** Within a single channel, the daemon has delivered messages to
  a given connected client in the order the daemon accepted them. Ordering
  across different channels has carried no guarantee relative to each other.
- **REQ-093.** A send the daemon has not acknowledged (connection drop before
  ack) has been safely retryable by the client without risk of the message
  being accepted twice, via a client-generated 16-byte idempotency token
  distinct from the server-assigned message id, carried on the `SEND` frame
  and de-duplicated by a persisted `(channel, token) → id` mapping (ARCH-44).
- **REQ-094.** A message acknowledged to its sender has been durably committed
  to the tenant's local database before the ack was sent (ARCH-23) — the daemon
  has never acked on the strength of an in-memory buffer alone. **Off-box
  durability, and therefore the deployment's recovery point objective, has been
  a property of the deployment rather than of the daemon** (ARCH-3): in the
  hosted model the operator of the box has provided continuous replication and
  stated an RPO (implemented in the `openchime-saas` repo, not this one); in the
  self-hosted models the operator has backed up the SQLite file by their own
  means, and their RPO has been whatever that backup schedule gives them. The
  daemon has neither required nor assumed any particular backup mechanism.
- **REQ-095.** *(Partly built)* The system has optionally surfaced **read state** — for a direct
  message, whether the other participant has seen a message; for a channel, an
  aggregate "seen by" on inspection — derived from the per-(user, channel)
  delivery cursor the daemon already maintains (REQ-090, `CLIENT_ACK`). Read
  receipts have been a privacy-governed setting, off where a deployment or user
  disables them, and have never applied retroactively to messages sent before the
  feature was enabled. **[needs ARCH decision — receipt scope (DM vs. channel),
  privacy controls, and whether channel-level "seen by" is offered at all.]**

### 3.2 Reconnect and Offline Behavior

- **REQ-100.** A client that has lost its connection has reconnected
  automatically and has resumed the session without requiring the user to
  re-authenticate, so long as the JWT used to establish the prior session
  has not expired (REQ-023).
- **REQ-101.** On reconnect, the client has requested and received a replay
  of messages accepted by the daemon after the last message the client
  acked, across all channels the client is a member of, so that no message
  sent during the disconnection window has been silently missed, via a
  `BACKFILL_REQUEST` carrying per-channel `after_message_id` cursors answered
  with replayed messages and a `BACKFILL_DONE` marker (ARCH-46).
- **REQ-102.** *(Partly built)* A message composed while the client is disconnected has been
  queued locally in the client's **offline outbox** and sent automatically on
  reconnect, in the order composed, each with its original idempotency token
  (REQ-093), without requiring the user to resend it manually. The outbox lives
  **in memory** for the life of the process (ARCH-88): "queued locally and sent
  on reconnect" is what this requirement asks for, and surviving a process exit
  is not. **No client has embedded a database engine or written any file** — see
  REQ-201. The stated cost is that a message still queued when the client closes
  is lost, so a frontend warns before quitting with a non-empty outbox.

### 3.3 Protocol Versioning and Compatibility

- **REQ-110.** The daemon has rejected a connection whose declared protocol
  `version` it does not support, cleanly and before parsing any subsequent
  frame, rather than attempting to misparse a frame in an unrecognized
  version's layout (ARCH-8, ARCH-10).
- **REQ-111.** A protocol version rejection has carried a machine-readable
  reason code distinguishing "client too old" from "client too new," so a
  client has been able to present "please update the app" only when that is
  actually the correct remedy, via the `VERSION_TOO_OLD` / `VERSION_TOO_NEW`
  reason codes returned in the handshake `REJECT` frame (ARCH-41, ARCH-47).

---

## 4. Presence and Real-Time State

- **REQ-120.** Clients have reflected other users' online, away, and offline
  state in near-real-time, propagated via the `PRESENCE_UPDATE` message type
  (ARCH-8). Presence is derived from live connection state on the network thread
  and never persisted (ARCH-67): a `SET_PRESENCE` marks a connection away/online,
  a user is online while any connection is active and offline once the last
  closes, and each transition fans out tenant-wide to other authenticated
  clients while a newly authenticated client gets a snapshot of who is online.
- **REQ-121.** *(Partly built)* Clients have shown a transient "user is typing" indicator
  scoped to the active channel or thread, which has expired automatically
  client-side if no further typing signal arrives within a short window
  (avoiding a stuck indicator on an ungraceful disconnect). A `TYPING` signal is
  relayed as a member-scoped `TYPING_UPDATE` (ARCH-68), so private-channel and DM
  typing never leaks to non-members; the server keeps no expiry timer. Resolved:
  the indicator expires **client-side after ~6 seconds**, refreshed by each new
  `TYPING` signal — no "stopped typing" frame is sent.
- **REQ-122.** A user's **do-not-disturb and custom status** (REQ-136/278/241)
  has been surfaced to other users alongside their presence (REQ-120) — a
  distinct indicator on the presence dot plus a status line in the roster — so a
  colleague has known someone was unavailable before messaging them. This has
  been display only: it has not changed delivery, which the schedule and the
  pause govern separately.
  **Being away is said with the pause and the custom status**, not with a
  separate out-of-office state: "back Monday" is a status, and not being
  interrupted until then is a pause. A third mechanism would be a third thing to
  set and a third thing to forget to clear.

  **Availability and do-not-disturb are two INDEPENDENT axes, as in Slack.**
  Presence (REQ-120) answers *are they around* — active / away / offline.
  Do-not-disturb answers *may they be interrupted* — on / off. Neither implies
  the other: someone active can be paused, and someone away can be perfectly
  interruptible. Slack keeps them in separate APIs for exactly this reason
  (`users.getPresence` versus `dnd.info`) and renders DND as a badge *over* the
  presence dot rather than as a fourth presence value. Collapsing them into one
  enum would make "away" and "do not disturb" mutually exclusive, which is wrong
  in both directions.

  **What other people see is the FACT, not the timing.** A viewer learns that
  someone is not to be disturbed; they do not learn when it ends. Slack's API is
  explicit — every `snooze_*` field is visible only to the user themselves, and a
  query about somebody else returns only whether DND is on plus the next schedule
  boundaries. A colleague needs the first to decide whether to write; the second
  is a movement report about a person. (Slack's help centre says others "see a
  snooze icon", which is consistent: the badge reflects the fact. If finer
  behaviour ever matters it would have to be observed against Slack, whose docs
  do not say.)

  **How it is projected.** `PRESENCE_UPDATE` carries a second, **independent**
  DND byte beside the status byte — not a fourth status value, per the two-axis
  note above. **Both** do-not-disturb mechanisms answer it: the recurring
  schedule (REQ-136) and the transient pause (REQ-278). A sender wants the fact,
  and which mechanism produced it is not their business.

  The schedule half is evaluated through the shared `oc_notify_quiet`, the same
  rule the push path decides delivery with, so a colleague's badge and a phone's
  silence cannot disagree. Both halves are held in the net thread's memory, which
  has no database of its own (ARCH-67) — and are re-announced by its maintenance
  tick when the clock, rather than a person, changes the answer.

  Custom status with expiry (migration 0027) is shown beside names in the member
  pane and profile.

---

## 5. Notifications

- **REQ-130.** Each user has been able to independently set a notification
  level — all messages, mentions only, or none — per channel they belong to.
  Built (ARCH-72): `SET_NOTIFY_PREF` stores the level in `notification_prefs`
  (server-authoritative), and a `NOTIFY_PREFS` snapshot syncs it to all the
  user's devices.
- **REQ-131.** Absorbed by **REQ-136**, the single recurring quiet-hours
  mechanism: a daily do-not-disturb window is REQ-136's *Every day* mode with one
  start and end. What this requirement contributes survives there — quiet hours
  suppress notification delivery (push, and the client's own desktop toasts)
  without altering in-app unread state, because badges and counts are a record
  rather than an interruption. The **transient pause** to an absolute instant is
  REQ-278 (a different type — a minutes-of-day window is periodic by
  construction and cannot express "until 5pm today"), and a **workspace-level
  default** is REQ-279. The naming follows Slack's: the recurring half is the
  **schedule**, the manual half is the **pause/snooze**, they are separate
  mechanisms with separate ways to cancel, and cancelling one never cancels the
  other.
- **REQ-132.** *(Built in the daemon; no client reaches it)* Push notifications have been delivered via APNs on iOS/macOS
  and FCM on Android (ARCH-16), at no per-notification cost, per the
  providers' free tiers as of this writing.

  **Deferred until a mobile client exists — recorded, not scheduled.** Slack
  carries three settings that only mean anything once a phone is in the picture,
  and they are noted here rather than given requirement ids that would sit
  unbuilt indefinitely: (a) **mobile timing** — notify the phone immediately, only
  once the desktop is inactive, or after a further delay; (b) the **desktop
  inactivity threshold** that (a) is measured against; and (c) a **per-conversation
  mobile override**, so one channel can be louder on the phone than on the desktop
  (or the reverse). All three are refinements of the *same* notify decision
  (REQ-281), not new decisions — they choose the device and the moment, never
  whether the user is notified at all. They become real requirements when a mobile
  client does.
- **REQ-133.** *(Built in the daemon; no client reaches it)* Push delivery has been a **federated function** (ARCH-76): the
  published mobile clients are signed under the maintaining project's Apple and
  Google developer accounts, so only the project has been able to mint valid
  APNs/FCM credentials for them, and a self-hoster has not been able to register
  its own application identity for the published apps (ARCH-16). Consequently
  push has been available in the **self-hosted federated** and **hosted** models
  — routed through the project's push gateway — and **absent in self-hosted
  stand-alone**, whose mobile clients have fallen back to in-app/foreground
  notification only. Where the gateway has been used it has received only the
  fact that a notification is due and for whom, never message content (REQ-041).
  An operator unwilling to depend on the gateway and unwilling to lose push has
  had one remaining option: build and sign their own mobile clients under their
  own developer accounts, which is outside what this project distributes.
- **REQ-134.** Each user has had a **global (workspace-default) notification
  level** — all / mentions / none — applying to every channel lacking an explicit
  per-channel override (REQ-130), so a user has not had to set each channel
  individually. Where a per-channel level was set, it has won. Built:
  `users.notify_default` (migration 0028), `SET_NOTIFY_DEFAULT` (0x0077), carried on
  every `NOTIFY_PREFS` snapshot so no client infers it, and honoured by the push
  decision as `COALESCE(np.level, u.notify_default)`. Storage is on `users` rather
  than a sentinel row in `notification_prefs`: a default is a property of the person,
  and a channel_id of 0 in a table keyed by channel is a trap for every later query.
- **REQ-135.** *(Partly built)* A user has been able to define **keyword (highlight-word)
  notifications** — terms that notify them regardless of a channel's level
  (REQ-130/134) — and to designate **priority people** whose messages always
  notify. Both have driven notification like a mention (REQ-221).

  **Keywords are part of the `mentions` level, not a switch of their own.**
  Slack's middle level is literally "Mentions & keywords", so a
  channel set to *mentions* notifies on both and a channel set to *none* notifies
  on neither. Matching is **case-insensitive and exact** — "deploy" does not
  match "deployment" — and phrases are allowed, as Slack's are. A hit surfaces in
  the activity feed as a **mention**, not a fourth kind, for the same reason.

  **Keywords fire in threads, which is a deliberate divergence.** Slack's own
  help says keywords in thread messages never notify; that reads as a limitation
  rather than a decision, and a thread is where the substantive discussion
  usually is — the worst place to go deaf. REQ-061 already notifies thread
  participants, so this costs nothing extra.

  **Priority people pierce a level, and a pause, but never a mute.** A level and a
  pause are statements about *when and how much*; a VIP is a statement about
  *who*, and it is the recipient's own list — which is precisely why REQ-278
  declines a sender-side override while accepting this. Mute is different in kind:
  REQ-137 deliberately separated it from level as the strongest "I do not want to
  hear from this", and a VIP that overrode it would make mute unreliable at the
  moment somebody is reaching for it. Slack documents the pause case only and is
  silent on the other two; these are ours. **Settled by ARCH-103:**
  keywords one per row in `notify_keywords`, priority people as a
  relation in `priority_people`, and a hit written into `mentions` with its own
  kind — so the push query, the activity feed and the reader's highlight all keep
  working unchanged. Matched by `shared/mention.c`, never by SQL.
- **REQ-136.** A user has been able to configure a **recurring notification
  schedule**: notifications allowed **Every day**, **Weekdays**, or **Custom** —
  Custom carrying an independent start and end time **per day of the week** —
  suppressing notification delivery outside the allowed hours without altering
  in-app unread state. Slack's shape, and the reason its schedule is a *schedule*
  rather than a window: "quiet after 18:00 on weekdays and all weekend" is not
  expressible as one daily range. A user's own schedule has overridden any
  workspace-level default (REQ-279).

  **This is the ONLY recurring mechanism — it absorbs REQ-131.**
  Slack has exactly one: *Every day* with a single start and end simply **is**
  the daily-window case, which is why its API exposes one schedule and not two
  settings. A single window beside a per-weekday schedule would be
  two things able to disagree about the same question, which is the trap REQ-278
  is written to avoid.

  **Stored against the user's local calendar day** (their timezone, REQ-241), not
  UTC. A per-weekday window is only meaningful locally: in UTC somebody's "Friday
  evening" silently becomes Saturday for a large part of the world.

  **Settled by ARCH-103:** `dnd_mode` selects
  off/every day/weekdays/custom, the window columns carry the first
  three, and only *custom* adds rows to `notify_schedule`. The local day is
  resolved against a stored UTC offset the client refreshes on connect, not an
  IANA zone the daemon would have to parse per push.
- **REQ-278.** A user has been able to **pause notifications until a chosen
  instant** — "do not disturb until 17:00" — as a **one-shot** act distinct from
  any recurring window (REQ-131/136). The client has offered **durations**
  (30 minutes, 1 hour, 2 hours, until tomorrow) and a **custom** end time; the
  pause has expired on its own; and the user has been able to **resume
  immediately** without waiting for it. While paused, the **fact** that the user
  is not to be disturbed has been visible to other people beside their presence
  (REQ-122), so a sender knows before writing rather than after being ignored.

  **This is a different type from REQ-131, not a longer version of it.** A
  recurring window is a pair of minutes-of-day and is periodic by construction; a
  pause is a single **absolute instant**. Neither can express the other, which is
  why both exist in every product that ships this and why REQ-131 alone left "DND
  until 5pm" unbuildable.

  **The storage shape is already proven in-tree.** Custom status expiry
  (REQ-122/240, migration 0027) solved exactly this problem: an absolute expiry
  stamp, **enforced by the daemon on read** — because a client that is not
  running cannot clear its own state, and an expired value that simply *reads* as
  absent needs no clock on the reader's side and no sweep. A `dnd_until_ms`
  column applied the same way costs the push worker one comparison it already
  makes for the window. The client has computed the absolute instant (only it
  knows the user's timezone, so "until tomorrow morning" is a different moment
  per user), exactly as custom status does.

  **Settled: match Slack's model, which its API states exactly.**
  `dnd.info` exposes two independent mechanisms, and naming them apart is the
  whole design:

  | Slack | What it is | Ours |
  |---|---|---|
  | `dnd_enabled`, `next_dnd_start_ts`, `next_dnd_end_ts` | the **schedule** — recurring, planned | REQ-131 + REQ-136 |
  | `snooze_enabled`, `snooze_endtime`, `snooze_remaining` | the **pause** — manual, one-shot | this requirement |

  `dnd.setSnooze` takes **minutes from now**, so every preset is a duration and
  the absolute instant is derived — 30 minutes, 1 hour, 2 hours, until tomorrow,
  or a custom end time. `dnd.endSnooze` ends the pause early and is a distinct
  act from `dnd.endDnd`, which ends the *current scheduled* period: two
  mechanisms, two ways to cancel, and cancelling one has never cancelled the
  other. Slack also carries `snooze_is_indefinite`, which its own reference says
  cannot be set to true; we do not implement it.

  **A pause only ever ADDS silence.** It cannot un-silence someone inside their
  scheduled quiet hours — there is no "notify me anyway until 17:00" in Slack or
  in this requirement — so a pause and a schedule never disagree in a way needing
  a precedence rule. When a pause expires the schedule simply applies again.

  **The fact is public; the timing is private.** Slack's help says other people
  see a snooze icon, while its API marks every `snooze_*` field self-only and
  returns just `dnd_enabled` and the next schedule boundaries for another user.
  The coherent reading, and the one adopted here: others learn **that** someone
  is not to be disturbed, never **when they will be back**. A colleague needs the
  first to decide whether to write; the second is a movement report. See REQ-122
  for the presence surface that carries it.

  **VIPs pierce a pause; senders do not.** Slack allows both — a nominated VIP
  always gets through, and a sender may additionally force one urgent DM past a
  pause once a day. The VIP half is adopted (it is REQ-135's priority people, and
  it is the *recipient* choosing who may reach them). **The sender override is a
  deliberate divergence and the only one in this requirement:** a
  do-not-disturb any sender can pierce is a weaker promise than the words
  suggest, and the legitimate need behind it is already served from the right
  side by the VIP list. Recorded here so the difference from Slack is a choice
  rather than an omission.

  **The storage** is `dnd_until_ms` on `users` beside the ARCH-72 window, the
  proven pattern above. Ending early sets it to 0; there is no second op.
- **REQ-279.** *(Not built)* A workspace **owner or admin has been able to set default
  do-not-disturb hours** for the workspace, applying to members who have not
  configured their own, and **any member has been able to override them** with
  their own schedule (REQ-136) or turn do-not-disturb off entirely. A default
  that could not be overridden would be an availability policy rather than a
  notification preference, and this is deliberately the latter. **[needs ARCH
  decision — tenant-level setting storage (no tenant settings surface exists
  today, REQ-042) and the resolution order against REQ-131/136/278.]**
- **REQ-137.** A user has been able to **mute a channel or DM**: muting has
  suppressed its notifications **and de-emphasized it in the sidebar** (dimmed,
  excluded from the unread badge) without the user leaving it — distinct from
  setting its level to `none` (REQ-130), which governs notification only.
  Storage is `notification_prefs.muted` (migration 0026), set with `SET_MUTE`;
  push excludes a muted conversation unconditionally, so mute outranks both the
  level and a priority person.

  **What a muted conversation contributes to a badge is nothing**, and every
  badge asks one function for it. A muted conversation's unread is excluded;
  thread replies are included, because they bump no channel's unread and a day
  of nothing but replies must not leave a badge claiming there is nothing. The
  taskbar overlay and the workspace rail's "N elsewhere" both read that one
  rule — they each summed it themselves once, and disagreed, which is how a
  muted channel in a background workspace still lit the rail.

  REQ-284's finer question — whether a badge should count every unread or only
  what would have notified — is still open and is deliberately not answered
  here.
- **REQ-138.** *(Partly built)* A client has surfaced **OS-native desktop notifications** (a system
  toast) for messages due under the user's notification settings (REQ-130/134),
  with a content-preview toggle, plus optional **notification sounds and unread
  badges** on the app icon. These are per-client rendering of the server's notify
  decision (ARCH-72), not a new server surface. **Built on Win32:** toasts and
  tray balloons gated by the shared notify evaluator (`shared/notify.c`,
  REQ-281), a taskbar overlay **unread badge**, and a taskbar **flash** on
  notification (REQ-286). Not built: every other frontend.

  **Two settings, because there are two questions.** *Where* a notification
  appears — Windows' notification centre, OpenChime's own window, or nowhere —
  and *how much* it says (Off / Count / Preview). Someone who wants the OS
  surface and someone who wants a preview are not answering the same thing.

  **The default is the real OS notification**, and it is a real one: a
  Notification Center toast, not a tray balloon. **Delivery is a CHAIN, not a
  mechanism** — OS toast, else the balloon, else the client's own window. The
  balloon is no longer how notifications arrive but remains the one surface
  needing no identity, and the chain exists because a single mechanism that
  fails silently is what this replaced: a `Shell_NotifyIcon` that failed at
  logon dropped every notification for the session with nothing to reach for.

  **The client's own notification is its own window** — borderless, topmost,
  and never taking focus — not a panel inside the shell. A notification exists
  for when you are *not* looking at the app, and since closing the window hides
  it the shell's resting state is invisible. It shows the app's mark, the
  conversation, and who said what; Windows already labels its own toasts with
  the application, so spending the title on the app name says only what the
  icon says.

  **A click opens the conversation**, on every surface. The OS toast carries an
  `openchime://` permalink and is activated by protocol, so it needs no COM
  activator: the app already speaks those links. A URL arriving on the command
  line is handed to the **already-running** client rather than starting a
  second one.

  **Sounds are the SYSTEM'S OWN, named rather than shipped.** Windows already
  has notification sounds and the user has already chosen how they behave, so
  both surfaces ask for one by name — the toast XML through `ms-winsoundevent:`,
  the client's own window through `PlaySound`'s `SND_ALIAS`. Nothing is bundled,
  and somebody who has quietened or changed their notification sound is followed
  rather than talked over. Per event, because a cue is only useful if it
  distinguishes, with one mute that leaves the individual choices intact.
  Exactly one thing plays it: Windows for the toast — which is also the only way
  the sound obeys Focus Assist — and the client for its own window.

  **A notification can be acted on without opening the app**: a reply box and a
  quick reaction, sent from the toast itself. The reply is the reason the
  client registers a COM activator at all — Windows delivers an `<input>`'s text
  only to one, and protocol activation, which carries the toast body's own
  click, cannot. Buttons activate in the *background*, because reacting to a
  message should not drag you into the application.

  **Text size is the only size control.** A per-window zoom was removed: it was
  a process-wide global in an application with one window, so its only distinction
  from the text-size preference was being forgotten on restart. Display scaling is
  the display's, read from the OS and applied at the rendering seam.

  **Closing the window quits, unless you ask it not to.** The close button is a
  setting — *Quit* or *Hide to tray* — under System, and it defaults to **Quit**,
  because close meaning close is what a window button is generally taken to
  mean. The cost is real and is the reason the other option exists: a client
  that quits on close stops notifying, which is the whole point of the
  application. Minimising is a separate switch, off by default, because wanting
  the taskbar button gone while working is a different wish from wanting the
  close button to mean something unusual.

  **When set to hide, the app keeps running and keeps notifying.**
  A chat client that stops notifying the moment its window closes has stopped
  being one, and the tray icon existed as a mailbox with nothing behind it. The
  window is restored by clicking the tray icon, and quitting is a deliberate act
  — Ctrl+Q, or Quit from the tray menu — which is where the unsent-outbox
  question is now asked. **Said once**, by a balloon the first time it happens:
  an app that vanishes without a word reads as a crash. That notice is remembered
  per account, because the thing you learn is what closing does, and you learn it
  once.
  The fallback matters: if the tray icon could not be created there is nothing to
  restore the window from, so closing quits as it always did rather than leaving
  a process that can be neither reached nor closed.
  A **hidden window can still be the foreground window** — `SW_HIDE` hands the
  foreground to nobody — so "are you looking at this?" is answered by
  foreground **and** visible **and** not minimised. Asking only the first
  suppressed every toast while the window was closed to the tray, which is
  exactly the case this exists for.

  **Sounds have been per event type, not one sound for everything** — a distinct,
  user-chosen sound for a new message, a priority-person message (REQ-135), a
  call starting (REQ-285), and a direct message received while already viewing
  that conversation — plus a single **mute-all-sounds** switch that has silenced
  every one of them without disturbing the individual choices. Slack's set, and
  the reason for it is that an audible cue is only useful if it distinguishes:
  one sound for all events carries no more information than a badge. Sound choice
  is a **client preference** (the synced `client_settings` bucket, ARCH-88), never
  a server surface — the daemon decides *whether* to notify, never how it sounds.
  **[needs ARCH decision — per-platform notification API mapping +
  preview/privacy toggle.]**
- **REQ-139.** A client has offered an **activity feed / notification inbox** — an
  aggregated, filterable view of @mentions (REQ-221), reactions to the user's
  messages (REQ-070), and thread replies (REQ-060/061) across all channels — so a
  user has caught up on what involved them without scanning every channel.
  **Built (ARCH-95):** neither of the two options this marker offered — a
  client-side fold is impossible for a client that stores nothing (ARCH-88), and a
  maintained list would duplicate rows already indexed and add three write paths
  that can drift. It is a **union of three bounded queries** over `mentions`,
  `reactions` and threaded `messages`, excluding your own actions and gated on
  current membership. `users.activity_seen_ms` is a watermark — enough to mark
  what is new, deliberately not per-item read state.

  **The filter set is Slack's.** The three kinds above answer
  "what involved me". Slack's Activity also answers "what have I not read",
  through three further filters that are now in scope:

  | Filter | What it lists |
  |---|---|
  | **Unreads** | every message not yet read, in any conversation the user is in |
  | **DMs** | unread direct messages |
  | **Channels** | unread messages in channels whose notification level is *all* |

  This is a genuine widening, and worth naming: the feed stops being only "things
  addressed to me" and becomes an inbox. The three share one shape — *messages
  past my read cursor, in conversations I belong to*, filtered by channel kind or
  notification level — so they are one query with three predicates rather than
  three features, and the cursor they need is `delivery_cursors`, which REQ-090
  already maintains per (user, channel).

  Slack's **saved custom views** are not adopted: they are a way to cope with a
  filter set larger than this one, and inventing persistence for view
  combinations before the filters exist would be building the lid before the box.
- **REQ-280.** *(Excluded by decision)* **Email notifications have not been provided, in any deployment
  model.** No digest, no "you were mentioned" mail, no unread summary — no
  notification email of any kind. (The one outbound-mail path contemplated
  anywhere in the product is REQ-276's opt-in compliance journaling, which is an
  operator-configured compliance export, not a notification.) Two reasons, both deliberate. **Operationally**, it
  would oblige every self-hoster to run or rent a mail relay and inherit
  deliverability, bounce, and unsubscribe handling — a second delivery system
  with its own failure modes, for a product whose entire delivery story is
  otherwise one TLS connection. **On principle**, an email notification puts
  message content (or, at minimum, who is talking to whom) into a third-party
  mail system, which is precisely what a self-hosted deployment exists to avoid;
  "we email your messages to Gmail" would undercut REQ-040/041's whole claim.

  **The consequence has been stated rather than discovered.** Push is a federated
  function (REQ-133) — only the project can mint APNs/FCM credentials for the
  published apps — so with email excluded, a **self-hosted stand-alone
  deployment has no out-of-app notification path at all**. Not a degraded one:
  none. A user who is not looking at a client is not reached, and their unread
  state is waiting when they return. That is an acceptable, even coherent, trade
  for an air-gappable deployment, but an operator has been told it up front
  rather than inferring it after choosing stand-alone.
- **REQ-281.** *(Partly built)* The decision *whether to notify a given user about a given
  message* has been made in **exactly one evaluator**, with a **documented
  precedence order**, and covered by a **truth table** rather than case-by-case
  tests. **The evaluator exists:** `shared/notify.c` (`oc_notify_decide`, with
  `oc_notify_quiet` for the schedule) states the precedence — own message never;
  mute is absolute; a priority person pierces everything else; then the schedule
  and the pause; then the level, with keywords part of *mentions* — and the Win32
  client's toast gate consults it. **What keeps the marker partial:** the
  daemon's push query still states the same order in SQL (a test suite pins the
  two against each other), and the intended end state is that it fetches rows
  and asks the shared function instead. Every input has fed that one function: the per-channel level (REQ-130),
  the global default (REQ-134), mute (REQ-137), follow-every-thread (REQ-282),
  the recurring DND window (REQ-131), the per-weekday schedule (REQ-136), the
  transient pause (REQ-278), the workspace default (REQ-279), keywords and
  priority people (REQ-135), and mention resolution (REQ-221).

  **This exists because the failure mode is precedence errors assembled
  inline** — silent, and invisible until somebody who muted a conversation is
  notified anyway. Two properties make the order testable rather than merely
  written down. First, the evaluator is **pure** — inputs in, a decision out, no
  I/O — so the truth table is a unit test with no daemon, matching how
  `shared/mention.c` and `shared/searchq.c` are already shared and tested.
  Second, it must be the **single** implementation both the push worker and
  every client consult, for the same reason ARCH-89 gives for the mention
  scanner: two copies of a notify rule will disagree, and the disagreement will
  be invisible — one side silently notifying where the other would not.
  **[needs ARCH decision — the evaluator is implemented without a decision
  record; the precedence it enforces (mute absolute; priority people piercing
  the level, the schedule and the pause but never a mute; keywords inside the
  *mentions* level) should be recorded as an ARCH entry when the push query is
  folded into it.]**
- **REQ-283.** *(Not built)* A user has been able to set a **reminder** — on a message ("remind
  me about this" at a chosen time) and as a **due date on a saved item**
  (REQ-231) — and has been notified when it came due, through the same notify
  path as everything else (REQ-281) and rendered by the same client surfaces
  (REQ-138/139). A reminder has been **private** to the user who set it, exactly
  as a saved item is, and has survived a client restart because it lives on the
  server.

  **In scope, and narrower than Slack's.** Delivery has been **in-app and push
  only** — never email (REQ-280) — so a stand-alone deployment's reminder waits
  in the activity feed rather than chasing the user, consistent with that model's
  stated trade. Slack routes reminders through Slackbot as a DM; a conversation
  with a bot is not a thing this product has (REQ-275), so the reminder has
  surfaced as an activity-feed entry and a notification instead of a synthetic
  message from a fake user.

  **The cheap parts and the one real part.** Storage is small — saved items
  (ARCH-95, migration 0025) already hold (user, message) and want only a
  `remind_at_ms` beside the existing stamp — and **delivery has a precedent**: the
  daemon already runs a bounded periodic maintenance pass off the net loop's tick
  (ARCH-78), which is the natural place for a due-reminder sweep and means no new
  scheduler, thread, or timer subsystem. **[needs ARCH decision — whether the
  sweep is that pass or its own, and the granularity guarantee: a reminder that
  may fire minutes late is a different product promise from one that fires on the
  minute, and the maintenance interval defaults to 5 minutes.]**
- **REQ-284.** *(Not built)* What the **unread badge counts** has been defined and
  user-controllable, rather than left implicit. A user has been able to choose
  whether the badge counts **every unread message** in a conversation or only
  messages that would notify them under their settings, and whether **thread
  replies that do not mention them** count toward it at all. The default has been
  the quieter reading — the badge reflects what was worth notifying — because a
  badge that counts everything is a badge nobody reads.

  **This is a requirement about meaning, not three settings.** Today the product
  ships badges (REQ-138) and per-channel unread (REQ-014) with no statement
  anywhere of what a number on a conversation *represents*, which is how two
  clients end up disagreeing about the same count and both looking correct.
  Mute already removes a conversation from the badge (REQ-137); this defines the
  rest of the rule. **[needs ARCH decision — whether the choice is a client
  preference or server state; it affects the count a client displays, not the
  notify decision, which argues for the synced `client_settings` bucket rather
  than a new server surface.]**
- **REQ-285.** *(Not built)* A user has been **notified when a call has started** in a channel
  or DM they are a member of, subject to the same notification settings as a
  message (REQ-281) — because a call is time-sensitive in a way a message is not:
  a missed message is read later, a missed call is simply missed. The
  notification has named the conversation and who started it, and joining from it
  has landed the user in the call.

  Recorded now, ahead of the client it needs. The daemon's call signaling and
  ephemeral roster already exist (REQ-150–152, ARCH-73) — `CALL_JOIN` on an empty
  roster *is* the "call started" event — so this is a notify decision over state
  the server already keeps, not new call machinery. It ships with the audio
  client rather than before it. **[needs ARCH decision — whether call-start is a
  level a user can set independently, as Slack does, or simply follows the
  conversation's level.]**
- **REQ-286.** *(Partly built)* A desktop client's **window and notification-area behaviour has
  been specified and user-controllable**: whether closing the window **quits the
  application or leaves it running in the notification area / tray** still
  receiving notifications, and whether the taskbar entry is **flashed or
  highlighted** when a notification arrives. The default has been to keep running
  in the tray, because a chat client that stops notifying the moment its window
  is closed silently breaks every other notification requirement in this section.

  **The flash half is built on Win32** (the taskbar entry flashes, and carries an
  unread overlay badge, REQ-138). **The close-to-tray half is not:** the Win32
  client ships a tray icon and raises tray balloons while `WM_CLOSE` quits after
  warning about a non-empty outbox — so closing the window ends notifications,
  against this requirement's stated default. This is per-platform surface
  (ARCH-92), not a server concern. **[needs ARCH decision —
  none expected; this is a client preference in the synced bucket, but the
  *default* is a product call.]**

---

## 6. Attachments and Media

### 6.1 File Attachments

- **REQ-140.** A user has been able to upload and share a file attachment in
  a channel, thread, or direct message, persisted outside SQLite (ARCH-17) —
  either on the box's **local disk** or in an **operator-supplied S3-compatible
  service**, selected by whether S3 credentials are configured (ARCH-70).
  OpenChime has shipped **no object-storage server of its own** in any
  deployment model: where S3 is used the daemon has been purely a *client* of a
  service the operator provides. Bytes are **proxied through the daemon** over the
  existing pinned-TLS connection in chunks (ARCH-69, PROTOCOL.md §5.14); the
  blob lands in object storage behind a swappable adapter (ARCH-70) while only a
  pointer + metadata row is stored in SQLite (SCHEMA.md migration 0009). An
  attachment is published by referencing it from a message, so it rides the one
  message model through delivery, backfill, threads, and DMs.
- **REQ-141.** An attachment has remained retrievable by any user authorized
  to read the message it is attached to (REQ-031), and by no one else, for
  as long as the message itself exists — **except where storage pressure forced
  its eviction (REQ-215) or an operator's retention policy aged it out
  (REQ-250)**. In both exceptions the attachment's metadata has survived as a
  tombstone so the message stays intelligible; the *authorization* rule below is
  unconditional and has never been relaxed for any reason. Resolved (ARCH-69):
  because every byte
  is proxied through the daemon, access control is a **single in-daemon check on
  the same membership path as reading the message** (`channel_read_access`) —
  there is no signed-URL scheme, TTL, or object-store ACL. Proxying (not
  presigned URLs) is forced by TOFU pinning (ARCH-10, one trusted cert, no CA
  bundle) and the island model (ARCH-4/26, the tenant's object store stays private).
- **REQ-142.** *(Built in the Win32 client only)* A graphical client has **rendered image attachments inline** — a
  thumbnail in the transcript, expandable to a preview — rather than showing only
  a filename line, for the common image types. This is a graphical-frontend
  capability; the **text-only TUI is exempt** (ARCH-75, no graphics), continuing
  to show the attachment line. Non-image types have shown a typed placeholder.
  The Win32 implementation decodes the common image types client-side (WIC) with
  a client-side thumbnail cache; no server-side thumbnailing exists.
- **REQ-143.** A user has been able to browse a channel's **files** — a view
  listing the attachments shared in a channel (and, per-user, files they shared or
  that were shared with them) — building on the attachment metadata already
  stored (REQ-140, SCHEMA.md migration 0009). **Built (ARCH-91):** `LIST_FILES`
  streams a channel's shared files newest-first, or (channel 0) every channel the
  caller can read; migration 0023 adds the one index that access pattern needed.
  Pending uploads are excluded, reclaimed rows are listed and flagged. Filtering
  by type is client-side over the returned `mime`.

### 6.2 Audio Conferencing

*Design: [AUDIO.md](./AUDIO.md).*

- **REQ-150.** *(Built in the daemon; no client reaches it)* A user has been able to start or join a server-relayed audio
  call scoped to a channel or a direct message. No peer-to-peer or ICE
  negotiation path has existed (ARCH-18). Built (server side): `CALL_JOIN`
  forms/joins the channel's call and returns a UDP media endpoint + token.
- **REQ-151.** *(Built in the daemon; no client reaches it)* Audio has been encoded with Opus and relayed over an isolated
  UDP-based sidecar process, kept out of the daemon's TCP event loop so a
  call cannot starve message delivery on the same tenant (ARCH-18). Built: the
  forked `audio_sidecar` relays opaque Opus payloads over UDP (ARCH-28/31/73);
  the daemon never touches the codec. The client-side Opus encode/decode is
  Phase-2 client work.
- **REQ-152.** *(Built in the daemon; no client reaches it)* A participant's connection loss during a call has not
  terminated the call for other participants; the daemon has continued
  relaying for remaining participants and has allowed the disconnected
  participant to rejoin. Resolved (ARCH-73): the call roster is ephemeral
  net-thread state; a `CALL_LEAVE` or TCP disconnect drops the participant and
  re-rosters the rest, but the call persists while ≥1 participant remains, and
  rejoin is a fresh `CALL_JOIN` (new token). The media-side mirror is a UDP
  silence timeout in the sidecar. **Signaling built (PROTOCOL.md §5.17); the UDP
  relay + timeout land in the sidecar milestone.**

### 6.3 Video — camera video excluded, screenshare admitted

*Design: [VIDEO.md](./VIDEO.md).*

- **REQ-160.** *(Excluded by decision)* **Camera video** calling, and video streaming or playback beyond
  generic file-attachment handling (REQ-140), have not been supported. This is a
  deliberate scope exclusion, not a deferred feature pending an architecture
  decision. **Scoped by ARCH-86:** the exclusion covers *camera* video
  and general playback; **screenshare** is admitted separately as REQ-161, whose
  content profile (mostly static, low frame rate, a single sender) is
  fundamentally cheaper and whose use case is concrete. The exclusion is narrowed,
  not repealed.
- **REQ-161.** *(Not built)* A participant in a call has been able to **share a screen or
  window** to the other participants, view-only. Screenshare has ridden the
  existing server-relay media path unchanged (ARCH-73/86): the sidecar forwards
  the encoded payload opaquely exactly as it forwards Opus, so no server-side
  codec has existed and no transcoding has been possible. Consequently **all
  clients have spoken one mandatory codec — VP9 via libvpx (ARCH-87)** — since a
  call may hold clients on different platforms simultaneously and a codec
  disagreement would break the call outright; the codec has therefore been
  negotiated on the wire (PROTOCOL.md §5.17) rather than chosen per frontend.
  Remote control, recording, screen-audio capture, simultaneous sharers, and
  camera video have all been out of scope (REQ-160). A **text-only frontend has
  been permanently exempt** (ARCH-75 — the TUI renders no graphics), showing only
  that a share is in progress and by whom. **Not started; sequenced behind the
  audio client** (REQ-150–152), whose media transport it builds on.

---

## 7. Integrations

- **REQ-170.** A third-party service has been able to post a message into a
  channel via an incoming webhook URL scoped to that channel, without that
  service holding a user session or JWT, over the embedded HTTP listener
  (ARCH-32). Resolved (ARCH-71): the URL is `POST /webhook/<token>` where the
  token is 32 random bytes (hex), stored **hashed** (SHA-256) like a session and
  minted per-channel by a member via `CREATE_WEBHOOK` (shown once). The payload
  is `application/json {"text": "..."}` or a raw `text/plain` body, capped at
  `MAX_BODY_SIZE`; the message is posted as the webhook's creator. A per-token
  fixed-window rate limit (60/min) bounds abuse. The endpoint is ALPN-demuxed on
  the proto port (ARCH-54): the daemon advertises `oc/1` and `http/1.1`, and a
  connection that doesn't negotiate `oc/1` is routed to the HTTP handler.
- **REQ-171.** *(Not built)* A tenant that has enabled webhooks has had a CA-signed TLS
  certificate obtained on-demand for that endpoint, since third-party
  webhook senders validate against a standard CA trust store and cannot pin
  a custom certificate (ARCH-34). A tenant with webhooks disabled has had no
  such certificate and no such requirement. **Deferred (infrastructure):** the
  webhook receiver (REQ-170) currently reuses the daemon's TOFU cert, which
  works for controlled senders and the test suite. On-demand ACME/CA issuance is
  intentionally left for a later infrastructure milestone — it needs public HTTP
  or DNS reachability to complete a challenge (ARCH-10's rationale), which is a
  deployment concern separate from the daemon's request handling. The SNI-based
  cert selection at the TLS layer (ARCH-34) is the wiring that lands with it.
- **REQ-172.** *(Not built)* A tenant has been able to install **app integrations** that post
  and respond in channels under a **bot identity** distinct from human users,
  including **slash-command apps** invoked as `/command` from the composer and
  dispatched to a registered integration endpoint. Install has been role-gated to
  owner/admin. **[needs ARCH decision — bot identity model, slash-command
  registration + dispatch, install authorization.]**
- **REQ-173.** *(Not built)* The system has supported **outgoing webhooks / event
  subscriptions**: a tenant-registered endpoint has received notifications of
  selected events (new message in a channel, mention, membership change) so
  external systems could react to activity — the outbound complement to the
  incoming webhooks of REQ-170. Delivery has been at-least-once with retry and a
  signed payload so the receiver could verify origin. **[needs ARCH decision —
  event catalog, delivery/retry, payload signing.]**
- **REQ-174.** *(Not built)* The system has offered **workflow automation** — declarative
  triggers (a message match, a schedule, a form submission) driving actions (post
  a message, call a webhook) — so common routines could run without an external
  app. This may reduce to a first-party consumer of the app platform (REQ-172).
  **[needs ARCH decision — workflow model + execution surface.]**
- **REQ-175.** *(Not built)* Installable integrations have been discoverable through an **app
  directory** curated by the maintaining project, from which an owner/admin could
  install an app into their tenant. Directory hosting has been a **federated
  function** (ARCH-76, cf. ARCH-56), not the daemon's — so the directory has been
  available to self-hosted federated and hosted deployments, while a self-hosted
  stand-alone deployment has installed integrations by direct configuration
  instead of browsing a curated catalog. **[needs ARCH decision — directory
  hosting + per-tenant install/permission model.]**
- **REQ-176.** *(Not built)* The system has offered a **third-party API / SDK** — a documented
  programmatic surface for external tools to read and post on a user's or bot's
  (REQ-172) behalf — as the general-purpose complement to the single-purpose
  incoming/outgoing webhooks (REQ-170/173). Because the client-daemon wire is a
  custom binary protocol (ARCH-6), an external API has been an additional surface
  (likely HTTP over the ALPN-demuxed listener, ARCH-32/54), not the wire itself.
  **[needs ARCH decision — API transport + shape (REST vs. exposing the binary
  protocol), auth, and rate limiting.]**
- **REQ-177.** *(Not built)* A channel has been able to receive posts by **email-to-channel
  ingestion** — a per-channel address that turns an inbound email into a channel
  message — the inbound-email complement to incoming webhooks (REQ-170). Delivery
  has required inbound email reception, a deployment capability separate from the
  daemon's request handling (cf. the webhook CA-cert note, REQ-171). **[needs ARCH
  decision — inbound-mail placement (out-of-daemon helper), address→channel
  mapping, and spam/authorization controls.]**

---

## 8. Security Posture

### 8.1 Transport and Session Security

- **REQ-180.** Every client-daemon connection has been encrypted in transit;
  the system has offered no unencrypted TCP fallback (ARCH-6, ARCH-10).
- **REQ-181.** A session has been the daemon's own to control: after a
  successful auth (REQ-023) the daemon has minted an opaque session token with a
  daemon-set expiry, recorded in a local `sessions` table (ARCH-58), rather than
  binding the session lifetime to an external provider token's expiry. The
  daemon has not extended a session past its recorded expiry.
- **REQ-182.** A user has been able to revoke an active session (e.g. "log
  out other devices") from any authenticated client; the daemon has deleted the
  targeted `sessions` row and terminated the targeted connection on its next
  protocol interaction (ARCH-58). Because sessions are validated against the
  daemon's own local session table — not against a stateless provider token —
  revocation is an immediate row delete.
- **REQ-183.** Certificate trust for the client-daemon connection has been
  established via TOFU (trust-on-first-connect) pinning against a
  self-signed certificate the daemon generates on first run, not CA-chain
  validation, uniformly across all three deployment models (ARCH-76) (ARCH-10).
  The one exception has been the incoming-webhooks endpoint (REQ-171), which
  uses a real CA-signed certificate because its clients are uncontrolled
  third parties.
- **REQ-184.** *(Not built)* In local mode (REQ-024) the daemon has supported optional
  **multi-factor authentication** — a second factor (TOTP) enrolled per account
  and required after the password check (ARCH-59) — so a compromised password
  alone has not granted a session. OIDC mode (REQ-020) has inherited whatever MFA
  the provider enforced, so this requirement targets local accounts. **[needs ARCH
  decision — second-factor type (TOTP), enrollment/recovery flow, and storage.]**

### 8.2 Abuse Prevention and Rate Limiting

- **REQ-190.** The daemon has rate-limited message-send frequency
  per-connection, rejecting excess sends with a distinct error rather than
  silently dropping them. **Built:** a fixed-window counter in the net loop —
  **30 sends per 3 s per connection** (`OC_SEND_RATE_MAX` /
  `OC_SEND_RATE_WINDOW_MS`, `daemon/netloop.c`) — over the limit returns the
  non-fatal `SEND_RATE_LIMITED` (3004). A fixed window rather than a token bucket
  because the check runs per frame on the hot path and the window is short enough
  that the boundary burst it permits is irrelevant at these numbers. The values
  are compile-time, not configurable: nothing has yet needed to tune them.
- **REQ-191.** The daemon has rate-limited failed authentication attempts,
  per account and per source, to blunt credential-stuffing and brute-force
  against local-mode passwords, answering excess attempts with
  `AUTH_RATE_LIMITED` (ARCH-59). (OIDC-mode credential checking is a signature
  verification against a pinned key, not a guessable secret, so the local-auth
  path is the meaningful target.)
- **REQ-192.** *(Not built)* A deployment has optionally restricted access by **source
  network** — an IP allowlist / CIDR restriction enforced at connection accept,
  refusing a connection from outside the permitted ranges before authentication —
  for organizations requiring network-level access control. The default has been
  unrestricted. **[needs ARCH decision — allowlist config + accept-time
  enforcement, adjacent to the per-IP connection throttle already in the accept
  loop.]**

---

## 9. Client and Platform Support

- **REQ-200.** *(Built in the Win32 client only)* Native clients have run on Linux, Windows, macOS, iOS, and
  Android. This platform list is inferred from the packaging decisions
  (ARCH-20, ARCH-21), the platform-native auth session APIs required by
  REQ-020 (iOS/macOS), and push delivery via both APNs and FCM (REQ-132);
  it has not been separately confirmed as a standalone decision and should
  be. **[needs ARCH decision — explicit platform commitment, including
  whether Android/macOS packaging follows the Linux/Windows model in
  ARCH-20/ARCH-21]**

- **REQ-201.** **No client has stored anything locally beyond its credentials.**
  Every client's durable state — the session token, the TOFU pin (REQ-183) and the
  remembered-workspace book (REQ-012) — has lived in the **operating system's
  credential store**, one entry per workspace, and a client has embedded no
  database engine and written **no files** (ARCH-88). Cached history has not been
  kept at all, and the offline outbox (REQ-102) has lived in memory for the life
  of the process. This has been possible because the **read position is
  server-side** (REQ-090): a client that remembers nothing asks the daemon where
  it was. Where no OS credential store exists, nothing has been persisted and the
  user has signed in again. This is a client-side rule only — the daemon's own
  store is SQLite by ARCH-2 and is unaffected.

  The rule governs **state the client maintains about itself**, not configuration
  the **user** authors: a hand-editable preferences file (the TUI's
  `~/.config/openchime/config`, CLIENT.md §3) is the user's document, in the same
  sense as a shell rc file, and is deliberately machine-local. What the rule
  forbids is the client keeping its own cache, queue, or credential on disk.

---

## 10. Infrastructure and Resource Constraints

- **REQ-210.** The daemon has operated within a lean memory profile
  (~256MB) for a low-usage tenant and has scaled to a standard profile
  (~512MB) for a higher-concurrency tenant, without requiring an
  architecture change between the two profiles (ARCH-4).
- **REQ-211.** *(Partly built)* The daemon has supported at least a low-hundreds count of
  concurrent client connections per tenant within the lean memory profile
  (ARCH-30), sufficient for the 50-100 target customer scale referenced
  elsewhere in this project.

### 10.1 Disk-Space Management

*Design premise: a self-hosted box is assumed to be **unmanaged** — nobody is
watching a dashboard, and nobody will intervene before the disk fills. The
daemon has therefore had to keep itself alive on a finite disk without operator
action (ARCH-77). These requirements apply to the **local** blob backend; where
attachments live in external S3 (REQ-140) only database growth is local, and
only REQ-212/213/214 apply.*

- **REQ-212.** **Text messaging has survived attachment-storage exhaustion.**
  No amount of attachment data has been able to prevent the daemon from
  accepting, storing, delivering, or searching messages: the database's required
  headroom has been reserved and never consumed by attachment bytes. A box out
  of attachment space has degraded to "attachments unavailable," never to "chat
  down." This has been the highest-priority invariant of this subsection —
  every mechanism below exists to preserve it.
- **REQ-213.** The daemon has continuously reclaimed storage that nothing was
  promised to keep, without operator action and without user-visible loss:
  staged bytes from aborted or interrupted uploads, and blobs orphaned by
  message deletion (REQ-052) or by an upload that was never referenced by a
  message. This garbage collection has run before any destructive measure, so
  that reclaimable waste has always been freed ahead of data a user could still
  see.
- **REQ-214.** The daemon has monitored available disk space and surfaced it to
  owners and admins — current attachment usage, free space, and whether the
  daemon is currently under storage pressure — so that an operator who *is*
  paying attention has been able to act before automation did.
- **REQ-215.** Under sustained storage pressure, after REQ-213's reclamation was
  exhausted, the daemon has **automatically evicted attachment blobs, oldest
  first, by default and without operator action**, until free space returned
  above its recovery watermark. This is a deliberate trade of durability for
  unattended uptime, made because the target deployment is an unmanaged box
  where the alternative outcome is a full disk and a dead tenant (REQ-212).
  Constrained as follows, so the trade stays bounded:
  - **Messages have never been evicted.** Only attachment *bytes* are
    reclaimed; message history has remained complete and searchable, so
    REQ-053's "no retention cutoff" has continued to hold for messages.
  - **An evicted attachment has left a tombstone, not a hole.** Its metadata row
    — filename, size, uploader, timestamp — has been retained and marked
    unavailable, so a client has rendered "this attachment is no longer
    available" rather than failing an opaque download. The conversation has
    stayed intelligible.
  - **A grace period has protected recent uploads.** An attachment younger than
    a configured minimum age has never been evicted, so a file shared into a
    live conversation could not vanish mid-discussion.
  - **Eviction has been auditable** — each eviction recorded (REQ-251) and
    reported through REQ-214 — so an operator has been able to discover after
    the fact exactly what was reclaimed and why.
  - **An operator has been able to disable eviction**, accepting upload refusal
    (REQ-216) instead. The behavior is default-on, not mandatory.
- **REQ-216.** When free space has fallen below its floor and neither
  reclamation (REQ-213) nor eviction (REQ-215) could recover it — because
  eviction was disabled, or everything remaining was within the grace period —
  the daemon has **refused new uploads** with a distinct, user-facing error
  rather than failing partway through a transfer or writing into the database's
  reserve. Refusal has been the terminal state, never silent failure, and has
  never affected messaging (REQ-212).

- **REQ-217.** The daemon has expired attachments by **age**: a configuration
  value has set the maximum attachment age, and attachments older than it have
  been deleted — blob bytes reclaimed, metadata left as a tombstone (REQ-215) so
  the conversation stayed intelligible. This has been a standing policy applied
  continuously, distinct from the two other paths that remove attachment bytes:
  it is not triggered by storage pressure (REQ-215) and it is not the
  compliance-driven, opt-in retention policy that can also age out *messages*
  (REQ-250). A deployment that set no maximum age has kept attachments
  indefinitely, bounded only by REQ-215.
- **REQ-218.** The daemon has run a **periodic maintenance pass** — on its own
  schedule, not merely when other work happened to occur — that has performed
  the storage housekeeping of this subsection: reclaiming orphaned and aborted
  uploads (REQ-213), expiring attachments past the maximum age (REQ-217),
  measuring free space and publishing it (REQ-214), and, under pressure,
  evicting (REQ-215). Running on a timer rather than opportunistically has been
  the point: **an idle tenant is precisely the one whose disk fills without
  anything prompting a cleanup**, so maintenance driven only by incoming writes
  would stop exactly when it was most needed. The pass has been bounded in the
  work it does per run, so housekeeping has never blocked message delivery or
  monopolized the daemon, and its interval has been configurable so a test could
  compress it.
---

## 11. Rich Text and Message Composition

*Content-level messaging features layered on the core message model (Section 2).
None are yet backed by an architecture decision.*

- **REQ-220.** A message body has supported **inline rich-text formatting** —
  bold, italic, strikethrough, inline `code`, fenced code blocks, blockquotes,
  and ordered/unordered lists — authored in a markdown-like syntax and rendered
  by each client per its capabilities (a text frontend renders the same structure
  without proportional styling; the TUI shows code blocks and emphasis in-band).
  The stored body has remained plain UTF-8 (REQ-054) with formatting expressed
  in-band, so no schema change is required, and a client that does not render a
  construct has shown its literal source legibly.

  **Settled by ARCH-100:** a Slack-compatible subset for inline
  emphasis, extended with the list syntax Slack's markup lacks, parsed
  **client-side in `client/core/`** and never by the daemon, returning spans over
  the unchanged body. Full dialect, the escaping rules, and the places we
  deliberately diverge from Slack are in [MARKDOWN.md](./MARKDOWN.md).
- **REQ-221.** *(Partly built)* A message has been able to **@mention** a user, and the broadcast
  audiences `@here` / `@channel` / `@everyone`. A mention has been stored as a
  stable reference (user id) that survives display-name changes, has highlighted
  for the mentioned party, and has driven notification delivery under the
  recipient's per-channel level (REQ-130) — the "mentions" notification level
  *depends* on this feature. **Built (ARCH-89):** the body stays plain UTF-8 with
  the literal `@name`; migration 0021's `mentions` table carries the resolved id,
  kind and byte span; `shared/mention.c` is the one scanner both sides link, so
  highlight and notify cannot disagree. Known limitation: `@here` is treated as a
  broadcast for *push*, because presence is not visible to the push worker.
- **REQ-287.** *(Built in the Win32 client only)* Mentioning someone who is **not in the channel** has
  never been silent. The sender has been told — privately, in a notice only they see, since
  it concerns their action and not the conversation — that the mention reached
  nobody, and offered the remedy that fits the channel: **add them**, or **send
  them the message** so they have it without joining, or do nothing.

  **The problem it fixes is not the missing invite; it is the false
  confirmation.** Resolution requires channel membership (REQ-221/ARCH-89) but
  the *highlight* is syntactic — `shared/mention.c` marks any `@name`, and the
  client colours the span without asking whether it resolved. So a mention that
  notified nobody renders **identically** to one that worked: accent-coloured,
  semi-bold, apparently fine. The sender has every reason to believe the person
  was pinged and no way to discover otherwise. A feature that fails silently is
  worse than one that is absent, because the absent one does not lie.

  **Public and private are genuinely different and have been treated as such.**
  In a **public** channel the person can already read the message, so the only
  thing missing is the notification (REQ-288) and adding them is a convenience.
  In a **private** channel they cannot see the message at all until invited, and
  inviting them **discloses the channel's history** — the same disclosure
  REQ-036a records for making a private channel public. So the private case has
  named that consequence rather than offering a one-click reflex, and has stayed
  refusable: not every mention of a name is a decision to admit its owner.

  Where the sender cannot add anyone — a DM or group DM, an archived channel
  (REQ-035), or a channel whose membership they may not change — the notice has
  said so plainly instead of offering an action that would fail. The mentioned
  name has also been **rendered as ordinary text rather than as a live mention**
  once known to be unresolved, so the transcript stops asserting something untrue.
- **REQ-288.** A mention in a **public** channel has notified the mentioned
  person **even when they are not a member**, landing in their activity feed
  (REQ-139), because a public channel is one they can already read: the message
  is available to them, and withholding the notification only means they find out
  later or never. In a **private** channel it has NOT — they cannot open the
  message, and a notification pointing at something unreadable is worse than
  silence.

  This is Slack's split, verified in its documentation: a mention in a public
  channel "will receive a notification in their Activity feed", while in a private
  channel they "won't be notified and can't see your message until you invite
  them to join the channel". Ours currently does neither, because `store_mentions`
  joins `channel_members` for every channel kind — correct reasoning for private,
  applied too widely.

  **Built, and deliberately narrow.** Delivery is the **activity feed
  only**: push stays membership-gated, so this never rings a phone about a
  channel somebody never joined. The mention notifies **regardless of notification
  level**, because a mention is a direct address and a non-member has no
  per-channel preference to consult — routing it through their global default
  would apply a setting never made with this case in mind. It touched **three**
  gates, not the one first estimated: mention resolution, the activity-feed query
  (where it actually lands — without that the row is stored and nobody ever sees
  it), and push, which was left alone on purpose.
- **REQ-222.** *(Partly built)* A URL in a message has been **unfurled** into a preview
  (title, description, thumbnail) fetched from the linked page. The fetch has been
  performed **server-side by the daemon or an isolated helper** — never by pushing
  arbitrary client-side fetches — consistent with the island model. **Unfurling
  is simply on — no switch, no per-tenant disable.** The safety this feature
  needs lives in the fetch itself (the SSRF gate and its caps), not in an off
  button. **Built per ARCH-105:** an in-daemon fetch worker behind
  an SSRF gate, always on, the URL boundary rules shared (`shared/url.c`) so the
  daemon unfurls exactly what a client links, the `UNFURL` frame fanned on fetch
  completion and replayed on backfill/history, edits dropping stale previews.
  The app-core folds the frame into the message model, and the **Win32 client
  renders the card** — title and description under the message, the way Slack
  lays them. What keeps the marker partial: the **thumbnail** is deferred
  (title + description only), and the TUI does not render the card (the fixed
  frontend order; its gap is untracked by design).
- **REQ-223.** An unsent composer's contents (per channel/thread/DM) have been
  preserved as a **draft** across app restarts and, for a signed-in identity,
  synced across that user's devices, so a half-written message has not been lost.
  **Settled by ARCH-101:** server-stored, in its own `drafts` table
  keyed `(user_id, channel_id, thread_root)` with its own ops — deliberately not
  the `client_settings` bucket, which is partitioned per frontend and would leave
  a GUI draft invisible in the TUI. `thread_root` is in the key from the start
  (0 = the channel) so the thread half of this requirement costs a client change
  later rather than a migration.

  **A draft is user content, with the consequences that follow.** It is in scope
  for compliance capture (REQ-276). For DLP (REQ-277) the *timing* is what
  matters: that requirement redacts on write, and applying it literally to a
  draft would rewrite the author's own half-typed text underneath them on every
  save. **Decided: redaction happens once, at send.** DLP over a draft is
  **advisory — it may flag, never rewrite** — so the author's own unsent text is
  never mutated underneath them, and the single point at which content is
  redacted stays the moment it becomes a message.

  **The surface follows Slack's as closely as it can.** A conversation holding an
  unsent draft is marked in the sidebar, the draft is restored into the composer
  on returning to it, and it clears on send — the behaviour people already have
  the habit for. The hub Slack pairs this with is **REQ-228** — the Drafts,
  scheduled & sent pane, reached from the Home sidebar rather than the rail,
  which is not where Slack puts it.

  The sidebar marks a conversation that has a draft because a draft you
  cannot see you have is one you never return to. It is not counted as unread:
  it is your own text and must not make a channel look like it wants attention.
  A "Drafts" hub is out of scope here and pairs with scheduled send (REQ-224).
- **REQ-224.** A user has been able to **schedule a message** for future delivery
  to a channel or DM; the message has been held until its send time, then
  delivered through the ordinary path (REQ-090), and cancelable before it fired.
  **Settled by ARCH-102:** held in its own `scheduled_messages`
  table rather than in `messages` — it is not a message yet, and a "pending" flag
  would make every history, search, backfill and unread query responsible for
  excluding it. Fired by a daemon sweep on its own ~15 s timer (storage
  maintenance's five minutes is not a send time), delivered through the ordinary
  send path so mentions and notifications cannot diverge, with the **daemon
  minting the idempotency token at fire time** — the client's token belongs to
  the scheduling request. Editable and cancelable until it fires. A channel
  archived or an author removed in between marks it **failed with a reason the
  author sees**: a message promised and not sent is the one case where silence is
  indefensible.

  Surfaced in the **Scheduled** tab of the Drafts, scheduled & sent pane
  (REQ-228), which is where Slack puts it and where a user goes looking.
- **REQ-228.** A user has been able to reach one place holding **everything they
  have written but not said, and everything they have said** — drafts (REQ-223),
  scheduled messages (REQ-224) and a **sent list**: their own messages, newest
  first, grouped by day, across every conversation they can see, each row naming
  its destination and reading its first line. Slack's *Drafts & sent*, reached
  from the Home sidebar rather than from a top-level destination, carrying a
  count of the drafts waiting in it.

  **The sent half needs no new server work** and is recorded as a requirement so
  it stops being an unnumbered feature: REQ-080's search already accepts a
  filters-only query, scopes results to what the user may read, orders newest
  first and pages on a keyset cursor, so "everything I sent" is `from:me` through
  the query that exists.

- **REQ-229.** A user has been able to start a message **before choosing who it
  is for** — composing in a first-class pane, addressing it to any mix of
  channels and people, with the half-written result **preserved as an
  unaddressed draft** (REQ-223, ARCH-101) so closing the pane has not
  lost it. Sending has resolved the recipients into the right conversation:
  an existing channel, a direct message, or a group DM (REQ-056).

- **REQ-225.** *(Not built)* A user has been able to post a **poll** — a question with options
  other members vote on, results tallied and shown live — as a first-class message
  type rather than via an external app. **[needs ARCH decision — poll storage
  (message-linked), vote model (one-per-user, changeable), and result delivery.]**
- **REQ-226.** *(Not built)* A user has been able to share a **snippet** — a named, multi-line
  block of text or code with optional syntax highlighting — as a first-class
  object distinct from an inline fenced code block (REQ-220), so a long paste has
  not flooded the transcript. **[needs ARCH decision — snippet storage (a
  message-linked text blob vs. an attachment, ARCH-70) + rendering.]**

---

## 12. Message Organization and Retrieval

*Ways to mark, find, and revisit individual messages. None are yet backed by an
architecture decision.*

- **REQ-230.** Any channel member has been able to **pin** a message to its
  channel; pinned messages have been listed for the channel, visible to all
  members, and unpinnable by a member or an admin. **Built (ARCH-90):** migration
  0022's `pins` table is keyed on the message, so a pin is channel state rather
  than per-user; any member may pin or unpin (including someone else's pin), a
  channel holds at most 100, and the list streams each pinned message with its
  body. Pin state is replayed on backfill, so it survives a reconnect.
- **REQ-231.** A user has been able to **save (bookmark)** any message they can
  read into a private, personal list for later retrieval, visible only to them.
  **Built (ARCH-95, migration 0025):** `saved_items` keyed `(user_id, message_id)`
  — the deliberate mirror of a pin (ARCH-90), which is keyed on the message alone
  because it belongs to the channel. Two people may save the same message and
  neither sees the other's list; nothing is fanned out. Saving twice keeps the
  original time, and leaving a channel stops its messages appearing in the list.
- **REQ-232.** *(Partly built)* Every message has had a stable **permalink** — an addressable
  reference resolving to the message in its channel/thread — that a client could
  follow to **jump to that message** in context, loading surrounding history as
  needed. **Built (ARCH-96):** the link is `openchime://<host>/c/<channel>/m/<id>`
  — **ids, not names**, because a channel can be renamed (REQ-036) and a link
  built from a name would rot the moment it was. The half that matters is
  `HISTORY_AROUND`, a second mode on the history read that returns the messages
  surrounding an id: every surface that points at a message (pins, files,
  activity, saved items, search) otherwise dead-ends with "that message is
  older than the loaded history". *Not built:* registering the `openchime://`
  scheme with the OS — a machine-wide registry write is an install-time act, not
  something a chat client should do as a side effect.
- **REQ-233.** Folded into **REQ-283**, the single reminder requirement — one
  mechanism, one delivery path (the activity feed and the notify path, never a
  bot DM, per REQ-275).
- **REQ-234.** A user has been able to **star (favorite)** channels and DMs and
  organize their sidebar into **custom sections** — per-user view state that has
  synced across their devices without affecting other users. Built:
  both halves live in `oc_sidebar_opts` and persist through the daemon's
  `client_settings` bucket, so the client stores nothing locally (ARCH-88) and the
  state follows the account. A conversation appears exactly once — a section lifts it
  out of Channels/DMs and a star lifts it out of everything, with the star taking
  precedence. Caps refuse rather than evict: 32 stars, 8 sections, 32 conversations
  per section.
- **REQ-235.** A user has been able to **mark a message (or a whole conversation)
  as unread**, moving their read marker back so the conversation re-surfaces as
  unread for later attention, without altering anyone else's state. This has been
  a per-user adjustment of the read cursor the daemon already maintains
  (REQ-090/095, `CLIENT_ACK`). **[needs ARCH decision — mark-unread as a client-set
  cursor position vs. a distinct flag.]**
- **REQ-236.** A client has shown a **new-message divider** marking where unread
  messages begin in a conversation, and offered **jump-to-unread** (and moving
  between unread conversations by keyboard), so a returning user has found the
  first thing they had not seen without scrolling. This has been derived
  client-side from the per-user read cursor (REQ-090/235); no server change is
  required.
- **REQ-237.** A client has offered **aggregate unread views** — an "all unreads"
  surface showing unread messages across every conversation in one place, and an
  **unreads-only** sidebar mode hiding read conversations — so triaging a backlog
  has not meant opening each channel. **[needs ARCH decision — a client-side fold
  over per-channel unread state (REQ-014) vs. a server-side unread summary for
  efficiency at scale.]**
- **REQ-238.** *(Partly built)* A user has been able to **mark all as read** — clearing unread
  state across the workspace, or catching up channel-by-channel — advancing every
  read cursor to the latest message in one action (REQ-090/235). **[needs ARCH
  decision — a single bulk cursor-advance operation vs. per-channel
  `CLIENT_ACK`s.]**

---

## 13. User Profiles

*Per-user identity presentation beyond the display name already carried on
messages (author name, ARCH-74). Not yet backed by an architecture decision.*

- **REQ-240.** Each user has had a **profile** — full name, display name, avatar
  image, title/role text, pronouns, phone, and timezone — set by the user and
  shown wherever the user appears. Avatars have been stored as image assets in
  object storage (ARCH-17), not SQLite.

  **One screen owns it.** The fields are edited together on a single Edit
  profile card and committed by one button, rather than scattered across a
  dialog per field: two sheets both titled "Edit profile" is one more than a
  person can tell apart. Status is deliberately not on it — it is transient and
  has its own dialog (REQ-241).

  **Full name and display name are both carried, and answer different
  questions** — what somebody is called on paper, and what a transcript calls
  them. Neither substitutes for the other.

  **Timezone is chosen from a list, not typed.** A free-text box asks a person
  to recall an IANA zone from memory and validates nothing. The list leads with
  US and North American zones. What it records is *where somebody is*: quiet
  hours run on the offset the client refreshes from the OS on each connect
  (REQ-136, ARCH-103), not on this field.

  The avatar is an ordinary attachment id (`users.avatar_attachment_id`), so it
  lands in the existing blob store — object storage where one is configured —
  rather than inventing storage; it is readable workspace-wide, and only an
  attachment the setter uploaded can become one. Edit authority is the user's
  own: no admin-managed fields, which is the simpler half of the choice and the
  one a self-hosted deployment can live with.
- **REQ-289.** A user has been able to **browse the people in the workspace** — a
  directory listing everyone with their avatar, display name, title, custom status
  and presence, searchable by name or title, opening a person's profile. Without
  it the channels directory (REQ-038) has no counterpart for people, and the only
  ways to find somebody are the DM picker and the command palette, both of which
  answer "who do I already know the name of".

  Full name, pronouns, title, timezone and custom status ride `USER_LIST` as
  well as `PROFILE_INFO` for this directory's sake: `PROFILE_INFO` goes only to
  the person who edited the fields, so the roster is the one place every client
  can learn everyone else's (a repeated-list layout change, hence a
  protocol-version bump). A **phone number does not ride the roster** — it is
  contact detail rather than a name decoration, and reaches a client through
  `PROFILE_INFO` when a card is opened.
- **REQ-241.** A user has been able to set a transient **custom status** — a
  short text plus an emoji, with an optional expiry — shown alongside their name
  and presence (Section 4). **Built** (migration 0027): `status_emoji` /
  `status_text` / `status_expires_ms` on `users`, set with `SET_STATUS`, expiry
  **enforced by the daemon on read** (a client that is not running cannot clear
  its own state), carried on `PROFILE_INFO` and `USER_LIST`, and shown beside
  names in the member pane and profile (REQ-122/240).

---

## 14. Compliance and Data Governance

*Enterprise-tier data-lifecycle and governance surface. Some items may be
explicitly excluded for the self-hosted / small-team target; each is flagged.
None are yet backed by an architecture decision.*

- **REQ-250.** *(Not built)* A tenant has optionally configured a **message retention policy** —
  retain forever (the default, REQ-053) or delete messages/attachments older than
  a set age — applied uniformly and irreversibly once messages age out. This has
  been opt-in for organizations with data-minimization obligations and is **not** a
  paid-tier history *cap* (REQ-053 stands). **[needs ARCH decision — retention job
  + its interaction with backfill, search, and attachment cleanup.]**
- **REQ-251.** The daemon has recorded an **audit log** of administrative and
  security-relevant actions, **append-only, admin-readable, and kept apart from
  the message store** (ARCH-79). The catalog has covered four families:
  **administrative** (role change, user invite/remove, channel archive,
  retention or storage-policy change), **account lifecycle** (registration,
  password change, invite redeemed, owner bootstrap), **security** (session
  revocation, failed authentication, denied privileged action), and
  **moderation** (a moderator deleting another user's message, removing a member
  from a channel). Each entry has recorded when, who acted, what action, on what
  target, and the outcome — **never the secret involved**: that a password
  changed, never the password; that an invite was redeemed, never the token.
- **REQ-251a.** The audit log has been **bounded**, since an unbounded table on a
  finite disk is the failure mode REQ-212 exists to prevent: entries older than a
  configured maximum age have been dropped by the maintenance pass (REQ-218).
  "Append-only" has therefore described how entries are *written* — never updated,
  never selectively deleted — rather than promising infinite retention.
- **REQ-251b.** **The cap has been partitioned by family**, so that a
  high-volume family could not evict a low-volume one. Without this, an
  unauthenticated attacker able to generate failed logins at will could flood the
  log and age out the record of their own earlier successful actions — turning
  the audit trail into a means of erasing evidence. Each family has therefore
  aged out against its own budget, and administrative history has survived a
  flood of security noise.
- **REQ-252.** *(Not built)* A tenant subject to legal/compliance obligations has been able to
  place a **legal hold** on message history (including DMs, subject to
  authorization policy), suspending retention (REQ-250) for the held scope.
  The *export* and *DLP* halves are their own requirements — REQ-276 and
  REQ-277 — because they are different features with different risk: one reads,
  the other writes to other people's messages. Legal hold is this requirement.
  **[needs ARCH decision — hold model and its interaction with retention.]**

- **REQ-253.** *(Not built)* A tenant has been able to **provision and deprovision
  accounts via SCIM**, brokered as a **federated function** (ARCH-76): the
  project-operated service carries provisioning metadata only — which accounts to
  create, update or disable — never message content (REQ-041). A self-hosted
  stand-alone deployment, which federates nothing, manages accounts locally
  instead (REQ-024/033). **[needs ARCH decision — the SCIM surface's placement
  (control plane vs. daemon) and its mapping onto the local account model.]**

- **REQ-276.** *(Not built)* A tenant has been able to **capture its history for compliance** —
  every message, thread, file, channel and user, including **edits and deletions**
  where retention preserved them — so an eDiscovery or archiving system holds a
  faithful record without a human exporting files by hand.

  **Two delivery mechanisms, because the market has two and they are not
  interchangeable.**

  **(a) Vendor-format push (Global Relay EML).** The archiving vendors define the
  interchange, not the chat products: Mattermost's "Actiance XML" is Smarsh's
  ingest format and its "Global Relay EML" is Global Relay's. The reachable one for
  us is **RFC-5322 email**, because it turns up twice independently — Global Relay
  ingests it, and Microsoft **Purview**'s third-party connectors (the documented
  Slack path uses 17a-4's DataParser) convert messages *into* email and import them
  to mailboxes before litigation hold and retention apply. RFC 5322/2045 are public
  and any mail parser validates our output locally, which no gated vendor schema
  allows.

  **Transport is SMTP, and that is a real dependency.** Global Relay and Proofpoint
  are *journaling* destinations: the platform mails each record to a per-customer
  address using vendor-issued SMTP credentials (Mattermost's config is customer
  account type A9/A10/Custom plus SMTP user, password, address, and for Custom a
  server and port). Actiance XML and CSV are written to a **local directory** for a
  collector to take instead. So:

  | Mechanism | Delivery | New machinery |
  |---|---|---|
  | Global Relay EML / Proofpoint | SMTP submission, authenticated | MIME assembly **+ an SMTP client** (AUTH, TLS, queue, retry) |
  | File drop | Files in a spool directory | Encoder only |

  The daemon has no outbound mail today. The **file drop is therefore the first
  slice** — same extract, no network client, no credentials, nothing new to fail
  under someone's compliance obligation — and SMTP follows when a customer names
  Global Relay or Proofpoint, because only then are there credentials to test
  against.

  **(b) A first-class pull API, ours, documented and published.** A whole-instance
  read interface for a compliance consumer that wants to fetch rather than be sent
  to. Deliberately **our own**, not a Slack impersonation: Slack publishes no
  machine-readable Discovery spec (its OpenAPI repo carries only the Web and Events
  APIs; `api.slack.com/admins/discovery` is 403; there is no `docs.slack.dev` method
  page), so "wire compatible" would be guesswork about field names. We conform to
  what we can verify and document it properly instead.

  **Constraints that follow from decisions already made here:**

  - **The caller is not a person.** A compliance consumer authenticates with an
    owner-minted **scoped, revocable, audited** credential — never a user session,
    which would hand it every user-facing operation and would die when that owner
    signs out everywhere (REQ-182).
  - **Paging is a keyset cursor**, never an offset — the same reasoning as search
   : a message posted mid-export must not make a row repeat or vanish.
  - **DMs and private channels are the point and the danger.** Including them is
    what makes it a compliance tool, so it is a deliberate, separately audited act.
  - **The self-hosted operator already has the database**, so this earns its keep in
    the hosted model (ARCH-76) and wherever a connector is the requirement.

  For contrast, **Pumble** offers a workspace-owner ZIP of PUBLIC channels only — no
  DMs, no private channels, no API, deleted 10 days after download. Compliance
  capture is a differentiator in this segment, not table stakes.

  **[gaps, held open deliberately — (i) the extract's own schema, which both
  mechanisms encode from; (ii) the credential model, which nothing in the product
  has yet; (iii) whether the pull API is served on the daemon's existing admin/health
  listener or its own; (iv) user→email mapping, load-bearing for EML and for
  Actiance's `LoginName` alike; (v) no vendor ingest has been tested, so no vendor may
  be named as "supported" until one is.]**

- **REQ-277.** *(Not built)* A tenant has been able to apply **data-loss prevention to messages
  before they are stored** — content is offered to a configured **DLP webhook**, and
  what the webhook returns is what gets posted. A redaction therefore *replaces* the
  message on the way in; the original is never stored, never delivered, and never
  needs recalling.

  **Drafts (REQ-223) are advisory only.** They are stored user content and are
  captured (REQ-276), but DLP does not rewrite them: redaction happens once, at
  send. Rewriting a draft on save would mutate the author's own half-typed text
  underneath them on every keystroke-batch, which is a different and worse
  promise than redacting a message nobody has seen yet.

  **This is deliberately not Slack's model.** Slack's Discovery API redacts
  *afterwards* (`discovery.chats.tombstone`), which needs a restore window, a
  tombstone rendered faithfully in every surface — transcript, thread, search, pins,
  saved items, files, exports — and accepts that the original was already delivered
  to everyone. Redacting **at send time** removes all of that: there is no
  after-the-fact mutation of other people's messages, no second deletion semantics
  beside REQ-052's, and nothing to un-show.

  **The example we ship and test: US Social Security numbers.** A reference webhook
  that rewrites `123-45-6789` to a redaction marker, exercised in the test suite so
  the contract is executable rather than described — the daemon posts the redacted
  text, and the original never reaches the database.

  **What the design has to answer, and the answer IS the compliance posture:**

  - **The send path becomes blocking on an external call.** Timeout, retry and
    failure policy are the feature: **fail-open** (post the original when the webhook
    is unreachable — availability first, and a gap in enforcement) or **fail-closed**
    (refuse the send — enforcement first, and the product stops when the webhook
    does). It must be configurable and it must be logged either way, because "which
    one was in force" is the first question an auditor asks.
  - **Latency is now in the user's send.** A budget belongs in the config, and
    exceeding it is a failure that hits the policy above.
  - **The webhook sees everything before anyone else does**, including DMs. That is
    the most sensitive egress in the product and needs its own scope, its own audit
    family, and TLS with a pinned or operator-supplied trust anchor.
  - **Edits and attachments.** An edit is a new send and goes through the same path;
    file CONTENT scanning is a different problem (size, binary formats) and is
    **out of scope for the first pass** — recorded rather than silently omitted.

  **[gaps — the request/response contract itself (shape, versioning, how a webhook
  signals "block entirely" versus "post this instead"), the signing scheme that lets
  a webhook trust the daemon, and whether a redaction is visible to the SENDER as
  having happened.]**

---

## 15. Client Experience and UI Shell

*Cross-client UX-parity requirements a competitive analysis against Slack and
Pumble surfaced — capabilities every
graphical client is expected to have, largely independent of the daemon. "The
client" here means each native frontend (ARCH-74); the TUI was the reference for
most of these (CLIENT.md §3), though the Win32 GUI now leads on several.
**ARCH-92 settles how they are decided:** these are per-frontend renderings of
state the shared core already holds, so there is no cross-client architecture to
choose — which makes each one a per-frontend *obligation*, tracked per client on each requirement
rather than as a single tick. The lone exception is
REQ-269, whose accessibility half is a real open decision.*

- **REQ-260.** Every client has offered a **command palette / quick switcher** —
  a keyboard-driven surface (the TUI's Ctrl+K, ARCH-83) to jump to any channel/DM
  or run any action by fuzzy search — so navigation and actions have not required
  hunting through menus. (ARCH-92: per-frontend.)
- **REQ-261.** Every client has exposed an **in-app Settings/Preferences hub** —
  notifications, appearance, time format, and sidebar behavior editable in the app
  rather than only via a config file — writing through the client's existing
  settings mechanism (the daemon's per-`(user, client_type)` bucket plus the local
  config file, CLIENT.md §3). (ARCH-92: per-frontend.)
- **REQ-262.** Every client has offered **theme/appearance selection** — at least
  light, dark, and follow-system — applied in-app. The TUI ships a 256-color theme
  (ARCH-83); the GUIs honor the OS dark-mode signal (ARCH-80). **[needs ARCH
  decision — theme model + whether the preference is client-local or synced.]**
- **REQ-263.** Every client has presented a **transient error/toast and
  connection-status surface** — a visible, non-blocking channel for failures
  (failed send, rate-limit REQ-190, bad login, storage pressure REQ-214) and for
  connection state (reconnecting with a countdown, REQ-100) — so a failure or a
  dropped connection has never been silent. (ARCH-92: per-frontend.)
- **REQ-264.** Every client has provided a **keyboard-shortcut reference** — a
  discoverable list of its shortcuts (the TUI's `?` help overlay, ARCH-83). (ARCH-92: per-frontend.)
- **REQ-265.** A client's composer has offered **input aids** — inline
  autocomplete for `@user`, `#channel`, and `:emoji:` (REQ-221/072) and a full,
  searchable **emoji picker** (beyond a hardcoded set) — so mentioning, linking a
  channel, or reacting has not required typing exact names. (ARCH-92:
  per-frontend, over the shared `client/core/complete.c` catalogue.)
- **REQ-266.** A client has let a user **view another user's profile** — opening a
  profile pane from a name or avatar anywhere the user appears — showing the fields
  of REQ-240 (display name, avatar, title, timezone, status). **[needs ARCH
  decision — per-GUI profile-viewer surface over the roster/profile data.]**
- **REQ-267.** A client's **sidebar** has organized conversations to parity with
  the reference clients — a dedicated **direct-message section**, collapsible
  Public/Private/DM groups, per-user custom sections and starred/favorite
  conversations (whose storage is REQ-234), and a **sidebar search/filter** —
  rather than a flat, unsearchable list. (ARCH-92: per-frontend.)
- **REQ-268.** A client has provided **first-run onboarding** — a signup /
  first-owner setup UI that redeems the one-time owner setup token or an invite
  (REQ-024/026) into a working account — so bringing up a new tenant, or joining
  one, has not required a command line. (ARCH-92: per-frontend.)
- **REQ-269.** *(Partly built)* Every graphical client has been **operable without a mouse and
  legible to assistive technology**. Concretely: every action reachable by
  pointer has had a keyboard route; focus has been visible and has moved in a
  predictable order; and the client has exposed its structure — conversation
  list, transcript, message authorship and timestamps, composer, unread state —
  to the platform's accessibility layer, so a screen reader can convey a
  conversation rather than an unlabelled rectangle.

  *Status: built on Win32 (ARCH-99); other frontends pending, which is what
  keeps the marker partial.* The client is keyboard-operable in the ordinary
  paths (composer, completion, conversation movement, command palette, shortcut
  sheet) and answers `WM_GETOBJECT` with a **UI Automation provider over the
  self-drawn UI**, raising UIA events and carrying a real system caret. A
  self-drawn UI gets nothing for free here, which is exactly why this is a
  requirement and not an assumption: the cost grows with every pane added.

  **The approach (ARCH-99): accessibility is implemented *for* the custom
  controls, not by retreating from them** — the self-drawn surfaces are the
  product's rendering strategy (ARCH-82/98) and a deliberate choice.

- **REQ-290.** Every element a user can act on in a graphical client has carried a
  **stable, unique automation identifier** — Win32's UIA `AutomationId`, and the
  equivalent on each other platform — that does not change with layout, wording,
  window size, theme, language or the element's position in a list.

  **This is a testability requirement, not a second accessibility one.** REQ-269
  is about what assistive technology can *convey*; this is about what an automated
  test can *address*. The two share the UIA provider (ARCH-99) and are otherwise
  different obligations: a control can be perfectly legible to a screen reader and
  still be impossible to locate reliably from a test.

  **Why it is a requirement at all.** The supported way to drive a Windows
  application is UI Automation, and the discipline that makes it reliable is
  locating elements by id rather than by coordinates, visible text or tree
  position. Coordinates move whenever a pane is re-laid out; visible text moves
  when wording or locale changes; tree position moves when a list gains a row.
  Our own harness drives by coordinates today, and that is precisely why its
  failures have needed interpretation — a click that lands two pixels outside a
  chip is indistinguishable from a feature that does not work.

  **The identifier is part of the control, not of the test.** It is assigned where
  the control is defined, is reviewed like any other name, and does not change to
  suit a test that has broken — a renamed id is a breaking change to the automation
  surface in the same way a renamed wire field is to the protocol.

  **Dynamic elements get composed ids**: a per-conversation row is
  `conv.<channel_id>`, not `conv.3`, because the third row is a different
  conversation tomorrow. The id is derived from the thing's identity, never from
  where it happens to be drawn.

  *Status: built on Win32.* Every rail and shelf row,
  conversation, message, composer control, formatting button, filter chip, tab,
  pane row and modal button carries an id and — where it is a control rather than
  content — an `InvokePattern`, so a client can press it rather than click at a
  measured point. Verified from OUTSIDE the process: `scripts/uia_probe.ps1`
  asserts that no actionable element lacks an id, that no id is claimed twice and
  that each is invokable, and `scripts/uia_invoke.ps1` presses one by id in the
  suite.

  **Invokes are marshalled, not executed where they arrive.** UIA calls come in on
  an RPC thread; the app posts the element's token to its own window and runs it
  there, because every rect, menu and model pointer it touches belongs to the UI
  thread. Each token ends in the same call the mouse path makes — an automation
  route that reimplemented the action would be testing a second implementation.

---

## 16. Explicitly Out of Scope

*Features present in the reference products (Slack, Pumble) that OpenChime
deliberately does not implement, recorded so each omission is on the record rather
than merely absent — the same intent as REQ-160's camera-video exclusion. Each may
be revisited, but none is planned; several are reachable by a third party through
the app platform (REQ-172) or webhooks (REQ-170/173) without being a first-party
feature.*

- **REQ-270.** *(Excluded by decision)* **GIF/Giphy and sticker pickers** have not been a first-party
  feature. In the reference products these are app-provided; an integration
  (REQ-172) could add one. Out of scope for the core client.
- **REQ-271.** *(Excluded by decision)* **Canvas / collaborative documents** (in-workspace rich documents
  with embedded media and comments) have not been supported — a document-editing
  product adjacent to chat, out of scope for a messaging system.
- **REQ-272.** *(Excluded by decision)* **Lists / tables / project boards** (structured task/records
  surfaces) have not been supported — project-management territory (the space
  Pumble's sibling product Plaky occupies), out of scope.
- **REQ-273.** *(Excluded by decision)* **Clips / asynchronous voice and video messages** (recording a
  short audio or video clip posted into a conversation) have not been supported.
  Live audio is REQ-150–152 and screenshare REQ-161; *recorded* async media is a
  separate capability and is excluded, consistent with the camera-video exclusion
  (REQ-160).
- **REQ-274.** *(Excluded by decision)* **Slack-Connect-style cross-organization shared channels and
  external DMs** have not been supported. Cross-tenant messaging is precluded by
  the island model (ARCH-4/REQ-040); federation between tenants is a separate
  deployment concern (ARCH-76), not an in-product shared-channel feature.
- **REQ-275.** *(Excluded by decision)* A **first-party bot/assistant and an MCP (Model Context Protocol)
  server surface** have not been provided beyond the third-party app platform
  (REQ-172). OpenChime ships no built-in assistant and exposes no MCP server; such
  a thing could be built as an installed app, but is not a core feature.
