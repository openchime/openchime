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
and reconnect backfill, plus the error frame. It **also now covers** presence and
typing (§5.13, REQ-120/121), attachments (§5.14), incoming webhooks (§5.15),
notification preferences (§5.16), synced client settings (§5.16a), self-service
profile (§5.16b), audio-call signaling (§5.17, REQ-150–152), and push device
tokens.

**§9 is the complete list; §5's prose is not.** The registry is generated from
`shared/protocol.h` and carries all 154 message types. The narrative sections
below specify the payload layouts for the families they name, and a number of
later families — drafts, scheduled send, the notification schedule and pause,
keywords, custom emoji, group DMs, invite and session management, webhook
enable/rotate, cross-channel threads, unresolved-mention notices, and the
per-channel file census — are listed in §9 with their opcodes and directions but
are specified in §§5.16d–5.16i. The remaining protocol-level deferral is the
**screenshare** addition reserved in §5.17 (REQ-161, unbuilt).

**Status.** Implemented. The frames in this document are realized in
`shared/protocol.c` (codec), `daemon/dbwriter.c` (handlers), and
`daemon/netloop.c` (dispatch). Coverage is uneven: the auth-and-message vertical
is exercised end to end against the deployed container, and the rest in-process
(TESTING.md §3.3). Where this document and the codec disagree, the codec is
right — three such disagreements were found on 2026-08-02 and are recorded in
[BACKLOG.md](./BACKLOG.md) rather than silently corrected here.

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
- The daemon advertises **`oc/1` then `http/1.1`** (`OC_ALPN_HTTP11`), and
  selection follows the *server's* order, so a peer offering both still gets the
  binary protocol.
- A connection that negotiates `http/1.1`, or offers no ALPN at all, is routed to
  the HTTP handler instead. Third-party webhook clients are ordinary HTTPS and
  never offer `oc/1`, so they land on the HTTP side automatically.
- A client offering neither — `h2` alone, say — is refused during the handshake
  with `no_application_protocol`, because the daemon speaks no protocol it asked
  for.

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

Both sides exist. A connection that negotiates `oc/1` reaches the binary
protocol; one that does not is read by the HTTP/1.1 handler, which serves
`POST /webhook/<token>` (§5.15) and nothing else on this port — `/healthz` and
the landing page are on the separate plaintext health port (ARCH-25). The
CA-signed certificate for the webhook endpoint (ARCH-34, REQ-171) is the part
that remains unbuilt; the endpoint currently answers on the daemon's TOFU cert.

The `oc` version suffix (`/1`) tracks the transport-framing generation, distinct from the
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
  self-describing. A session negotiates a single version at handshake (§3), both
  sides store it on the connection, and **both sides check it on every
  post-handshake frame before dispatch**. A frame stamped with anything else is
  refused rather than decoded: the field names the layout the payload is in, so a
  frame that disagrees has no correct reading regardless of what it would parse
  as. The daemon answers a fatal `ERROR VERSION_MISMATCH` (`1006`) — itself
  stamped with the *negotiated* version, so the peer being hung up on can read
  it — and closes; the client drops the connection and reports it. The handshake
  frames are exempt, being frozen at 1 (§3).
- The `HELLO`, `WELCOME` and `REJECT` handshake frames are **frozen at version 1**
  (`OC_HANDSHAKE_VERSION`), so negotiation itself can never hit a version
  mismatch: the frames that exist to discover a mismatch must not be able to be
  one. `REJECT` is the case that decides it — it is sent *to* a peer whose
  version has just been refused, so it has to be decodable by a peer that agrees
  about nothing else. Their layout is frozen with the number; a new handshake
  field means a new frame, not a new version of these.

> **Bump `OC_PROTOCOL_VERSION` whenever a frame's LAYOUT changes** — a new field,
> a reordering, a field that stops being optional — and not only when a frame is
> *added*. Adding a frame is safe: a peer that does not know it never sends it and
> never expects it. Changing a layout is not, because both sides still claim the
> same version number while disagreeing about what it means, and the failure
> surfaces as a decode error mid-session rather than a clean rejection.
>
> **Reassigning an opcode is the same kind of change** and gets the same bump,
> even though no payload moved: v8 moved `TYPING`/`TYPING_UPDATE` to
> `0x007E`/`0x007F`, and to a v7 peer a v8 `TYPING` frame *is* a `PROFILE_INFO` —
> decoded as the wrong struct rather than rejected, which is precisely what the
> version number exists to prevent.
>
> This is written down because it was learned the hard way: `CHANNEL_INFO` and
> `CHANNEL_LIST` both grew fields while the version stayed at 1, and a client
> built from the new source talking to a daemon still running the old binary
> connected happily and then dropped the link, reporting only "connection lost —
> reconnecting". Both sides send the version as `min` **and** `max`, so a bump
> turns exactly that situation into a `REJECT` carrying `VERSION_TOO_OLD` /
> `VERSION_TOO_NEW`, which says what is wrong.
>
> **Current version: 7.** Since the client and daemon ship together (ARCH-61)
> there is no compatibility window to preserve — only a mismatch to detect
> loudly, which is why a frame *layout* change moves the number even when it adds
> one byte.
>
> | Version | Change |
> |---|---|
> | 2 | `CHANNEL_INFO` gained `topic`/`archived` and made `peer_id` unconditional; `CHANNEL_LIST` gained `topic`, `archived`, `created_at`, `preview`, `preview_author` (2026-07-29). |
> | 3 | Mute and mark-unread (REQ-137/235, migration 0026). |
> | 4 | `USER_LIST` carries each user's avatar attachment id. A repeated list, so the added field shifts every entry after the first. |
> | 5 | `PRESENCE_UPDATE` carries the do-not-disturb **fact** beside the status byte (REQ-122/278). |
> | 6 | `NOTIFY_PREFS` **drops** the three DND-window fields: the recurring schedule is its own frame (REQ-136) and `SET_DND` is retired with them. |
> | 7 | `USER_LIST` carries `title`, `timezone` and custom status (REQ-289). |
>
> Versions 4–7 are recorded in `shared/protocol.h`; version 3's entry is not, and
> is reconstructed here from the commit that raised it.

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
first frame **closes the connection with no frame written** — the specification
reserves `REJECT`/`UNEXPECTED_MSG_TYPE` for it, and the daemon does not send it
(see BACKLOG.md). The
connection is closed.

The `HELLO`/`WELCOME`/`REJECT` frames are **frozen at `version = 1`**
(`OC_HANDSHAKE_VERSION`, stamped by the three encoders that take no version
argument): their layout is guaranteed never to change, so a client of any era can
always speak the handshake and receive an intelligible answer.

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

### 4.3a `WORKSPACE_INFO` (server → client), msg_type `0x0014`

Pushed immediately after `AUTH_OK` (before the presence snapshot). Carries the
daemon's static workspace facts, sourced from infra config (env, read once at
boot — see `daemon/config.h`), so a client can render a branded header instead of
a bare host address. Clients that don't recognize the frame ignore it.

| Field             | Type | Notes                                                       |
|-------------------|------|-------------------------------------------------------------|
| `deployment_mode` | u8   | `0` standalone, `1` federated, `2` managed (ARCH-76).       |
| `max_users`       | u32  | Registered-user cap; `0` = unlimited.                       |
| `workspace_name`  | str  | Admin-set display name; **empty** ⇒ the client falls back to the connection host's subdomain (e.g. `acme.openchime.io` → "acme"). |

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
| `peer_id`    | u64  | Always written since protocol 2 (`0` when not a DM). For a 1:1 DM, the other participant *from the recipient's view*. |
| `topic`      | str  | Empty when unset (REQ-034). |
| `archived`   | u8   | `1` when the channel is archived and read-only (REQ-035). |
| `n_peers`    | u16  | Participant count, then that many `u64` ids — a group DM's people (REQ-056). `0` for a named channel. |

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
| `entries[]` | `count` × entry | See below. |

Each entry, in wire order:

`channel_id` (u64) · `name` (str) · `is_public` (u8) · `joined` (u8) ·
`kind` (u8 — `0` channel, `1` DM) · `last_message_at` (u64) · `unread` (u32) ·
`peer_id` (u64) · `topic` (str) · `archived` (u8) · `created_at` (u64) ·
`preview` (str) · `preview_author` (u64) · `n_peers` (u16) followed by `n_peers` ×
`u64` participant ids.

Nothing here is optional: every field is written for every entry, so a decoder
that stops early desynchronises the whole repeated list rather than losing one
row. The trailing participant list carries a group DM's people (REQ-056) and is
`0`-length for anything else.

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

### 5.7a Changing a channel: topic, rename, archive (REQ-034/035/036, ARCH-93)

**`UPDATE_CHANNEL` (client → server), msg_type `0x005F`:**

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `channel_id` | u64  | The channel to change.                                   |
| `op`         | u8   | `0` set-topic · `1` rename · `2` archive · `3` unarchive. |
| `value`      | str  | The new topic (≤250 bytes, empty clears it) or name (≤64). Ignored for archive/unarchive. |

One frame for four verbs because they all mutate one row and all fan out the same
state — the op *is* the difference.

**Authority** splits on blast radius: **any member may set the topic** (already
visible to the channel, corrected in seconds); **owner/admin only** for rename,
archive and unarchive, which change what people *not* looking at the channel see.

On success the daemon replies **`CHANNEL_INFO`** to the actor and **fans the same
frame to every other member** — a rename or archive changes what everyone's
sidebar should say. `CHANNEL_INFO` therefore carries `topic` and `archived`:

