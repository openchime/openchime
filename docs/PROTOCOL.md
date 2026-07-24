# OpenChime Wire Protocol — v1 (core messaging path)

This document specifies the OpenChime binary wire protocol at the byte level.
It is the detailed realization of the frame decisions in
[ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-6 through ARCH-9, ARCH-30) and
resolves the protocol-shaped `[needs ARCH decision]` items in
[REQUIREMENTS.md](./REQUIREMENTS.md).

**Scope of this revision.** This covers connection handshake and version
negotiation, authentication, the message send/broadcast/ack cycle, message
edit/delete (§5.5/5.6), reactions (§5.9), threads (§5.10), search (§5.11),
direct messages (§5.12), channel management (§5.7), tenant administration (§5.8),
and reconnect backfill, plus the error frame. Presence (REQ-120/121), typing indicators, thread notifications
(REQ-061/130), and audio-call signaling (REQ-150–152) are deliberately out of
scope here and will be added in later revisions of this document, reusing the
framing and encoding rules defined below. New message types are additive; the
header format and the frozen handshake frames (§3) do not change.

**Status.** Implemented. The frames in this document are realized in
`shared/protocol.c` (codec), `daemon/dbwriter.c` (handlers), and
`daemon/netloop.c` (dispatch), exercised end-to-end by the test suite.

---

## 1. Transport

- The protocol runs over a single TLS/TCP connection per client (ARCH-6,
  ARCH-10, REQ-180). There is no unencrypted fallback.
- TLS trust is TOFU pinning against the daemon's self-signed certificate
  (ARCH-10, REQ-183). Everything below §1 describes the plaintext *inside* the
  TLS session.
- The connection is bidirectional and full-duplex. After the handshake and
  auth complete, either side may send an applicable frame at any time (e.g.
  the server may push a `BROADCAST` while the client is composing a `SEND`).
- One TCP connection carries exactly one authenticated session. There is no
  multiplexing of multiple users over one connection.

### 1.1 Port and ALPN multiplexing (ARCH-54)

This is a client-facing contract: it is fixed now and clients must conform,
because changing it later means re-releasing every client.

**Port.** The public port is **443** — the standard TLS port, chosen so clients
behind restrictive networks reach the daemon on the one port those networks
almost always allow outbound, and so third-party webhook senders (which only
speak standard ports) can reach the same host. `443` is a default, not a
hardcode: a client resolves the port with this precedence —

1. the **port in a SRV record**, if discovery used SRV (a self-hoster on a
   non-standard port is honored automatically — SRV carries a port);
2. a **`port` field in `.well-known` metadata**, if present;
3. otherwise **`OC_DEFAULT_PORT` = 443**.

The daemon *binds* a configurable port (`OPENCHIME_PROTO_PORT`). In production it
binds 443 (self-hosted: the systemd unit grants `CAP_NET_BIND_SERVICE`, per
ARCH-20; hosted: Fly maps external 443 → the container). Local/dev binds a
high port (8443) to avoid needing privilege — a deploy-time override, never a
value a client assumes.

**ALPN demultiplexing.** Port 443 is shared between this binary protocol and the
daemon's HTTP/1.1 surface (incoming webhooks, ARCH-32/34; the health check,
ARCH-25) by **ALPN** negotiated during the TLS handshake:

- The client **MUST** offer ALPN **`oc/1`** (`OC_ALPN_PROTO`). The daemon
  selects it and routes the connection to the binary-protocol handler.
- A connection that negotiates anything else (or offers `http/1.1`) is routed to
  the HTTP handler instead. Third-party webhook clients are ordinary HTTPS and
  never offer `oc/1`, so they land on the HTTP side automatically.

```
                          TLS on :443
   client (ALPN oc/1) ───────────────▶ ┌───────────────┐
                                        │ ALPN = oc/1 ? │
   webhook sender (HTTP) ──────────────▶└──────┬────────┘
                                    yes  │      │  no
                                         ▼      ▼
                              binary protocol   HTTP/1.1 (webhooks, /healthz)
                              (this document)   (ARCH-32/34)
```

Only the binary-protocol side and TLS termination exist today; the HTTP handler
and its CA-signed cert (ARCH-34) are a later milestone. The **`oc/1` ALPN and
the 443 default are fixed now** so clients are correct from first release and
the HTTP surface can be added on the same port without touching them. The `oc`
version suffix (`/1`) tracks the transport-framing generation, distinct from the
per-frame `version` field in §2.

---

## 2. Frame format

Every frame on the wire is a fixed 8-byte header followed by a
type-specific payload. All multi-byte integers are **network byte order**
(big-endian), converted at the edges (ARCH-9).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          length (u32)                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         version (u16)         |        msg_type (u16)         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    payload (length - 4 bytes)                 |
+                              ...                              +
```

| Field      | Type | Meaning                                                             |
|------------|------|---------------------------------------------------------------------|
| `length`   | u32  | Total number of bytes **after** this field: `4 + len(payload)`. Makes the stream self-framing over TCP (ARCH-8). |
| `version`  | u16  | Protocol version governing the layout of `payload` (ARCH-8).        |
| `msg_type` | u16  | Discriminates the frame type; see §9.                               |
| `payload`  | var  | `length - 4` bytes, laid out per the message type at that version.  |

- `version` is present on **every** frame, not only the first, so a frame is
  self-describing. In practice a session negotiates a single version at
  handshake (§3) and all subsequent frames carry it; a frame whose `version`
  differs from the negotiated one is a protocol error (`ERROR`
  `MALFORMED_FRAME`, §8).
- The `HELLO`, `WELCOME`, and `REJECT` handshake frames are **frozen at
  version 1 forever** (§3), so version negotiation itself never has a version
  mismatch problem.

### 2.1 Size limits (ARCH-30, REQ-054)

- `MAX_FRAME_SIZE` = **66,560 bytes** (65 KiB) total on the wire
  (`4 + length`). A peer that reads a `length` implying a larger frame MUST
  reject the connection with `ERROR FRAME_TOO_LARGE` (fatal) before reading
  the payload.
- `MAX_BODY_SIZE` = **65,536 bytes** (64 KiB) for a message body (REQ-054).
  The 1 KiB gap between body and frame limits is framing/field-header
  headroom, reconciling REQ-054's "~64KB body" with ARCH-30's "~64KB frame."
- `MAX_ATTACHMENT_SIZE` = **100 MiB** (default; `OPENCHIME_MAX_ATTACHMENT_SIZE`
  overridable, REQ-140). A file's *bytes* are never in SQLite (they go to object
  storage, ARCH-17/70), but they **do** transport over this protocol — proxied
  through the daemon, split into chunks (§5.14). Each chunk is an ordinary frame:
  its `data` payload is capped so the whole frame stays `<= MAX_FRAME_SIZE`, so
  the reassembly/parse path is unchanged. Attachment bytes are exempt from
  `MAX_BODY_SIZE` (that governs message text only).

### 2.2 Reading discipline (ARCH-9)

A reader consumes a frame in two phases, each looping to tolerate partial TCP
reads:

1. Read exactly 4 bytes to get `length`. Validate `4 <= length` and
   `4 + length <= MAX_FRAME_SIZE`.
2. Read exactly `length` more bytes; parse `version`, `msg_type`, and the
   payload from that buffer.

No frame is acted upon until fully read. Integers are converted from network
byte order as they are parsed; nothing is `memcpy`'d over a struct (ARCH-7).

---

## 3. Handshake and version negotiation (resolves REQ-110, REQ-111)

The first frame on a new connection MUST be `HELLO` from the client. Any other
first frame is answered with `REJECT` (`UNEXPECTED_MSG_TYPE`) and the
connection is closed.

The `HELLO`/`WELCOME`/`REJECT` frames are **frozen at `version = 1`**: their
layout is guaranteed never to change, so a client of any era can always speak
the handshake and receive an intelligible answer.

### 3.1 `HELLO` (client → server), msg_type `0x0001`

The client advertises the inclusive range of protocol versions it can speak.

| Field         | Type | Notes                                        |
|---------------|------|----------------------------------------------|
| `min_version` | u16  | Lowest protocol version the client supports. |
| `max_version` | u16  | Highest protocol version the client supports. `min_version <= max_version`. |
| `client_info` | str  | Free-form build/platform string, for logging only (e.g. `openchime-desktop/0.1 linux`). |

(`str` is the u16-length-prefixed UTF-8 encoding defined in §7.)

### 3.2 `WELCOME` (server → client), msg_type `0x0002`

Sent when the server can speak a version in the client's range. It selects
`chosen_version = min(server_max, client_max)`, which is guaranteed
`>= server_min` and `>= client_min` when the ranges overlap. All subsequent
frames in **both** directions use `chosen_version`.

| Field            | Type | Notes                                                        |
|------------------|------|--------------------------------------------------------------|
| `chosen_version` | u16  | The version both sides will use for the rest of the session. |
| `server_time`    | u64  | Server wall-clock time, ms since Unix epoch UTC (§7), so the client can estimate clock skew. |

After `WELCOME`, the server sends `AUTH_CHALLENGE` and expects `AUTH` (§4) next.

### 3.3 `REJECT` (server → client), msg_type `0x0003`, fatal

Sent when the handshake cannot proceed. The server closes the connection
immediately after writing this frame.

| Field     | Type | Notes                                    |
|-----------|------|------------------------------------------|
| `code`    | u16  | A reason code from §8's table.           |
| `message` | str  | Human-readable detail, for logs/display. |

Version negotiation reason codes (REQ-111) let the client show the right
remedy:

- `VERSION_TOO_OLD` (client's `max_version` < server's minimum): the client is
  outdated. This is the **only** case in which a client should tell the user
  "please update the app."
- `VERSION_TOO_NEW` (client's `min_version` > server's maximum): the *server*
  is behind. The client must **not** tell the user to update the app; the
  correct remedy is upgrading the daemon, which is the operator's action.

```
Client                                  Server
  | ---- HELLO(min=1,max=1,info) ------> |
  |                                      |  ranges overlap?
  | <--- WELCOME(chosen=1, srv_time) --- |  yes
  |                                      |
  |  (or, no overlap:)                   |
  | <--- REJECT(VERSION_TOO_OLD, msg) -- |  server then closes
