# OpenChime — Requirements

**Status:** target-state specification, partially implemented. Written in
descriptive, present-perfect tense ("the system has supported X") as a
target-state contract — the form the finished system is required to take. It is
**not** a record of what is built; for that, see
[STATUS.md](./STATUS.md), which reconciles each `REQ-NNN` below against the
current tree. As of this writing the daemon's messaging/auth/roles/channels/
reactions/threads/search/presence/typing/notification-settings/attachments/
webhooks core is implemented, along with a shared C client app-core and a
termbox2 TUI (ARCH-74/75); mobile push (REQ-132/133), the client-side audio codec
(REQ-150–152), the full multi-platform client, and all of Sections 11–14 are
forward scope. This present-perfect style mirrors the reproduction-grade style of
OpenChime's sibling projects; here it functions as a forward specification, with
STATUS.md tracking progress against it.

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

---

## 1. Tenancy, Identity, and Access

### 1.1 Tenant Discovery and Resolution

- **REQ-010.** The client has collected a **workspace** (the tenant's
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
- **REQ-015.** Per-workspace view state — the focused channel, scroll position,
  and a partly-typed message — has survived switching away and back, so that a
  switch has not discarded work in progress.

### 1.2 Authentication

- **REQ-020.** The system has authenticated a user in one of two deployment-selected
  modes (ARCH-19, ARCH-55, [AUTH.md](./AUTH.md)): **local** (daemon-managed
  username+password) or **OIDC** (social login via the project's central
  service). Because OIDC is a federated function (ARCH-76), self-hosted
  stand-alone deployments have used **local** mode; self-hosted federated
  deployments have used either, choosing OIDC precisely by opting in; hosted
  deployments have used OIDC. In OIDC mode the login has been a client-driven browser flow using
  platform-native auth session APIs — `ASWebAuthenticationSession` on iOS/macOS,
  a loopback redirect on desktop — with PKCE. The daemon has advertised its mode
  to the client before authentication.
- **REQ-021.** The system has supported OIDC login against Microsoft Entra
  ID and Google Workspace as identity providers. Provider integration has lived
  in the central service (ARCH-56), not the daemon.
- **REQ-022.** The system has supported Apple Sign-In, required for iOS App
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
- **REQ-025.** In OIDC mode the maintainer's central service has held the
  provider app credentials and re-issued a workspace-scoped identity token that
  the daemon trusts; self-hosted deployments have reached it through a relay so
  their users get social login without registering provider apps, and the client
  (not the central service) has carried the token to the daemon (ARCH-56).

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
  member (or restricted to admins per a deployment setting). It is metadata on
  the channel, distinct from the channel name. **[needs ARCH decision — who may
  set a topic + storage column.]**
- **REQ-035.** A channel has been **archivable** by an owner or admin: an
  archived channel has become read-only and hidden from the default channel list
  while its history remained searchable and retrievable (REQ-031/080), and it has
  been restorable. Archiving (reversible) is distinct from deletion, which is not
  offered for channels holding history. **[needs ARCH decision — archive flag +
  read-only enforcement.]**

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
  (REQ-050/055), extended to N participants. **[needs ARCH decision — group-DM
  identity/membership model vs. reusing a private channel.]**

### 2.2 Threads

- **REQ-060.** Every message has been eligible to be replied to as a
  thread. A thread reply has not appeared inline in the parent channel's
  main scroll; the parent message has displayed a reply count and the most
  recent repliers.
- **REQ-061.** Replying in a thread has notified the thread's participants
  (message author plus prior repliers) according to their per-channel
  notification setting (REQ-130), independent of whether they were
  `@mentioned`.

### 2.3 Reactions

- **REQ-070.** A user has been able to attach one or more emoji reactions to
  any message they can read, including their own. A given user has been
  limited to one reaction of the same emoji per message (toggled, not
  stacked).
- **REQ-071.** A message has displayed an aggregate count per distinct
  emoji reacted with, and the identities of the reacting users have been
  available on inspection (hover/tap).
- **REQ-072.** A tenant has been able to register **custom emoji** (a named
  image usable in reactions and message text) beyond the built-in Unicode set,
  under admin control of who may add them. A reaction (REQ-070) references an
  emoji by a stable shortcode that resolves to either a Unicode sequence or a
  tenant custom-emoji asset; a text-only client (the TUI) shows the shortcode
  rather than the image. **[needs ARCH decision — custom-emoji asset storage
  (object store, ARCH-17) + shortcode namespace.]**

### 2.4 Search

- **REQ-080.** Full-text search has covered the complete message history
  visible to the searching user (i.e., channels they are a member of, per
  REQ-031), with no retention cutoff, implemented via SQLite FTS5 (ARCH-15).
- **REQ-081.** Search has accepted **structured operators** that scope a query —
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
- **REQ-095.** The system has optionally surfaced **read state** — for a direct
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
- **REQ-102.** A message composed while the client is disconnected has been
  queued locally in the client's **offline outbox** (a table in its SQLite
  store, ARCH-64) and sent automatically on reconnect, in the order composed,
  each with its original idempotency token (REQ-093), without requiring the user
  to resend it manually.

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
- **REQ-121.** Clients have shown a transient "user is typing" indicator
  scoped to the active channel or thread, which has expired automatically
  client-side if no further typing signal arrives within a short window
  (avoiding a stuck indicator on an ungraceful disconnect). A `TYPING` signal is
  relayed as a member-scoped `TYPING_UPDATE` (ARCH-68), so private-channel and DM
  typing never leaks to non-members; the server keeps no expiry timer. Resolved:
  the indicator expires **client-side after ~6 seconds**, refreshed by each new
  `TYPING` signal — no "stopped typing" frame is sent.

