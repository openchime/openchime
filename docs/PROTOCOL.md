# OpenChime Wire Protocol — v1 (core messaging path)

This document specifies the OpenChime binary wire protocol at the byte level.
It is the detailed realization of the frame decisions in
[ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-6 through ARCH-9, ARCH-30) and
resolves the protocol-shaped `[needs ARCH decision]` items in
[REQUIREMENTS.md](./REQUIREMENTS.md).

**Scope of this revision.** This covers the *core messaging path* only:
connection handshake and version negotiation, authentication, the message
send/broadcast/ack cycle, reconnect backfill, and the error frame. Presence
(REQ-120/121), typing indicators, reactions (REQ-070/071), threads
(REQ-060/061), notification configuration, and audio-call signaling
(REQ-150–152) are deliberately out of scope here and will be added in later
revisions of this document, reusing the framing and encoding rules defined
below. New message types are additive; the header format and the frozen
handshake frames (§3) do not change.

**Status.** Specification, not yet implemented. `daemon/main.c` today is the
placeholder daemon (heartbeat + `/healthz`); none of the frames below exist in
code yet.

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
- Attachments are never carried in a frame; they go through object storage
  (ARCH-17, REQ-140). Nothing in this protocol transports file bytes.

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
| `oidc_params` | str  | Empty unless `oidc` is offered; otherwise a small opaque blob the client passes to its OIDC helper (central authorize URL/`client_id`, this instance's `audience`). Ignorable by clients that only reconnect. |

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

## 5. Messaging (REQ-050, REQ-090, REQ-092, REQ-093)

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
| `3005` | `FORBIDDEN`           | admin      | no    | The actor's role may not perform the action (ARCH-60, §6).     |
| `3006` | `LAST_OWNER`          | admin      | no    | Would remove or demote the tenant's last owner (REQ-030).      |
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
| `0x0040` | `LIST_USERS`       | C → S     | no        | (reserved — user enumeration) |
| `0x0041` | `USER_LIST`        | S → C     | no        | (reserved) |
| `0x0042` | `UPDATE_PROFILE`   | C → S     | no        | (reserved — profile edit) |
| `0x0043` | `INVITE_USER`      | C → S     | no        | (reserved — REQ-033) |
| `0x0044` | `REMOVE_USER`      | C → S     | no        | (reserved — REQ-033) |
| `0x0020` | `SEND`             | C → S     | no        | §5.1    |
| `0x0021` | `SEND_ACK`         | S → C     | no        | §5.2    |
| `0x0022` | `BROADCAST`        | S → C     | no        | §5.3    |
| `0x0023` | `CLIENT_ACK`       | C → S     | no        | §5.4    |
| `0x0030` | `BACKFILL_REQUEST` | C → S     | no        | §6.1    |
| `0x0031` | `BACKFILL_DONE`    | S → C     | no        | §6.2    |
| `0x00FF` | `ERROR`            | S → C     | no        | §8.1    |

Ranges are left sparse (`0x0001–` handshake, `0x0010–` auth/session, `0x0020–`
messaging, `0x0030–` reconnect, `0x0040–` users/profiles, `0x00FF` error) so
later revisions can slot in presence, typing, reactions, threads, and audio
signaling without renumbering. The `0x0040–0x0044` entries are **reserved** —
their frame layouts are defined when the corresponding milestone (user
enumeration / profiles / management, AUTH.md §6) lands.

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

Related decisions newly recorded in ARCHITECTURE.md: ARCH-41 (handshake &
version negotiation), ARCH-42 (primitive field encodings), ARCH-43 (message id
allocation), ARCH-44 (idempotency mapping), ARCH-45 (client-side dedup),
ARCH-46 (reconnect backfill shape), ARCH-47 (error frame & code table).