```

---

## 4. Authentication (REQ-020, REQ-023; ARCH-19/55–60)

After `WELCOME`, the daemon sends `AUTH_CHALLENGE` advertising the auth method(s)
it accepts; the client MUST authenticate before sending any messaging frame. A
messaging frame received before `AUTH_OK` is answered with `ERROR AUTH_REQUIRED`
(fatal). Full design in [AUTH.md](./AUTH.md).

### 4.1 `AUTH_CHALLENGE` (server → client), msg_type `0x0012`

Sent immediately after `WELCOME`. Advertises the deployment's auth mode (ARCH-55)
so the client presents the right login UI.

| Field         | Type | Notes                                                          |
|---------------|------|----------------------------------------------------------------|
| `methods`     | u8   | Bitset of accepted methods: `0x01` local, `0x02` oidc, `0x04` session (reconnect is always accepted alongside the primary mode). |
| `oidc_params` | str  | Empty unless `oidc` is offered; otherwise a small opaque blob the client passes to its OIDC helper (central authorize URL/`client_id`, this workspace's `audience`). Ignorable by clients that only reconnect. |

### 4.2 `AUTH` (client → server), msg_type `0x0010`

A `method` discriminator selects the credential the payload carries (ARCH-59 for
`local`, ARCH-56/57 for `oidc`, ARCH-58 for `session`).

| Field        | Type | Notes                                                          |
|--------------|------|----------------------------------------------------------------|
| `method`     | u8   | `0x01` local, `0x02` oidc, `0x04` session.                     |
| `credential` | lstr | Method-specific, bounded by `MAX_BODY_SIZE`: **local** — `username` (str) then `password` (str); **oidc** — the central-issued ES256 JWT (AUTH.md §3.3); **session** — the 32-byte session token from a prior `AUTH_OK`. |

The daemon verifies per its mode and rejects on any mismatch with a fatal
`ERROR`: `AUTH_INVALID_TOKEN` (bad/expired/ wrong-audience token or bad
password), `AUTH_RATE_LIMITED` (too many failed attempts, REQ-191), or
`AUTH_REQUIRED` (method not offered by this deployment). The daemon never
validates raw *provider* JWTs or fetches provider JWKS — in `oidc` it verifies a
central-issued ES256 JWT against a single pinned key (AUTH.md §3).

### 4.3 `AUTH_OK` (server → client), msg_type `0x0011`

On success the daemon mints a session (ARCH-58) and returns:

| Field           | Type | Notes                                                          |
|-----------------|------|----------------------------------------------------------------|
| `user_id`       | u64  | The authenticated user's stable tenant-local id.               |
| `role`          | u8   | Tenant role: `0` member, `1` admin, `2` owner (ARCH-60).       |
| `session_expiry`| u64  | Ms since epoch UTC after which the session is invalid (the daemon's own expiry, REQ-181). |
| `session_token` | bytes| 32-byte token the client stores and re-presents on reconnect (`AUTH` method `session`). Sent only on a fresh (non-`session`) auth; empty on a `session` re-auth. |

A freshly-authenticated client typically follows `AUTH_OK` with a
`BACKFILL_REQUEST` (§6) to catch up on anything missed while disconnected.

### 4.4 `LOGOUT` (client → server), msg_type `0x0013`

Revokes daemon-issued sessions (REQ-182) — the local revocation a stateless
provider JWT cannot offer, and the reason the daemon issues its own sessions.

| Field           | Type | Notes                                                          |
|-----------------|------|----------------------------------------------------------------|
| `scope`         | u8   | `0` this session only, `1` all of the caller's sessions.       |
| `session_token` | bytes| The 32-byte token to revoke when `scope=0`; ignored for `scope=1`. |

The daemon deletes the matching `sessions` row(s) — the `scope=0` delete is
scoped to the authenticated user, so a leaked token still cannot revoke another
user's session — then closes the connection (the client re-authenticates to
continue).

---

## 5. Messaging (REQ-050, REQ-090, REQ-092, REQ-093; edit/delete REQ-051/052/032)

### 5.1 `SEND` (client → server), msg_type `0x0020`

| Field                | Type   | Notes                                                                 |
|----------------------|--------|-----------------------------------------------------------------------|
| `channel_id`         | u64    | Target channel or DM conversation.                                    |
| `idempotency_token`  | 16 B   | Client-generated random 128-bit token (§7 `idem`). Enables safe retry (REQ-093). Distinct from the server message id. |
| `body`               | lstr   | Message body, u32-length-prefixed UTF-8 (§7), `<= MAX_BODY_SIZE` (REQ-054). |

On accept, the daemon's single DB-writer thread assigns a `message_id` and a
server timestamp inside the WAL commit, then answers `SEND_ACK`. The client
send is acked after the local commit only, not after remote replication
(ARCH-23).

Idempotency (REQ-093): the daemon keeps a persisted mapping
`(channel_id, idempotency_token) → message_id` (see ARCH-44). A `SEND` whose
`(channel_id, idempotency_token)` is already present is **not** stored again;
the daemon returns the original `SEND_ACK` (same `message_id`). A dropped
connection before the first ack is therefore safely retryable with the same
token.

Errors: `BODY_TOO_LARGE`, `UNKNOWN_CHANNEL`, `NOT_A_MEMBER` (REQ-031),
`SEND_RATE_LIMITED` (REQ-190) — all non-fatal `ERROR` frames carrying the
offending `idempotency_token` in `context` (§8) so the client can correlate.

### 5.2 `SEND_ACK` (server → client), msg_type `0x0021`

Confirms acceptance to the sender.

| Field               | Type | Notes                                              |
|---------------------|------|----------------------------------------------------|
| `idempotency_token` | 16 B | Echoes the `SEND` token, so the client can match the ack to its pending send. |
| `channel_id`        | u64  | Same channel.                                      |
| `message_id`        | u64  | Server-assigned id (ARCH-43), tenant-monotonic.    |
| `server_time`       | u64  | Server-assigned timestamp, ms since epoch UTC (REQ-050). |

### 5.3 `BROADCAST` (server → client), msg_type `0x0022`

Delivers an accepted message to every currently-connected, authorized member
of the channel (REQ-090), including — for simplicity and echo confirmation —
the original sender. Ordering within a channel matches accept order
(REQ-092), which is exactly ascending `message_id` because ids are
tenant-monotonic (ARCH-43).

| Field          | Type | Notes                                        |
|----------------|------|----------------------------------------------|
| `message_id`   | u64  | Server id; the client's dedup key (ARCH-45). |
| `channel_id`   | u64  | Channel the message belongs to.              |
| `author_id`    | u64  | Authoring user's id.                         |
| `server_time`  | u64  | Server timestamp, ms since epoch UTC.        |
| `body`         | lstr | Message body (§7).                           |

The frame may then carry the **optional trailing block** (§5.14): the attachment
metadata list, followed by an optional `author_name` (str) — the **display name
to show** for the message: a webhook's label override (§5.15) if set, otherwise
the author's `display_name` (so clients render "dana" rather than a bare id,
ARCH-74); empty means fall back to `author_id`. Both are self-describing —
present only when non-empty — so a plain message with a known author name is
still compact; because two optional fields share the tail, a name with no
attachments is preceded by a zero
attachment count.

Client-side dedup (REQ-091): each client keeps a per-channel high-water mark of
the highest `message_id` it has seen. A `BROADCAST` with
`message_id <= high_water[channel_id]` is a redelivery and is suppressed rather
than rendered twice (ARCH-45).

### 5.4 `CLIENT_ACK` (client → server), msg_type `0x0023`

The client confirms receipt, giving the daemon a per-client delivery cursor for
at-least-once accounting (REQ-090) and reconnect backfill (§6).

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `channel_id` | u64  | Channel being acked.                                     |
| `message_id` | u64  | Highest contiguously-received id in this channel. Acks are cumulative per channel: acking `N` acks everything `<= N` in that channel. |

```
Client                                  Server
  | ---- SEND(ch, idem, body) --------> |  assign message_id, WAL commit
  | <--- SEND_ACK(idem, id, time) ----- |
  | <--- BROADCAST(id, ch, ...) ------- |  (to every member, incl. sender)
  | ---- CLIENT_ACK(ch, id) ----------> |  advance delivery cursor
