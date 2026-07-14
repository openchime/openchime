# OpenChime — Requirements

**Status:** target-state specification, partially implemented. Written in
descriptive, present-perfect tense ("the system has supported X") as a
target-state contract — the form the finished system is required to take. It is
**not** a record of what is built; for that, see
[STATUS.md](./STATUS.md), which reconciles each `REQ-NNN` below against the
current tree (the daemon's messaging/auth/roles/channels/reactions/threads/search
core is implemented; presence, notifications, attachments, audio, webhooks, and a
full client are not). This present-perfect style mirrors the reproduction-grade
style of OpenChime's sibling projects; here it functions as a forward
specification, with STATUS.md tracking progress against it.

This document is technical scope only. Business model, pricing, licensing,
and go-to-market decisions are out of scope; they are not tracked in any
document in this repository as of this writing. Technical design decisions —
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
- "Tenant" refers to one self-hosted or hosted-service organization instance
  (one daemon, one SQLite database, per ARCH-4). "Organization" and "tenant"
  are used interchangeably.
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

---

## 1. Tenancy, Identity, and Access

### 1.1 Tenant Discovery and Resolution

- **REQ-010.** The client has collected an **instance** (the tenant's
  address) and the user's **email** at sign-in, and has resolved the instance
  to a daemon address by plain DNS before opening a connection — with no
  hosted resolution service involved (ARCH-14). A self-hosted instance given
  as a full domain (e.g. `chat.acme.com`) has resolved via SRV records plus
  optional `.well-known` metadata; a hosted-tier instance given as a bare name
  (e.g. `acme`) has had the service's known DNS suffix appended client-side
  (`acme` → `acme.openchime.example`) and then resolved by ordinary DNS. The
  email has been used only to drive the OIDC login (REQ-020), not to derive
  the instance.
- **REQ-011.** Resolution failure (the instance name does not resolve in DNS,
  no SRV record, malformed `.well-known` metadata) has produced a distinct,
  user-facing error rather than being conflated with an authentication or
  network failure, since the user has needed to know which of "this org
  doesn't exist," "the org is unreachable," and "your login failed" applies.

### 1.2 Authentication

- **REQ-020.** The system has authenticated a user in one of two deployment-selected
  modes (ARCH-19, ARCH-55, [AUTH.md](./AUTH.md)): **local** (daemon-managed
  username+password) or **OIDC** (social login via the maintainer's central
  service). In OIDC mode the login has been a client-driven browser flow using
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
  provider app credentials and re-issued an instance-scoped identity token that
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

### 1.4 Multi-Tenant Data Isolation

- **REQ-040.** No query path in the daemon has been able to return data
  belonging to a different tenant, because each tenant has run as a
  separate daemon process against a separate SQLite database file with no
  shared connection or shared query surface (ARCH-4, ARCH-7). Isolation has
  therefore been a property of the deployment topology, not of a
  tenant-ID filter inside shared queries.
- **REQ-041.** No shared, always-on runtime component has existed in the
  message/data path across tenants. Instance resolution has been plain DNS
  (REQ-010, ARCH-14), so the name-to-daemon-address mapping has lived in DNS
  records — provisioned at tenant creation — rather than in a bespoke hosted
  resolution service. The cross-tenant surfaces touching data have therefore been
  static (DNS, and for the hosted tier the tenant-provisioning step that writes
  those records), holding no message, channel, or user content and running no
  request-serving process in the message path — which strengthens REQ-040's
  isolation guarantee. **The one shared runtime component has been the central
  OIDC service (ARCH-56), and only in OIDC mode:** it is contacted at *login
  time* only (never per-message), brokers *identity* only (it never sees message
  or channel content), and is absent entirely in local mode. So multi-tenant
  *data* isolation has been unconditional; the only shared dependency has been a
  login-time identity broker, present only where a deployment opts into OIDC.

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
  (ARCH-30). Attachments (REQ-140) have not been subject to this cap, since
  they are never sent as protocol frames.

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

### 2.4 Search

- **REQ-080.** Full-text search has covered the complete message history
  visible to the searching user (i.e., channels they are a member of, per
  REQ-031), with no retention cutoff, implemented via SQLite FTS5 (ARCH-15).

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
- **REQ-094.** The system's recovery point objective (RPO) on total loss of
  a tenant's box has been 15 seconds — the interval at which committed
  writes are shipped to remote object storage (ARCH-23). Messages
  acknowledged to a client less than 15 seconds before a total box loss have
  been at risk of not being recoverable.

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
  state in near-real-time, propagated via the `presence-update` message type
  (ARCH-8).
- **REQ-121.** Clients have shown a transient "user is typing" indicator
  scoped to the active channel or thread, which has expired automatically
  client-side if no further typing signal arrives within a short window
  (avoiding a stuck indicator on an ungraceful disconnect). **[needs ARCH
  decision — expiry window]**

---

## 5. Notifications

- **REQ-130.** Each user has been able to independently set a notification
  level — all messages, mentions only, or none — per channel they belong to.
- **REQ-131.** Each user has been able to configure a do-not-disturb
  schedule that has suppressed push notification delivery without altering
  in-app unread state (badges/counts have still updated).
- **REQ-132.** Push notifications have been delivered via APNs on iOS/macOS
  and FCM on Android (ARCH-16), at no per-notification cost, per the
  providers' free tiers as of this writing.
- **REQ-133.** A self-hosted deployment has routed push delivery through an
  operator-run push gateway rather than registering its own APNs/FCM
  application identity, because the published mobile clients are signed
  under the maintaining project's developer accounts, not the self-hoster's
  (ARCH-16).

---

## 6. Attachments and Media

### 6.1 File Attachments

- **REQ-140.** A user has been able to upload and share a file attachment in
  a channel, thread, or direct message, persisted in object storage rather
  than in SQLite (ARCH-17).
- **REQ-141.** An attachment has remained retrievable by any user authorized
  to read the message it is attached to (REQ-031), and by no one else, for
  as long as the message itself exists. **[needs ARCH decision — signed URL
  scheme / access control on the object storage layer]**

### 6.2 Audio Conferencing

- **REQ-150.** A user has been able to start or join a server-relayed audio
  call scoped to a channel or a direct message. No peer-to-peer or ICE
  negotiation path has existed (ARCH-18).
- **REQ-151.** Audio has been encoded with Opus and relayed over an isolated
  UDP-based sidecar process, kept out of the daemon's TCP event loop so a
  call cannot starve message delivery on the same tenant (ARCH-18).
- **REQ-152.** A participant's connection loss during a call has not
  terminated the call for other participants; the daemon has continued
  relaying for remaining participants and has allowed the disconnected
  participant to rejoin. **[needs ARCH decision]**

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
  (ARCH-32). **[needs ARCH decision — webhook token format, payload schema,
  and rate limit]**
- **REQ-171.** A tenant that has enabled webhooks has had a CA-signed TLS
  certificate obtained on-demand for that endpoint, since third-party
  webhook senders validate against a standard CA trust store and cannot pin
  a custom certificate (ARCH-34). A tenant with webhooks disabled has had no
  such certificate and no such requirement.

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
  validation, uniformly across hosted and self-hosted tenants (ARCH-10).
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