---

## 5. Notifications

- **REQ-130.** Each user has been able to independently set a notification
  level — all messages, mentions only, or none — per channel they belong to.
  Built (ARCH-72): `SET_NOTIFY_PREF` stores the level in `notification_prefs`
  (server-authoritative), and a `NOTIFY_PREFS` snapshot syncs it to all the
  user's devices.
- **REQ-131.** Each user has been able to configure a do-not-disturb
  schedule that has suppressed push notification delivery without altering
  in-app unread state (badges/counts have still updated). Built (ARCH-72):
  `SET_DND` stores a daily UTC minutes-of-day window on `users`; it governs push
  only. The push *delivery* it gates (REQ-132/133) is the deferred mobile-push
  milestone.
- **REQ-132.** Push notifications have been delivered via APNs on iOS/macOS
  and FCM on Android (ARCH-16), at no per-notification cost, per the
  providers' free tiers as of this writing.
- **REQ-133.** Push delivery has been a **federated function** (ARCH-76): the
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

### 6.2 Audio Conferencing

- **REQ-150.** A user has been able to start or join a server-relayed audio
  call scoped to a channel or a direct message. No peer-to-peer or ICE
  negotiation path has existed (ARCH-18). Built (server side): `CALL_JOIN`
  forms/joins the channel's call and returns a UDP media endpoint + token.
- **REQ-151.** Audio has been encoded with Opus and relayed over an isolated
  UDP-based sidecar process, kept out of the daemon's TCP event loop so a
  call cannot starve message delivery on the same tenant (ARCH-18). Built: the
  forked `audio_sidecar` relays opaque Opus payloads over UDP (ARCH-28/31/73);
  the daemon never touches the codec. The client-side Opus encode/decode is
  Phase-2 client work.
- **REQ-152.** A participant's connection loss during a call has not
  terminated the call for other participants; the daemon has continued
  relaying for remaining participants and has allowed the disconnected
  participant to rejoin. Resolved (ARCH-73): the call roster is ephemeral
  net-thread state; a `CALL_LEAVE` or TCP disconnect drops the participant and
  re-rosters the rest, but the call persists while ≥1 participant remains, and
  rejoin is a fresh `CALL_JOIN` (new token). The media-side mirror is a UDP
  silence timeout in the sidecar. **Signaling built (PROTOCOL.md §5.17); the UDP
  relay + timeout land in the sidecar milestone.**

### 6.3 Video (Out of Scope)

- **REQ-160.** Video calling and video streaming or playback beyond generic
  file-attachment handling (REQ-140) have not been supported. This is a
  deliberate scope exclusion, not a deferred feature pending an architecture
  decision.

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
  the proto port (ARCH-54): a connection that doesn't negotiate `oc/1` is routed
  to the HTTP handler.