```

When a `CLIENT_ACK` **advances** the stored cursor, the daemon also drives
read-receipts (seen-by, REQ-090): it fans a `READ_CURSOR` to the channel's other
members and backfills the acker with those members' current cursors.

**`READ_CURSOR` (S → C), `0x0033`** `{ channel_id: u64, user_id: u64, message_id:
u64 }` — `user_id` has read `channel_id` up to `message_id`. A client folds these
per-channel (advance-only) and renders "seen by …" on the last message for every
member whose cursor has reached it. A duplicate/stale ack (no advance) fans
nothing, so idle re-acks are silent.

### 5.5 Editing a message (REQ-051)

A user may edit **their own** message; there is no moderator edit (REQ-032). The
edit replaces the body and stamps `edited_at_ms`, keeping the original
`message_id`, `created_at_ms`, and position in history.

**`EDIT` (client → server), msg_type `0x0024`:**

| Field        | Type | Notes                                             |
|--------------|------|---------------------------------------------------|
| `channel_id` | u64  | Channel the message belongs to.                   |
| `message_id` | u64  | The message to edit.                              |
| `body`       | lstr | Replacement body, `<= MAX_BODY_SIZE` (REQ-054).   |

On success the daemon fans a **`MSG_EDITED` (server → client), msg_type
`0x0026`** out to every connected member of the channel — including the editor,
whose copy doubles as the acknowledgement:

| Field        | Type | Notes                                            |
|--------------|------|--------------------------------------------------|
| `message_id` | u64  | The edited message (the client's dedup key).     |
| `channel_id` | u64  | Channel the message belongs to.                  |
| `author_id`  | u64  | The message's author.                            |
| `edited_at`  | u64  | Server edit time, ms since epoch UTC (REQ-051).  |
| `body`       | lstr | The new body (§7).                               |

### 5.6 Deleting a message (REQ-052, REQ-032)

Deletion is a **tombstone**, not a row removal: the body is nulled while
`message_id`, `author_id`, and timestamps survive, so thread links and reply
counts do not break (REQ-052). A user may delete **their own** message; an
**admin or owner who belongs to the channel** may delete **any** message in it
for moderation (REQ-032). `deleted_by` records who removed it, distinguishing a
self-delete from a moderator delete.

**`DELETE` (client → server), msg_type `0x0025`:**

| Field        | Type | Notes                                             |
|--------------|------|---------------------------------------------------|
| `channel_id` | u64  | Channel the message belongs to.                   |
| `message_id` | u64  | The message to delete.                            |

On success the daemon fans a **`MSG_DELETED` (server → client), msg_type
`0x0027`** out to every connected member:

| Field         | Type | Notes                                           |
|---------------|------|-------------------------------------------------|
| `message_id`  | u64  | The tombstoned message.                         |
| `channel_id`  | u64  | Channel the message belongs to.                 |
| `author_id`   | u64  | The message's original author.                  |
| `deleted_by`  | u64  | Who deleted it — the author (self) or a moderator (REQ-032). |
| `deleted_at`  | u64  | Server delete time, ms since epoch UTC.         |

Errors for both `EDIT` and `DELETE` are non-fatal `ERROR` frames carrying the
offending `message_id` (8 bytes, big-endian) in `context` so the client can
correlate: `UNKNOWN_MESSAGE` (no such message in the channel, or it is already
tombstoned) or `FORBIDDEN` (not the author — and, for delete, neither the author
nor a channel moderator who belongs to the channel).

### 5.7 Channel management (REQ-050, REQ-031, REQ-033)

Channels are the containers messages belong to. A channel is either **public**
(open to every tenant user) or **private** (members-only). Membership
(`channel_members`) is the fan-out set for `BROADCAST`/`MSG_EDITED`/`MSG_DELETED`
and the read/post gate:

- **Read** (backfill, §6) a channel: allowed if it is public **or** the user is
  a member (REQ-031). Private channels reveal nothing to non-members.
- **Post** (`SEND`/`EDIT`/`DELETE`): a member may always post; posting to a
  **public** channel the user has not joined **auto-joins** them (so subsequent
  broadcasts reach them); a **private** channel the user does not belong to
  rejects with `NOT_A_MEMBER`. A `channel_id` that does not exist rejects with
  `UNKNOWN_CHANNEL`.

Each operation below is acknowledged with a **`CHANNEL_INFO` (server → client),
msg_type `0x0051`** describing the channel and the caller's membership:

| Field        | Type | Notes                                             |
|--------------|------|---------------------------------------------------|
| `channel_id` | u64  | The channel.                                      |
| `kind`       | u8   | `0` channel, `1` DM.                              |
| `name`       | str  | Channel name (empty for a DM).                    |
| `is_public`  | u8   | `1` public, `0` private.                          |
| `joined`     | u8   | `1` if the recipient is now a member.             |
| `created_at` | u64  | Creation time, ms since epoch UTC.                |
| `peer_id`    | u64  | **Optional trailing.** For a DM (`kind=1`), the other participant *from the recipient's view* — the client titles the DM from the roster. Written only for a DM, so a named-channel `CHANNEL_INFO` is byte-identical to the pre-DM layout. |

Failures are non-fatal `ERROR` frames carrying the `channel_id` (8 bytes,
big-endian) in `context`: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER`, `FORBIDDEN`, or
`INVALID_CHANNEL` (empty/oversized name on create).

**`CREATE_CHANNEL` (client → server), msg_type `0x0050`** — the creator
auto-joins; replies `CHANNEL_INFO`.

| Field       | Type | Notes                                                    |
|-------------|------|----------------------------------------------------------|
| `name`      | str  | 1..64 bytes (`INVALID_CHANNEL` otherwise).               |
| `is_public` | u8   | `1` public, `0` private.                                 |

**`LIST_CHANNELS` (client → server), msg_type `0x0052`** — empty payload;
replies **`CHANNEL_LIST` (server → client), msg_type `0x0053`** with every
public channel plus the private channels the user belongs to:

| Field       | Type            | Notes                                             |
|-------------|-----------------|---------------------------------------------------|
| `count`     | u16             | Number of entries.                                |
| `entries[]` | `count` × entry | Each: `channel_id` (u64), `name` (str), `is_public` (u8), `joined` (u8), `kind` (u8 — `0` channel, `1` DM). |

The list includes the caller's DMs (§5.12) alongside channels; DM entries have
`kind=1`, an empty `name`, and `is_public=0`.

**`JOIN_CHANNEL` (client → server), msg_type `0x0054`** `{ channel_id: u64 }` —
public channels are self-joinable; a private channel rejects with `FORBIDDEN`
unless the caller was already invited. Replies `CHANNEL_INFO` (`joined=1`).

**`LEAVE_CHANNEL` (client → server), msg_type `0x0055`** `{ channel_id: u64 }` —
drops the caller's membership (idempotent). Replies `CHANNEL_INFO` (`joined=0`).

**`INVITE_TO_CHANNEL` (client → server), msg_type `0x0056`**
`{ channel_id: u64, user_id: u64 }` — any existing **member** may add another
user (REQ-033, channel-level, not gated to admins); this is how a private
channel becomes reachable. The actor gets a `CHANNEL_INFO` ack, and the invited
user's live connections are **pushed** a `CHANNEL_INFO` (`joined=1`) so their
client learns of the new channel immediately.

**`REMOVE_FROM_CHANNEL` (client → server), msg_type `0x0057`**
`{ channel_id: u64, user_id: u64 }` — any existing member may remove another
(REQ-033). Replies `CHANNEL_INFO` to the actor.

Every tenant has one auto-provisioned public **`general`** channel (id `1`) that
every user joins at authentication, so the messaging path always has a channel
to deliver to.

### 5.8 Tenant administration (REQ-030, REQ-033)

Managing the tenant's people: enumerating them, changing roles, and adding or
removing members. Role changes and member add/remove are gated by the tenant
role policy (owner/admin/member, ARCH-60, AUTH.md §6); enumeration is open to any
authenticated user.

**`LIST_USERS` (client → server), msg_type `0x0040`** — empty payload; replies
**`USER_LIST` (server → client), msg_type `0x0041`**:

| Field       | Type            | Notes                                                        |
|-------------|-----------------|--------------------------------------------------------------|
| `count`     | u16             | Number of entries.                                           |
| `entries[]` | `count` × entry | Each: `user_id` (u64), `role` (u8), `disabled` (u8), `email` (str), `display_name` (str). |

**`SET_ROLE` (client → server), msg_type `0x0042`** `{ user_id: u64, role: u8 }` —
changes a user's tenant role. Enforced by the role policy: only owner/admin may
change roles, only an owner may grant/revoke owner, an admin may only
promote/keep members; demoting the last owner is refused (`LAST_OWNER`, REQ-030).
On success the actor is acked with **`USER_UPDATED` (server → client), msg_type
`0x0045`** `{ user_id: u64, role: u8, disabled: u8 }`, and the same frame is
pushed to the affected user's live connections so their client updates its
capabilities immediately.

**`INVITE_USER` (client → server), msg_type `0x0043`** `{ role: u8 }` — owner/admin
only (only an owner may invite at admin/owner role). Mints a single-use invite
token for a **new** local account and replies **`INVITE_CREATED` (server →
client), msg_type `0x0046`**:

