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

**Status.** Specification, not yet implemented. `src/main.c` today is the
placeholder daemon (heartbeat + `/healthz`); none of the frames below exist in
code yet.

---

## 1. Transport

- The protocol runs over a single TLS/TCP connection per client (ARCH-6,
  ARCH-10, REQ-180). There is no unencrypted fallback.
- TLS trust is TOFU pinning against the daemon's self-signed certificate
  (ARCH-10, REQ-183). TLS is out of scope for this document; everything below
  describes the plaintext *inside* the TLS session.
- The connection is bidirectional and full-duplex. After the handshake and
  auth complete, either side may send an applicable frame at any time (e.g.
  the server may push a `BROADCAST` while the client is composing a `SEND`).
- One TCP connection carries exactly one authenticated session. There is no
  multiplexing of multiple users over one connection.

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

After `WELCOME`, the server expects `AUTH` (§4) as the next client frame.

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

## 4. Authentication (REQ-020, REQ-023; ARCH-19)

After `WELCOME`, the client MUST authenticate before sending any messaging
frame. A messaging frame received before `AUTH_OK` is answered with `ERROR
AUTH_REQUIRED` (fatal).

### 4.1 `AUTH` (client → server), msg_type `0x0010`

| Field | Type | Notes                                                         |
|-------|------|---------------------------------------------------------------|
| `jwt` | str  | The OIDC ID/access token obtained via the browser flow (ARCH-19). Bounded by `MAX_BODY_SIZE`. |

The daemon validates the JWT against the issuing provider's published JWKS —
signature, audience, and expiry (REQ-023) — and rejects on any mismatch with
`ERROR AUTH_INVALID_TOKEN` (fatal). Repeated failures are rate-limited per
tenant (REQ-191) with `ERROR AUTH_RATE_LIMITED` (fatal). Auth token *contents*
and JWKS caching are out of scope for this document.

### 4.2 `AUTH_OK` (server → client), msg_type `0x0011`

| Field           | Type | Notes                                                          |
|-----------------|------|----------------------------------------------------------------|
| `user_id`       | u64  | The authenticated user's stable tenant-local id.               |
| `session_expiry`| u64  | Ms since epoch UTC after which this session is invalid; equals the JWT's expiry (REQ-181). The daemon does not extend a session past this. |

A freshly-authenticated client typically follows `AUTH_OK` with a
`BACKFILL_REQUEST` (§6) to catch up on anything missed while disconnected.

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
| `0x0010` | `AUTH`             | C → S     | no        | §4.1    |
| `0x0011` | `AUTH_OK`          | S → C     | no        | §4.2    |
| `0x0020` | `SEND`             | C → S     | no        | §5.1    |
| `0x0021` | `SEND_ACK`         | S → C     | no        | §5.2    |
| `0x0022` | `BROADCAST`        | S → C     | no        | §5.3    |
| `0x0023` | `CLIENT_ACK`       | C → S     | no        | §5.4    |
| `0x0030` | `BACKFILL_REQUEST` | C → S     | no        | §6.1    |
| `0x0031` | `BACKFILL_DONE`    | S → C     | no        | §6.2    |
| `0x00FF` | `ERROR`            | S → C     | no        | §8.1    |

Ranges are left sparse (`0x0001–` handshake, `0x0010–` auth, `0x0020–`
messaging, `0x0030–` reconnect, `0x00FF` error) so later revisions can slot in
presence, typing, reactions, threads, and audio signaling without renumbering.

---

## 10. Connection state machine

```
             HELLO
   (new) ─────────────▶ AWAIT_WELCOME
                             │ WELCOME
                             ▼
                        AWAIT_AUTH ──── AUTH ───▶ (validate JWT)
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