- **REQ-171.** A tenant that has enabled webhooks has had a CA-signed TLS
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
- **REQ-172.** A tenant has been able to install **app integrations** that post
  and respond in channels under a **bot identity** distinct from human users,
  including **slash-command apps** invoked as `/command` from the composer and
  dispatched to a registered integration endpoint. Install has been role-gated to
  owner/admin. **[needs ARCH decision — bot identity model, slash-command
  registration + dispatch, install authorization.]**
- **REQ-173.** The system has supported **outgoing webhooks / event
  subscriptions**: a tenant-registered endpoint has received notifications of
  selected events (new message in a channel, mention, membership change) so
  external systems could react to activity — the outbound complement to the
  incoming webhooks of REQ-170. Delivery has been at-least-once with retry and a
  signed payload so the receiver could verify origin. **[needs ARCH decision —
  event catalog, delivery/retry, payload signing.]**
- **REQ-174.** The system has offered **workflow automation** — declarative
  triggers (a message match, a schedule, a form submission) driving actions (post
  a message, call a webhook) — so common routines could run without an external
  app. This may reduce to a first-party consumer of the app platform (REQ-172).
  **[needs ARCH decision — workflow model + execution surface.]**
- **REQ-175.** Installable integrations have been discoverable through an **app
  directory** curated by the maintaining project, from which an owner/admin could
  install an app into their tenant. Directory hosting has been a **federated
  function** (ARCH-76, cf. ARCH-56), not the daemon's — so the directory has been
  available to self-hosted federated and hosted deployments, while a self-hosted
  stand-alone deployment has installed integrations by direct configuration
  instead of browsing a curated catalog. **[needs ARCH decision — directory
  hosting + per-tenant install/permission model.]**

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

### 8.2 Abuse Prevention and Rate Limiting

- **REQ-190.** The daemon has rate-limited message-send frequency
  per-connection, rejecting excess sends with a distinct error rather than
  silently dropping them. **[needs ARCH decision — limit values and
  algorithm]**
- **REQ-191.** The daemon has rate-limited failed authentication attempts,
  per account and per source, to blunt credential-stuffing and brute-force
  against local-mode passwords, answering excess attempts with
  `AUTH_RATE_LIMITED` (ARCH-59). (OIDC-mode credential checking is a signature
  verification against a pinned key, not a guessable secret, so the local-auth
  path is the meaningful target.)

---

## 9. Client and Platform Support

- **REQ-200.** Native clients have run on Linux, Windows, macOS, iOS, and
  Android. This platform list is inferred from the packaging decisions
  (ARCH-20, ARCH-21), the platform-native auth session APIs required by
  REQ-020 (iOS/macOS), and push delivery via both APNs and FCM (REQ-132);
  it has not been separately confirmed as a standalone decision and should
  be. **[needs ARCH decision — explicit platform commitment, including
  whether Android/macOS packaging follows the Linux/Windows model in
  ARCH-20/ARCH-21]**

---

## 10. Infrastructure and Resource Constraints

- **REQ-210.** The daemon has operated within a lean memory profile
  (~256MB) for a low-usage tenant and has scaled to a standard profile
  (~512MB) for a higher-concurrency tenant, without requiring an
  architecture change between the two profiles (ARCH-4).
- **REQ-211.** The daemon has supported at least a low-hundreds count of
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
  construct has shown its literal source legibly. **[needs ARCH decision — the
  markup dialect + whether it is parsed server-side or purely client-side.]**
- **REQ-221.** A message has been able to **@mention** a user or channel, and the
  broadcast audiences `@here` / `@channel` / `@everyone`. A mention has been
  stored as a stable reference (user/channel id) that survives display-name
  changes, has highlighted for the mentioned party, and has driven notification
  delivery under the recipient's per-channel level (REQ-130) — the "mentions"
  notification level *depends* on this feature. **[needs ARCH decision — mention
  encoding in the body + resolution, and the mention→notify decision, which is
  the deferred half of ARCH-72.]**
- **REQ-222.** A URL in a message has optionally been **unfurled** into a preview
  (title, description, thumbnail) fetched from the linked page. The fetch has been
  performed **server-side by the daemon or an isolated helper** — never by pushing
  arbitrary client-side fetches — consistent with the island model, and
  disable-able per tenant for privacy/egress reasons. **[needs ARCH decision —
  unfurl-fetcher placement + egress/SSRF controls; likely out-of-daemon.]**