| Field        | Type  | Notes                                                       |
|--------------|-------|-------------------------------------------------------------|
| `token`      | bytes | The 32-byte invite token (only its SHA-256 is stored). The actor shares it out-of-band. |
| `role`       | u8    | The role the redeemed account will receive.                 |
| `expires_at` | u64   | Ms since epoch UTC after which the token is dead.           |

**`REDEEM_INVITE` (client → server), msg_type `0x0047`** — sent **before** `AUTH`
(the invitee has no account yet), it creates the account and authenticates in one
step:

| Field      | Type  | Notes                                                        |
|------------|-------|--------------------------------------------------------------|
| `token`    | bytes | The invite token from `INVITE_CREATED`.                      |
| `username` | str   | Desired username; must be unused.                            |
| `password` | str   | The account password (PBKDF2-hashed server-side, AUTH.md §2).|

On success the daemon consumes the token (single-use), creates the account with
the invite's role, and replies with a normal **`AUTH_OK`** (§4.3) — the
connection is now authenticated. Any failure (bad/expired/consumed token, taken
username) is a fatal `ERROR AUTH_INVALID_TOKEN`, non-disclosing by design.

**`REMOVE_USER` (client → server), msg_type `0x0044`** `{ user_id: u64 }` —
owner/admin only; an admin cannot remove an admin/owner, and the last owner
cannot be removed (`LAST_OWNER`). The member is **locked out** (their sessions,
channel memberships, and local password are dropped, and a `disabled` flag bars
every future login) rather than deleted, so their authored messages keep a valid
author. The actor is acked with `USER_UPDATED` (`disabled=1`); the removed user's
live connections receive the same notice and are then dropped.

`SET_ROLE`/`INVITE_USER`/`REMOVE_USER` failures are non-fatal `ERROR` frames
(`FORBIDDEN`, `LAST_OWNER`, `INTERNAL_ERROR`); a `REDEEM_INVITE` failure is fatal
(the connection never authenticated).

### 5.9 Reactions (REQ-070, REQ-071)

A user may attach emoji reactions to any message they can **read** (public
channel or a member of a private one, §5.7). A given `(message, user, emoji)` is
unique — a repeat add is a no-op, so a reaction is toggled, never stacked
(REQ-070).

**`REACT` (client → server), msg_type `0x0028`:**

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `channel_id` | u64  | The message's channel (for the read-access check).       |
| `message_id` | u64  | The message being reacted to.                            |
| `emoji`      | str  | 1..32 bytes (fits multi-codepoint sequences).            |
| `op`         | u8   | `1` add, `0` remove.                                     |

On success the daemon fans a **`REACTION_UPDATED` (server → client), msg_type
`0x0029`** out to every connected member of the channel:

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `message_id` | u64  | The reacted message.                                     |
| `channel_id` | u64  | The message's channel.                                   |
| `user_id`    | u64  | Who added/removed the reaction.                          |
| `emoji`      | str  | The emoji.                                               |
| `op`         | u8   | `1` add, `0` remove.                                     |
| `count`      | u64  | New aggregate count of this emoji on the message (REQ-071). |

**`LIST_REACTIONS` (client → server), msg_type `0x002A`**
`{ channel_id: u64, message_id: u64 }` — replies **`REACTIONS` (server → client),
msg_type `0x002B`** with the full reactor list for inspection (REQ-071):

| Field        | Type            | Notes                                             |
|--------------|-----------------|---------------------------------------------------|
| `message_id` | u64             | The inspected message.                            |
| `count`      | u16             | Number of entries.                                |
| `entries[]`  | `count` × entry | Each: `emoji` (str) + `user_id` (u64), ordered by emoji then user. |

Failures are non-fatal `ERROR` frames carrying the `message_id` (8 bytes,
big-endian) in `context`: `UNKNOWN_MESSAGE` (no such message, or it is
tombstoned — a tombstone's reactions are cleared, §5.6), `NOT_A_MEMBER` (cannot
read the channel), or `INVALID_REACTION` (empty/oversized emoji).

### 5.10 Threads (REQ-060)

Any message can be replied to as a thread. A reply threads under a **top-level
root** — replying to a reply flattens to that reply's root, so threads are one
level deep. A reply is **not** delivered to the channel's main scroll (§5.3): it
is fanned out as a `THREAD_REPLY`, and the parent shows a reply count.

**`SEND_REPLY` (client → server), msg_type `0x002C`:**

| Field        | Type | Notes                                                     |
|--------------|------|-----------------------------------------------------------|
| `channel_id` | u64  | The channel.                                              |
| `idem`       | 16 B | Idempotency token (§7), exactly as `SEND` (REQ-093).      |
| `parent_id`  | u64  | The message being replied to (resolved to its root).      |
| `body`       | lstr | Reply body, `<= MAX_BODY_SIZE`.                           |

The sender is acked with a normal `SEND_ACK` (§5.2). Then the daemon fans a
**`THREAD_REPLY` (server → client), msg_type `0x002D`** out to every connected
member of the channel:

| Field         | Type | Notes                                                    |
|---------------|------|----------------------------------------------------------|
| `message_id`  | u64  | The reply's server id.                                   |
| `channel_id`  | u64  | The channel.                                             |
| `parent_id`   | u64  | The thread root.                                         |
| `author_id`   | u64  | Who replied.                                             |
| `server_time` | u64  | Server timestamp, ms since epoch UTC.                    |
| `reply_count` | u32  | The root's total reply count after this reply (REQ-060). |
| `body`        | lstr | The reply body.                                          |

**`LIST_THREAD` (client → server), msg_type `0x002E`**
`{ channel_id: u64, parent_id: u64 }` opens a thread. The daemon **streams** the
replies (oldest first) as `THREAD_REPLY` frames — each self-framed, so a 64KB
body is fine — then closes with a **`THREAD` (server → client), msg_type
`0x002F`** terminator, mirroring backfill's `BACKFILL_DONE` (§6.2):

| Field       | Type | Notes                                        |
|-------------|------|----------------------------------------------|
| `parent_id` | u64  | The thread root.                             |
| `count`     | u32  | Number of replies streamed.                  |

On reconnect backfill (§6) the main scroll replays only top-level messages; a
parent that has replies is followed by a **`THREAD_META` (server → client),
msg_type `0x0032`** so the client can show the reply count without opening the
thread:

| Field          | Type | Notes                                       |
|----------------|------|---------------------------------------------|
| `message_id`   | u64  | The parent message.                         |
| `reply_count`  | u32  | Its total reply count.                      |
| `last_reply_at`| u64  | Timestamp of the most recent reply, ms UTC. |

`SEND_REPLY`/`LIST_THREAD` failures are non-fatal `ERROR` frames:
`UNKNOWN_MESSAGE` (no such parent, or it is tombstoned), `NOT_A_MEMBER` /
`UNKNOWN_CHANNEL` (cannot post to / read the channel).

*Thread notifications (REQ-061) — notifying a thread's prior participants per
their per-channel notification setting — depend on notification configuration
(REQ-130) and are a later revision; today a `THREAD_REPLY` reaches every
connected channel member.*

### 5.11 Search (REQ-080)

Full-text search over message history, scoped to what the searching user can
read (public channels and the private channels they belong to, REQ-031).
Implemented with a SQLite FTS5 index over message bodies (ARCH-15, SCHEMA.md
§3d); there is no history cutoff.

**`SEARCH` (client → server), msg_type `0x0060`:**

| Field   | Type | Notes                                                          |
|---------|------|----------------------------------------------------------------|
| `query` | str  | Free text. The daemon quotes each whitespace-separated term, so query punctuation is literal and multiple terms are ANDed; it never errors on syntax. |
| `limit` | u16  | Max results wanted; the daemon caps it (currently 50).         |

Replies **`SEARCH_RESULTS` (server → client), msg_type `0x0061`**, newest match
first:

| Field       | Type            | Notes                                                |
|-------------|-----------------|------------------------------------------------------|
| `count`     | u16             | Number of entries.                                   |
| `entries[]` | `count` × entry | Each: `message_id` (u64), `channel_id` (u64), `author_id` (u64), `server_time` (u64), `snippet` (str — an FTS5 excerpt around the match). |

Tombstoned messages (§5.6) are excluded, and an edit (§5.5) re-indexes the
message. An empty or all-punctuation query returns zero results (never an error).

### 5.12 Direct messages (REQ-050)

A direct message conversation is a **`kind='dm'` channel with exactly its
participants** — two for a normal DM, or **one for a self-DM** (a personal
"notes to self" space, REQ-055). Once opened, everything else (send/broadcast,
backfill, search, reactions, threads) works through the ordinary membership path
with no DM-specific machinery. DMs are members-only (`is_public=0`) and never
named.

**`OPEN_DM` (client → server), msg_type `0x0058`** `{ user_id: u64 }` — opens (or
returns) the DM between the caller and `user_id`. `user_id` may be the caller's
own id, which opens a **self-DM** (single participant). Idempotent: an existing
DM is returned rather than a second created; an unknown target is refused
(`FORBIDDEN`). Replies **`CHANNEL_INFO`** (§5.7, `kind=1`) to the caller, and —
for a two-party DM — pushes the same to the peer's live connections so their
client learns of it. Thereafter the client sends to the returned `channel_id`
with an ordinary `SEND`; all participants (just the sender, for a self-DM)
receive the `BROADCAST`.