`CHANNEL_LIST` entries also carry a **`preview`** (the newest top-level message,
truncated to 120 bytes, tombstones skipped) and its **`preview_author`**. A client
that caches nothing (ARCH-88) has no other way to show a conversation list you can
skim — and it is per *channel*, not per DM, so the ordinary sidebar can use it too.

> **Layout note.** `CHANNEL_INFO`'s `peer_id` used to be an *optional trailing*
> field, written only for DMs. That trick does not survive a second optional
> field, so as of this change the layout is **fixed**: `peer_id` is always
> written (0 when not a DM), followed by `topic` and `archived`. `CHANNEL_LIST`
> entries gained `topic`, `archived` and `created_at` for the same reason — a
> client that caches nothing (ARCH-88) must be able to render the sidebar and the
> channel's About surface from the list alone.

**An archived channel is read-only** on the client-facing wire: `SEND`,
`SEND_REPLY` and `UPLOAD_BEGIN` share one access check and all return
`CHANNEL_ARCHIVED` (3019). **The incoming-webhook post path does not call it** —
it resolves the token and inserts the message, so a third party can still post
into an archived channel and archiving does not disable a channel's webhooks.
That is a defect, not a design; it is in [BACKLOG.md](./BACKLOG.md). It is hidden from `CHANNEL_LIST` for
non-members; members keep it, flagged, so they can find their way back in.
History, search and membership are untouched — archiving is the reversible
alternative to a deletion that is not offered for channels holding history.

Failures are non-fatal `ERROR` frames: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER`,
`FORBIDDEN` (rename/archive by a non-admin), `CHANNEL_EXISTS` (the new name is
taken — the same unique-name rule as create, §5.7), or `INVALID_CHANNEL` (empty
or oversized name, oversized topic, or **any of these attempted on a DM** — a DM
has no name to rename and archiving one is a different feature).

### 5.8 Tenant administration (REQ-030, REQ-033)

Managing the tenant's people: enumerating them, changing roles, and adding or
removing members. Role changes and member add/remove are gated by the tenant
role policy (owner/admin/member, ARCH-60, AUTH.md §6); enumeration is open to any
authenticated user.

**`LIST_USERS` (client → server), msg_type `0x0040`** — empty payload; replies
**`USER_LIST` (server → client), msg_type `0x0041`** — as of protocol 7 each entry
also carries `title`, `timezone`, `status_emoji` and `status_text` (REQ-289).
They existed on `PROFILE_INFO` alone, which is sent only to the user who edited
them, so no client ever learned anyone else's; the people directory needs them,
and so did the profile card that had to say the fields were not built.

**`USER_LIST` (server → client), msg_type `0x0041`**:

| Field       | Type            | Notes                                                        |
|-------------|-----------------|--------------------------------------------------------------|
| `count`     | u16             | Number of entries.                                           |
| `entries[]` | `count` × entry | See below. |

Each entry, in wire order:

`user_id` (u64) · `role` (u8) · `disabled` (u8) · `email` (str) ·
`display_name` (str) · `avatar_id` (u64) · `title` (str) · `timezone` (str) ·
`status_emoji` (str) · `status_text` (str).

`avatar_id` arrived at protocol 4 and the last four at protocol 7. Because this
is a repeated list, an added field shifts **every** entry after the first — which
is why each of those raised the protocol version rather than riding along.
`title` and `timezone` are here, and not only on `PROFILE_INFO`, because that
frame is sent to the person who edited them: before REQ-289 no client ever
learned anybody else's.

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

### 5.9a Pins (REQ-230, ARCH-90)

A pin belongs to the **channel**, not to the person who placed it: every member
sees the same set. Any member may pin, and any member may unpin — including
someone else's pin. Pinning an already-pinned message is a no-op, so a pin is
never stacked. A channel holds at most **100** pins.

**`PIN` (client → server), msg_type `0x0035`:**

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `channel_id` | u64  | The message's channel (for the membership check).        |
| `message_id` | u64  | The message being pinned.                                |
| `op`         | u8   | `1` pin, `0` unpin.                                      |

On success the daemon fans a **`PIN_UPDATED` (server → client), msg_type
`0x0036`** out to every connected member:

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `message_id` | u64  | The pinned message.                                      |
| `channel_id` | u64  | Its channel.                                             |
| `user_id`    | u64  | Who placed the pin. On a repeat pin this is the **original** pinner, not the repeater, so every client agrees with a later `PINS`. |
| `op`         | u8   | `1` pinned, `0` unpinned.                                |
| `pinned_at`  | u64  | When it was pinned (ms since epoch).                     |

**`LIST_PINS` (client → server), msg_type `0x0037`** `{ channel_id: u64 }`. The
daemon streams one **`PINNED_MSG` (server → client), msg_type `0x0038`** per
pin, newest pin first, then a terminator. This is the `LIST_THREAD` shape (§5.10)
rather than the `REACTIONS` one: each pinned message carries its **body**, which
needs its own frame, and a pin is usually far outside the client's loaded
history — a list of bare ids would turn opening it into a fetch storm.

| Field         | Type | Notes                                                   |
|---------------|------|---------------------------------------------------------|
| `message_id`  | u64  | The pinned message.                                     |
| `channel_id`  | u64  | Its channel.                                            |
| `author_id`   | u64  | Who wrote it.                                           |
| `server_time` | u64  | When it was sent.                                       |
| `pinned_by`   | u64  | Who pinned it (`0` if that account is gone).            |
| `pinned_at`   | u64  | When it was pinned.                                     |
| `body`        | str  | The message body.                                       |
| `attach_name` | str  | The first attachment's filename, or empty. An attachment-only message has no body at all, so without this a pinned file rendered as a blank row. |

**`PINS` (server → client), msg_type `0x0039`** `{ channel_id: u64, count: u32 }`
terminates the response; `count` is how many `PINNED_MSG` frames preceded it.

Tombstoned messages are excluded from the list and lose their pin outright
(§5.6): there is nothing left to pin to.

**Pin state is replayed on backfill.** A `BROADCAST` has no field for it, so
after replaying a channel's messages the daemon emits a `PIN_UPDATED` for each
pinned one (§6). Without this every pin silently vanished the moment a client
reconnected — the same failure the reaction replay exists to prevent.

Failures are non-fatal `ERROR` frames carrying the `message_id` (8 bytes,
big-endian) in `context`: `UNKNOWN_MESSAGE` (no such message in that channel, or
it is tombstoned), `NOT_A_MEMBER`, or `TOO_MANY_PINS` (3018 — the channel
already holds 100 pins). Unpinning something that is not pinned is **not** an
error: two clients racing the same unpin must not produce a spurious failure.

### 5.9b Channel details: members and files (REQ-031, REQ-143, ARCH-91)

Both follow the `LIST_PINS` shape — stream the entries, then a terminator.

**`LIST_MEMBERS` (client → server), msg_type `0x003A`** `{ channel_id: u64 }`
streams **`MEMBER_ENTRY` (`0x003B`)** `{ channel_id, user_id, role: u8,
joined_at: u64 }` in join order, then **`MEMBERS` (`0x003C`)**
`{ channel_id, count: u32 }`. Capped at 500.

Membership has been stored since migration 0001, but nothing on the wire listed
it, so a client showing "members" beside a channel name could only show the
**tenant** roster — right only while the workspace has one channel everyone is
in. Enumerating a channel **requires being a member of it**; otherwise this is a
way to discover who is in a private channel you were never invited to
(`NOT_A_MEMBER`).

**`LIST_FILES` (client → server), msg_type `0x003D`** `{ channel_id: u64 }`
streams **`FILE_ENTRY` (`0x003E`)** newest-first, then **`FILES` (`0x003F`)**
`{ channel_id, count: u32 }`. Capped at 200.

| Field           | Type | Notes                                                 |
|-----------------|------|-------------------------------------------------------|
| `attachment_id` | u64  | Download it with the usual attachment path (§5.14).   |
| `channel_id`    | u64  | The file's own channel — which differs per entry in the workspace-wide form. |
| `message_id`    | u64  | The message it was shared with.                       |
| `uploader_id`   | u64  | Who shared it.                                        |
| `size`          | u64  | Bytes.                                                |
| `created_at`    | u64  | When it was shared (ms).                              |
| `reclaimed`     | u8   | `1` when the bytes are gone (REQ-215/217); the row is kept so the loss is visible, but there is nothing to download. |
| `filename`      | str  | As uploaded.                                          |
| `mime`          | str  | Declared type; **type filtering is client-side over this**. |

**`channel_id` 0 means "every channel I can read"** — the same query with a
membership filter instead of a channel filter, which is what makes a
workspace-wide files view free rather than a second op.

Pending uploads (`message_id` NULL, §5.14) are **excluded**: an upload that never
reached a message was never shared with anyone.

Both lists are refreshed on open and never cached — a client holds nothing on
disk (ARCH-88), and both change from other clients.

### 5.9c Saved items and the activity feed (REQ-231, REQ-139, ARCH-95)

Both are **per-user** surfaces, and neither fans out to anyone else.

**`SAVE_ITEM` (client → server), msg_type `0x0062`** `{ message_id: u64, op: u8 }`
(`1` save, `0` unsave). You may save anything you can read; a tombstone cannot be
saved. Replies **`SAVED_UPDATED` (`0x0063`)** `{ message_id, op, saved_at }` **to
the actor only** — a personal bookmark has no one to announce it to. Saving twice
is a no-op and reports the **original** `saved_at`, so a list ordered by when you
saved does not reshuffle when you click again.

**`LIST_SAVED` (`0x0064`)** (no body) streams **`SAVED_MSG` (`0x0065`)** newest
save first, then **`SAVED` (`0x0066`)** `{ count: u32 }`. Each entry carries the
message body and its first attachment's name, like a pin — a saved message is
usually far outside loaded history. Entries whose channel you have since left are
omitted: leaving a channel must stop it leaking through your saved list.

### Drafts (REQ-223, ARCH-101)

**`SET_DRAFT` (client → server), msg_type `0x00C0`**
`{ channel_id: u64, thread_root: u64, body: str }`. Upserts the draft for that
conversation; **an empty body deletes it**, which is the same frame rather than a
second op. Requires membership of the channel — a draft is user content *about* a
conversation, and storing one for a channel you cannot see would report its
existence back to you on every other device. Bodies over `OC_DRAFT_BODY_MAX`
(16 KB) are **truncated, not refused**: no composer can type one that long, so a
frame that size is a bug or a probe, and losing its tail beats losing the draft.

**`DRAFT` (`0x00C2`)** `{ channel_id, thread_root, updated_ms, body }` is sent to
the writer's **other** connections as a device sync — never back to the
connection that wrote it, which already has the text and would otherwise be at
risk of overwriting a composer still being typed in. A delete arrives as the same
frame with an empty body.

**`LIST_DRAFTS` (`0x00C1`)** (no body) streams **`DRAFT` (`0x00C2`)** newest
first, then **`DRAFTS` (`0x00C3`)** `{ count: u16 }`, capped at 256. Entries for
channels you have since left are omitted but **not deleted**: leaving is
reversible, so the draft waits for you rather than being discarded.

*One frame for two jobs.* `DRAFT` is both the streamed list entry and the sync
push, exactly as `CLIENT_SETTINGS` doubles as snapshot and push — the client
folds it the same way whatever prompted it, instead of two frames that can drift.

**`LIST_ACTIVITY` (`0x0067`)** `{ filter: u8 }` streams **`ACTIVITY_ENTRY`
(`0x0068`)** newest first, then **`ACTIVITY` (`0x0069`)** `{ count: u32,
seen_at: u64 }`. `filter` is `0` involved-me (the original question) · `1` unread
· `2` unread DMs · `3` unread channels I am notified about. An **empty body** is
read as `0`, so a client built before the filter existed keeps working — the
frame it sends is exactly the frame it always sent.

| Field        | Type | Notes                                                     |
|--------------|------|------------------------------------------------------------|
| `kind`       | u8   | `0` mention · `1` reaction to your message · `2` reply under your thread · `3` unread (only in an unread answer) |
| `message_id` | u64  | What to jump to.                                           |
| `channel_id` | u64  | Where it happened.                                         |
| `actor_id`   | u64  | Who did it — never you: a feed of your own doings is noise. |
| `at`         | u64  | When.                                                      |
| `text`       | str  | The message body for a mention or reply; the **emoji** for a reaction. |

The feed is a **union of three queries** over existing rows, not a maintained
list (ARCH-95). `seen_at` is the watermark **as it was before this call** — the
server stamps the current time as part of answering, so a client compares each
`at` against it to mark what is new. That is deliberately coarser than per-item
read state, which would require the table ARCH-95 argues against.

### 5.9d Fetch-around, for permalinks (REQ-232, ARCH-96)

**`HISTORY_AROUND` (client → server), msg_type `0x006A`**
`{ channel_id: u64, message_id: u64, limit: u16 }` — the messages *surrounding*
an id, `limit/2` either side, so a jump target lands mid-screen with context
rather than pinned to an edge.

It replies as an ordinary **backfill replay** (§6): same rows, same ascending
order, same attachments and reply counts, so the client's high-water dedup
(ARCH-45) and every downstream fold work unchanged — only the `WHERE` differs
from the backwards paging of §6.3. Read access is checked exactly as it is there:
a permalink is not a way around membership.

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
| `truncated` | u8   | 1 if the reply stream was capped (more replies exist than were streamed). |

**`LIST_THREADS` (client → server), msg_type `0x00D1`** `{ filter: u8 }` — every
thread the caller is in, across every channel (REQ-062). `0` all, `1` unread only;
an empty body reads as `0`. The daemon streams **`THREAD_SUMMARY` (`0x00D2`)**,
newest activity first, and closes with **`THREADS` (`0x00D3`)** `{ count: u32 }`.

| Field           | Type | Notes                                                  |
|-----------------|------|--------------------------------------------------------|
| `root_id`       | u64  | The thread's root message.                             |
| `channel_id`    | u64  | Where it lives — the reason this list is worth having. |
| `root_author`   | u64  | Who started it.                                        |
| `root_at`       | u64  | When it started.                                       |
| `last_reply_at` | u64  | When it last moved; the sort key.                      |
| `reply_count`   | u32  | Replies, excluding the root.                           |
| `unread`        | u32  | Replies by others past my per-thread cursor.           |
| `following`     | u8   | Resolved, not stored: participation counts (ARCH-104). |
| `preview`       | str  | The root's first bytes.                                |

**`SET_THREAD_FOLLOW` (client → server), `0x00D4`** `{ root_id: u64,
channel_id: u64, on: u8 }` and **`MARK_THREAD_READ` (client → server), `0x00D5`**
`{ root_id: u64, up_to: u64 }` (`0` = every reply in it, which is what opening one
means). Both answer with a single `THREAD_SUMMARY` — the row that changed, fanned
to all of the user's connections — rather than a fresh list: a client folds a
summary the same way whichever prompted it, and re-listing after every read mark
would make opening a thread cost the whole view.

*Why a second thread family.* The ops above are each scoped to ONE parent, which
is the right shape for reading a thread and useless for finding one. Nothing in
the product could answer "what have I missed, everywhere" without walking every
channel.

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

| Field         | Type | Notes                                                    |
|---------------|------|----------------------------------------------------------|
| `query`       | str  | Free text. The daemon quotes each whitespace-separated term, so query punctuation is literal and multiple terms are ANDed; it never errors on syntax. May be empty when the filters below carry the whole query. |
| `limit`       | u16  | Max results wanted; the daemon caps it (currently 50).   |
| `before_id`   | u64  | **Keyset paging cursor** — return only messages with a lower id. `0` for the first page. Not an offset, so a message posted mid-paging cannot make a row repeat or vanish. |
| `from_name`   | str  | `from:` — restrict to one author. Empty for no constraint. |
| `in_channel`  | str  | `in:` — restrict to one channel. Empty for no constraint. |
| `has_mask`    | u8   | `has:` — bitmask, `0x01` file, `0x02` link, `0x04` image. `0` for no constraint. |
| `after_ms`    | u64  | `after:` — lower time bound, ms since epoch UTC. `0` for none. |
| `before_ms`   | u64  | `before:` — upper time bound. `0` for none. |

The operator grammar is parsed by `shared/searchq.c`, which the daemon and both
frontends link, so the filter line a client shows and the `WHERE` clause the
daemon builds cannot disagree (REQ-081). Results stay scoped to history the
caller may read (REQ-031).

Replies **`SEARCH_RESULTS` (server → client), msg_type `0x0061`**, newest match
first:

| Field       | Type            | Notes                                                |
|-------------|-----------------|------------------------------------------------------|
| `count`     | u16             | Number of entries.                                   |
| `entries[]` | `count` × entry | Each: `message_id` (u64), `channel_id` (u64), `author_id` (u64), `server_time` (u64), `snippet` (str — an FTS5 excerpt around the match). |
| `truncated` | u8              | 1 if more matches exist past the cap (`limit`).      |

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
status: u8, dnd: u8 }` — sent tenant-wide to every *other* authenticated
connection when a user's aggregate status changes (a client tracks its own presence locally, so it
is excluded from its own broadcast). On authenticating, a client also receives a
**one-shot snapshot** — one `PRESENCE_UPDATE` per currently-online user — so it
starts with an accurate roster without polling.