- **REQ-223.** An unsent composer's contents (per channel/thread/DM) have been
  preserved as a **draft** across app restarts and, for a signed-in identity,
  synced across that user's devices, so a half-written message has not been lost.
  **[needs ARCH decision — draft storage: client-local (ARCH-64) vs.
  server-synced.]**
- **REQ-224.** A user has been able to **schedule a message** for future delivery
  to a channel or DM; the message has been held until its send time, then
  delivered through the ordinary path (REQ-090), and cancelable before it fired.
  **[needs ARCH decision — where a scheduled message is held (a server-side queue)
  and its interaction with idempotency (REQ-093).]**

---

## 12. Message Organization and Retrieval

*Ways to mark, find, and revisit individual messages. None are yet backed by an
architecture decision.*

- **REQ-230.** Any channel member has been able to **pin** a message to its
  channel; pinned messages have been listed for the channel, visible to all
  members, and unpinnable by a member or an admin. **[needs ARCH decision — pin
  storage + who may pin/unpin.]**
- **REQ-231.** A user has been able to **save (bookmark)** any message they can
  read into a private, personal list for later retrieval, visible only to them.
  **[needs ARCH decision — per-user saved-item storage.]**
- **REQ-232.** Every message has had a stable **permalink** — an addressable
  reference resolving to the message in its channel/thread — that a client could
  follow to **jump to that message** in context, loading surrounding history as
  needed. **[needs ARCH decision — permalink form + a fetch-around-an-id backfill
  mode, cf. ARCH-46's cursor replay.]**
- **REQ-233.** A user has been able to set a **reminder** on a message or a
  free-text note for a chosen time, delivered to them (typically as a bot DM) when
  due. **[needs ARCH decision — reminder storage + delivery, cf. the scheduled-
  delivery mechanism of REQ-224.]**
- **REQ-234.** A user has been able to **star (favorite)** channels and DMs and
  organize their sidebar into **custom sections** — per-user view state that has
  synced across their devices without affecting other users. **[needs ARCH
  decision — per-user sidebar state storage: client-local vs. synced.]**

---

## 13. User Profiles

*Per-user identity presentation beyond the display name already carried on
messages (author name, ARCH-74). Not yet backed by an architecture decision.*

- **REQ-240.** Each user has had a **profile** — display name, avatar image,
  title/role text, timezone, and pronouns — set by the user (some fields possibly
  admin-managed in a corporate deployment) and shown wherever the user appears.
  Avatars have been stored as image assets in object storage (ARCH-17), not
  SQLite. **[needs ARCH decision — profile field set, edit authority, avatar asset
  storage.]**
- **REQ-241.** A user has been able to set a transient **custom status** — a
  short text plus an emoji, with an optional expiry — shown alongside their name
  and presence (Section 4). **[needs ARCH decision — status storage + expiry and
  its relation to presence (ARCH-67).]**

---

## 14. Compliance and Data Governance

*Enterprise-tier data-lifecycle and governance surface. Some items may be
explicitly excluded for the self-hosted / small-team target; each is flagged.
None are yet backed by an architecture decision.*

- **REQ-250.** A tenant has optionally configured a **message retention policy** —
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
- **REQ-252.** A tenant subject to legal/compliance obligations has been able to
  place a **legal hold** and **export** message history (including DMs, subject to
  authorization policy) for eDiscovery, and to apply data-loss-prevention scanning
  to content. This is an enterprise concern and **may be explicitly out of scope**
  for the self-hosted/small-team target. **[needs ARCH decision — export/hold
  model, or a decision to exclude it.]**
- **REQ-253.** In OIDC/enterprise deployments, user provisioning and
  deprovisioning have been automatable via **SCIM** from the organization's
  identity provider, so account lifecycle matched the directory. Like OIDC this
  has been a **federated function** brokered through the central service (ARCH-76,
  ARCH-56), not the daemon — available to self-hosted federated and hosted
  deployments, with no local-mode or stand-alone applicability. **[needs ARCH decision — SCIM endpoint placement
  (central service).]**
