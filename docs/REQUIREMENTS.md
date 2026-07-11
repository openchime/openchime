# OpenChime — Requirements

**Status:** pre-implementation specification. Written in descriptive,
present-perfect tense ("the system has supported X") as a target-state
contract — the form the finished system is required to take — even though no
code has yet been written against it. This mirrors the reproduction-grade
style of OpenChime's sibling projects; here it functions as a forward
specification rather than a record of an existing build.

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

- **REQ-010.** The client has resolved a tenant from a user-supplied name
  before opening a connection. A bare name (e.g. `acme`) has resolved via the
  hosted service's DNS suffix; a name containing a domain (e.g. `acme.com`)
  has resolved via SRV records, with optional `.well-known` metadata used for
  self-hosted deployments (ARCH-14).
- **REQ-011.** Resolution failure (no matching DNS suffix entry, no SRV
  record, malformed `.well-known` metadata) has produced a distinct,
  user-facing error rather than being conflated with an authentication or
  network failure, since the user has needed to know which of "this org
  doesn't exist," "the org is unreachable," and "your login failed" applies.

### 1.2 Authentication

- **REQ-020.** The system has authenticated a user via a client-driven
  browser flow using platform-native auth session APIs — `ASWebAuthenticationSession`
  on iOS/macOS, a loopback redirect on desktop — with PKCE (ARCH-19).
- **REQ-021.** The system has supported OIDC login against Microsoft Entra
  ID and Google Workspace as identity providers.
- **REQ-022.** The system has supported Apple Sign-In, required for iOS App
  Store compliance for any app offering third-party login. The system has
  not supported Facebook login.
- **REQ-023.** The daemon has validated a returned JWT against the issuing
  provider's published JWKS on every session establishment, and has rejected
  a connection outright on signature, audience, or expiry mismatch, without
  attempting partial trust of an unverified token (ARCH-19).

### 1.3 Authorization and Roles

- **REQ-030.** Every user in a tenant has held exactly one tenant-level role
  at a time: **owner**, **admin**, or **member**. A tenant has had at least
  one owner at all times; the system has refused an action that would remove
  the last owner. **[needs ARCH decision — role storage/enforcement point]**
- **REQ-031.** Channel membership has been independent of tenant role: a
  member has belonged to zero or more channels, and only channel members
  have been able to read or post in a channel that is not public.
  **[needs ARCH decision]**
- **REQ-032.** A user has been able to edit or delete only their own
  messages, with one exception: a tenant admin or owner has been able to
  delete (not edit) any message in a channel they belong to, for moderation
  purposes. Deletion by a non-author has been distinguishable in the message
  record from self-deletion (REQ-052). **[needs ARCH decision]**
- **REQ-033.** Only an owner or admin has been able to invite or remove a
  member from the tenant. Channel-level invite/remove for private channels
  has been available to any existing member of that channel, not gated to
  admins. **[needs ARCH decision]**

### 1.4 Multi-Tenant Data Isolation

- **REQ-040.** No query path in the daemon has been able to return data
  belonging to a different tenant, because each tenant has run as a
  separate daemon process against a separate SQLite database file with no
  shared connection or shared query surface (ARCH-4, ARCH-7). Isolation has
  therefore been a property of the deployment topology, not of a
  tenant-ID filter inside shared queries.
- **REQ-041.** The hosted service's tenant-resolution layer (REQ-010) has
  been the only shared component across tenants, and it has held no message,
  channel, or user content — only the name-to-daemon-address mapping.
  **[needs ARCH decision — hosted resolution service design]**

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
  keyed on the server-assigned message id (REQ-050). **[needs ARCH decision
  — dedup key and window]**
- **REQ-092.** Within a single channel, the daemon has delivered messages to
  a given connected client in the order the daemon accepted them. Ordering
  across different channels has carried no guarantee relative to each other.
- **REQ-093.** A send the daemon has not acknowledged (connection drop before
  ack) has been safely retryable by the client without risk of the message
  being accepted twice, via a client-generated idempotency token distinct
  from the server-assigned message id. **[needs ARCH decision — idempotency
  token placement in the frame format]**

### 3.2 Reconnect and Offline Behavior

- **REQ-100.** A client that has lost its connection has reconnected
  automatically and has resumed the session without requiring the user to
  re-authenticate, so long as the JWT used to establish the prior session
  has not expired (REQ-023).
- **REQ-101.** On reconnect, the client has requested and received a replay
  of messages accepted by the daemon after the last message the client
  acked, across all channels the client is a member of, so that no message
  sent during the disconnection window has been silently missed.
  **[needs ARCH decision — backfill request shape]**
- **REQ-102.** A message composed while the client is disconnected has been
  queued locally and sent automatically on reconnect, in the order composed,
  without requiring the user to resend it manually. **[needs ARCH decision]**

### 3.3 Protocol Versioning and Compatibility

- **REQ-110.** The daemon has rejected a connection whose declared protocol
  `version` it does not support, cleanly and before parsing any subsequent
  frame, rather than attempting to misparse a frame in an unrecognized
  version's layout (ARCH-8, ARCH-10).
- **REQ-111.** A protocol version rejection has carried a machine-readable
  reason code distinguishing "client too old" from "client too new," so a
  client has been able to present "please update the app" only when that is
  actually the correct remedy. **[needs ARCH decision — version negotiation
  and error code table]**

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
  service holding a user session or JWT. **[needs ARCH decision — webhook
  token format, payload schema, and rate limit]**

---

## 8. Security Posture

### 8.1 Transport and Session Security

- **REQ-180.** Every client-daemon connection has been encrypted in transit;
  the system has offered no unencrypted TCP fallback (ARCH-6, ARCH-10).
- **REQ-181.** A session established from a validated JWT (REQ-023) has
  remained valid only until that JWT's expiry; the daemon has not
  independently extended a session past the token's stated expiry.
- **REQ-182.** A user has been able to revoke an active session (e.g. "log
  out other devices") from any authenticated client, and the daemon has
  terminated the targeted connection on the next protocol interaction after
  revocation. **[needs ARCH decision — revocation propagation mechanism,
  since sessions are validated against provider JWKS rather than a local
  session table]**

### 8.2 Abuse Prevention and Rate Limiting

- **REQ-190.** The daemon has rate-limited message-send frequency
  per-connection, rejecting excess sends with a distinct error rather than
  silently dropping them. **[needs ARCH decision — limit values and
  algorithm]**
- **REQ-191.** The daemon has rate-limited authentication attempts per
  tenant to blunt credential-stuffing and brute-force attempts against the
  JWT validation path (REQ-023). **[needs ARCH decision]**

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