DMs a user belongs to appear in `LIST_CHANNELS` (§5.7) with `kind=1`. The
channel-management ops (join/leave/invite/remove) operate only on named channels
and reject a DM id with `UNKNOWN_CHANNEL`.

---

### 5.13 Presence and typing (REQ-120, REQ-121)

Presence and typing are **ephemeral real-time state**, never persisted. Presence
is derived from live connections on the network thread (ARCH-67); typing is a
member-scoped relay (ARCH-68). Both are advisory — a dropped frame is
self-correcting and never fatal.

**Presence status values:** `0 = offline`, `1 = online`, `2 = away`.

**`SET_PRESENCE` (client → server), msg_type `0x0070`** `{ status: u8 }` — the
client marks *this connection* online (`1`) or away (`2`); `offline` is not
settable (it is implied by disconnect). The server recomputes the user's
aggregate status — **online** if any of their connections is active, **away** if
all are away, **offline** once the last closes — and, if it changed, broadcasts.

**`PRESENCE_UPDATE` (server → client), msg_type `0x0071`** `{ user_id: u64,
status: u8 }` — sent tenant-wide to every *other* authenticated connection when a
user's aggregate status changes (a client tracks its own presence locally, so it
is excluded from its own broadcast). On authenticating, a client also receives a
**one-shot snapshot** — one `PRESENCE_UPDATE` per currently-online user — so it
starts with an accurate roster without polling.

**`TYPING` (client → server), msg_type `0x0072`** `{ channel_id: u64 }` — the
caller signals they are composing in `channel_id`. The server resolves the
channel's members (on the read connection, ARCH-66) and, if the caller has access,
relays to the other connected members only — private-channel and DM typing never
leaks to non-members.

**`TYPING_UPDATE` (server → client), msg_type `0x0073`** `{ channel_id: u64,
user_id: u64 }` — delivered to each connected member of `channel_id` except the
sender. There is **no expiry frame and no "stopped typing" signal**: the client
displays the indicator and expires it locally after **~6 seconds**, refreshed by
each new `TYPING_UPDATE` for that `(channel, user)`.

---

### 5.14 Attachments (REQ-140, REQ-141)

File bytes are **proxied through the daemon** over this same pinned-TLS
connection, split into chunks — never fetched from a directly-reachable object
store via a signed URL (ARCH-69). This makes access control a single in-daemon
check on the ordinary membership path (§5.7, REQ-031), identical to reading the
message the attachment hangs off — there is no URL to leak and no object-store
ACL to configure (resolves REQ-141). The blob itself lands in object storage,
never SQLite (ARCH-17/70); only pointer + metadata rows are stored.

A transfer is a short state machine over the existing frame stream; the
`attachment_id` correlates all frames of one transfer, and either side may abort
with `TRANSFER_CANCEL`. Chunk `data` is bounded so each frame stays
`<= MAX_FRAME_SIZE`; total bytes are bounded by `MAX_ATTACHMENT_SIZE` (§2.1).

**Upload (client → server bytes):**

1. **`UPLOAD_BEGIN` (C → S), `0x0080`** `{ channel_id: u64, idempotency_token:
   16B, filename: str, mime: str, total_size: u64 }` — the client declares an
   upload targeting a channel/DM it can post to. The server authorizes
   membership, checks `total_size <= MAX_ATTACHMENT_SIZE`, allocates an
   `attachment_id` + opaque storage key, and opens a streaming write to object
   storage. Idempotent on `(channel, token)` like `SEND` (§5.1, ARCH-44). On
   failure → `ERROR` (`FORBIDDEN`, `ATTACHMENT_TOO_LARGE`, …).
2. **`UPLOAD_READY` (S → C), `0x0081`** `{ attachment_id: u64, chunk_size: u32,
   window_bytes: u32 }` — the server advertises the max `data` per chunk and the
   **in-flight window**: the client may have at most `window_bytes` of un-acked
   chunk data outstanding.
3. **`UPLOAD_CHUNK` (C → S), `0x0082`** `{ attachment_id: u64, seq: u32, data:
   bytes }` — sequential chunks. The server streams each to storage. If the
   storage writer falls behind, the net thread stops reading this connection
   (drops `EPOLLIN`) so TCP flow control throttles the client; the window plus
   `UPLOAD_ACK` bound app-level in-flight bytes.
4. **`UPLOAD_ACK` (S → C), `0x0083`** `{ attachment_id: u64, acked_through: u32 }`
   — advances the window as chunks are durably handed to storage; the client may
   send more once space frees.
5. **`UPLOAD_END` (C → S), `0x0084`** `{ attachment_id: u64 }` — the client
   signals completion. The server finalizes the object, verifies the received
   size equals `total_size`, and commits the `attachments` row on the DB-writer
   thread. On mismatch → `ERROR` and the partial blob is discarded.
6. **`UPLOAD_OK` (S → C), `0x0085`** `{ attachment_id: u64, size: u64, sha256:
   bytes }` — the attachment is now **pending**: owned by the uploader, bound to
   the target channel, not yet visible. The client references it in a subsequent
   message to publish it (below).

**Linking to a message.** An attachment becomes visible only when referenced by
a message. `SEND` gains an **optional trailing attachment list** (`count: u16`
then `count × { attachment_id: u64 }`, at most `OC_MAX_ATTACH` = 16). It is a
*self-describing optional field*: written only when non-empty, and read only if
bytes remain after the base fields — so a message with no attachments is
byte-identical to the pre-attachment layout and **no protocol-version bump is
needed** (client and daemon share this codec, so there is no older peer to
negotiate against). The server links each id that is a finalized, still-unlinked
attachment the caller uploaded to this same channel (others are ignored, i.e.
simply not shared) and sets its `message_id`. `BROADCAST` correspondingly carries
a trailing attachment list of `{ attachment_id, filename, mime, size }`, so every
reader — live or via backfill (§6) — sees the attachment through the one message
model, with no attachment-specific delivery path. Thread replies work the same
way: `SEND_REPLY` carries the id list and `THREAD_REPLY` the metadata, live and
via `LIST_THREAD` (§5.10). A message body may
be empty when it carries attachments.

**Download (server → client bytes):**

1. **`DOWNLOAD_BEGIN` (C → S), `0x0086`** `{ attachment_id: u64 }` — the server
   authorizes the requester against the message the attachment belongs to
   (`channel_read_access`, REQ-141); an unauthorized or unknown id → `ERROR`
   (`FORBIDDEN` / `NOT_FOUND`). It opens a streaming read from storage on a
   transfer worker.
2. **`DOWNLOAD_INFO` (S → C), `0x0087`** `{ attachment_id: u64, filename: str,
   mime: str, total_size: u64, sha256: bytes }` — sent once before the bytes.
3. **`DOWNLOAD_CHUNK` (S → C), `0x0088`** `{ attachment_id: u64, seq: u32, data:
   bytes }` — sequential chunks. Backpressure is implicit: the worker pauses
   reading from storage when this connection's output buffer nears the 1 MiB cap
   and resumes on `EPOLLOUT` drain, so a slow reader never grows daemon memory
   (the same discipline as message fan-out).
4. **`DOWNLOAD_END` (S → C), `0x0089`** `{ attachment_id: u64 }` — all bytes
   sent; the client verifies the reassembled size (and optionally `sha256`).

**`TRANSFER_CANCEL` (C → S), `0x008A`** `{ attachment_id: u64 }` — aborts an
in-progress upload or download; the server tears down the transfer and, for an
uncommitted upload, discards the partial blob. Abandoned uploads that never
finalize are swept by a time-gated cleanup (ARCH-70).

---

### 5.15 Incoming webhooks (REQ-170, ARCH-32/71)

An incoming webhook lets an uncontrolled third party post a message into one
channel over HTTP, with no user session. There are two surfaces: the
**binary-protocol frames** a client uses to mint a token, and the **HTTP
endpoint** the third party posts to.

**`CREATE_WEBHOOK` (C → S), `0x0059`** `{ channel_id: u64, label: str }` — an
authenticated member of `channel_id` mints a webhook (same post access as
sending). `label` is a human note. Fails (`ERROR`) with `UNKNOWN_CHANNEL` /
`NOT_A_MEMBER` if the caller can't post there.

**`WEBHOOK_INFO` (S → C), `0x005A`** `{ webhook_id: u64, channel_id: u64, token:
bytes }` — the minted **32-byte token, shown once**. Only its SHA-256 is stored
(ARCH-71); the client hex-encodes the token into the POST URL and cannot recover
it later.

**Management.** `LIST_WEBHOOKS` (C → S, `0x005B`) `{ channel_id: u64 }` → a
`WEBHOOK_LIST` (S → C, `0x005C`) of `{ webhook_id, channel_id, label, disabled }`
for a channel the caller can read — **never the token**. `DELETE_WEBHOOK` (C → S,
`0x005D`) `{ webhook_id: u64 }`, allowed to any member of the webhook's channel,
removes it and replies `WEBHOOK_DELETED` (S → C, `0x005E`) `{ webhook_id: u64 }`
(or `ERROR UNKNOWN_WEBHOOK` / `NOT_A_MEMBER`); the token then stops resolving.