`dnd` is the do-not-disturb **fact** (REQ-122/278) — that the user is not to be
disturbed, never when they will be back. It is a second axis beside presence, not
a status value: somebody can be online and paused, and collapsing the two would
make "away" and "do not disturb" indistinguishable. The net thread holds each
connected user's pause instant in memory (seeded at `AUTH_OK`, ARCH-66/67 gives it
no database of its own) and compares it per frame, so an expiry needs no sweep —
but a pause that runs out is re-announced on the next tick, since only a frame can
untell the people who were told.

**`TYPING` (client → server), msg_type `0x007E`** `{ channel_id: u64 }` — the
caller signals they are composing in `channel_id`. The server resolves the
channel's members (on the read connection, ARCH-66) and, if the caller has access,
relays to the other connected members only — private-channel and DM typing never
leaks to non-members.

**`TYPING_UPDATE` (server → client), msg_type `0x007F`** `{ channel_id: u64,
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
   storage. The frame carries an idempotency token, but **the daemon does not use
   it**: the insert is unconditional and `attachments` has no token column, so a
   retried `UPLOAD_BEGIN` allocates a second attachment rather than resuming the
   first. On failure → `ERROR` (`FORBIDDEN`, `ATTACHMENT_TOO_LARGE`, …).
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
a trailing attachment list of `{ attachment_id, filename, mime, size, reclaimed }`
— the `reclaimed` field is a `u8`, 1 when the blob has been reclaimed by age or
storage pressure so the row is a tombstone (REQ-215) and no download id is offered
— so every
reader — live or via backfill (§6) — sees the attachment through the one message
model, with no attachment-specific delivery path. Thread replies work the same
way: `SEND_REPLY` carries the id list and `THREAD_REPLY` the metadata, live and
via `LIST_THREAD` (§5.10). A message body may
be empty when it carries attachments.

**Download (server → client bytes):**

1. **`DOWNLOAD_BEGIN` (C → S), `0x0086`** `{ attachment_id: u64 }` — the server
   authorizes the requester against the message the attachment belongs to
   (`channel_read_access`, REQ-141); an unauthorized or unknown id → `ERROR`
   (`FORBIDDEN`; an unknown or unfinalized id is `UNKNOWN_ATTACHMENT` 3011 and a
   reclaimed one `ATTACHMENT_GONE` 3013 — there is no `NOT_FOUND` code). It opens
   a streaming read from storage on a
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
Responses: `200 {"ok":true,"message_id":N}` on success; `400` (empty or bad
body, **including a declared `Content-Length` over `MAX_BODY_SIZE`** — the parser
rejects it before the handler sees it), `404` (unknown or disabled token), `405`
(non-POST), `413` (the raw request exceeded the read buffer, `MAX_BODY_SIZE` plus
16 KiB, before parsing completed), `429` (per-token rate limit, 60/min).

One gap, in [BACKLOG.md](./BACKLOG.md): REQ-171's CA-signed certificate for this
endpoint is not implemented, so it answers on the daemon's TOFU cert. **An
archived channel does not stop a webhook post** — the archived check that
`SEND` performs is absent here.

---

### 5.16 Notification preferences (REQ-130, REQ-131)

A user's notification settings are **server-authoritative and synced across their
devices**. Two settings: a per-channel **level** (REQ-130) and a **do-not-disturb
window** (REQ-131). They are the config that clients honor and that the push emitter
(REQ-132/133, ARCH-85) consults; DND suppresses *push*, not in-app unread (REQ-131).

**`SET_NOTIFY_PREF` (C → S), `0x0090`** `{ channel_id: u64, level: u8 }` — set the
level for a channel the caller can read: `0` all, `1` mentions-only, `2` none. An
absent pref falls back to the user's own default (`SET_NOTIFY_DEFAULT`, REQ-134),
not to a fixed "all". Refused with `ERROR NOT_A_MEMBER` for a
channel the caller can't access, or an invalid level.

**`SET_DND` (`0x0091`) is RETIRED.** REQ-136 replaced the single quiet window
with a schedule that states the hours notifications are **allowed** — the same
two integers, the opposite sense — so the op number is retired rather than
redefined. A number that means the reverse of what a peer thinks it means is how
two sides agree loudly and behave differently.

**`LIST_NOTIFY_PREFS` (C → S), `0x0092`** (empty) — request the caller's full
settings.

**`NOTIFY_PREFS` (S → C), `0x0093`** `{ notify_default: u8, count: u16,
count × { channel_id: u64, level: u8, muted: u8 } }` — the snapshot. It is sent both as the reply to `LIST_NOTIFY_PREFS` and, after any
`SET_*`, **pushed to every one of the user's connections** so a change on one
device updates the others.

**`SET_SNOOZE` (C → S), `0x00CA`** `{ minutes: u32 }` — **pause** notifications
for that many minutes from now; `0` ends the pause. Slack's `dnd.setSnooze` takes
minutes for the same reason ours does: every preset the client offers is a
duration, and only the client knows the timezone that turns "until tomorrow" into
an instant. The daemon resolves it once and stores the absolute end.

**`SNOOZE` (S → C), `0x00CB`** `{ until_ms: u64 }` — when the pause ends, `0` for
none. Sent to **that user's own connections only**, after a `SET_SNOOZE` and
alongside every `NOTIFY_PREFS`. Other people are told the *fact* through
`PRESENCE_UPDATE`'s `dnd` byte and never the instant (REQ-122/278): a colleague
needs to know whether to write; when you are back is a movement report.

**`SET_SCHEDULE` (C → S), `0x00CC`** and **`SCHEDULE` (S → C), `0x00CD`**
`{ mode: u8, tz_offset_min: i16, start_min: u16, end_min: u16, count: u8,
count × { weekday: u8, enabled: u8, start_min: u16, end_min: u16 } }` — the
recurring schedule (REQ-136). `mode` is `0` off · `1` every day · `2` weekdays ·
`3` custom; the window is the hours notifications are **allowed**, and the
per-weekday rows apply to `custom` only (weekday `0` = Sunday). `tz_offset_min`
rides with it because a per-weekday window without one is a window on the wrong
day for half the world — and only the client knows the zone. One shape both
directions: the schedule a client sends and the schedule the server echoes are
the same fact.

**`SET_KEYWORDS` (C → S), `0x00CE`** `{ count: u8, count × str }` and
**`SET_PRIORITY` (C → S), `0x00CF`** `{ count: u8, count × u64 }` — replace my
keyword list / my priority people wholesale (REQ-135). Wholesale because both
lists are short, a client editing one holds all of it, and a term is its own
identity — there is nothing an add/remove pair could name. **`ALERT_PREFS`
(S → C), `0x00D0`** `{ n_terms: u8, terms…, n_people: u8, people… }` returns both.

*One request, all of it.* `LIST_NOTIFY_PREFS` answers with `NOTIFY_PREFS`, then
`SNOOZE`, `SCHEDULE` and `ALERT_PREFS`, in that order — so a client never has to
assemble its notification settings from four round trips or decide what to show
while half of them are outstanding.

*Why separate frames.* `NOTIFY_PREFS` ends in a repeated list, so anything added
to its fixed part shifts every entry after the first — the trap that cost protocol
version 3. And the separation is the design, not just an encoding convenience: the
**pause** is manual and one-shot, the **schedule** at `0x0091` is recurring and
planned, cancelling one has never cancelled the other, and a pause only ever *adds*
silence — so the two can never disagree in a way needing a precedence rule.

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

### 5.16c Storage report and audit log (REQ-214, REQ-251)

Two owner/admin-only read surfaces. Both are authorized in the **writer against
the user's current role**, so a demotion takes effect mid-session rather than at
next login (ARCH-77/79).

**`STORAGE_STATUS_REQ` (C → S), `0x0097`** — empty payload. Requests the storage
usage report (REQ-214). A member's request is refused with `ERROR FORBIDDEN`
rather than answered with zeros.

**`STORAGE_STATUS` (S → C), `0x0098`** — current usage, free space, the active
retention/eviction policy, and cumulative reclamation counts **by reason** (orphan
/ aged out / evicted under pressure, migration 0015). Fields are fixed-width, so a
later revision may append without a version bump.

**`AUDIT_QUERY` (C → S), `0x0099`** — pages the audit log (REQ-251). `before_ms`
pages **backwards from a timestamp** rather than by offset, so a page boundary
stays stable as new entries arrive; `0` asks for the newest page.

**`AUDIT_PAGE` (S → C), `0x009A`** — a page of entries, newest first: time,
family (admin / account / security / moderation), action, actor, target, outcome,
detail. It **never carries the secret involved** — that a password changed, never
the password; that an invite was redeemed, never the token (ARCH-79).

---

### 5.16d Notification schedule, pause, and the notify default

Everything a user says about *when* they are notified, beyond §5.16's per-channel
level. `SET_DND` (`0x0091`) is **retired**: REQ-136's schedule replaced it and
states the hours notifications are **allowed** — the same two integers with the
opposite meaning, which is why the opcode was retired rather than redefined
(ARCH-103).

Each of these answers with a full `NOTIFY_PREFS` (`0x0093`) to **all** of the
user's connections, followed by `SNOOZE`, `SCHEDULE` and `ALERT_PREFS`. That
four-frame trailer follows every `NOTIFY_PREFS`, not only a `LIST` request, so a
client always folds a complete picture rather than a delta.

**`SET_SNOOZE` (C → S), `0x00CA`** — Pause my notifications for N minutes from now; 0 ends the pause. (REQ-278, WIN-92)

    minutes (u32)

**`SNOOZE` (S → C), `0x00CB`** — Tell the user's own connections the absolute instant their pause ends. (REQ-278, REQ-122)

    until_ms (u64)

**`SET_SCHEDULE` (C → S), `0x00CC`** — Replace my recurring notification schedule: mode, base window, and the per-weekday rows. (REQ-136, ARCH-103, WIN-94)

    mode (u8), tz_offset_min (u16 — int16 two's complement, minutes east of UTC), start_min (u16), end_min (u16), count (u8), then count x { weekday (u8), enabled (u8), start_min (u16), end_min (u16) }

**`SCHEDULE` (S → C), `0x00CD`** — The stored recurring schedule echoed back to its owner. (REQ-136, ARCH-103)

    mode (u8), tz_offset_min (u16 — int16 two's complement, minutes east of UTC), start_min (u16), end_min (u16), count (u8), then count x { weekday (u8), enabled (u8), start_min (u16), end_min (u16) }

**`SET_NOTIFY_DEFAULT` (C → S), `0x0077`** — Set the workspace-wide fallback notification level used by channels with no per-channel override. (REQ-134)

    level (u8)

**`SET_MUTE` (C → S), `0x006D`** — Mute or unmute one conversation for the calling user. (REQ-137, WIN-40)

    channel_id (u64), muted (u8)

**`SET_READ_CURSOR` (C → S), `0x006E`** — Set my read cursor for a channel deliberately, including BACKWARDS (mark unread). (REQ-235, WIN-52)

    channel_id (u64), message_id (u64)

`SET_READ_CURSOR` is deliberately **not** `CLIENT_ACK`: the ack path upserts
`MAX(existing, new)` so a replayed ack can never rewind anyone, and mark-unread
needs to move the cursor *backwards*. It replies with `READ_CURSOR` frames — the
actor's new position to the channel's other members, and every other member's
position back to the actor (REQ-090's seen-by).

`SNOOZE` is **self-only**. Other people learn *that* someone is not to be
disturbed, through the DND byte on `PRESENCE_UPDATE` (protocol 5), and never
when they are back (REQ-122). The instant is enforced on read: a stamp already in
the past reports as `0`, so no sweep and no shared clock are needed to agree a
pause is over.

**Keywords and priority people** (REQ-135, ARCH-103) are replaced wholesale
rather than edited, which keeps the ops idempotent and the caps enforceable:

**`SET_KEYWORDS` (C → S), `0x00CE`** — replace my keyword list.

    count (u8), then count × term (str)

**`SET_PRIORITY` (C → S), `0x00CF`** — replace my priority-people list.

    count (u8), then count × user_id (u64)

**`ALERT_PREFS` (S → C), `0x00D0`** — both lists as stored, self only.

    n_terms (u8), n_terms × term (str), n_people (u8), n_people × user_id (u64)

Both lists cap at 64 (`OC_MAX_KEYWORDS`, `OC_MAX_PRIORITY`) and a term at 64
bytes; the encoder truncates to the cap rather than failing. A keyword hit is
**written into `mentions` with its own kind**, so the push query, the activity
feed and the reader's highlight all keep working unchanged — and it is matched by
`shared/mention.c`, never by SQL, so a client highlights exactly what the server
notified on. Priority people pierce a level and a pause but never a mute
(REQ-135/137).

### 5.16e Profile depth: custom status, title, timezone and avatar

**`SET_STATUS` (C → S), `0x006F`** — Set or clear my transient custom status. (REQ-241, REQ-122, WIN-53)

    emoji (str), text (str), expires_at (u64)

**`SET_PROFILE` (C → S), `0x0070`** — Set my job title and timezone. (REQ-240, WIN-47)

    title (str), timezone (str)

**`SET_AVATAR` (C → S), `0x0078`** — Point my avatar at an already-uploaded attachment, or clear it. (REQ-240, WIN-47 (image travels via the REQ-140 upload path))

    attachment_id (u64)

**`PROFILE_INFO` (S → C), `0x0072`** — One person's complete profile — everything a roster shows about them — as the reply to a profile read and to any of SET_STATUS / SET_PROFILE / SET_AVATAR. (REQ-240, REQ-241, WIN-47, WIN-53)

    user_id (u64), display_name (str), email (str), status_emoji (str), status_text (str), status_expires (u64), title (str), timezone (str), avatar_id (u64), role (u8)

An **empty `text` on `SET_STATUS` clears all three fields together** — emoji,
text and expiry — because "no status" is one state rather than two, and a client
must not be able to leave an orphan emoji behind. `expires_at` is an absolute
instant, `0` meaning "until I change it", and the **daemon** enforces it on read:
a client that is not running cannot clear its own status.

`SET_AVATAR` takes an **attachment id, never bytes**, so the upload path's dedup,
size cap and reclamation all keep working. The id is validated rather than
trusted: it must exist, be finalized, carry an `image/*` MIME type, **and have
been uploaded by the caller**. That last check is load-bearing — avatars are
readable workspace-wide, so without it a member could point their avatar at
somebody else's private-channel attachment.

> **`SET_PROFILE` cannot be delivered.** Its opcode `0x0070` is also
> `SET_PRESENCE`; the daemon dispatches presence first, and a title/timezone
> payload fails the presence decoder, so the connection is dropped. Do not
> implement against `0x0070` for this frame until the number is resolved. See
> [BACKLOG.md](./BACKLOG.md).

### 5.16f Drafts and scheduled messages

Server-stored user content (ARCH-101/102), not client settings: the
`client_settings` bucket is partitioned per frontend, which would make a draft
written in one client invisible in another.

**`SET_DRAFT` (C → S), `0x00C0`** — Upsert the caller's draft for a conversation; an empty body deletes it. (REQ-223, REQ-229, ARCH-101)

    channel_id (u64), thread_root (u64), recipients (str), body (str)

**`LIST_DRAFTS` (C → S), `0x00C1`** — Ask for every draft the caller holds. (REQ-223, ARCH-101)

    (empty)

**`DRAFT` (S → C), `0x00C2`** — One draft — both a LIST_DRAFTS stream entry and the cross-device sync push. (REQ-223, REQ-229, ARCH-101)

    id (u64), channel_id (u64), thread_root (u64), updated_ms (u64), recipients (str), body (str)

**`DRAFTS` (S → C), `0x00C3`** — Terminator for a LIST_DRAFTS stream, carrying how many DRAFT frames preceded it. (REQ-223, ARCH-101)

    count (u16)

**`SCHEDULE_MESSAGE` (C → S), `0x00C4`** — Hold a message and deliver it through the ordinary send path at send_at_ms. (REQ-224, ARCH-102)

    channel_id (u64), thread_root (u64), send_at_ms (u64), body (str)

**`LIST_SCHEDULED` (C → S), `0x00C5`** — Ask for everything the caller still has waiting. (REQ-224, ARCH-102)

    (empty)

**`CANCEL_SCHEDULED` (C → S), `0x00C6`** — Drop a scheduled message before it fires. (REQ-224, ARCH-102)

    id (u64)

**`UPDATE_SCHEDULED` (C → S), `0x00C7`** — Change a scheduled message's fire time, its text, or both. (REQ-224, ARCH-102)

    id (u64), send_at_ms (u64), body (str)

**`SCHEDULED` (S → C), `0x00C8`** — One scheduled row — both a LIST_SCHEDULED stream entry and the ack/sync push. (REQ-224, ARCH-102)

    id (u64), channel_id (u64), thread_root (u64), send_at_ms (u64), created_ms (u64), state (u8), fail_reason (str), body (str)

**`SCHEDULED_LIST` (S → C), `0x00C9`** — Terminator for a LIST_SCHEDULED stream, carrying how many SCHEDULED frames preceded it. (REQ-224, ARCH-102)

    count (u16)

A draft may be **unaddressed** — `channel_id` absent with a `recipients` list
beside it — because the New Message pane autosaves before a recipient is chosen
(REQ-229). A scheduled message is held in its own table and fired by a daemon
sweep on its own ~15 s timer (`OPENCHIME_SCHED_TICK_MS`), delivered through the
ordinary send path so mentions and notifications cannot diverge, with the
**daemon minting the idempotency token at fire time** — the client's token
belongs to the scheduling request.

### 5.16g Custom emoji, group DMs and unresolved mentions

**`ADD_EMOJI` (C → S), `0x007A`** — Register a custom emoji shortcode pointing at an already-uploaded image. (REQ-072)

    name (str), attachment_id (u64)

**`DELETE_EMOJI` (C → S), `0x007B`** — Remove a custom emoji shortcode by name. (REQ-072)

    name (str)

**`LIST_EMOJI` (C → S), `0x007C`** — Ask for the workspace's custom-emoji catalogue. (REQ-072)

    (empty)

**`EMOJI_LIST` (S → C), `0x007D`** — The whole custom-emoji catalogue — a reply to LIST_EMOJI and also the push after any add or delete. (REQ-072)

    count (u16), then count repetitions of: name (str), attachment_id (u64), created_by (u64)

**`OPEN_GROUP_DM` (C → S), `0x0079`** — Open — or reopen — the group DM whose participant set is the caller plus the named users. (REQ-056)

    count (u16), then count repetitions of: user_id (u64)

**`MENTION_UNRESOLVED` (S → C), `0x00B3`** — Tell the sender alone which names in a just-sent message resolved to real people who are not in this channel, so the mention notified nobody. (REQ-287)

    channel_id (u64), message_id (u64), can_add (u8), is_private (u8), count (u16), then count repetitions of: user_id (u64), name (str)

**`LIST_FILE_CHANNELS` (C → S), `0x0073`** — Ask which channels hold files, and how many each holds. (WIN-82)

    (empty)

**`FILE_CHANNELS` (S → C), `0x0074`** — One (channel_id, file count) pair per channel the caller can read that holds at least one file. (WIN-82)

    count (u16), then count repetitions of: channel_id (u64), count (u32)

A custom emoji's **name is its identity**, flat and workspace-wide: a duplicate
is refused rather than replacing an image existing messages already refer to. A
group DM is not a second kind — it is the same `kind='dm'` channel under the same
participant-set key, which is why it needed no migration. `MENTION_UNRESOLVED`
goes to the **sender only**: nothing failed and the message was stored, so it is
a notice rather than an `ERROR`, and it carries `can_add`/`is_private` so the
client never offers an action that would fail.

### 5.16h Invite, session and webhook management

**`LIST_INVITES` (C → S), `0x004B`** — Ask for the outstanding (unconsumed, unexpired) tenant invites. (REQ-026, WIN-46)

    (empty)

**`INVITE_LIST` (S → C), `0x004C`** — The outstanding invites, identified by a server-side id — never the token. (REQ-026, WIN-46)

    count (u16), then count x { invite_id (u64), role (u8), created_at (u64), expires_at (u64), created_by (u64) }

**`REVOKE_INVITE` (C → S), `0x004D`** — Revoke one outstanding invite by its id. (REQ-026, WIN-46)

    invite_id (u64)

**`INVITE_REVOKED` (S → C), `0x004E`** — Ack for REVOKE_INVITE, carrying the id so the client can drop that row without re-listing. (REQ-026, WIN-46)

    invite_id (u64)

**`LIST_SESSIONS` (C → S), `0x0075`** — Ask for the caller's own active sessions. (REQ-182)

    (empty)

**`SESSION_LIST` (S → C), `0x0076`** — The caller's unexpired sessions, so a device can be identified before it is revoked. (REQ-182)

    count (u16), then count x { session_id (u64), created_at (u64), last_seen (u64), expires_at (u64), current (u8), device_label (str) }

**`SET_WEBHOOK_STATE` (C → S), `0x006B`** — Enable or disable one incoming webhook. (REQ-170, WIN-48)

    webhook_id (u64), disabled (u8)

**`ROTATE_WEBHOOK` (C → S), `0x006C`** — Mint a new token for an existing webhook, invalidating the old one. (REQ-170, WIN-48)

    webhook_id (u64)

`SESSION_LIST` never carries a token — only the metadata needed to recognise a
session and revoke it (REQ-182). Rotating a webhook mints a new token and returns
it once, on the same terms as creation: only the SHA-256 is stored, so a token
cannot be re-shown.

### 5.16i Threads across channels

Delivers REQ-062 (ARCH-104). Participation is **derived** — you are in a thread
if you wrote its root or any reply — so the follow table holds only overrides,
and an explicit unfollow outranks having replied.

**`LIST_THREADS` (C → S), `0x00D1`** — Ask for every thread I am in, across all channels, optionally only the unread ones. (REQ-062, ARCH-104)

    filter (u8) — OPTIONAL: an EMPTY payload is legal and decodes as filter = OC_THREADF_ALL (0)

**`THREADS` (S → C), `0x00D3`** — Terminator of a LIST_THREADS response, stating how many THREAD_SUMMARY frames preceded it. (REQ-062, ARCH-104)

    count (u32)

**`THREAD_SUMMARY` (S → C), `0x00D2`** — One thread's aggregate row: its root, its activity, my unread count and whether I follow it. (REQ-062, ARCH-104)

    root_id (u64), channel_id (u64), root_author (u64), root_at (u64), last_reply_at (u64), reply_count (u32), unread (u32), following (u8), preview (str)

**`SET_THREAD_FOLLOW` (C → S), `0x00D4`** — Follow or unfollow one thread explicitly. (REQ-062, ARCH-104)

    root_id (u64), channel_id (u64), on (u8)

**`MARK_THREAD_READ` (C → S), `0x00D5`** — Mark a thread's replies read up to a reply id. (REQ-062, ARCH-104)

    root_id (u64), up_to (u64)

Unread replies are counted against a **per-thread cursor**, not the channel's:
the channel cursor advances when the channel is read and says nothing about a
thread inside it, and threads are deliberately outside the main scroll (REQ-060).
The follow and read acks return the one row that changed rather than a fresh
list, so opening a thread does not cost the whole view.

### 5.17 Audio call signaling (REQ-150, REQ-152)

Audio is **server-relayed** (no P2P/ICE, ARCH-18): the media itself flows over a
separate UDP sidecar (ARCH-31) — built, forked at daemon startup, and handed each
join's token over its IPC socket — but a call is *set up* over
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

**Screenshare — reserved wire additions (REQ-161, ARCH-86/87; not built).**
Screenshare rides this same call and relay unchanged: the sidecar forwards an
encoded video payload opaquely exactly as it forwards Opus, so there is no
server-side codec. That has one consequence the wire must carry, recorded here
while the frames are still cheap to extend — **the server cannot transcode**
(ARCH-18/73 forbid it decoding anything), and a call may hold clients on
different platforms at once, so a codec disagreement breaks the call outright.
The codec is therefore a **wire contract**, not a frontend build choice, and
`CALL_JOINED` **carries no codec field today**. Adding video without one would
break every older client on the first codec change — the failure ARCH-41 exists
to prevent for frames. The reserved additions, none implemented:

- a **codec identifier** on `CALL_JOINED`, so the ARCH-87 baseline (VP9) can be
  succeeded (by AV1) without a flag day;
- **share start/stop signaling** — who is sharing, so clients render the right
  surface and the roster reflects it;
- a **fragment header** in the sidecar's UDP framing — `OC_AUDIO_MAX_PACKET` is
  1400 bytes, correct for an ~80-byte Opus frame and useless for a keyframe of
  tens of KB;
- a **keyframe-request** path from receiver to sharer, since a lost video packet
  corrupts the picture until the next IDR (Opus conceals loss with PLC; video
  does not).

The last two are relay-*visible* but not relay-*interpreted* — the sidecar keeps
forwarding opaque payloads behind a larger header — so ARCH-18/73 hold. Note also
that `seq` is `u16`, which wraps in roughly a minute at video packet rates, so
reassembly must tolerate wrap or the field widens. Full design in
[VIDEO.md](./VIDEO.md).

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

**A cursor of `0` means "I hold no history", and the server answers with the
channel's NEWEST page** — its last N top-level messages — not its oldest. A
client that keeps nothing locally (ARCH-88) wants the bottom of the transcript,
which is where it will be scrolled to; older history is paged in by scrolling
back. For a channel the client omits entirely, and for `count = 0`, the same
rule applies to every channel the user belongs to.

The server does **not** resume a zero cursor from the user's stored delivery
cursor (§5.4). That cursor is where they last read *to*, so a caught-up user
would be sent nothing and would face an empty channel on every launch. The read
cursor positions the unread divider (REQ-236); it does not decide which messages
exist.

A **non-zero** cursor keeps its literal meaning — strictly greater than
`after_message_id` — so a reconnecting client that still holds history receives
only what it missed, with no duplicate replay.

The server replays a channel the caller may **read** — public, or one they are a
member of — which is the same rule §5.7 applies to reading generally (REQ-031).
A cursorless request is the narrower case: with no cursors the server derives the
caller's *member* channels and resumes each from its stored read cursor.

### 6.2 Replay and `BACKFILL_DONE` (server → client), msg_type `0x0031`

In response, the server streams ordinary `BROADCAST` frames (§5.3) for every
message with `message_id > after_message_id` in each requested channel, in
ascending `message_id` order per channel, then sends a single `BACKFILL_DONE`
to mark the catch-up complete.

The replay also carries the **reaction state** of the messages it sends: after
the `BROADCAST` frames, the server emits one `REACTION_UPDATED` (§5) per
(message, emoji) with the aggregate count. A `BROADCAST` has no room for
reactions, so without this a client that keeps no local cache (ARCH-88) lost
every reaction the moment it reloaded. The `user_id` on those frames is the
requesting user whenever they are one of the reactors, so the client can render
its own reactions as such without a second round trip.

`BACKFILL_DONE` payload:

| Field         | Type | Notes                                                         |
|---------------|------|---------------------------------------------------------------|
| `high_water`  | u64  | Highest `message_id` in the tenant at the moment backfill completed. Purely informational (progress/consistency check); the client's authoritative state remains its per-channel high-water marks. |
| `more`        | u8   | 1 if the replay hit the per-response cap; the client issues a follow-up `BACKFILL_REQUEST` with an advanced cursor. |

Replay volume is bounded per response; if a channel's backlog exceeds the
per-response cap, the server replays up to the cap and the client issues a
follow-up `BACKFILL_REQUEST` with an advanced cursor. (The exact cap is an
implementation tuning value, not a wire-format constant.)

### 6.3 `HISTORY_REQUEST` (client → server), msg_type `0x0034`

Pages **backwards** through one channel: the newest `limit` top-level messages
**strictly older than** `before_message_id`.

| Field               | Type | Notes                                                        |
|---------------------|------|--------------------------------------------------------------|
| `channel_id`        | u64  | The channel to page.                                          |
| `before_message_id` | u64  | Return only ids strictly below this. `0` means "from the newest". |
| `limit`             | u16  | Page size; `0` means the server's default. Clamped server-side. |

*Why a separate frame.* A `BACKFILL_REQUEST` cursor only points forward, and
§6.1 makes a zero cursor mean "send the newest page". Neither can express "give
me what came before this", so a client could reach the newest page of a channel
and no further — permanently, since clients keep no local history (ARCH-88).
Adding a field to the existing cursor entry would have changed a frame already
in use; a new opcode is additive.

The response reuses §6.2 exactly: `BROADCAST` frames in **ascending** id order
(the order the client's high-water dedup expects), the matching
`REACTION_UPDATED` frames, then `BACKFILL_DONE`. On this path `more` means
**"older messages exist above this page"**, which is how a client knows to stop
asking rather than retrying at the top of a channel forever.

Read access is checked as everywhere else; a channel the user cannot read
returns an empty page rather than an error.

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
| `1003` | `MALFORMED_FRAME`     | any        | yes   | Unparseable frame.                                             |
| `1004` | `FRAME_TOO_LARGE`     | any        | yes   | `length` implies a frame larger than `MAX_FRAME_SIZE`.         |
| `1005` | `UNEXPECTED_MSG_TYPE` | any        | yes   | Reserved for a frame not valid in the current state. **Never emitted**: a non-`HELLO` first frame closes the socket silently and an unrecognised post-auth type is ignored. A `SEND` before `AUTH_OK` gets `AUTH_REQUIRED` instead. |
| `1006` | `VERSION_MISMATCH`    | post-hello | yes   | A post-handshake frame carried a `version` other than the negotiated one. Distinct from `1001`/`1002`, which answer a `HELLO` and precede a session: this says the peer agreed and then sent something else. The `ERROR` itself is stamped with the negotiated version, so it is readable by the peer being disconnected. |
| `2001` | `AUTH_REQUIRED`       | post-hello | yes   | A messaging frame arrived before successful `AUTH`.            |
| `2002` | `AUTH_INVALID_TOKEN`  | auth       | yes   | JWT failed signature/audience/expiry validation (REQ-023).     |
| `2003` | `AUTH_RATE_LIMITED`   | auth       | yes   | Too many auth attempts for this tenant (REQ-191).              |
| `2004` | `USER_LIMIT`          | auth       | yes   | Workspace at its registered-user cap (`OPENCHIME_MAX_USERS`); a new user cannot be created. An existing user still logs in. |
| `3001` | `BODY_TOO_LARGE`      | messaging  | no    | `SEND` body exceeded `MAX_BODY_SIZE`.                           |
| `3002` | `NOT_A_MEMBER`        | messaging  | no    | Sender is not a member of the target channel (REQ-031).        |
| `3003` | `UNKNOWN_CHANNEL`     | messaging  | no    | `channel_id` does not exist in this tenant.                    |
| `3004` | `SEND_RATE_LIMITED`   | messaging  | no    | Per-connection send rate exceeded (REQ-190).                   |
| `3005` | `FORBIDDEN`           | admin/msg  | no    | The actor may not perform the action — role (ARCH-60, §6) or not the message's author (§5.5/5.6). |
| `3006` | `LAST_OWNER`          | admin      | no    | Would remove or demote the tenant's last owner (REQ-030).      |
| `3007` | `UNKNOWN_MESSAGE`     | messaging  | no    | `EDIT`/`DELETE` names a message not in the channel, or already tombstoned (§5.5/5.6). |
| `3008` | `INVALID_CHANNEL`     | messaging  | no    | `CREATE_CHANNEL` name empty or over 64 bytes (§5.7). |
| `3009` | `INVALID_REACTION`    | messaging  | no    | `REACT` emoji empty or over 32 bytes (§5.9). |
| `3010` | `ATTACHMENT_TOO_LARGE`| messaging  | no    | Upload exceeds `MAX_ATTACHMENT_SIZE` (REQ-140). |
| `3011` | `UNKNOWN_ATTACHMENT`  | messaging  | no    | No such attachment, or not finalized (REQ-141). |
| `3012` | `STORAGE_FULL`        | messaging  | no    | Upload refused: below the DB reserve (REQ-216). |
| `3013` | `ATTACHMENT_GONE`     | messaging  | no    | Reclaimed by age or storage pressure (REQ-215/217). |
| `3014` | `INVALID_DEVICE_TOKEN`| messaging  | no    | `REGISTER_DEVICE_TOKEN` had an empty token or unknown platform (REQ-132). |
| `3015` | `TRANSFER_PROTOCOL`   | messaging  | no    | Out-of-order/oversized chunk or bad transfer state. |
| `3016` | `UNKNOWN_WEBHOOK`     | webhook    | no    | No such (or disabled) incoming webhook token (REQ-170). |
| `3017` | `CHANNEL_EXISTS`      | channel    | no    | `CREATE_CHANNEL` name already taken, compared case-insensitively (REQ-040). |
| `3018` | `TOO_MANY_PINS`       | channel    | no    | The channel already holds 100 pins (REQ-230, ARCH-90). |
| `3019` | `CHANNEL_ARCHIVED`    | channel    | no    | The channel is archived and read-only (REQ-035). Returned by `SEND`, `SEND_REPLY` and `UPLOAD_BEGIN`; **not** by the webhook post path. |
| `3020` | `INVALID_MESSAGE`     | messaging  | no    | Nothing to send — an empty body on a scheduled message (REQ-224). |
| `9001` | `INTERNAL_ERROR`      | any        | maybe | Server-side failure; `fatal` indicates whether the connection survives. |

Handshake-stage version codes (`1001`/`1002`) are delivered via `REJECT`, which
carries the same `code`; the other codes are delivered via `ERROR`.

---

## 9. Message type registry

**Generated from `shared/protocol.h`** — every `OC_MSG_*` the codec defines, in
opcode order, 154 of them. The sections above specify the payload layouts; this
table is the index and the authority on which values are taken.

One opcode is **used by two message types** (`0x0070`), marked below:
`SET_PROFILE`/`SET_PRESENCE` are both client→server and collide for real. See
[BACKLOG.md](./BACKLOG.md). The other two shared values are gone — `TYPING` and
`TYPING_UPDATE` moved to `0x007E`/`0x007F` in v8, leaving `0x0072` to
`PROFILE_INFO` and `0x0073` to `LIST_FILE_CHANNELS` alone. `scripts/check_opcodes.sh`
(run by `make test` and CI) fails on any duplicate outside that one tracked
exception, so this table cannot silently regain a shared value.

| msg_type | Name | Direction | Notes |
|---|---|---|---|
| `0x0001` | `HELLO` | C → S | frozen@v1 |
| `0x0002` | `WELCOME` | S → C | frozen@v1 |
| `0x0003` | `REJECT` | S → C | frozen@v1, fatal |
| `0x0010` | `AUTH` | C → S |  |
| `0x0011` | `AUTH_OK` | S → C |  |
| `0x0012` | `AUTH_CHALLENGE` | S → C |  |
| `0x0013` | `LOGOUT` | C → S |  |
| `0x0014` | `WORKSPACE_INFO` | S → C | pushed after AUTH_OK |
| `0x0020` | `SEND` | C → S |  |
| `0x0021` | `SEND_ACK` | S → C |  |
| `0x0022` | `BROADCAST` | S → C |  |
| `0x0023` | `CLIENT_ACK` | C → S |  |
| `0x0024` | `EDIT` | C → S | (REQ-051) |
| `0x0025` | `DELETE` | C → S | (REQ-052) |
| `0x0026` | `MSG_EDITED` | S → C | edit fan-out |
| `0x0027` | `MSG_DELETED` | S → C | tombstone fan-out |
| `0x0028` | `REACT` | C → S | (REQ-070) |
| `0x0029` | `REACTION_UPDATED` | S → C | reaction fan-out |
| `0x002A` | `LIST_REACTIONS` | C → S | (REQ-071) |
| `0x002B` | `REACTIONS` | S → C |  |
| `0x002C` | `SEND_REPLY` | C → S | (REQ-060), a threaded reply |
| `0x002D` | `THREAD_REPLY` | S → C | reply fan-out (not in main scroll) |
| `0x002E` | `LIST_THREAD` | C → S | open a thread |
| `0x002F` | `THREAD` | S → C | a thread's replies |
| `0x0030` | `BACKFILL_REQUEST` | C → S |  |
| `0x0031` | `BACKFILL_DONE` | S → C |  |
| `0x0032` | `THREAD_META` | S → C | a parent's reply count (backfill) |
| `0x0033` | `READ_CURSOR` | S → C | a member's read cursor advanced (REQ-090 seen-by) |
| `0x0034` | `HISTORY_REQUEST` | C → S | page BACKWARDS through a channel (§6.3) |
| `0x0035` | `PIN` | C → S | (REQ-230), pin/unpin a message |
| `0x0036` | `PIN_UPDATED` | S → C | pin fan-out to every channel member |
| `0x0037` | `LIST_PINS` | C → S | a channel's pinned messages |
| `0x0038` | `PINNED_MSG` | S → C | one pinned message (streamed, body included) |
| `0x0039` | `PINS` | S → C | terminator of a LIST_PINS response |
| `0x003A` | `LIST_MEMBERS` | C → S | (REQ-031), a channel's members |
| `0x003B` | `MEMBER_ENTRY` | S → C | one channel member (streamed) |
| `0x003C` | `MEMBERS` | S → C | terminator of a LIST_MEMBERS response |
| `0x003D` | `LIST_FILES` | C → S | (REQ-143), files in a channel (0 = everywhere) |
| `0x003E` | `FILE_ENTRY` | S → C | one shared file (streamed) |
| `0x003F` | `FILES` | S → C | terminator of a LIST_FILES response |
| `0x0040` | `LIST_USERS` | C → S | tenant user enumeration |
| `0x0041` | `USER_LIST` | S → C |  |
| `0x0042` | `SET_ROLE` | C → S | (ARCH-60, REQ-030) |
| `0x0043` | `INVITE_USER` | C → S | (REQ-033, tenant-level; owner/admin) |
| `0x0044` | `REMOVE_USER` | C → S | (REQ-033, tenant-level; owner/admin) |
| `0x0045` | `USER_UPDATED` | S → C | ack/push for SET_ROLE + REMOVE_USER |
| `0x0046` | `INVITE_CREATED` | S → C | the minted invite token |
| `0x0047` | `REDEEM_INVITE` | C → S | pre-auth account creation |
| `0x0048` | `SET_DISPLAY_NAME` | C → S | change your own display name (REQ-020) |
| `0x0049` | `CHANGE_PASSWORD` | C → S | change your own local password (verify old) |
| `0x004A` | `PROFILE_UPDATED` | S → C | a user's display name changed (also the self ack) |
| `0x004B` | `LIST_INVITES` | C → S | outstanding invites (owner/admin) |
| `0x004C` | `INVITE_LIST` | S → C | the list (token HASHES only, never tokens) |
| `0x004D` | `REVOKE_INVITE` | C → S | revoke one by id |
| `0x004E` | `INVITE_REVOKED` | S → C | ack |
| `0x0050` | `CREATE_CHANNEL` | C → S | (REQ-050) |
| `0x0051` | `CHANNEL_INFO` | S → C | ack for create/join/leave/invite/remove |
| `0x0052` | `LIST_CHANNELS` | C → S |  |
| `0x0053` | `CHANNEL_LIST` | S → C |  |
| `0x0054` | `JOIN_CHANNEL` | C → S |  |
| `0x0055` | `LEAVE_CHANNEL` | C → S |  |
| `0x0056` | `INVITE_TO_CHANNEL` | C → S | (REQ-033, channel-level) |
| `0x0057` | `REMOVE_FROM_CHANNEL` | C → S | (REQ-033, channel-level) |
| `0x0058` | `OPEN_DM` | C → S | open/get a 1:1 DM (REQ-050) |
| `0x0059` | `CREATE_WEBHOOK` | C → S | mint an incoming-webhook token (REQ-170) |
| `0x005A` | `WEBHOOK_INFO` | S → C | the minted webhook id + token (shown once) |
| `0x005B` | `LIST_WEBHOOKS` | C → S | list a channel's webhooks (REQ-170) |
| `0x005C` | `WEBHOOK_LIST` | S → C | the webhook list (no tokens) |
| `0x005D` | `DELETE_WEBHOOK` | C → S | remove a webhook |
| `0x005E` | `WEBHOOK_DELETED` | S → C | ack for DELETE_WEBHOOK |
| `0x005F` | `UPDATE_CHANNEL` | C → S | (REQ-034/035/036), mutate a channel |
| `0x0060` | `SEARCH` | C → S | (REQ-080) |
| `0x0061` | `SEARCH_RESULTS` | S → C |  |
| `0x0062` | `SAVE_ITEM` | C → S | save/unsave a message (private) |
| `0x0063` | `SAVED_UPDATED` | S → C | ack to the actor only |
| `0x0064` | `LIST_SAVED` | C → S | my saved items |
| `0x0065` | `SAVED_MSG` | S → C | one saved message (streamed) |
| `0x0066` | `SAVED` | S → C | terminator |
| `0x0067` | `LIST_ACTIVITY` | C → S | (REQ-139), what involved me — or what I have not read |
| `0x0068` | `ACTIVITY_ENTRY` | S → C | one activity item (streamed) |
| `0x0069` | `ACTIVITY` | S → C | terminator + the seen watermark |
| `0x006A` | `HISTORY_AROUND` | C → S | (REQ-232), the messages AROUND an id |
| `0x006B` | `SET_WEBHOOK_STATE` | C → S | enable/disable one |
| `0x006C` | `ROTATE_WEBHOOK` | C → S | mint a new token for it |
| `0x006D` | `SET_MUTE` | C → S | mute/unmute a conversation |
| `0x006E` | `SET_READ_CURSOR` | C → S | set the cursor (may move BACK) |
| `0x006F` | `SET_STATUS` | C → S | my custom status (empty text clears) |
| `0x0070` | `SET_PRESENCE` | C → S | set own presence (REQ-120) **(opcode shared)** |
| `0x0070` | `SET_PROFILE` | C → S | my title/timezone **(opcode shared)** |
| `0x0071` | `PRESENCE_UPDATE` | S → C | a user's presence changed |
| `0x0072` | `PROFILE_INFO` | S → C | a user's full profile (also a push) |
| `0x0073` | `LIST_FILE_CHANNELS` | C → S |  |
| `0x0074` | `FILE_CHANNELS` | S → C | (channel_id, count) pairs |
| `0x0075` | `LIST_SESSIONS` | C → S |  |
| `0x0076` | `SESSION_LIST` | S → C | never the tokens |
| `0x0077` | `SET_NOTIFY_DEFAULT` | — |  |
| `0x0079` | `OPEN_GROUP_DM` | C → S | open/create a group DM (REQ-056) |
| `0x007A` | `ADD_EMOJI` | C → S | name + attachment id (REQ-072) |
| `0x007B` | `DELETE_EMOJI` | C → S | name |
| `0x007C` | `LIST_EMOJI` | C → S | ask for the catalogue |
| `0x007D` | `EMOJI_LIST` | S → C | the catalogue (also a push) |
| `0x007E` | `TYPING` | C → S | "I am typing" in a channel (REQ-121) |
| `0x007F` | `TYPING_UPDATE` | S → C | relay of a typing signal |
| `0x0080` | `UPLOAD_BEGIN` | C → S | declare an attachment upload (REQ-140) |
| `0x0081` | `UPLOAD_READY` | S → C | id + chunk size + in-flight window |
| `0x0082` | `UPLOAD_CHUNK` | C → S | one upload chunk |
| `0x0083` | `UPLOAD_ACK` | S → C | window advance |
| `0x0084` | `UPLOAD_END` | C → S | finish the upload |
| `0x0085` | `UPLOAD_OK` | S → C | upload finalized (id + size + sha256) |
| `0x0086` | `DOWNLOAD_BEGIN` | C → S | request an attachment (REQ-141) |
| `0x0087` | `DOWNLOAD_INFO` | S → C | metadata preceding the bytes |
| `0x0088` | `DOWNLOAD_CHUNK` | S → C | one download chunk |
| `0x0089` | `DOWNLOAD_END` | S → C | all bytes sent |
| `0x008A` | `TRANSFER_CANCEL` | C → S | abort an in-progress transfer |
| `0x0090` | `SET_NOTIFY_PREF` | C → S | set a channel's notification level (REQ-130) |
| `0x0092` | `LIST_NOTIFY_PREFS` | C → S | request all notification settings |
| `0x0093` | `NOTIFY_PREFS` | S → C | DND + per-channel levels (also a sync push) |
| `0x0094` | `SET_CLIENT_SETTING` | C → S | upsert a synced client setting (empty value = delete) |
| `0x0095` | `LIST_CLIENT_SETTINGS` | C → S | request a client_type bucket's settings |
| `0x0096` | `CLIENT_SETTINGS` | S → C | a bucket snapshot (also a device-sync push) |
| `0x0097` | `STORAGE_STATUS_REQ` | C → S | owner/admin: ask for storage usage (REQ-214) |
| `0x0098` | `STORAGE_STATUS` | S → C | usage + policy + what maintenance reclaimed |
| `0x0099` | `AUDIT_QUERY` | C → S | owner/admin: page the audit log (REQ-251) |
| `0x009A` | `AUDIT_PAGE` | S → C | a page of entries, newest first |
| `0x00A0` | `CALL_JOIN` | C → S | join a channel's audio call (REQ-150) |
| `0x00A1` | `CALL_LEAVE` | C → S | leave the call |
| `0x00A2` | `CALL_JOINED` | S → C | to the joiner: call id + UDP endpoint/token + roster |
| `0x00A3` | `CALL_ROSTER` | S → C | to participants: roster changed |
| `0x00B0` | `REGISTER_DEVICE_TOKEN` | C → S | register a mobile push token (REQ-132) |
| `0x00B1` | `UNREGISTER_DEVICE_TOKEN` | C → S | drop a push token (logout / token change) |
| `0x00B2` | `DEVICE_TOKEN_ACK` | S → C | register/unregister acknowledged |
| `0x00B3` | `MENTION_UNRESOLVED` | — | Drafts (REQ-223, ARCH-101). DRAFT is deliberately BOTH the streamed list * entry and the dev… |
| `0x00C0` | `SET_DRAFT` | C → S | upsert a draft (empty body = delete) |
| `0x00C1` | `LIST_DRAFTS` | C → S | all of my drafts |
| `0x00C2` | `DRAFT` | S → C | one draft: a list entry AND the sync push |
| `0x00C3` | `DRAFTS` | S → C | terminator |
| `0x00C4` | `SCHEDULE_MESSAGE` | C → S | hold this until send_at_ms |
| `0x00C5` | `LIST_SCHEDULED` | C → S | everything I have waiting |
| `0x00C6` | `CANCEL_SCHEDULED` | C → S | drop one before it fires |
| `0x00C7` | `UPDATE_SCHEDULED` | C → S | change its time or its text |
| `0x00C8` | `SCHEDULED` | S → C | one row: list entry AND push |
| `0x00C9` | `SCHEDULED_LIST` | S → C | terminator |
| `0x00CA` | `SET_SNOOZE` | C → S | pause for N minutes from now (0 = end it) |
| `0x00CB` | `SNOOZE` | S → C | to that user's own connections: when it ends |
| `0x00CC` | `SET_SCHEDULE` | C → S | mode + base window + per-weekday rows |
| `0x00CD` | `SCHEDULE` | S → C | self only: the same, as stored |
| `0x00CE` | `SET_KEYWORDS` | C → S | replace my keyword list wholesale |
| `0x00CF` | `SET_PRIORITY` | C → S | replace my priority-people list |
| `0x00D0` | `ALERT_PREFS` | S → C | self only: both lists, as stored |
| `0x00D1` | `LIST_THREADS` | C → S | {filter} 0 all / 1 unread only |
| `0x00D2` | `THREAD_SUMMARY` | S → C | one thread (streamed) |
| `0x00D3` | `THREADS` | S → C | terminator |
| `0x00D4` | `SET_THREAD_FOLLOW` | C → S | follow/unfollow one thread |
| `0x00D5` | `MARK_THREAD_READ` | C → S | its replies are read up to here |

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
- A frame invalid for the current state is specified to yield `ERROR
  UNEXPECTED_MSG_TYPE`, and does not: the pre-auth case answers `AUTH_REQUIRED`,
  and the others close or ignore silently
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
| REQ-130/136/278 | `SET_NOTIFY_PREF`/`SET_NOTIFY_DEFAULT`/`SET_MUTE`/`SET_SCHEDULE`/`SET_SNOOZE`/`LIST_NOTIFY_PREFS` → `NOTIFY_PREFS`; server-authoritative settings synced to all the user's devices (§5.16, ARCH-72/103). REQ-131's `SET_DND` is retired — the schedule replaced it. |

Related decisions newly recorded in ARCHITECTURE.md: ARCH-41 (handshake &
version negotiation), ARCH-42 (primitive field encodings), ARCH-43 (message id
allocation), ARCH-44 (idempotency mapping), ARCH-45 (client-side dedup),
ARCH-46 (reconnect backfill shape), ARCH-47 (error frame & code table).