**HTTP endpoint — `POST /webhook/<hex-token>`.** Reached over the same TLS proto
port, demultiplexed by ALPN (ARCH-54): a connection that does **not** negotiate
`oc/1` is served by the HTTP/1.1 handler. The body is either
`application/json {"text": "..."}` or a raw `text/plain` message, capped at
`MAX_BODY_SIZE`. The daemon hex-decodes the token, looks it up by hash, and posts
the text as a message **authored by the webhook's creator**, carrying the
webhook's **label as a display-name override** (the `author_name` field on
`BROADCAST`, so the post shows as e.g. "GitHub CI") — delivered to the channel's
members as an ordinary `BROADCAST` (§5.3) and included in backfill.
Responses: `200 {"ok":true,"message_id":N}` on success; `400` (empty/bad body),
`404` (unknown or disabled token), `405` (non-POST), `413` (too large), `429`
(per-token rate limit, 60/min). Note: REQ-171's CA-signed certificate for this
endpoint is not yet implemented — it currently uses the daemon's TOFU cert.

---

### 5.16 Notification preferences (REQ-130, REQ-131)

A user's notification settings are **server-authoritative and synced across their
devices**. Two settings: a per-channel **level** (REQ-130) and a **do-not-disturb
window** (REQ-131). They are the config that clients honor and that the future
push gateway (REQ-132/133, deferred) will consult; DND suppresses *push*, not
in-app unread (REQ-131).

**`SET_NOTIFY_PREF` (C → S), `0x0090`** `{ channel_id: u64, level: u8 }` — set the
level for a channel the caller can read: `0` all, `1` mentions-only, `2` none. An
absent pref means the default (all). Refused with `ERROR NOT_A_MEMBER` for a
channel the caller can't access, or an invalid level.

**`SET_DND` (C → S), `0x0091`** `{ enabled: u8, start_min: u16, end_min: u16 }` —
set the do-not-disturb window as a daily `[start, end)` range in **minutes of day
(UTC**; the client converts from local time), wrapping past midnight when
`start > end`.

**`LIST_NOTIFY_PREFS` (C → S), `0x0092`** (empty) — request the caller's full
settings.

**`NOTIFY_PREFS` (S → C), `0x0093`** `{ dnd_enabled: u8, dnd_start_min: u16,
dnd_end_min: u16, count: u16, count × { channel_id: u64, level: u8 } }` — the
snapshot. It is sent both as the reply to `LIST_NOTIFY_PREFS` and, after any
`SET_*`, **pushed to every one of the user's connections** so a change on one
device updates the others.

---

### 5.16a Synced client settings

The daemon-side layer of the client config: portable UI prefs a frontend syncs
across its devices, keyed by a `client_type` bucket so a TUI's prefs stay
separate from a future GUI's. The daemon is opaque about the keys/values — it
stores and fans them back; the frontend owns their meaning (SCHEMA.md §3k).

**`SET_CLIENT_SETTING` (C → S), `0x0094`** `{ client_type: str, key: str, value:
str }` — upsert one key in the caller's `(user, client_type)` bucket. An **empty
`value` deletes** the key (it then falls back to the client's machine-local
default). Answered with a `CLIENT_SETTINGS` snapshot.

**`LIST_CLIENT_SETTINGS` (C → S), `0x0095`** `{ client_type: str }` — request the
caller's full bucket for a `client_type`. Answered with `CLIENT_SETTINGS`.

**`CLIENT_SETTINGS` (S → C), `0x0096`** `{ client_type: str, count: u16, count ×
{ key: str, value: str } }` — the bucket snapshot. Sent as the reply to
`LIST_CLIENT_SETTINGS` and, after any `SET_CLIENT_SETTING`, **pushed to every one
of the user's connections**; a client folds only the snapshot whose
`client_type` matches its own, so the sync reaches the user's other same-type
devices without leaking one frontend's bucket to another.

---

### 5.16b Self-service profile (REQ-020)

A user editing their own account: renaming themselves and rotating their local
password. Both act on the **authenticated** user (`user_id` from the session, not
a field), so neither can touch another account.

**`SET_DISPLAY_NAME` (C → S), `0x0048`** `{ name: str }` — set your own
`users.display_name` (non-empty, ≤ 48 bytes). Answered with `PROFILE_UPDATED`.

**`CHANGE_PASSWORD` (C → S), `0x0049`** `{ old_password: str, new_password: str }`
— rotate your local password. The daemon verifies `old_password` (constant-time)
against `local_credentials` and stores a fresh PBKDF2 salt+hash. A non-local
(OIDC) account or a wrong `old_password` is `FORBIDDEN` (an `ERROR`, non-fatal);
success answers with `PROFILE_UPDATED` (the name unchanged) as the ack.

**`PROFILE_UPDATED` (S → C), `0x004A`** `{ user_id: u64, display_name: str }` — a
user's display name (after a rename; or the caller's unchanged name after a
password change). **Fanned to every authed connection** so all rosters update in
place; the originator reads its own as the operation's success ack.

---

### 5.17 Audio call signaling (REQ-150, REQ-152)

Audio is **server-relayed** (no P2P/ICE, ARCH-18): the media itself flows over a
separate UDP sidecar (ARCH-31, a later milestone), but a call is *set up* over
this TCP protocol. A call is **one per channel** (`call_id == channel_id`), and
its roster is **ephemeral net-thread state** (like presence, ARCH-67) — it holds
no DB rows and resets on daemon restart.

**`CALL_JOIN` (C → S), `0x00A0`** `{ channel_id: u64 }` — join (or start) the
channel's call. Authorized by the ordinary channel-read gate; a non-member gets
`ERROR NOT_A_MEMBER`.

**`CALL_JOINED` (S → C, to the joiner), `0x00A2`** `{ channel_id: u64, call_id:
u64, udp_port: u16, token: bytes, count: u16, count × { user_id: u64 } }` — the
joiner's confirmation: the current roster plus their **private** media endpoint —
the audio sidecar's `udp_port` and a per-join 16-byte bearer `token`. The client
then speaks **UDP directly to the sidecar** (out of band from this TCP protocol):
`token(16) ‖ seq(u16) ‖ opus-payload` to it, and receives
`sender_user_id(u64) ‖ seq(u16) ‖ opus-payload` from it (the sidecar relays
opaque payloads to the other participants — it never decodes Opus). See
`daemon/audio.h` for the exact media framing.

**`CALL_ROSTER` (S → C, to the other participants), `0x00A3`** `{ channel_id:
u64, call_id: u64, count: u16, count × { user_id: u64 } }` — pushed to every
other participant whenever the roster changes (a join, a leave, or a
disconnect).

**`CALL_LEAVE` (C → S), `0x00A1`** `{ channel_id: u64 }` — leave the call.

**Loss and rejoin (REQ-152).** A participant is dropped on `CALL_LEAVE` or on TCP
disconnect (the net thread removes them and pushes a fresh `CALL_ROSTER` to the
rest), but the **call persists as long as one participant remains**; the dropped
user simply re-`CALL_JOIN`s (minting a fresh token). The media-side silence
timeout that mirrors this lives in the sidecar.

### Push device tokens (REQ-132/133, ARCH-85)

**`REGISTER_DEVICE_TOKEN` (C → S), `0x00B0`** `{ platform: u8, token: str }` —
register the caller's mobile push token (`platform` 0 = APNs, 1 = FCM). Upserts on
`(user, token)`. The daemon owns this registry; the control-plane gateway stores
nothing (REQ-041). Answered with `DEVICE_TOKEN_ACK`.

**`UNREGISTER_DEVICE_TOKEN` (C → S), `0x00B1`** `{ token: str }` — drop the caller's
registration of a token (logout / token rotation). Answered with `DEVICE_TOKEN_ACK`.

**`DEVICE_TOKEN_ACK` (S → C), `0x00B2`** `{ ok: u8, code: u16 }` — `ok=1` on success;
on failure `code` is the reason (`OC_ERR_INVALID_DEVICE_TOKEN` for an empty token or
unknown platform). A committed SEND then drives contentless APNs/FCM delivery to
registered tokens via the emitter (ARCH-85), gated by each recipient's level + DND.

---

## 6. Reconnect backfill (resolves REQ-101)

A client that reconnects (REQ-100) requests everything it missed since its
last acks, so no message sent during the disconnect window is silently lost.

### 6.1 `BACKFILL_REQUEST` (client → server), msg_type `0x0030`

Sent after `AUTH_OK`. Carries a per-channel cursor list — one `after_message_id`
per channel the client cares about. Per-channel (rather than a single global)
cursors are required because cross-channel ordering carries no guarantee
(REQ-092): a client may have acked a high id in one channel while still missing
a lower id in another, and a single global cursor would skip it.

| Field       | Type            | Notes                                                        |
|-------------|-----------------|--------------------------------------------------------------|
| `count`     | u16             | Number of channel cursors following.                         |
| `cursors[]` | `count` × entry | Each entry: `channel_id` (u64) + `after_message_id` (u64).   |

For a channel the client omits, the server treats the cursor as `0` (replay
from the beginning of that channel's visible history) — a fresh client can send
`count = 0` to get bootstrapped. The server only replays messages in channels
the requesting user is a member of (REQ-031, REQ-080).

### 6.2 Replay and `BACKFILL_DONE` (server → client), msg_type `0x0031`

In response, the server streams ordinary `BROADCAST` frames (§5.3) for every
message with `message_id > after_message_id` in each requested channel, in
ascending `message_id` order per channel, then sends a single `BACKFILL_DONE`
to mark the catch-up complete.

`BACKFILL_DONE` payload:

| Field         | Type | Notes                                                         |
|---------------|------|---------------------------------------------------------------|
| `high_water`  | u64  | Highest `message_id` in the tenant at the moment backfill completed. Purely informational (progress/consistency check); the client's authoritative state remains its per-channel high-water marks. |

Replay volume is bounded per response; if a channel's backlog exceeds the
per-response cap, the server replays up to the cap and the client issues a
follow-up `BACKFILL_REQUEST` with an advanced cursor. (The exact cap is an
implementation tuning value, not a wire-format constant.)

Messages composed while offline are re-sent by the client on reconnect using
their original `idempotency_token` (REQ-102), which the idempotency mapping
(§5.1) de-duplicates against anything the server already accepted.

---

## 7. Primitive field encodings (ARCH-7; recorded as ARCH-42)

All frames are built from this small set of primitives. Encoding is explicit
and field-by-field; no struct is `memcpy`'d onto the wire.

| Name    | On-wire form                                                             |
|---------|--------------------------------------------------------------------------|
| `u8`    | 1 byte.                                                                   |
| `u16`   | 2 bytes, big-endian.                                                      |
| `u32`   | 4 bytes, big-endian.                                                      |
| `u64`   | 8 bytes, big-endian.                                                      |
| `str`   | `u16` byte-length `n`, then `n` bytes of UTF-8 (no NUL terminator). `n <= 65535`. Used for short strings (client info, error messages, JWT). |
| `lstr`  | `u32` byte-length `n`, then `n` bytes of UTF-8. Used for message bodies, which may exceed a `u16` length. `n <= MAX_BODY_SIZE`. |
| `bytes` | `u32` byte-length `n`, then `n` raw bytes.                                |
| `idem`  | Exactly 16 raw bytes (a 128-bit client-generated idempotency token); no length prefix, since the width is fixed. |

Timestamps are `u64` milliseconds since the Unix epoch, UTC. Ids (`user_id`,
`channel_id`, `message_id`) are `u64`.

---

## 8. Error handling

### 8.1 `ERROR` (server → client), msg_type `0x00FF`

A request-scoped or connection-scoped error. The handshake stage uses `REJECT`
(§3.3) instead; `ERROR` is for everything after `WELCOME`.

| Field     | Type | Notes                                                                |
|-----------|------|----------------------------------------------------------------------|
| `code`    | u16  | Reason code from §8.2.                                                |
| `fatal`   | u8   | `1` if the server is closing the connection after this frame; `0` if the connection continues and only the triggering request failed. |
| `context` | bytes| Optional correlation data. For a failed `SEND`, this is the 16-byte `idempotency_token`; otherwise empty (`n = 0`). |
| `message` | str  | Human-readable detail, for logs/display.                             |

### 8.2 Reason code table (REQ-111)

Codes are grouped by range so a client can categorize an unrecognized code.

| Code   | Name                  | Stage      | Fatal | Meaning                                                        |
|--------|-----------------------|------------|-------|----------------------------------------------------------------|
| `1001` | `VERSION_TOO_OLD`     | handshake  | yes   | Client's `max_version` < server minimum. Show "please update." |
| `1002` | `VERSION_TOO_NEW`     | handshake  | yes   | Client's `min_version` > server maximum. Operator must upgrade the daemon; do **not** prompt the user to update the app. |
| `1003` | `MALFORMED_FRAME`     | any        | yes   | Unparseable frame, or `version` not equal to the negotiated one. |
| `1004` | `FRAME_TOO_LARGE`     | any        | yes   | `length` implies a frame larger than `MAX_FRAME_SIZE`.         |
| `1005` | `UNEXPECTED_MSG_TYPE` | any        | yes   | A frame not valid in the current state (e.g. non-`HELLO` first frame, or a `SEND` before `AUTH_OK`). |
| `2001` | `AUTH_REQUIRED`       | post-hello | yes   | A messaging frame arrived before successful `AUTH`.            |
| `2002` | `AUTH_INVALID_TOKEN`  | auth       | yes   | JWT failed signature/audience/expiry validation (REQ-023).     |
| `2003` | `AUTH_RATE_LIMITED`   | auth       | yes   | Too many auth attempts for this tenant (REQ-191).              |
| `3001` | `BODY_TOO_LARGE`      | messaging  | no    | `SEND` body exceeded `MAX_BODY_SIZE`.                           |
| `3002` | `NOT_A_MEMBER`        | messaging  | no    | Sender is not a member of the target channel (REQ-031).        |
| `3003` | `UNKNOWN_CHANNEL`     | messaging  | no    | `channel_id` does not exist in this tenant.                    |
| `3004` | `SEND_RATE_LIMITED`   | messaging  | no    | Per-connection send rate exceeded (REQ-190).                   |
| `3005` | `FORBIDDEN`           | admin/msg  | no    | The actor may not perform the action — role (ARCH-60, §6) or not the message's author (§5.5/5.6). |
| `3006` | `LAST_OWNER`          | admin      | no    | Would remove or demote the tenant's last owner (REQ-030).      |
| `3007` | `UNKNOWN_MESSAGE`     | messaging  | no    | `EDIT`/`DELETE` names a message not in the channel, or already tombstoned (§5.5/5.6). |
| `3008` | `INVALID_CHANNEL`     | messaging  | no    | `CREATE_CHANNEL` name empty or over 64 bytes (§5.7). |
| `3009` | `INVALID_REACTION`    | messaging  | no    | `REACT` emoji empty or over 32 bytes (§5.9). |
| `9001` | `INTERNAL_ERROR`      | any        | maybe | Server-side failure; `fatal` indicates whether the connection survives. |

Handshake-stage version codes (`1001`/`1002`) are delivered via `REJECT`, which
carries the same `code`; the other codes are delivered via `ERROR`.

---

## 9. Message type registry

| msg_type | Name               | Direction | Frozen@v1 | Section |
|----------|--------------------|-----------|-----------|---------|
| `0x0001` | `HELLO`            | C → S     | yes       | §3.1    |
| `0x0002` | `WELCOME`          | S → C     | yes       | §3.2    |
| `0x0003` | `REJECT`           | S → C     | yes       | §3.3    |
| `0x0010` | `AUTH`             | C → S     | no        | §4.2    |
| `0x0011` | `AUTH_OK`          | S → C     | no        | §4.3    |
| `0x0012` | `AUTH_CHALLENGE`   | S → C     | no        | §4.1    |
| `0x0013` | `LOGOUT`           | C → S     | no        | §4.4    |
| `0x0040` | `LIST_USERS`       | C → S     | no        | §5.8    |
| `0x0041` | `USER_LIST`        | S → C     | no        | §5.8    |
| `0x0042` | `SET_ROLE`         | C → S     | no        | §5.8    |
| `0x0043` | `INVITE_USER`      | C → S     | no        | §5.8    |
| `0x0044` | `REMOVE_USER`      | C → S     | no        | §5.8    |
| `0x0045` | `USER_UPDATED`     | S → C     | no        | §5.8    |
| `0x0046` | `INVITE_CREATED`   | S → C     | no        | §5.8    |
| `0x0047` | `REDEEM_INVITE`    | C → S     | no        | §5.8    |
| `0x0048` | `UPDATE_PROFILE`   | C → S     | no        | (reserved — profile edit) |
| `0x0060` | `SEARCH`           | C → S     | no        | §5.11   |
| `0x0061` | `SEARCH_RESULTS`   | S → C     | no        | §5.11   |
| `0x0020` | `SEND`             | C → S     | no        | §5.1    |
| `0x0021` | `SEND_ACK`         | S → C     | no        | §5.2    |
| `0x0022` | `BROADCAST`        | S → C     | no        | §5.3    |
| `0x0023` | `CLIENT_ACK`       | C → S     | no        | §5.4    |
| `0x0033` | `READ_CURSOR`      | S → C     | no        | §5.4    |
| `0x0024` | `EDIT`             | C → S     | no        | §5.5    |
| `0x0025` | `DELETE`           | C → S     | no        | §5.6    |
| `0x0026` | `MSG_EDITED`       | S → C     | no        | §5.5    |
| `0x0027` | `MSG_DELETED`      | S → C     | no        | §5.6    |
| `0x0028` | `REACT`            | C → S     | no        | §5.9    |
| `0x0029` | `REACTION_UPDATED` | S → C     | no        | §5.9    |
| `0x002A` | `LIST_REACTIONS`   | C → S     | no        | §5.9    |
| `0x002B` | `REACTIONS`        | S → C     | no        | §5.9    |
| `0x002C` | `SEND_REPLY`       | C → S     | no        | §5.10   |
| `0x002D` | `THREAD_REPLY`     | S → C     | no        | §5.10   |
| `0x002E` | `LIST_THREAD`      | C → S     | no        | §5.10   |
| `0x002F` | `THREAD`           | S → C     | no        | §5.10   |
| `0x0032` | `THREAD_META`      | S → C     | no        | §5.10   |
| `0x0050` | `CREATE_CHANNEL`   | C → S     | no        | §5.7    |
| `0x0051` | `CHANNEL_INFO`     | S → C     | no        | §5.7    |
| `0x0052` | `LIST_CHANNELS`    | C → S     | no        | §5.7    |
| `0x0053` | `CHANNEL_LIST`     | S → C     | no        | §5.7    |
| `0x0054` | `JOIN_CHANNEL`     | C → S     | no        | §5.7    |
| `0x0055` | `LEAVE_CHANNEL`    | C → S     | no        | §5.7    |
| `0x0056` | `INVITE_TO_CHANNEL`| C → S     | no        | §5.7    |
| `0x0057` | `REMOVE_FROM_CHANNEL`| C → S   | no        | §5.7    |
| `0x0058` | `OPEN_DM`          | C → S     | no        | §5.12   |
| `0x0059` | `CREATE_WEBHOOK`   | C → S     | no        | §5.15   |
| `0x005A` | `WEBHOOK_INFO`     | S → C     | no        | §5.15   |
| `0x005B` | `LIST_WEBHOOKS`    | C → S     | no        | §5.15   |
| `0x005C` | `WEBHOOK_LIST`     | S → C     | no        | §5.15   |
| `0x005D` | `DELETE_WEBHOOK`   | C → S     | no        | §5.15   |
| `0x005E` | `WEBHOOK_DELETED`  | S → C     | no        | §5.15   |
| `0x0070` | `SET_PRESENCE`     | C → S     | no        | §5.13   |
| `0x0071` | `PRESENCE_UPDATE`  | S → C     | no        | §5.13   |
| `0x0072` | `TYPING`           | C → S     | no        | §5.13   |
| `0x0073` | `TYPING_UPDATE`    | S → C     | no        | §5.13   |
| `0x0090` | `SET_NOTIFY_PREF`  | C → S     | no        | §5.16   |
| `0x0091` | `SET_DND`          | C → S     | no        | §5.16   |
| `0x0092` | `LIST_NOTIFY_PREFS`| C → S     | no        | §5.16   |
| `0x0093` | `NOTIFY_PREFS`     | S → C     | no        | §5.16   |
| `0x0094` | `SET_CLIENT_SETTING`   | C → S | no        | §5.16a  |
| `0x0095` | `LIST_CLIENT_SETTINGS` | C → S | no        | §5.16a  |
| `0x0096` | `CLIENT_SETTINGS`      | S → C | no        | §5.16a  |
| `0x0097` | `STORAGE_STATUS_REQ` | C→S | Owner/admin: request the storage usage report (REQ-214). No body. |
| `0x0098` | `STORAGE_STATUS` | S→C | Usage, the active retention/eviction policy, and cumulative reclamation counts by reason. Fixed-width fields, so a later version may append without a version bump. |
| `0x0099` | `AUDIT_QUERY` | C→S | Owner/admin: page the audit log (REQ-251). `before_ms` pages backwards from a timestamp rather than by offset, so a boundary stays stable as entries arrive; 0 asks for the newest page. |
| `0x009A` | `AUDIT_PAGE` | S→C | A page of entries, newest first: time, family, action, actor, target, outcome, detail. Never carries the secret involved. |
| `0x0048` | `SET_DISPLAY_NAME` | C → S     | no        | §5.16b  |
| `0x0049` | `CHANGE_PASSWORD`  | C → S     | no        | §5.16b  |
| `0x004A` | `PROFILE_UPDATED`  | S → C     | no        | §5.16b  |
| `0x00A0` | `CALL_JOIN`        | C → S     | no        | §5.17   |
| `0x00A1` | `CALL_LEAVE`       | C → S     | no        | §5.17   |
| `0x00A2` | `CALL_JOINED`      | S → C     | no        | §5.17   |
| `0x00A3` | `CALL_ROSTER`      | S → C     | no        | §5.17   |
| `0x0080` | `UPLOAD_BEGIN`     | C → S     | no        | §5.14   |
| `0x0081` | `UPLOAD_READY`     | S → C     | no        | §5.14   |
| `0x0082` | `UPLOAD_CHUNK`     | C → S     | no        | §5.14   |
| `0x0083` | `UPLOAD_ACK`       | S → C     | no        | §5.14   |
| `0x0084` | `UPLOAD_END`       | C → S     | no        | §5.14   |
| `0x0085` | `UPLOAD_OK`        | S → C     | no        | §5.14   |
| `0x0086` | `DOWNLOAD_BEGIN`   | C → S     | no        | §5.14   |
| `0x0087` | `DOWNLOAD_INFO`    | S → C     | no        | §5.14   |
| `0x0088` | `DOWNLOAD_CHUNK`   | S → C     | no        | §5.14   |
| `0x0089` | `DOWNLOAD_END`     | S → C     | no        | §5.14   |
| `0x008A` | `TRANSFER_CANCEL`  | C → S     | no        | §5.14   |
| `0x0030` | `BACKFILL_REQUEST` | C → S     | no        | §6.1    |
| `0x0031` | `BACKFILL_DONE`    | S → C     | no        | §6.2    |
| `0x00FF` | `ERROR`            | S → C     | no        | §8.1    |

Ranges are left sparse (`0x0001–` handshake, `0x0010–` auth/session, `0x0020–`
messaging, `0x0030–` reconnect, `0x0040–` users/profiles, `0x0050–` channel
management, `0x0060–` search, `0x0070–` presence/typing, `0x0080–` attachment
transfer, `0x0090–` notification prefs, `0x00FF` error) so later revisions can
slot in audio signaling without renumbering. The `0x0040–0x0047`
management frames (user enumeration, roles, tenant invite/remove; §5.8) are
defined; `0x0048` `UPDATE_PROFILE` (avatar/display-name edit) remains **reserved**
until that milestone lands.

---

## 10. Connection state machine

```
             HELLO
   (new) ─────────────▶ AWAIT_WELCOME
                             │ WELCOME + AUTH_CHALLENGE
                             ▼
                        AWAIT_AUTH ──── AUTH ───▶ (verify per mode:
                             │                     local pw / oidc token /
                             │                     session token)
                             │                        │ ok
                             │ bad/none               ▼
                             ▼                    AUTHENTICATED ◀─┐
                     ERROR/REJECT(fatal)              │           │
                          close                       │  SEND / CLIENT_ACK /
                                                       │  BACKFILL_REQUEST
                                                       └───────────┘
                                                   (server may push
                                                    BROADCAST / SEND_ACK /
                                                    BACKFILL_DONE / ERROR)
```

- Any fatal `ERROR`/`REJECT` transitions to closed from any state.
- A frame invalid for the current state yields `ERROR UNEXPECTED_MSG_TYPE`
  (fatal).
- On an unexpected transport close, the client reconnects (REQ-100) and starts
  over from `HELLO`, following `AUTH_OK` with a `BACKFILL_REQUEST` (§6).

---

## 11. Requirements resolved by this document

| Requirement | Resolution                                                                 |
|-------------|----------------------------------------------------------------------------|
| REQ-091     | Client-side dedup on `message_id` per-channel high-water mark (§5.3, ARCH-45). |
| REQ-093     | 16-byte `idempotency_token` on `SEND`, persisted `(channel, token) → id` mapping (§5.1, ARCH-44). |
| REQ-101     | Per-channel cursor `BACKFILL_REQUEST` + `BACKFILL_DONE` (§6, ARCH-46).      |
| REQ-110     | First frame must be `HELLO`; version validated before any further parse (§3). |
| REQ-111     | `VERSION_TOO_OLD` / `VERSION_TOO_NEW` reason codes distinguish the remedy (§3.3, §8.2, ARCH-41). |
| REQ-120     | `SET_PRESENCE` / `PRESENCE_UPDATE` + auth-time online snapshot (§5.13, ARCH-67). |
| REQ-121     | Member-scoped `TYPING` / `TYPING_UPDATE`; ~6s client-side expiry, no stop frame (§5.13, ARCH-68). |
| REQ-140     | Chunked upload/download proxied through the daemon; blob in object storage, pointer in SQLite (§5.14, ARCH-69/70). |
| REQ-141     | Every byte proxied, so access control is the ordinary membership check on the attached message — no signed URLs (§5.14, ARCH-69). |
| REQ-170     | `CREATE_WEBHOOK`/`WEBHOOK_INFO` mint a hashed per-channel token; `POST /webhook/<token>` (ALPN-demuxed HTTP) posts as the creator (§5.15, ARCH-71). |
| REQ-130/131 | `SET_NOTIFY_PREF`/`SET_DND`/`LIST_NOTIFY_PREFS` → `NOTIFY_PREFS`; server-authoritative settings synced to all the user's devices (§5.16, ARCH-72). |

Related decisions newly recorded in ARCHITECTURE.md: ARCH-41 (handshake &
version negotiation), ARCH-42 (primitive field encodings), ARCH-43 (message id
allocation), ARCH-44 (idempotency mapping), ARCH-45 (client-side dedup),
ARCH-46 (reconnect backfill shape), ARCH-47 (error frame & code table).
