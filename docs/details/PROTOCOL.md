# OpenChime Wire Protocol

This document specifies the OpenChime binary wire protocol at the byte level.
It is the detailed realization of the frame decisions in
[ARCHITECTURE.md](../ARCHITECTURE.md) (ARCH-6 through ARCH-9, ARCH-30) and of
the protocol-shaped requirements in [REQUIREMENTS.md](../REQUIREMENTS.md).

**Scope.** This covers connection handshake and version
negotiation, authentication, the message send/broadcast/ack cycle, message
edit/delete (§5.5/5.6), reactions (§5.9), threads (§5.10), search (§5.11),
direct messages (§5.12), channel management (§5.7), tenant administration (§5.8),
reconnect backfill and the error frame — plus presence and
typing (§5.13, REQ-120/121), attachments (§5.14), incoming webhooks (§5.15),
notification preferences (§5.16), synced client settings (§5.16a), self-service
profile (§5.16b), call signaling and screen sharing (§5.17, REQ-150–152, REQ-161), push device
tokens (§5.18), and encrypted DMs (§5.19).

**§9 is the complete list; §5's prose is not.** The registry is kept in step with
`shared/protocol.h` and carries all 239 message types. The narrative sections
below specify the payload layouts for the families they name, and a number of
later families — drafts, scheduled send, the notification schedule and pause,
keywords, custom emoji, group DMs, invite and session management, webhook
enable/rotate, cross-channel threads, unresolved-mention notices, and the
per-channel file census — are listed in §9 with their opcodes and directions and
specified in §§5.16d–5.16n.

**Code.** The frames in this document are realized in
`shared/protocol.c` (codec), `daemon/dbwriter.c` (handlers), and
`daemon/netloop.c` (dispatch). The auth-and-message vertical is exercised end to
end over a real socket, and the rest in-process (TESTING.md §3.3). Where this
document and the codec disagree, the codec is right.

---

## 1. Transport

- The protocol runs over a single TLS/TCP connection per client (ARCH-6,
  ARCH-10, REQ-180). There is no unencrypted fallback.
- The client verifies the daemon's certificate as any HTTPS client does — a
  trusted root, and the workspace's name — or, for a daemon with only its
  self-signed certificate, by the fingerprint the person trusted or the
  workspace published (ARCH-10, REQ-183, [TLS.md](./TLS.md)). Everything below
  §1 describes the plaintext *inside* the TLS session.
- The connection is bidirectional and full-duplex. After the handshake and
  auth complete, either side may send an applicable frame at any time (e.g.
  the server may push a `BROADCAST` while the client is composing a `SEND`).
- One TCP connection carries exactly one authenticated session. There is no
  multiplexing of multiple users over one connection.

### 1.1 Port and ALPN multiplexing (ARCH-54)

This is a client-facing contract: it is fixed and clients must conform,
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

A port typed with the workspace name (`chat.example.com:8443`) outranks all
three: the client connects to that host and port and looks up neither SRV nor
`.well-known`.

**An address is not discovered.** A workspace typed as an IP address — IPv4
(`192.0.2.7`, `192.0.2.7:8443`) or IPv6 in brackets (`[2001:db8::7]`,
`[2001:db8::7]:8443`; a bare `2001:db8::7` is accepted too, without a port) —
is dialled as typed, at the port given or 443. The client makes **no SRV query,
no DNS lookup and no `.well-known` request** for it, so a client on an
air-gapped network that is given its daemon's address contacts that daemon and
nothing else. The address is kept in one spelling (`inet_ntop`'s), so
`[2001:DB8:0::7]` and `[2001:db8::7]` are the same workspace, and no suffix is
ever appended to it. A name is dialled on every address it has, IPv6 and IPv4,
in the system's order, each given at most 3 s before the next is tried.

The daemon *binds* a configurable port (`OPENCHIME_PORT`, default 8443 so an
install cannot collide with a web server; the call relay's UDP socket is on the
same number). A real deployment sets it to 443 (ARCH-54; self-hosted: the
systemd unit grants `CAP_NET_BIND_SERVICE`, ARCH-20; in a container, a platform
or proxy maps external 443 to it). 8443 is never a value a client assumes.

**ALPN demultiplexing.** Port 443 is shared between this binary protocol and the
daemon's HTTP/1.1 surface (incoming webhooks, ARCH-32/34) by **ALPN** negotiated
during the TLS handshake:

- The client **MUST** offer ALPN **`oc/1`** (`OC_ALPN_PROTO`). The daemon
  selects it and routes the connection to the binary-protocol handler.
- The daemon advertises **`oc/1`**, then **`http/1.1`** (`OC_ALPN_HTTP11`),
  then `acme-tls/1` (chosen only by a CA validating a name, which offers it
  alone, TLS.md). Selection follows the *server's* order, so a peer offering
  both of the first two still gets the binary protocol.
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
                              binary protocol   HTTP/1.1 (webhooks)
                              (this document)   (ARCH-32/34)
```

Both sides exist. A connection that negotiates `oc/1` reaches the binary
protocol; one that does not is read by the daemon's HTTP/1.1 stack (ARCH-32),
which serves `POST /webhook/<token>` (§5.15), `POST /mcp` when the operator
turns MCP on (MCP.md: one JSON-RPC request per POST, `GET` and `DELETE` `405`)
and with it the MCP authorization server's routes (AUTH.md §8.13):
`GET /.well-known/oauth-protected-resource`,
`GET /.well-known/oauth-authorization-server`, `GET /oauth/authorize`,
`POST /oauth/token` and `POST /oauth/register`,
the WebSocket upgrade `GET /ws`
(§1.2), `POST /enroll-proof` (central's check of the address an enrolled
workspace reported, AUTH.md §3.6), the daemon's own sign-in pages (AUTH.md
§8.10/§8.11) and, when its wasm
is beside the binary, the web client under `/app/` (WEB.md): any other path is
`404`, and a served path under another method `405`.
`/healthz` and the landing page are on the separate plaintext health port
(ARCH-25), served by the same stack. Either port takes **one request per
connection**, answered with `Connection: close`; a request not whole within 10
seconds is answered `408`; a chunked body (`Transfer-Encoding`) is refused `400`.

The `oc` version suffix (`/1`) tracks the transport-framing generation, distinct from the
per-frame `version` field in §2.

### 1.2 WebSocket transport (the browser client)

A browser opens no socket of its own, so the web client (ARCH-74,
[WEB.md](./WEB.md)) reaches the daemon through a **WebSocket** (RFC 6455)
carrying the same frames:

- **`GET /ws`** with `Upgrade: websocket`, on the TLS port (a connection that
  negotiated `http/1.1` or no ALPN, §1.1) and on the plaintext health port
  (ARCH-25) for a page served from the same box in development. The daemon
  answers `101` with `Sec-WebSocket-Accept` and echoes the first subprotocol
  offered (a browser drops a connection whose offer went unanswered; the
  client offers `binary`).
- From then on the connection is a protocol peer as if it had negotiated
  `oc/1`: the handshake (§3), auth (§4) and every frame are unchanged. A
  client's frames are **binary** and masked, as the standard requires; the
  daemon's are binary and unmasked. The payloads of successive binary (and
  continuation) frames form **one byte stream** of §2 frames: one WebSocket
  message may hold several protocol frames, or part of one, and the receiver
  reassembles as it does over TCP. A ping is answered with a pong; a close, a
  text frame or a reserved bit ends the connection.
- **There is no TLS inside the WebSocket.** The page's HTTPS (`wss://`) is
  the wire's TLS, judged by the browser; the daemon's certificate pin (§1,
  ARCH-10) does not apply to this transport, and a deployment serves `/ws`
  only on 443 or behind the same front door.

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
> even though no payload moved: to a peer one version behind, a frame on a
> reassigned opcode decodes as the wrong struct rather than being rejected —
> precisely what the version number exists to prevent.
>
> Both sides send the version as `min` **and** `max`, so a mismatched pair gets a
> `REJECT` carrying `VERSION_TOO_OLD` / `VERSION_TOO_NEW`, which says what is
> wrong, instead of connecting happily and then dropping the link on the first
> undecodable frame.
>
> **The current version is 51** (`OC_PROTOCOL_VERSION` in `shared/protocol.h`,
> which is the authority; the per-version change notes live beside it). Version
> 51 put `via` in `BROADCAST` (after `call_id`) and `THREAD_REPLY` (after
> `also_channel`), each ahead of the body: the AI client a person posted
> through (AUTH.md §8.13, §5.16q). Version
> 50 appended `rec_by` to `CALL_STATE`: who started the recording going on
> (CALLS.md §8), and `faces` to each `CALL_HISTORY_PAGE` entry: who took part,
> up to three, first joined first. Version 49 made a call an object of its own (CALLS.md §1, §5.17): `CALL_START` and
> `CALL_SUBJECT` were added, every call frame names its call by `call_id` and no
> longer a conversation, `CALL_JOIN` lost its invitees, `CALL_STATE` gained
> `subject`, `from_channel` and `chat_channel`, and `BROADCAST` gained `call_id` after
> `parent_id`; `CALL_HISTORY`, `CALL_DETAIL` and `CALL_DELETE`, with their answers,
> were added, and each `SEARCH_RESULTS` entry gained `call_id` and `line_id`;
> `CALL_RECORD` was added, a participant's flags gained `RECORDER`, and
> `CALL_STATE` gained `flags` after `chat_channel`. Version
> 47 put `stream` in `CALL_MEDIA`, as in the relay's datagrams, gave a call
> participant's flags `CAMERA`, and added `CALL_CAMERA` and `CALL_VIDEO_SUB`
> (REQ-160, §5.17). Version 46 appended `channel_id` to each `AUDIT_PAGE` entry, the channel the entry
> happened in (REQ-251). Version 45 appended `audience` to `WORKSPACE_INFO`
> (CLIENT.md, "Links"). Version
> 44 appended `media_kind` to `UPLOAD_BEGIN`, so a snippet's text past its cap is
> refused before it is sent (REQ-226). Version
> 38 appended `remind_at_ms` to `SAVE_ITEM`, `SAVED_UPDATED` and `SAVED_MSG`, a
> saved item's reminder, and added the activity kind for one that fired (REQ-283). Version
> 37 appended `follow_threads` to each `NOTIFY_PREFS` entry (§5.12, REQ-282). Version
> 36 put `also_channel` in `SEND_REPLY` and `THREAD_REPLY` and `parent_id` in
> `BROADCAST`, each in the fixed part ahead of the body: a thread reply can also
> be posted to the channel's main scroll (REQ-062, §5.10). Version
> 35 appended `thread_root` to `TYPING` and `TYPING_UPDATE`, so typing shows in
> the thread it is in (§5.13, REQ-121). Version
> 34 appended `n_repliers` and the repliers' ids to `THREAD_META`, which is now
> also sent live when a reply is posted or deleted (§5.10). Version
> 33 appended `family`, `actor_id` and `action` to `AUDIT_QUERY`, filters the
> daemon applies (§5.16c). Version
> 32 gave the byte version 31 put in `BROADCAST` (and so `MSG_RESTORED`) and
> `THREAD_REPLY`, ahead of the body, a new meaning: `broadcast_stored`, whether
> the send stored a broadcast mention for the message, whatever its body says
> now (§5.3). A frame whose layout is unchanged but whose meaning is not moves
> the number too. Since the
> client and daemon ship together (ARCH-61) there is no compatibility window to
> preserve — only a mismatch to detect loudly, which is why a frame *layout*
> change moves the number even when it adds one byte.

### 2.1 Size limits (ARCH-30, REQ-054)

- `MAX_FRAME_SIZE` = **66,560 bytes** (65 KiB) total on the wire
  (`4 + length`). A peer that reads a `length` implying a larger frame closes
  the connection before reading the payload (the daemon writes no `ERROR`;
  `FRAME_TOO_LARGE` is reserved for it).
- `MAX_BODY_SIZE` = **65,536 bytes** (64 KiB) for a message body (REQ-054).
  The 1 KiB gap between body and frame limits is framing/field-header
  headroom, reconciling REQ-054's "~64KB body" with ARCH-30's "~64KB frame."
- `MAX_ATTACHMENT_SIZE` = **200 MiB** (the cap, REQ-140; nothing varies it). A file's *bytes* are never in SQLite (they go to object
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
first frame is answered with **`REJECT` carrying `UNEXPECTED_MSG_TYPE`** and the
connection is closed. `REJECT` rather than `ERROR` because no version has been
negotiated yet, and `REJECT` is frozen at version 1 — the one frame a peer of any
era can read.

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
| `client_info` | str  | Free-form build/platform string, for logging only (e.g. `openchime-client/0.1`). |

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

After `WELCOME`, the daemon sends `AUTH_CHALLENGE` listing the sources it signs
people in with; the client MUST authenticate before sending any messaging frame. A
messaging frame received before `AUTH_OK` is answered with `ERROR AUTH_REQUIRED`
(fatal). A refused `AUTH` is a fatal `ERROR` and the daemon closes the connection
after it; a connection not signed in within 60 seconds of being accepted is
closed, the 60 seconds starting again at each answered poll of a device code
that is still live (§4.2c). Full design in [AUTH.md](./AUTH.md); the exchange is its §8.1.

### 4.1 `AUTH_CHALLENGE` (server → client), msg_type `0x0012`

Sent immediately after `WELCOME`. One entry per identity source (ARCH-55), so the
client draws one control per entry. Resuming a session is always accepted and is
not listed.

| Field       | Type | Notes                                                          |
|-------------|------|----------------------------------------------------------------|
| `n_sources` | u8   | At most 8; a larger count is a malformed frame.                |
| `id`        | str  | Per source. What `AUTH` and `AUTH_BEGIN` name it by: `local`, `relay`, `oidc-1` to `oidc-4`. |
| `kind`      | u8   | Per source. `1` local (username + password), `2` relay, `3` oidc (the operator's own provider). Kinds `2` and `3` are browser sign-ins (§4.2a). |
| `label`     | str  | Per source. Text for the control.                              |

### 4.2 `AUTH` (client → server), msg_type `0x0010`

A `method` discriminator selects the credential the payload carries (ARCH-59 for
`local`, ARCH-56/57 for `oidc`, ARCH-58 for `session`).

| Field        | Type | Notes                                                          |
|--------------|------|----------------------------------------------------------------|
| `method`     | u8   | `0x01` local, `0x02` oidc, `0x04` session.                     |
| `source`     | str  | The `id` of the challenge entry being used; empty for `session`. |
| `credential` | lstr | Method-specific, bounded by `MAX_BODY_SIZE`: **local** — never sent in a frame (the method is refused; a local account signs in on the daemon's page and presents its token as **oidc** with source `local`); **oidc** — an ES256 JWT: the central-issued one (AUTH.md §8.3), or with source `local` the daemon's own (AUTH.md §8.10); with an `oidc-<n>` source, the authorization code the provider sent back (AUTH.md §8.5); **session** — the 32-byte session token from a prior `AUTH_OK`. |
| `proof`      | str  | **oidc** — the verifier whose hash the client sent as `challenge` in `AUTH_BEGIN` (AUTH.md §8.2). Empty otherwise. |
| `state`      | str  | **oidc** with an `oidc-<n>` source — the `state` the provider sent back, naming the sign-in `AUTH_BEGIN` began. Empty otherwise. Since version 22. |

The daemon rejects on any mismatch with an `ERROR`: `AUTH_INVALID_TOKEN`
(bad/expired/wrong-audience/replayed token, a verifier that is not the token's, or
a bad password), `AUTH_RATE_LIMITED` (too many failed attempts from this address,
REQ-191), `AUTH_NOT_ALLOWED` (a valid identity no join rule or invite admits), or
`AUTH_REQUIRED` (a source this deployment does not offer). The `oidc` credential
is a JWT some issuer the daemon trusts signed — the relay's, verified against its
pinned keys (AUTH.md §3), or the daemon's own for a local account (AUTH.md
§8.10) — or, for a direct connection, a code the daemon exchanges itself; never a
raw *provider* token from the client. A direct connection's `state` matching no
sign-in, or a verifier that is not its challenge's, is `AUTH_INVALID_TOKEN` and the
code goes nowhere; a provider that cannot be reached is
`AUTH_SOURCE_UNAVAILABLE`. A **local** credential, a password in the
frame, is refused with `AUTH_SOURCE_UNAVAILABLE` (fatal), in every build: a
local account signs in in the browser (AUTH.md §8.10).

### 4.2a `AUTH_BEGIN` (client → server), msg_type `0x0015`, and `AUTH_REDIRECT` (server → client), msg_type `0x0016`

How a browser sign-in starts. Pre-auth, and nothing is kept on the connection: the
client may disconnect while the person is in their browser and present `AUTH` on a
new one.

| Field          | Type | Notes                                                       |
|----------------|------|-------------------------------------------------------------|
| `source`       | str  | The challenge entry's `id`.                                 |
| `redirect_uri` | str  | Where the browser is sent back. **Loopback** (RFC 8252): `http://127.0.0.1`, `http://localhost` or `http://[::1]`, an optional port, an optional path; or, where the daemon serves the web client, that client's own `https://<host>/app/` on the host the page was loaded from. Anything else is refused. |
| `challenge`    | str  | `base64url(SHA-256(verifier))`, 43 characters, of a random verifier the client keeps. |

The daemon answers `AUTH_REDIRECT`:

| Field           | Type | Notes                                                      |
|-----------------|------|------------------------------------------------------------|
| `authorize_url` | str  | For a relay source or a direct connection, the whole URL to open; the client opens it only if it is `https` (plain `http` to loopback, for development), and neither builds nor parses it. For the `local` source, a **path** on the daemon's own sign-in pages, `/signin?redirect_uri=…&nonce=…`: the client opens it at the daemon's own `https` origin when a trusted authority vouched for its certificate, and through its loopback tunnel otherwise (AUTH.md §8.10). |

or a non-fatal `ERROR`: `AUTH_SOURCE_UNAVAILABLE` (no such source, or the box is
enrolled nowhere to send a browser), `AUTH_INVALID_TOKEN` (a `redirect_uri` that is
neither loopback nor the web client's, or a `challenge` of the wrong shape), or,
for a direct connection, `AUTH_RATE_LIMITED` (16 sign-ins already under way from
this address, or 256 in all).

### 4.2b `AUTH_CONTINUE` (server → client), msg_type `0x0017`

Reserved for a first step that is correct but not sufficient. The daemon sends
none: a local account's sign-in, and any step after it, runs on the daemon's own
pages (AUTH.md §8.10).

| Field  | Type | Notes                              |
|--------|------|------------------------------------|
| `step` | u8   | What is wanted next: `1` a TOTP code. |

### 4.2c A device code: `AUTH_DEVICE_BEGIN` `0x0018`, `AUTH_DEVICE` `0x0019`, `AUTH_DEVICE_POLL` `0x001A`, `AUTH_DEVICE_TOKEN` `0x001B`

How a client with no browser of its own signs in to a local account (AUTH.md
§8.11). Pre-auth, and nothing is kept on the connection: the daemon keeps the
request, and the client may poll from a new connection.

| Frame | Direction | Fields |
|---|---|---|
| `AUTH_DEVICE_BEGIN` | C → S | `source` str (`local`), `challenge` str (as `AUTH_BEGIN`'s) |
| `AUTH_DEVICE` | S → C | `device_code` str (the client's secret), `user_code` str (`XXXX-XXXX`), `verification_path` str (`/device`), `interval_s` u16, `expires_in_s` u16 |
| `AUTH_DEVICE_POLL` | C → S | `device_code` str |
| `AUTH_DEVICE_TOKEN` | S → C | `token` lstr — presented next on `AUTH{oidc, local}` with the verifier |

`AUTH_DEVICE_BEGIN` is refused with `AUTH_SOURCE_UNAVAILABLE` (no local
accounts), `AUTH_INVALID_TOKEN` (not a challenge) or `AUTH_RATE_LIMITED` (five
already waiting from this source, or the table full). A poll not yet approved is
`AUTH_PENDING`; one sooner than the interval `AUTH_SLOW_DOWN`, and the interval
grows by five seconds; a refusal `AUTH_DENIED`, once; a code that ran out, was
collected or never was `AUTH_EXPIRED`. All non-fatal.

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

After `AUTH_OK` the daemon sends, in order: `WORKSPACE_INFO`,
`WORKSPACE_ADDRESS` (when set), `SERVER_BUILD`, `CAPABILITIES`, `TTS_INFO`,
`STT_INFO`, `SNOOZE`, `ALERTS_SUMMARY` (owner/admin), `PROFILE_FIELDS`
(always, empty when the workspace has none), any `SUMMARY_READY`
notices, one `CALL_STATE` per call, the caller's encrypted-DM state, then the
presence snapshot.

### 4.3a `WORKSPACE_INFO` (server → client), msg_type `0x0014`

Pushed immediately after `AUTH_OK` (before the presence snapshot). Carries the
daemon's static workspace facts, sourced from infra config (env, read once at
boot — see `daemon/config.h`), so a client can render a branded header instead of
a bare host address. Clients that don't recognize the frame ignore it.

| Field             | Type | Notes                                                       |
|-------------------|------|-------------------------------------------------------------|
| `deployment_mode` | u8   | `0` self-hosted, `1` managed (ARCH-76).                     |
| `max_users`       | u32  | Registered-user cap; `0` = unlimited.                       |
| `workspace_name`  | str  | Admin-set display name; **empty** ⇒ the client falls back to the connection host's subdomain (e.g. `acme.openchime.io` → "acme"). |
| `call_max`        | u8   | The most people in one call, ten (`OC_CALL_MAX`, REQ-305) — so a client starting one in a large channel knows when the starter must pick (§5.17). |
| `mode`            | u8   | `OPENCHIME_MODE`: `0` normal, `1` development, `2` demo. A client in a public demo (`2`) says so for as long as it is open (AUTH.md §8.12). Appended in version 25. |
| `max_video_size`  | u64  | The most bytes a video message may hold (`OPENCHIME_MAX_VIDEO_SIZE`, 160 MiB by default, at most `MAX_ATTACHMENT_SIZE`; REQ-164). A client's recorder stops 5 MiB short of it, and `ATTACH_MEDIA_SET` refuses a larger video with `MEDIA_TOO_LARGE` (§5.14a). Appended in version 27. |
| `audience`        | str  | The workspace's audience id with OpenChime's services (AUTH.md §3.3), **empty** when it is not enrolled. An email link names the workspace by it (CLIENT.md, "Links"). Appended in version 45. |

### 4.3b `WORKSPACE_ADDRESS` (server → client), msg_type `0x001C`

Pushed right after `WORKSPACE_INFO` (and `WORKSPACE_SETTINGS`), and only where
the workspace has a canonical address: the deployment's
(`OPENCHIME_MANAGED_ADDRESS`), or, on a self-hosted daemon bound to central, the
name it took from the certificate central issued it (TLS.md, the `central`
source). A managed workspace that moved to a new address still answers at its
former one for a while, and a self-hosted one keeps answering at its IP address;
a client that connected by either files the workspace under the address it is
sent (CLIENT.md §5) and dials that next time. A new frame, so no version bump.

| Field     | Type | Notes                                             |
|-----------|------|---------------------------------------------------|
| `address` | str  | The workspace's address now, e.g. `acme.workspace.openchime.io`. |

### 4.3c `SERVER_BUILD` (server → client), msg_type `0x0106`

Pushed after `WORKSPACE_INFO` (and `WORKSPACE_ADDRESS`, when that is sent), on
every sign-in. The daemon's build id (docs/details/UPGRADING.md): the short commit it
was built from, or `dev` from a source build. A client shows it beside its own
in About OpenChime (CLIENT.md), so a person reporting a problem reads both builds
off one screen. Sent only once signed in, so an address alone does not learn
which build to probe. A new frame, so no version bump; a client talking to a
daemon too old to send it says the daemon did not report one.

| Field   | Type | Notes                                      |
|---------|------|--------------------------------------------|
| `build` | str  | The daemon's build id, e.g. `74359c0`.     |

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
continue); with `scope=1`, every other connection of the user's too.

---

## 5. Messaging (REQ-050, REQ-090, REQ-092, REQ-093; edit/delete REQ-051/052/032)

### 5.1 `SEND` (client → server), msg_type `0x0020`

| Field                | Type   | Notes                                                                 |
|----------------------|--------|-----------------------------------------------------------------------|
| `channel_id`         | u64    | Target channel or DM conversation.                                    |
| `idempotency_token`  | 16 B   | Client-generated random 128-bit token (§7 `idem`). Enables safe retry (REQ-093). Distinct from the server message id. |
| `body`               | lstr   | Message body, u32-length-prefixed UTF-8 (§7), `<= MAX_BODY_SIZE` (REQ-054). |
| `src_channel`        | u64    | Forward source channel (REQ-057), `0` when the message forwards nothing.  |
| `src_message`        | u64    | Forward source message (REQ-057), `0` when the message forwards nothing.  |
| *(optional tail)*    | u16 + u64[] | Attachment ids to link (REQ-140), written only when present.        |

**Forwarding (REQ-057, ARCH-117).** The client sends only the two source ids.
The daemon resolves the source author, a truncated excerpt and the attachment
count from the row it already holds, and announces them on a `FORWARD` frame —
so a client cannot claim someone said something they did not. The two fields sit
in the **fixed part, ahead of the optional attachment tail**: a second block
sharing that tail is how a decoder loses its place (§2). The forwarder must be
able to read the source or no reference is stored; a source that is gone or
unreadable makes this an ordinary send rather than an error.

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

Errors: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER` (REQ-031),
`SEND_RATE_LIMITED` (REQ-190), `BODY_TOO_LARGE` (a body longer than
`MAX_BODY_SIZE`, refused before anything is stored, so no `SEND_ACK` or
`BROADCAST` follows) — all non-fatal `ERROR` frames carrying the
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
| `kind`         | u8   | `0` something someone said; `1` a call event the daemon wrote — the message a call started from a conversation leaves there, or a missed-call line stored before version 49 (§5.17). Any other value is malformed. |
| `broadcast_stored` | u8 | `1` when the daemon stored a broadcast mention (`@here`, `@channel` or `@everyone`) for the message when it was sent, so a broadcast its body holds notifies (REQ-221); else `0`, whatever the body. Any other value is malformed. Since version 32 (version 31 carried `broadcast_held` here). |
| `parent_id`    | u64  | The thread root of a reply also posted to the channel (REQ-062, §5.10), which a replay of the main scroll carries with it; `0` for a message that is not a reply. Since version 36. |
| `call_id`      | u64  | The call a call message is about (kind `1`, §5.17); `0` for any other message, and for a missed-call line from before version 49. Since version 49. |
| `via`          | str  | The AI client a person posted the message through (AUTH.md §8.13), which a client shows as "via <client>" beside the author; empty for any other message. Since version 51. |
| `body`         | lstr | Message body (§7).                           |

`broadcast_stored` is the daemon's answer to whether a broadcast mention
reached anyone, read from the mentions it stored when the message was sent, so
a replay (§6) says what the live frame said. It does not depend on the body:
an edit changes the body and stores no mentions (§5.5), so a message keeps the
answer its send got, in history loaded after an edit as on the live frame. A
client counts a broadcast as a mention when the current body holds one and
`broadcast_stored` is `1`: it does not guess from a channel's size, which it
knows only for the channel whose member list it has loaded.

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
nothing, so idle re-acks are silent. One naming the receiving user is that
user's own position moved by `SET_READ_CURSOR` (REQ-235), from this device or
another: the client puts its read position there, backwards included, and counts
its unread badge again.

**`GET_CHANNEL_DESCRIPTION` (C → S), `0x008C`** `{ channel_id: u64 }` and
**`CHANNEL_DESCRIPTION` (S → C), `0x008D`** `{ channel_id: u64, description:
str }` — a channel's long-form description (REQ-034, ARCH-93). It is set with
`UPDATE_CHANNEL` op `OC_CHUP_DESCRIPTION` (6), by any member (on a public demo,
an owner or admin; §5.7a), up to
`OC_MAX_DESCRIPTION` (1,000) bytes, with `""` clearing it — and it is **not**
carried on `CHANNEL_INFO` or `CHANNEL_LIST`, because the list is one frame for
every channel and text this long on each entry would stop it fitting.

`CHANNEL_DESCRIPTION` is both the answer to a fetch, to the asker only, and the
announcement of a change, to every connected member — the way `MSG_EDITED` is
both an edit's ack and its broadcast. An empty `description` means none is set.
A fetch is gated like any read: a public channel's to anyone in the workspace, a
private one's to its members. A private channel an outsider asks about gets
`ERROR` `UNKNOWN_CHANNEL`, which is also what a channel that does not exist gets:
a different answer would say it exists.

**`MARK_ALL_READ` (C → S), `0x008B`** — no body. Catch-up in one round trip (REQ-238):
the daemon advances every one of the caller's memberships to that channel's
newest message, in one job on the writer.

The target is the daemon's newest message **when the request arrives**, not the
client's view when it was sent, so a message that landed in between is included
rather than left as one stubborn unread the user already tried to clear. There
is no reply of its own: each channel that actually moved produces exactly the
`READ_CURSOR` fan-out a `CLIENT_ACK` advancing it would, and a channel already
read produces nothing — so a repeated catch-up is silent, for the same reason an
idle re-ack is. It does not replace `CLIENT_ACK`, which still says the narrower
thing entering a conversation means.

Added as a frame rather than as a layout change, so `OC_PROTOCOL_VERSION` does
not move; both peers advertise exactly one version, so a daemon that predates
the frame is refused at the handshake and never receives it (§3).

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

An edit notifies nobody and stores no mentions: the message keeps those its
send stored. So `MSG_EDITED` carries no `broadcast_stored` (§5.3) and a client
keeps the one the message had: a broadcast the edit removes counts no more,
and one a later edit puts back counts again where the send stored one.

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

**A delete can be taken back.** For two minutes the daemon holds what a delete
gave up — the body, the files' ids, the reactions — and the person who deleted
the message may ask for it back with **`RESTORE` (client → server), msg_type
`0x00FA`** `{ channel_id: u64, message_id: u64 }`. Nobody else may, not even an
owner, and not after the window (the hold is then purged). Every connected
member is sent **`MSG_RESTORED` (server → client), msg_type `0x00FB`**, which is
`BROADCAST`'s layout — the message whole, with its original time and its files —
and replaces the tombstone in place. Its reactions return too, seen at the next
history load; a pin, link previews and a poll do not come back (a delete drops
the poll). The deleter must still be a member of the channel. Refusals are the
same non-fatal `ERROR`s as below: `FORBIDDEN` for someone else's delete or a
deleter who has left the channel, `UNKNOWN_MESSAGE` once the hold is gone.

Errors for both `EDIT` and `DELETE` are non-fatal `ERROR` frames carrying the
offending `message_id` (8 bytes, big-endian) in `context` so the client can
correlate: `UNKNOWN_MESSAGE` (no such message in the channel, or it is already
tombstoned), `FORBIDDEN` (not the author — and, for delete, not a member of the
channel, or neither the author nor a moderator), or `CHANNEL_ARCHIVED` (a closed
encrypted DM).

### 5.7 Channel management (REQ-050, REQ-031, REQ-033)

Channels are the containers messages belong to. A channel is either **public**
(open to every tenant user) or **private** (members-only). Membership
(`channel_members`) is the fan-out set for `BROADCAST`/`MSG_EDITED`/`MSG_DELETED`
and the read/post gate:

- **Read** (backfill, §6) a channel: allowed if it is public **or** the user is
  a member (REQ-031). Private channels reveal nothing to non-members.
- **Post** (`SEND`/`SEND_REPLY`/`UPLOAD_BEGIN`, and minting a webhook): a member
  may always post; posting to a **public** channel the user has not joined
  **auto-joins** them (so subsequent broadcasts reach them); a **private**
  channel the user does not belong to rejects with `NOT_A_MEMBER`. A
  `channel_id` that does not exist rejects with `UNKNOWN_CHANNEL`.
- **Edit and delete** do not use that rule and never auto-join: `EDIT` is the
  author's alone, and `DELETE` answers anyone who is not a member of the channel
  with `FORBIDDEN` (§5.5/5.6).

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
| `peer_id`    | u64  | Always written (`0` when not a DM). For a 1:1 DM, the other participant *from the recipient's view*. |
| `topic`      | str  | Empty when unset (REQ-034). |
| `archived`   | u8   | `1` when the channel is archived and read-only (REQ-035). |
| `n_peers`    | u16  | Participant count, then that many `u64` ids — a group DM's people (REQ-056). `0` for a named channel. |
| `e2e`        | u8   | `1` for an encrypted DM (§5.19, PRIVATE-DM.md), whose message bodies the daemon cannot read; `0` otherwise. |
| `creator_id` | u64  | Who created the channel (since version 42); `0` when not known — every channel made before then, and every DM. |
| `n_managers` | u16  | The channel's managers (since version 42, REQ-036b), then that many `u64` ids, at most 64. `0` when it has none. |

Failures are non-fatal `ERROR` frames carrying the `channel_id` (8 bytes,
big-endian) in `context`: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER`, `FORBIDDEN`,
`MEMBER_VIA_GROUP` (a leave or removal of someone in the channel only through a
group, §5.16k), or `INVALID_CHANNEL` (empty/oversized name on create).

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
| `count`     | u16             | Number of entries **in this frame**.              |
| `entries[]` | `count` × entry | See below. |

**A list is as many `CHANNEL_LIST` frames as it takes.** Each carries at most
`OC_CHANNEL_LIST_PAGE` (256) entries, and fewer when that many would not fit
`OC_MAX_FRAME_SIZE`; a client upserts every entry it receives, so the frames
merge and there is no terminator to wait for. The page bound is the receiver's:
a client decodes one frame into an array of that size and drops anything past
it, so a daemon sending more is how a sidebar silently loses channels. Both
sides read the one constant.

> A frame that fails to encode is **taken back out of the send buffer**
> (`oc_frame_end`), so a caller that does not check the result sends nothing
> rather than a frame whose length field was never written.

Each entry, in wire order:

`channel_id` (u64) · `name` (str) · `is_public` (u8) · `joined` (u8) ·
`kind` (u8 — `0` channel, `1` DM) · `last_message_at` (u64) · `unread` (u32) ·
`peer_id` (u64) · `topic` (str) · `archived` (u8) · `created_at` (u64) ·
`preview` (str) · `preview_author` (u64) · `preview_action` (u8) · `n_peers` (u16)
followed by `n_peers` × `u64` participant ids · `e2e` (u8, as `CHANNEL_INFO`'s).
`preview_action` is `1` when the
newest message is an action (REQ-058, §5.16l): `preview` is then its action
text, read beside `preview_author`. An encrypted DM's `preview` is always empty
and its `preview_action` `0`: its bodies are ciphertext.

Nothing here is optional: every field is written for every entry, so a decoder
that stops early desynchronises the whole repeated list rather than losing one
row. The participant list carries a group DM's people (REQ-056) and is
`0`-length for anything else.

The list includes the caller's DMs (§5.12) alongside channels; DM entries have
`kind=1`, an empty `name`, and `is_public=0`.

**`JOIN_CHANNEL` (client → server), msg_type `0x0054`** `{ channel_id: u64 }` —
public channels are self-joinable; a private channel rejects with `FORBIDDEN`
unless the caller was already invited. Replies `CHANNEL_INFO` (`joined=1`).

**`LEAVE_CHANNEL` (client → server), msg_type `0x0055`** `{ channel_id: u64 }` —
drops the caller's membership (idempotent); refused with `MEMBER_VIA_GROUP`
when they are in only through a group (§5.16k). Replies `CHANNEL_INFO`
(`joined=0`).

**`INVITE_TO_CHANNEL` (client → server), msg_type `0x0056`**
`{ channel_id: u64, user_id: u64 }` — any existing **member** may add another
user (REQ-033, channel-level, not gated to admins); this is how a private
channel becomes reachable. The actor gets a `CHANNEL_INFO` ack, and the invited
user's live connections are **pushed** a `CHANNEL_INFO` (`joined=1`) so their
client learns of the new channel immediately. An actor who is not a member gets
`NOT_A_MEMBER`; an unknown or removed target, `FORBIDDEN`.

**`REMOVE_FROM_CHANNEL` (client → server), msg_type `0x0057`**
`{ channel_id: u64, user_id: u64 }` — any existing member may remove another
(REQ-033). Replies `CHANNEL_INFO` to the actor. An actor who is not a member
gets `NOT_A_MEMBER`.

**`CHANNEL_ADD_MANAGER` (client → server), msg_type `0x013B`** and
**`CHANNEL_REMOVE_MANAGER` (client → server), msg_type `0x013C`**
`{ channel_id: u64, user_id: u64 }` — a channel's **managers** (REQ-036b, ARCH-134).
The creator is the first, made so by `CREATE_CHANNEL`. A manager of the channel,
an admin or an owner names another and takes the role from any manager, the
creator included; anyone else is refused with `FORBIDDEN`. Only a member of the
channel can be named (`NOT_A_MEMBER` otherwise), and at most 64
(`INVALID_CHANNEL` past that). Naming someone already a manager, or removing
someone who is not, succeeds and changes nothing. Leaving the channel or being
removed from it ends the role; rejoining does not restore it. A DM has no
managers (`INVALID_CHANNEL`). On success the actor gets `CHANNEL_INFO`, and every
other member the same frame, naming the managers now. Each change is audited
(`channel.manager.grant`, `channel.manager.revoke`).

Every tenant has one auto-provisioned public **`general`** channel (id `1`) that
every user joins at authentication, so the messaging path always has a channel
to deliver to. On a managed deployment (started with `OPENCHIME_MANAGED_TICKET`) the
daemon creates it at its first boot with a welcome in its topic and description —
the ordinary columns, nothing authored, no system user; a `general` that already
exists is never rewritten, and a self-hosted daemon creates it bare.

### 5.7a Changing a channel: topic, rename, archive (REQ-034/035/036, ARCH-93)

**`UPDATE_CHANNEL` (client → server), msg_type `0x005F`:**

| Field        | Type | Notes                                                    |
|--------------|------|----------------------------------------------------------|
| `channel_id` | u64  | The channel to change.                                   |
| `op`         | u8   | `0` set-topic · `1` rename · `2` archive · `3` unarchive · `4` make private · `5` make public · `6` set description (§5.4). |
| `value`      | str  | The new topic (≤250 bytes, empty clears it), name (≤64) or description (≤1,000). Ignored for archive/unarchive and private/public. |

One frame for every verb because they all mutate one row and all fan out the same
state — the op *is* the difference.

**Authority** splits on blast radius: **any member may set the topic or the
description** (already visible to the channel, corrected in seconds); **owner/admin,
or a manager of the channel** (§5.7), for rename, archive and unarchive, and
**owner/admin only** for a change of visibility: they change what people *not*
looking at the channel see. On a public demo (AUTH.md §8.12) the topic
and the description are an owner's or admin's too — a demo's channels say what the
demo is — and a member's attempt is `FORBIDDEN`.

On success the daemon replies **`CHANNEL_INFO`** to the actor and **fans the same
frame to every other member** — a rename or archive changes what everyone's
sidebar should say. `CHANNEL_INFO` therefore carries `topic` and `archived`.
Op `6` is the exception: it sends **`CHANNEL_DESCRIPTION`** (§5.4), not
`CHANNEL_INFO`, to the actor and every other member — the description does not
ride the channel-state frame, so a sidebar has nothing to redraw.

`CHANNEL_LIST` entries also carry a **`preview`** (the newest top-level message,
truncated to 120 bytes, tombstones skipped) and its **`preview_author`**. A client
that caches nothing (ARCH-88) has no other way to show a conversation list you can
skim — and it is per *channel*, not per DM, so the ordinary sidebar can use it too.

> **Layout note.** `CHANNEL_INFO`'s layout is **fixed** — `peer_id` is always
> written (0 when not a DM), followed by `topic` and `archived` — because an
> *optional trailing* field does not survive a second optional field beside it.
> `CHANNEL_LIST` entries carry `topic`, `archived` and `created_at` for the same
> reason a client that caches nothing (ARCH-88) must be able to render the
> sidebar and the channel's About surface from the list alone.

**An archived channel takes no new posts.** `SEND`, `SEND_REPLY`,
`UPLOAD_BEGIN`, `CREATE_WEBHOOK` and `EDIT_POLL` share one access check and all
return `CHANNEL_ARCHIVED` (3019), as does free-talk voice input. Changes to what
is already there are not refused: `EDIT`, `DELETE`, `REACT`, a vote and `PIN`
still work in an archived channel (the first four return `CHANNEL_ARCHIVED` only
in a closed encrypted DM).
The incoming-webhook post cannot use that check — it is the one writer with no
user behind it, and going through it would also re-test the creator's
membership — so it tests the same archived helper directly and answers
**`403 Forbidden`**. Deliberately not `404`: the token is real and the sender is
entitled to use it, and "not found" would tell an integration its token had been
revoked. The webhook itself is untouched, so unarchiving restores it. It is
hidden from `CHANNEL_LIST` for
non-members; members keep it, flagged, so they can find their way back in.
History, search and membership are untouched — archiving is the reversible
alternative to a deletion that is not offered for channels holding history.

Failures are non-fatal `ERROR` frames: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER`,
`FORBIDDEN` (rename/archive by someone neither an admin nor a manager of the
channel, or a change of visibility by a non-admin), `CHANNEL_EXISTS` (the new name is
taken — the same unique-name rule as create, §5.7), or `INVALID_CHANNEL` (empty
or oversized name, oversized topic, or **any of these attempted on a DM** — a DM
has no name to rename and archiving one is a different feature).

### 5.8 Tenant administration (REQ-030, REQ-033)

Managing the tenant's people: enumerating them, changing roles, and adding or
removing members. Role changes and member add/remove are gated by the tenant
role policy (owner/admin/member, ARCH-60, AUTH.md §6); enumeration is open to any
authenticated user.

**`LIST_USERS` (client → server), msg_type `0x0040`** — empty payload; replies
**`USER_LIST` (server → client), msg_type `0x0041`**:

| Field       | Type            | Notes                                                        |
|-------------|-----------------|--------------------------------------------------------------|
| `count`     | u16             | Number of entries: at most 512 (`OC_USER_LIST_MAX`), with no paging; a workspace with more users gets the first 512. |
| `entries[]` | `count` × entry | See below. |

Each entry, in wire order:

`user_id` (u64) · `role` (u8) · `disabled` (u8) · `email` (str) ·
`display_name` (str) · `avatar_id` (u64) · `title` (str) · `timezone` (str) ·
`status_emoji` (str) · `status_text` (str) · `full_name` (str) ·
`pronouns` (str) · `voice_id` (str) · `flags` (u8, since version 30) ·
`ooo_start` (u64) · `ooo_end` (u64) · `ooo_message` (str) (the three since
version 41).

`ooo_*` is the person's **out of office** (`SET_OUT_OF_OFFICE`, §5.21), as
stored: in force while `ooo_start` ≤ now < `ooo_end`, which the client decides,
so the mark goes at the end time with nothing sent. `ooo_end` 0 means none.

`flags` bit `0x01` (`OC_USER_SYSTEM`) marks **the workspace's own sender**: the
account that posts what the workspace says, such as the welcome a new member is
sent (`OPENCHIME_WELCOME_MESSAGE`). It is not a person. A client draws its
messages as the workspace's, with no avatar of a person, keeps it out of every
list of people (the roster, the directory, a member picker, mention
completion), and offers no DM, profile or role action on it. Nothing signs in
as it, and a message sent into its DM is refused with `WORKSPACE_SENDER`.

`flags` bit `0x02` (`OC_USER_AGENT`) marks **an agent** (§5.16p): an account
that acts only through a token. A client shows an **Agent** tag beside its name
wherever a name appears, lists it below the people in a channel's member list,
and shows no presence, status or local time for it. A new bit in the same byte,
so the entry's layout and the version are unchanged; a client that does not
know it shows the agent as a person.

A **phone number is deliberately absent**: it is contact detail rather than
something drawn beside a name, and every member's number in every roster
fan-out is a size and privacy cost with nothing asking for it. `PROFILE_INFO`
carries it to whoever opened the card. `voice_id` **is** here: everyone hears the
same voice for the same author (REQ-292), so it is a name decoration like the
pronouns beside it, and empty means the daemon has not chosen one yet.

The daemon also sends a `USER_LIST` unasked, to every signed-in connection, the
moment an account is created — an invitation redeemed, a first sign-in through a
provider, an account made on the daemon's pages: a list of one, the new member.
A client folds every `USER_LIST` into its roster entry by entry, so this adds the
newcomer to every open People list and member picker without waiting for a
`LIST_USERS`.

Because this is a repeated list, an added field shifts **every** entry after the
first — which is why adding one always raises the protocol version rather than
riding along. `title`, `timezone` and the status fields are here, and not only on
`PROFILE_INFO`, because that frame is sent to the person who edited them: the
people directory (REQ-289) and everyone else's profile card need them from the
roster.

**`SET_ROLE` (client → server), msg_type `0x0042`** `{ user_id: u64, role: u8 }` —
changes a user's tenant role. Enforced by the role policy: only owner/admin may
change roles, only an owner may grant/revoke owner, an admin may only
promote/keep members; demoting the last owner is refused (`LAST_OWNER`, REQ-030).
On success the actor is acked with **`USER_UPDATED` (server → client), msg_type
`0x0045`** `{ user_id: u64, role: u8, disabled: u8 }`, and the same frame is
pushed to the affected user's connections, so their client updates its
capabilities immediately. Nobody else is told: other clients see the new role
at their next `LIST_USERS`. (Removing a user and bringing one back, below, are
pushed to every signed-in connection.)

**`INVITE_USER` (client → server), msg_type `0x0043`** `{ role: u8, email: str, uses: u16 }` — owner/admin
only (only an owner may invite at admin/owner role). With `email` empty it mints an
invite token for a **new** local account; `uses` is how many may redeem it
(REQ-026): `1` for one person, `N`, or `0` for no limit until it expires. More
than one makes a link to pass around, which is member-role only: an admin or
owner invite with `uses` other than `1` is refused with `FORBIDDEN`. With `email` set the invite
is bound to that address instead (AUTH.md §8.4): it is spent by that address's
first verified sign-in through a provider, no token can redeem it, and the
`token` that comes back is 32 zero bytes. The `email` must have the shape of an
address — one `@`, a local part of at most 64 bytes with no spaces, controls or
list separators, a domain of two or more dot-separated labels, 254 bytes in all
(`oc_email_plausible`, which clients share) — or the invite is refused with
`FORBIDDEN`. An invite bound to an address needs a
source that could spend it, a provider sign-in through the relay or a direct
connection: where the deployment offers neither, it is
refused with the non-fatal `ERROR INVITE_UNREDEEMABLE` (3031), whose message says
to invite with a token instead. A client reads which kinds are possible from
`AUTH_CHALLENGE`: a `local` source redeems a token, a browser source an address.
Either way the reply is
**`INVITE_CREATED` (server → client), msg_type `0x0046`**:

| Field        | Type  | Notes                                                       |
|--------------|-------|-------------------------------------------------------------|
| `token`      | bytes | The 32-byte invite token (only its SHA-256 is stored). The actor shares it out-of-band. |
| `role`       | u8    | The role the redeemed account will receive.                 |
| `expires_at` | u64   | Ms since epoch UTC after which the token is dead.           |
| `uses`       | u16   | How many may redeem it: `1`, `N`, or `0` for no limit (REQ-026). An invite bound to an address is always `1`. |

**Redeeming an invitation** is not a frame: the invitee, who has no account
yet, opens the daemon's sign-up page with the token (`/signin?invite=<hex>`,
AUTH.md §8.10), which takes one of its uses (the last consumes it), creates the
account with the invite's role and ends with the token the client presents on
`AUTH`. The daemon's own web client opened with the token
(`/app/?invite=<hex>`, WEB.md) is the same page reached from a link. A bad,
expired or consumed token, or a taken username, shows the form again,
non-disclosing by design. (`0x0047 REDEEM_INVITE` once carried this in a frame;
the number is retired.)

**`RESET_CREDENTIAL` (client → server), msg_type `0x001D`** `{ user_id: u64, clear_factors: u8 }`
— an owner or admin makes a one-time link for a local account (AUTH.md §2). Who
may reset whom is who may remove whom; a provider's account cannot be reset
here. `clear_factors` 1 also turns the account's second step off when the link
is used. Answered with **`CREDENTIAL_RESET` (server → client), msg_type
`0x001E`** `{ user_id: u64, token: str }`, the token in hex, which the page
`/account/reset?t=<token>` takes; or a non-fatal `ERROR` `FORBIDDEN`.

**`ENABLE_USER` (client → server), msg_type `0x001F`** `{ user_id: u64 }` — bring a
removed member back (REQ-033): who may do it is who may remove them, and only a
removed member, and they take a seat as a new member would (`USER_LIMIT` at the
cap). Nothing removal dropped comes back — channels, groups, drafts — but the
default channel at their next sign-in; their DMs and group DMs never went. Answered with `USER_UPDATED`
(`disabled=0`) and, for a local account, whose password removal took, a
`CREDENTIAL_RESET` with the link that sets one; or a non-fatal `ERROR`.

**`REMOVE_USER` (client → server), msg_type `0x0044`** `{ user_id: u64 }` —
owner/admin only; an admin cannot remove an admin/owner, and the last owner
cannot be removed (`LAST_OWNER`). The member is **deactivated**, not deleted.
What goes is the account's reach: their sessions, local password, push device
tokens, drafts and scheduled messages are dropped, they leave every channel and
group, their webhooks are turned off and post nothing while they are removed,
and a `disabled` flag bars every future login. What stays is everything anyone
said: every message, file, reaction and pin, and every DM and group DM they were
in, whole, with them still a member, so the others keep the conversation and
`OPEN_DM` with them finds it again. Nobody can start a new DM or group DM with a
removed member or add them to a channel (`FORBIDDEN`, as for an unknown user).
Clients show them as **(deactivated)**, from `USER_LIST`'s and `USER_UPDATED`'s
`disabled`. Every signed-in connection receives `USER_UPDATED` (`disabled=1`),
the actor's as its ack; the removed user's live connections receive it and are
then dropped.

**`DEACTIVATE_SELF` (client → server), msg_type `0x0115`**, no payload — a
person deactivates their own account. The result is exactly a removal's: the
same rows dropped and kept, the same `USER_UPDATED` (`disabled=1`) to every
signed-in connection, and every connection of theirs, this one among them,
dropped. The audit log records it as their own act (`account.deactivate_self`,
beside a removal's `user.remove`). The workspace's last active owner is refused
with `LAST_OWNER`: another owner is needed first. The way back is a removed
account's, `ENABLE_USER` by an owner or admin.

`SET_ROLE`/`INVITE_USER`/`REMOVE_USER`/`DEACTIVATE_SELF` failures are non-fatal
`ERROR` frames (`FORBIDDEN`, `LAST_OWNER`, `INVITE_UNREDEEMABLE`, `INTERNAL`).

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

A connection's reactions are limited to 30 per 10 seconds; one past the limit is
refused with a non-fatal `SEND_RATE_LIMITED` carrying the `message_id`
(big-endian) in `context`.

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
msg_type `0x002B`** with the reactor list for inspection (REQ-071), at most
1024 entries (`OC_REACTION_LIST_MAX`):

| Field        | Type            | Notes                                             |
|--------------|-----------------|---------------------------------------------------|
| `message_id` | u64             | The inspected message.                            |
| `count`      | u16             | Number of entries.                                |
| `entries[]`  | `count` × entry | Each: `emoji` (str) + `user_id` (u64), ordered by emoji then user. |

Failures are non-fatal `ERROR` frames carrying the `message_id` (8 bytes,
big-endian) in `context`: `UNKNOWN_MESSAGE` (no such message, or, for
`REACT`, it is tombstoned — a tombstone's reactions are cleared, §5.6, so
`LIST_REACTIONS` on one answers an empty `REACTIONS`), `NOT_A_MEMBER` (cannot
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
| `body`        | str  | The message body — an action's text when `action` is `1`. |
| `attach_name` | str  | The first attachment's filename, or empty. An attachment-only message has no body at all, so without this a pinned file rendered as a blank row. |
| `action`      | u8   | `1` when the message is an action (REQ-058, §5.16l).     |

**`PINS` (server → client), msg_type `0x0039`** `{ channel_id: u64, count: u32 }`
terminates the response; `count` is how many `PINNED_MSG` frames preceded it.

Tombstoned messages are excluded from the list and lose their pin outright
(§5.6): there is nothing left to pin to.

**Pin state is replayed on backfill.** A `BROADCAST` has no field for it, so
after replaying a channel's messages the daemon emits a `PIN_UPDATED` for each
pinned one (§6) — state that only travels on a live fan-out silently vanishes on
reconnect, the same failure the reaction replay exists to prevent. The
history-paging path (§6.3) replays it too, so a pin survives a scroll-back and a
permalink as well as a reconnect.

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
in. Enumerating a channel **requires being able to read it** — being in it, or
its being public — as reading its messages does; otherwise this is a way to
discover who is in a private channel you were never invited to
(`NOT_A_MEMBER`). `LIST_FILES` and `LIST_PINS` follow the same rule.

**`LIST_FILES` (client → server), msg_type `0x003D`**
`{ channel_id: u64, before_ms: u64, before_id: u64 }` streams
**`FILE_ENTRY` (`0x003E`)** newest-first, then **`FILES` (`0x003F`)**
`{ channel_id, count: u32, more: u8 }`.

A page holds at most 200 rows (the most the frame budget allows; in development
mode `OPENCHIME_DEV_FILE_PAGE` may shorten it). `before_ms`/`before_id` are the **keyset cursor**: the
`created_at` and `attachment_id` of the last row the client already has, and
`0`/`0` for the first page. The daemon returns the rows strictly older than that
pair — `(created_at_ms, id) < (before_ms, before_id)`, the row-value comparison
the newest-first index serves directly — so a page is not disturbed by uploads
arriving between requests, as an offset would be. `more` says whether a further
page exists, which the daemon knows by asking for one row beyond the page and
not sending it. Search pages the same way (§5.11).

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
| `media_kind`    | u8   | `1` for a video message (§5.14a), else `0` — so the view can list it as video. |
| `duration_ms`   | u32  | A video message's length; `0` otherwise.              |

**`channel_id` 0 means "every channel I am a member of"** — the same query with a
membership filter instead of a channel filter, which is what makes a
workspace-wide files view free rather than a second op.

Pending uploads (`message_id` NULL, §5.14) are **excluded**: an upload that never
reached a message was never shared with anyone.

Both lists are refreshed on open and never cached — a client holds nothing on
disk (ARCH-88), and both change from other clients.

### 5.9c Saved items and the activity feed (REQ-231, REQ-139, ARCH-95)

Both are **per-user** surfaces, and neither fans out to anyone else.

**`SAVE_ITEM` (client → server), msg_type `0x0062`** `{ message_id: u64, op: u8, remind_at_ms: u64 }`
(`1` save, `0` unsave). You may save anything in a channel you are a member of
(`NOT_A_MEMBER` otherwise, even for a public channel you can read; `FORBIDDEN`
in an encrypted DM); a tombstone cannot be saved. Replies **`SAVED_UPDATED` (`0x0063`)** `{ message_id, op, saved_at, remind_at_ms: u64 }` **to
each of the actor's connections and nobody else's** — a personal bookmark has no
one else to announce it to, and their other devices show it. Saving twice
reports the **original** `saved_at`, so a list ordered by when you saved does not
reshuffle when you click again.

`remind_at_ms` is the item's **reminder** (REQ-283), `0` for none (protocol
35). With op `1` it is the item's whole state: a time sets the reminder, to fire
once more even if an earlier one had fired; `0` clears it and leaves the item
saved. A client saving again sends the reminder it holds. A reminder on an
unsaved message saves it; unsaving takes the reminder with it, as does deleting
the message. On the scheduled-message tick (fifteen seconds) every reminder whose time has passed fires once: the owner's
connections are sent an **`ACTIVITY_ENTRY`** of kind `5` (`OC_ACT_REMINDER`:
`message_id`, `channel_id`, `at` the moment it fired, no text), and their
devices are woken through push as for a mention, held back by a mute, quiet
hours or a pause; never mail. The fired reminder stays in `LIST_ACTIVITY` until
it is cleared or set again. Backfill's replayed `SAVED_UPDATED` carries it too.

**`LIST_SAVED` (`0x0064`)** (no body) streams **`SAVED_MSG` (`0x0065`)** newest
save first, then **`SAVED` (`0x0066`)** `{ count: u32 }`. Each entry is
`{ message_id: u64, channel_id: u64, author_id: u64, server_time: u64,
saved_at: u64, body: str, attach_name: str, action: u8, remind_at_ms: u64 }` — the body and first
attachment's name, like a pin — a saved message is
usually far outside loaded history — and, last, an `action` byte (REQ-058,
§5.16l): `1` when the message is an action, its body then being the action text, then its reminder, `0` for none. Entries whose channel you have since left are
omitted: leaving a channel must stop it leaking through your saved list.

**`SET_COLLAPSED` (client → server), msg_type `0x0117`** `{ message_id: u64, on: u8 }`
folds a message away for you (`1`), or opens it again (`0`). A message in a
channel you are a member of (`NOT_A_MEMBER` otherwise), not a tombstone
(`UNKNOWN_MESSAGE`). The set is yours on every device: **`COLLAPSED_UPDATED`
(`0x0118`)** `{ message_id: u64, on: u8 }` goes to **each of your connections**,
the one that asked among them, and to nobody else. Backfill replays your folded
messages among those it sends, as one `COLLAPSED_UPDATED` each, after their
saved state. Folding twice, or opening one that is open, changes nothing and is
answered the same. A delete takes the message's folds with it. (A message long
enough to be folded *for everyone* is the client's rule, CLIENT.md, and is not
on the wire.)

**`MOVE_MESSAGE` (client → server), msg_type `0x011C`** `{ message_id: u64,
dest_channel_id: u64, dest_parent_id: u64, leave_marker: u8 }` moves a message
(REQ-053): to `dest_channel_id`'s top level when `dest_parent_id` is `0`, or
into the thread of that top-level message there. A thread root moves with all
of its replies, so a thread is never split, and it cannot become a reply
(`CANNOT_MOVE`); a reply can move out to a top level. An owner or admin may move
any message, an author their own (`FORBIDDEN`). The mover must be able to read
the source (`UNKNOWN_MESSAGE` otherwise) and post in the destination
(`FORBIDDEN`; a public one joins them, as a send does). Neither channel may be
archived (`CHANNEL_ARCHIVED`), a DM or an encrypted DM (`CANNOT_MOVE`).
`leave_marker` is `0` or `1`, anything else malformed.

The message keeps its id, author, time, edits, reactions, attachments and pin;
its mentions, pin, files, previews and poll follow it, all in one transaction,
and the move is audited (`message.move`). With `leave_marker` `1`, the
workspace's own sender says in the source "<mover> moved a message to #dest"
(or "a thread"), as a `BROADCAST` like its other notices.

**`MESSAGE_MOVED` (`0x011D`)** `{ message_id: u64, from_channel: u64,
to_channel: u64, to_parent: u64, count: u32 }` goes to the members of both
channels, each connection once; `count` is how many messages moved. A client
drops the message (and a thread's replies) from where it was, and reads the
moved rows where they now are as it reads any others: `HISTORY_AROUND` the
message at a top level, the thread again when it went into an open one. A
`HISTORY_AROUND` naming the channel a moved message used to be in answers from
the channel it is in now, for a reader who may read that one, so a copied link
still finds it.

**`LIST_ACTIVITY` (`0x0067`)** `{ filter: u8 }` streams **`ACTIVITY_ENTRY`
(`0x0068`)** newest first, then **`ACTIVITY` (`0x0069`)** `{ count: u32,
seen_at: u64 }`. `filter` is `0` involved-me (the original question) · `1` unread
· `2` unread DMs · `3` unread channels I am notified about. An **empty body** is
read as `0`, so a client that sends no filter keeps working.

| Field        | Type | Notes                                                     |
|--------------|------|------------------------------------------------------------|
| `kind`       | u8   | `0` mention · `1` reaction to your message · `2` reply under your thread · `3` unread (only in an unread answer) · `4` a summary you asked for, made while you were not watching (§5.16m) |
| `message_id` | u64  | What to jump to; for kind `4`, the summary notice (`SUMMARY_OPEN`). |
| `channel_id` | u64  | Where it happened.                                         |
| `actor_id`   | u64  | Who did it — never you: a feed of your own doings is noise. |
| `at`         | u64  | When.                                                      |
| `text`       | str  | The message body for a mention or reply; the **emoji** for a reaction; for kind `4`, the summary's scope in digits. |
| `action`     | u8   | `1` when the message is an action (REQ-058, §5.16l): `text` is its action text and `actor_id` its actor. For kind `4`, the summary's status (`0` made). |

The feed is a **union of four queries** over existing rows (the fourth, the
person's summary notices), not a maintained
list (ARCH-95). `seen_at` is the watermark **as it was before this call** — on the
involved-me filter (`0`) the server stamps the current time as part of answering;
filters `1`–`3` read it without stamping — so a client compares each `at`
against it to mark what is new. That is deliberately coarser than per-item
read state, which would require the table ARCH-95 argues against.

### 5.9d Fetch-around, for permalinks (REQ-232, ARCH-96)

**`HISTORY_AROUND` (client → server), msg_type `0x006A`**
`{ channel_id: u64, message_id: u64, limit: u16 }` — the messages *surrounding*
an id, `limit/2` either side, so a jump target lands mid-screen with context
rather than pinned to an edge.

It replies with the **history replay encoding of §6.3**: same rows, same
ascending order, same attachments, reply counts and side-state (pins, saved
marks, reactions, unfurls, forwards, actions, polls), so the client's high-water dedup (ARCH-45) and every downstream fold work
unchanged — only the `WHERE` differs from the backwards paging. Read access is
checked exactly as it is there: a permalink is not a way around membership.

**`FIND_AT_TIME` (client → server), msg_type `0x011E`**
`{ channel_id: u64, parent_id: u64, at_ms: u64 }` finds where a date falls in a
conversation: its main scroll when `parent_id` is `0`, else the thread of that
top-level message (its root and replies). Read access is checked as for
history: `UNKNOWN_CHANNEL` for no such conversation, `NOT_A_MEMBER` for one the
asker cannot read. The client sends the reader's local midnight; the daemon
knows nothing of time zones.

**`FOUND_AT_TIME` (server → client), msg_type `0x011F`**
`{ channel_id: u64, parent_id: u64, message_id: u64, server_time: u64, exact: u8 }`,
to the asker alone: the first message at or after `at_ms` (`exact` `1`); when
none is later, the last one before it (`exact` `0`); `message_id` `0` when the
conversation has none. The client then opens it as a permalink does, with
`HISTORY_AROUND`, and says so when the message found is not on the day asked
("No messages on 3 May; showing the next, 5 May"). `exact` is `0` or `1`,
anything else malformed.

### 5.10 Threads (REQ-060)

Any message can be replied to as a thread. A reply threads under a **top-level
root** — replying to a reply flattens to that reply's root, so threads are one
level deep. A reply is **not** delivered to the channel's main scroll (§5.3): it
is fanned out as a `THREAD_REPLY`, and the parent shows a reply count -- unless
it was sent with `also_channel` (REQ-062). Then it is **one message in both
places**: stored once, flagged, never copied; live, one `THREAD_REPLY` with
`also_channel` 1 to every member, which a client shows in the thread and in the
channel without showing it twice; and in the main scroll's replay (§6) and
history pages as a `BROADCAST` carrying its `parent_id`. It counts in the
channel's unread, last message and preview as a top-level message does, once;
it is one message for the mentions it stores and the notifications it makes;
and an edit, delete, restore or reaction on it reaches every member, as on any
message. An archived channel refuses it as it refuses any reply.

**`SEND_REPLY` (client → server), msg_type `0x002C`:**

| Field        | Type | Notes                                                     |
|--------------|------|-----------------------------------------------------------|
| `channel_id` | u64  | The channel.                                              |
| `idem`       | 16 B | Idempotency token (§7), exactly as `SEND` (REQ-093).      |
| `parent_id`  | u64  | The message being replied to (resolved to its root).      |
| `also_channel` | u8 | `1` posts the reply to the channel's main scroll too (REQ-062); `0` as before. Any other value is malformed. Since version 36. |
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
| `participant` | u8   | 1 if **this recipient** is in the thread (REQ-061).      |
| `broadcast_stored` | u8 | As on `BROADCAST` (§5.3): `1` when the reply's send stored a broadcast mention (REQ-221). Since version 32. |
| `also_channel` | u8  | `1` when the reply is also in the channel's main scroll (REQ-062): the client shows it there too, from this one frame. Since version 36. |
| `via`         | str  | As on `BROADCAST` (§5.3): the AI client the reply came through, else empty. Since version 51. |
| `body`        | lstr | The reply body.                                          |

`participant` is the only per-recipient field on a fan-out frame, so the daemon
encodes this frame twice — once each way — and sends whichever matches the peer
it is writing to. It answers ARCH-104's question, "are you in this thread": you
wrote the root or a reply, or explicitly followed, and did not explicitly
unfollow. The client cannot derive it, because it holds a thread's replies only
while that thread is open.

It carries the **input**, not the verdict. The recipient still asks
`oc_notify_decide` with its own mute, level, priority people, schedule and pause,
so participation satisfies the *mentions* level exactly as a keyword hit does
rather than bypassing it — and one thing, not two, decides whether a message
notifies (ARCH-103). A `LIST_THREAD` replay carries the true value too; a client
suppresses its own toasts for history it asked for rather than the daemon
misreporting participation to achieve it.

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
| `action`        | u8   | `1` when the root is an action and `preview` is its text (REQ-058). |

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
msg_type `0x0032`** so the client can show the reply count, and who replied,
without opening the thread. The same frame is sent live to the channel's members
after a reply is posted or deleted, so an open transcript keeps it current:

| Field          | Type | Notes                                       |
|----------------|------|---------------------------------------------|
| `message_id`   | u64  | The parent message.                         |
| `reply_count`  | u32  | Its total reply count.                      |
| `last_reply_at`| u64  | Timestamp of the most recent reply, ms UTC. |
| `n_repliers`   | u8   | How many ids follow, at most 3.             |
| `repliers`     | u64 × `n_repliers` | The most recent distinct authors of its replies that are not deleted, newest first. |

`SEND_REPLY`/`LIST_THREAD` failures are non-fatal `ERROR` frames:
`UNKNOWN_MESSAGE` (no such parent, or, for `SEND_REPLY`, it is tombstoned —
`LIST_THREAD` still lists the replies under a deleted root), `NOT_A_MEMBER` /
`UNKNOWN_CHANNEL` (cannot post to / read the channel), and, for `SEND_REPLY`,
`BODY_TOO_LARGE` with its `idempotency_token` in `context`, as on `SEND` (§5.1).

A `THREAD_REPLY` also drives a notify decision (REQ-061): the reply's root goes
to the push emitter with it, which notifies the thread's **participants** —
derived from `messages.parent_id` with `thread_follows` as overrides (ARCH-104)
— under each recipient's own per-channel level. Being a participant satisfies
the *mentions* level the way a keyword hit does, so mute, the schedule and the
pause still silence it. The live fan-out above is unchanged and still reaches
every connected channel member.

### 5.11 Search (REQ-080)

Full-text search over message history, scoped to what the searching user can
read (public channels and the private channels they belong to, REQ-031).
Implemented with a SQLite FTS5 index over message bodies (ARCH-15, SCHEMA.md
§3d); there is no history cutoff.

**`SEARCH` (client → server), msg_type `0x0060`:**

| Field         | Type | Notes                                                    |
|---------------|------|----------------------------------------------------------|
| `query`       | str  | Free text. The daemon quotes each whitespace-separated term, so query punctuation is literal and multiple terms are ANDed; it never errors on syntax. May be empty when the filters below carry the whole query. |
| `limit`       | u16  | Max results wanted; the daemon caps it (50).             |
| `before_id`   | u64  | **Keyset paging cursor** — the id of the last row of the page before; `0` for the first page. Newest first, the next page is the lower ids; oldest first, the higher ones; by relevance, the rows after (`cursor_rank`, `before_id`). Not an offset, so a message posted mid-paging cannot make a row repeat or vanish. |
| `from_name`   | str  | `from:` — restrict to one author. Empty for no constraint. |
| `in_channel`  | str  | `in:` — restrict to one channel. Empty for no constraint. |
| `has_mask`    | u8   | `has:` — bitmask, `0x01` file, `0x02` link, `0x04` image, `0x08` video. `0` for no constraint. |
| `after_ms`    | u64  | `after:` — lower time bound, ms since epoch UTC. `0` for none. |
| `before_ms`   | u64  | `before:` — upper time bound. `0` for none. |
| `sort`        | u8   | `0` newest first (by id, descending), `1` oldest first (by id, ascending), `2` by relevance (best match first, ties by id ascending). Relevance with no `query` text falls back to newest first: nothing ranks a filters-only search. Since protocol 28. |
| `cursor_rank` | i64  | Relevance paging: the last row's `rank`, with its id in `before_id`. A message posted since moves every bm25 score, so the daemon measures that row's rank afresh and uses `cursor_rank` only when the row no longer matches (edited or deleted). Ignored by the other orders. Since protocol 28. |

The operator grammar is parsed by `shared/searchq.c`, which the daemon and both
frontends link, so the filter line a client shows and the `WHERE` clause the
daemon builds cannot disagree (REQ-081). Results stay scoped to history the
caller may read (REQ-031).

Replies **`SEARCH_RESULTS` (server → client), msg_type `0x0061`**, in the order
`sort` asked for:

| Field       | Type            | Notes                                                |
|-------------|-----------------|------------------------------------------------------|
| `count`     | u16             | Number of entries.                                   |
| `entries[]` | `count` × entry | Each: `message_id` (u64), `channel_id` (u64), `author_id` (u64), `server_time` (u64), `snippet` (str — an FTS5 excerpt around the match, or the body's first 160 bytes for a search with no text), `action` (u8 — `1` when the message is an action and the snippet is cut from its text, REQ-058), `rank` (i64 — the row's relevance, bm25 × 1,000,000 rounded, lower is better; `0` unless sorted by relevance; since protocol 28), `call_id` (u64) and `line_id` (u64) — a line of a call's transcript, both `0` for a message (§5.17; since protocol 49). |
| `truncated` | u8              | 1 if more matches exist past the cap (`limit`).      |

Tombstoned messages (§5.6) are excluded, and an edit (§5.5) re-indexes the
message. An empty or all-punctuation query with no filters returns zero results (never an error).

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
all are away, **offline** once the last closes — and announces it on every
`SET_PRESENCE`, changed or not.
A connection's broadcasts are limited to 5 per 10 seconds. The change itself is
always recorded; a broadcast past the limit is sent once the window allows,
carrying the status as it stands by then, so the last one always reaches
everybody.

**`PRESENCE_UPDATE` (server → client), msg_type `0x0071`** `{ user_id: u64,
status: u8, dnd: u8 }` — sent tenant-wide to every *other* authenticated
connection when a user's aggregate status changes (a client tracks its own presence locally, so it
is excluded from its own broadcast). On authenticating, a client also receives a
**one-shot snapshot** — one `PRESENCE_UPDATE` per currently-online user — so it
starts with an accurate roster without polling.

`dnd` is the do-not-disturb **fact** (REQ-122/136/278) — that the user is not to
be disturbed, never when they will be back. It is a second axis beside presence,
not a status value: somebody can be online and paused, and collapsing the two
would make "away" and "do not disturb" indistinguishable.

**Both halves of do-not-disturb answer it**, because a viewer deciding whether to
write cares about the fact and not which mechanism produced it: the transient
**pause** (`SET_SNOOZE`, REQ-278) and the recurring **schedule**
(`SET_SCHEDULE`, REQ-136). The schedule half is evaluated through the shared
`oc_notify_quiet` — the same rule the push path decides delivery with — so what a
colleague sees and what reaches a phone cannot disagree.

The net thread holds both in memory, seeded at `AUTH_OK` and refreshed wherever
each already fans out (ARCH-66/67 gives it no database of its own). The pause is
an instant compared per frame, so an expiry needs no sweep. Neither half ends
because anyone did something — a pause runs out, quiet hours begin at 18:00 — so
a maintenance tick re-announces a user whose answer differs from what they were
last announced as. Only a frame can untell the people who were told, and
comparing rather than re-announcing keeps an idle deployment silent.

**`TYPING` (client → server), msg_type `0x007E`** `{ channel_id: u64,
thread_root: u64 }` — the caller signals they are composing in `channel_id`: in
the thread whose root is `thread_root`, or in the main scroll when it is `0`
(REQ-121; since version 35). The server resolves the
channel's members (on the read connection, ARCH-66) and, if the caller has access,
relays to the other connected members only — private-channel and DM typing never
leaks to non-members. A connection's signals past 3 per 2 seconds are dropped
without reply; the next says the same.

**`TYPING_UPDATE` (server → client), msg_type `0x007F`** `{ channel_id: u64,
user_id: u64, thread_root: u64 }` — delivered to each connected member of
`channel_id` except the sender, with the `thread_root` it was sent with,
relayed as it came: it reaches only the channel's members, and a client shows
it only in that thread's pane (or the main scroll's, for `0`), so a wrong one
shows nowhere. There is **no expiry frame and no "stopped typing" signal**: the
client displays the indicator and expires it locally after **~6 seconds**,
refreshed by each new `TYPING_UPDATE` for that `(channel, user)`, which also
moves it to the thread that update names.

---

### 5.14 Attachments (REQ-140, REQ-141)

File bytes are **proxied through the daemon** over this same TLS
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
   16B, filename: str, mime: str, total_size: u64, media_kind: u8 }` — the client
   declares an upload targeting a channel/DM it can post to. `media_kind` is 2 for
   a snippet's text (§5.14e) and 0 otherwise (protocol 44). The server authorizes
   membership, checks `total_size <= MAX_ATTACHMENT_SIZE` (and, for a snippet,
   at most 1 MiB), allocates an
   `attachment_id` + opaque storage key, and opens a streaming write to object
   storage. **Idempotent on `(channel_id, uploader, idempotency_token)`** — as `SEND` is,
   but keyed on the uploader too, so one member is never handed another's row: a
   retry after a dropped connection returns the *same* `attachment_id` and the
   same storage key rather than allocating a second attachment, because the
   client that retries is one that never heard the first answer and whose next
   move is to stream the bytes. The token is held on the attachment row
   (migration 0037) rather than in a side map, so it is reclaimed with the row —
   a map would outlive a storage-swept attachment and hand back a dead id. On
   failure → `ERROR`: `UNKNOWN_CHANNEL`, `NOT_A_MEMBER` or `CHANNEL_ARCHIVED`
   (post access, §5.7), `FORBIDDEN` (an encrypted DM), `ATTACHMENT_TOO_LARGE`,
   `STORAGE_FULL`, `DEMO_LIMIT`, or `TRANSFER_PROTOCOL` (another transfer in
   flight, which the refusal also aborts).
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
a trailing attachment list of `{ attachment_id, filename, mime, size, reclaimed,
media_kind }` — `reclaimed` is a `u8`, 1 when the blob has been reclaimed by age or
storage pressure so the row is a tombstone (REQ-215) and no download id is offered;
`media_kind` is a `u8`, `0` for an ordinary file and `1` for a video message, in
which case `{ duration_ms: u32, width: u16, height: u16, poster_id: u64 }` follow
(§5.14a); any other kind is a malformed frame — so every
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
   reading from storage when this connection's pending output reaches 256 KiB,
   well under the 1 MiB cap, and resumes on `EPOLLOUT` drain, so a slow reader never grows daemon memory
   (the same discipline as message fan-out).
4. **`DOWNLOAD_END` (S → C), `0x0089`** `{ attachment_id: u64 }` — all bytes
   sent; the client verifies the reassembled size (and optionally `sha256`).

**`TRANSFER_CANCEL` (C → S), `0x008A`** `{ attachment_id: u64 }` — aborts an
in-progress upload or download; the server tears down the transfer and, for an
uncommitted upload, discards the partial blob. Abandoned uploads that never
finalize are swept by a time-gated cleanup (ARCH-70). A new transfer may begin
at once, even while the cancelled one's `UPLOAD_BEGIN` or `DOWNLOAD_BEGIN` is still
being answered: whatever that answer is — its attachment, or a refusal — it
belongs to the cancelled transfer and is dropped, never taken for the new one's.

---

### 5.14a Video messages (REQ-162–165, ARCH-110)

A video message is an **ordinary attachment** — a VP9 and Opus MP4 recorded in the
client (docs/details/VIDEO-MESSAGES.md) — plus a **media row** naming its length, size and
poster. Posting it is four steps, all on frames that already exist but one:

1. Upload the poster JPEG (`UPLOAD_BEGIN` … `UPLOAD_OK`).
2. Upload the MP4, `mime` `video/mp4`.
3. **`ATTACH_MEDIA_SET` (C → S), `0x00DA`** `{ attachment_id: u64, media_kind: u8,
   duration_ms: u32, width: u16, height: u16, poster_id: u64 }`, answered by
   **`ATTACH_MEDIA_OK` (S → C), `0x00DB`** `{ attachment_id: u64 }`, or an `ERROR`.
4. `SEND` (or `SEND_REPLY`) with the video's id in the attachment list. The poster
   is **not** listed; the media row names it.

The daemon refuses with `UNKNOWN_ATTACHMENT` 3011 when the video is not the
caller's finalized, unreclaimed upload, `MEDIA_TOO_LARGE` 3022 when it exceeds
the workspace's video cap (`OPENCHIME_MAX_VIDEO_SIZE`, 160 MiB by default, at
most `MAX_ATTACHMENT_SIZE`, and sent to clients as `WORKSPACE_INFO`'s
`max_video_size`), and `MEDIA_INVALID` 3021 when: the kind is not
`1`; the duration is `0` or over 300 000 ms; either dimension is odd, under 2, or
over 1920×1080; the video is not `video/mp4` or has already been sent; or the
poster is not a finalized, unsent `image/jpeg` of at most 1 MiB uploaded by the
same user **to the same channel**, or is itself a video message. A second
`ATTACH_MEDIA_SET` before the `SEND` replaces the first.

**The duration is the client's report.** The daemon links no codec and cannot
measure it; the byte cap is the bound it enforces, and duration and size are
display facts.

**The poster is readable exactly when the video is**, with no rule of its own:
download authorization is the read gate on the attachment's channel, and the two
share a channel by construction. It lives exactly as long as the video too: the
storage sweep keeps it while its video is sent and unreclaimed, reclaims it with
the video in the same pass, and collects it as an orphan once the video's message
is deleted.

The media row then rides every attachment entry — `BROADCAST`, `THREAD_REPLY`,
backfill, history and `LIST_THREAD` — and `FILE_ENTRY` (§5.9b). Search matches any
video attachment with `has:video` (§5.11).

### 5.14b Capabilities, workspace settings and read-aloud (REQ-291–295, ARCH-111)

Speech is **not** an attachment: nothing was uploaded, and the daemon makes the
file itself from the message's text. What the wire carries is therefore the
download shape without the upload half, keyed by the **message** id the client
already has.

**`WORKSPACE_SETTINGS` (S → C), `0x0119`** `{ count: u8, count × (key: str,
value: str) }` — the workspace's settings, **read-only**, sent once to every
client at sign-in, after `WORKSPACE_INFO`. They are the daemon's configuration,
the rows CONFIG.md marks "Clients are told it": each `key` is the variable's name
without `OPENCHIME_`, lower-cased, and `value` its effective value as an operator
writes it (empty when unset). Workspace settings are set by the control plane or
the operator and take effect when the daemon restarts; no frame changes one, so
nothing is sent while the daemon runs. A client acts on what it is told; the
daemon enforces whatever it must. At most 64; more is refused as malformed. A
client ignores a key it does not know.

**`READ_RECEIPTS` (both ways), `0x011A`** `{ share: u8 }` — whether this person
shares their read position (seen-by). From a client, `share` `1` or `0` sets
it and `2` (`OC_RECEIPTS_ASK`) asks; a client asks once after sign-in. From the
daemon, `share` `0` or `1` tells it: to the device that asked, or, when it
changes, to every device of the person's. Anything above `2` is malformed. Not
sharing keeps the person's `READ_CURSOR` from the others and still shows them
everyone else's. When the workspace has receipts off (`read_receipts` on
`WORKSPACE_SETTINGS`), no member's `READ_CURSOR` reaches any other. A read made
before a person shared, or while the workspace had receipts off, is never
shown: the daemon sends another member's position only when it was recorded
since then.

**`CAPABILITIES` (S → C), `0x00E1`** `{ count: u8, count × name: str }` — the
features this daemon offers, by name, sent once to every client just after
`SERVER_BUILD` (§4.3c) and before the frames that describe any feature. A name present
means the feature is available on this connection; absent means a client shows
nothing of it, which is the correct result rather than offering something that
would fail (REQ-295). `tts` is present exactly when read-aloud is running — built
in, turned on, and its voice data found and verified against its manifest. `stt`
is present exactly when voice input (§5.14c) is running, on the same terms for the
recognizer's data. `calls` is present when the audio relay is up to carry calls
(§5.17). `calls-tcp` comes with it: call media may travel over this connection
as `CALL_MEDIA` when UDP cannot reach the relay (§5.17). `pdm` is always
present, offering encrypted DMs (§5.19). `summarize` is present when summaries are on (§5.16m), including while
their model is still coming up — a request sent then waits in the queue until it
is ready; a daemon whose model did not come up answers whatever waited
with a `SUMMARY` of status UNAVAILABLE (1, §5.16m) and leaves the name out of every later sign-in. `polls` is always
present on a daemon that stores polls (§5.16n), so a client offers to make one
only where it can be kept. `h264-export` is present exactly when the export of a
video message is running (§5.14d): built in and turned on. `call-recording` is
present exactly when calls may be recorded here (§5.17): built in (`CALLREC=1`),
not turned off, not a public demo, and the relay up.
Names rather than bit positions, so there is no ceiling and nothing to misnumber; a
client ignores a name it does not know. At most 16 names: a longer list is a
malformed frame.

**`TTS_INFO` (S → C), `0x00DC`** `{ model_version: str, count: u8,
count × { id: str, label: str, lang: str }, preview: str }` — sent once to every
client just after `CAPABILITIES`, whatever the answer, with no voices when
read-aloud is not offered. Whether it is offered is the `tts` capability's to say:
this frame carries no `available` byte of its own, because two answers to one
question can disagree. `model_version` names the
language, the model, the pronunciation data and the speaking rate together;
`preview` is the sentence a client plays to audition a voice. `lang` is the BCP 47
language that voice speaks, carried per voice rather than per frame because a
voice belongs to a language — an English model cannot read German — so a client
can offer the right voices without a second lookup, and a daemon that grows a
second language needs no new frame. At most 16 voices: a longer list is
a malformed frame, not a truncated one.

**`AUDIO_GET` (C → S), `0x00DD`** `{ message_id: u64 }` — "read this message to
me". The gate is the one that guards reading the message itself
(`channel_read_access`), because the speech IS the message. Then, in order:

1. **`AUDIO_INFO` (S → C), `0x00DE`** `{ message_id: u64, duration_ms: u32,
   total_size: u64 }` — once, before the bytes.
2. **`AUDIO_CHUNK` (S → C), `0x00DF`** `{ message_id: u64, seq: u32, data: bytes }`
   — sequential chunks, with the download path's backpressure: the daemon stops
   reading from storage while this connection's output is above the soft cap and
   resumes on drain.
3. **`AUDIO_END` (S → C), `0x00E0`** `{ message_id: u64 }` — everything sent.

The payload is an **audio-only MP4** holding one Opus track (the video message
profile with the video track left out, docs/details/VIDEO-MESSAGES.md §5), 24 kHz mono at
24 kbit/s.

A first request **renders**, which takes about six tenths of the spoken duration,
and the answer arrives when it is done; later requests for the same text in the
same voice are served from the cache. Two listeners asking at once cost one
render. One transfer at a time per connection, as for an attachment: a second
`AUDIO_GET` mid-stream is refused with `TRANSFER_PROTOCOL`, and the refusal also
aborts the transfer that was running (as a second `UPLOAD_BEGIN` or
`DOWNLOAD_BEGIN` does).

Refusals are `ERROR` frames carrying the message id in `context`:
`NOT_RENDERABLE` 3023 when the message has nothing to say — a bare attachment,
only emoji (REQ-294) — `TTS_UNAVAILABLE` 3024 when read-aloud is off, its queue
is full, or the connection has asked for more than 60 renderings in a minute,
`FORBIDDEN` when the caller cannot read the channel, and
`UNKNOWN_MESSAGE` for an id that is not there.

**`VOICE_PREVIEW_GET` (C → S), `0x00E2`** `{ voice_id: str }` — "let me hear this
voice before I choose it" (REQ-292). The daemon renders `TTS_INFO`'s `preview`
sentence in that voice and answers with the same `AUDIO_INFO` / `AUDIO_CHUNK` /
`AUDIO_END` sequence, carrying **`message_id` 0**, which no message has. There is
no gate beyond being signed in: the sentence is the daemon's own, not anyone's
message. The rendering is cached like any other, so every user auditioning the same
voice after the first is served without a render. It is a transfer like any other —
one at a time per connection. Refusals carry `context` 0: `TTS_UNAVAILABLE` 3024
when read-aloud is off, the queue is full, the connection is past 60 renderings
a minute, or `voice_id` is not one `TTS_INFO` listed, and `TRANSFER_PROTOCOL` when another transfer is in flight.

The `TTS_UNAVAILABLE` refusals above come from a daemon built with read-aloud. A
daemon built without it (`TTS=0`, the plain build's default) answers `AUDIO_GET`
and `VOICE_PREVIEW_GET` the same way as one built with it and turned off:
`TTS_UNAVAILABLE`, naming the message asked about (0 for a preview), and the
connection stays open (READ-ALOUD.md §6). A client sends them only when
`CAPABILITIES` lists `tts`.

### 5.14c Voice input (REQ-296–300, ARCH-112)

The client sends a segment of speech it has already cut at a pause. In **push to
talk** the daemon answers with the words; in **free talk** it posts them as the
speaker through the ordinary send path — the one `SEND` and `SEND_REPLY` use — so the
text never travels back to be re-sent. Design in docs/details/VOICE-INPUT.md. Adding these
frames needed no protocol-version bump (§2).

**`STT_INFO` (S → C), `0x00E3`** `{ model_version: str, lang: str, max_segment_ms:
u32 }` — sent once to every client just after `TTS_INFO`; empty and `0` when voice
input is not offered, which is the `stt` capability's to say.

**A segment**, under a `segment_id: u32` the client chooses:

1. **`STT_BEGIN` (C → S), `0x00E4`** `{ segment_id: u32, mode: u8, channel_id: u64,
   thread_root: u64, idempotency_token: 16B, sample_count: u32 }` — `mode` `0` push
   to talk, `1` free talk; `thread_root` `0` for the channel itself. In free talk the
   target is checked with the post access `SEND` uses once `STT_END` arrives, before
   recognition; `STT_BEGIN` itself checks no access.
   16 kHz mono signed 16-bit little-endian PCM follows.
2. **`STT_CHUNK` (C → S), `0x00E5`** `{ segment_id: u32, seq: u32, data: bytes }` —
   sequential from `seq` 0, each frame `<= MAX_FRAME_SIZE`.
3. **`STT_END` (C → S), `0x00E6`** `{ segment_id: u32 }` — the total must equal
   `sample_count`.

**`STT_TEXT` (S → C), `0x00E7`** `{ segment_id: u32, message_id: u64, text: str }` —
closes every segment. In free talk `message_id` is the posted message, which also
reaches every member as the usual `BROADCAST` (or `THREAD_REPLY`); it is `0` in push
to talk and when nothing was recognized. The idempotency token makes a retried
segment post once, exactly as `SEND`'s does (§5.1).

One segment uploads at a time per connection and a bounded number wait behind it;
segments are answered, and posted, **in the order their `STT_END` arrived**. They are
separate from attachment transfers. The daemon frees a segment's samples once its
`STT_TEXT` is sent.

Refusals are `ERROR` frames carrying the `segment_id` in `context`: **3026
`SEGMENT_TOO_LONG`** when `sample_count` exceeds the cap; **3027 `STT_UNAVAILABLE`**
when voice input is off, too many segments are waiting or more than 60 arrived
in a minute, the queue is full or recognition failed; `FORBIDDEN` in an
encrypted DM (§5.19); `NOT_A_MEMBER`, `UNKNOWN_CHANNEL`, `CHANNEL_ARCHIVED` and
`SEND_RATE_LIMITED` in free talk, as for `SEND`; and `TRANSFER_PROTOCOL` for a chunk
out of order, a total that does not match, an `STT_BEGIN` mid-segment, a
`sample_count` of 0, an unknown `mode`, or free talk with `channel_id` 0.

### 5.14d Export of a video message (REQ-382–384, ARCH-135)

A copy of a video message that players other than OpenChime open: H.264 and MP3
in MP4, made by the daemon from the VP9 and Opus the message holds. It is the
download's shape keyed by the **attachment** id, as read-aloud's is keyed by the
message's.

**`EXPORT_GET` (C → S), `0x013E`** `{ attachment_id: u64 }` — "the playable copy
of this video message". The gate is the download's (`channel_read_access` on the
video's channel), because the copy is the video. Then, in order:

1. **`EXPORT_INFO` (S → C), `0x013F`** `{ attachment_id: u64, duration_ms: u32,
   total_size: u64 }` — once, before the bytes.
2. **`EXPORT_CHUNK` (S → C), `0x0140`** `{ attachment_id: u64, seq: u32, data: bytes }`
   — sequential chunks, with the download path's backpressure.
3. **`EXPORT_END` (S → C), `0x0141`** `{ attachment_id: u64 }` — everything sent.

A first request **transcodes**, about a quarter of the video's length on a
desktop and longer on a small machine, and the answer arrives when it is done;
later requests are served from the cache. Two people asking for the same video at
once cost one transcode. One transfer at a time per connection: an `EXPORT_GET`
while another transfer runs is refused `TRANSFER_PROTOCOL` and the running one is
left alone.

Refusals are `ERROR` frames carrying the attachment id in `context`, and the
connection stays open: `EXPORT_UNAVAILABLE` 3074 when export is not running here —
not built in (`EXPORT=0`) or turned off (`OPENCHIME_H264_EXPORT`) — or the
person already has three requests in hand, or the daemon's queue is full;
`FORBIDDEN` when the caller cannot read the channel; `MEDIA_INVALID` when the
attachment is not a video message, or is an encrypted DM's; `ATTACHMENT_GONE` when
its bytes have been reclaimed; `UNKNOWN_ATTACHMENT` for an id that is not there or
not yet sent; and `INTERNAL` when the transcode failed. A client sends it only when
`CAPABILITIES` lists `h264-export`.

### 5.14e Snippets (REQ-226)

A snippet is an **ordinary attachment** too: a `text/plain` upload of at most
1 MiB, marked with a title and an optional language, shown as a card with its
first lines and opened whole with its words coloured (`shared/lexer.h`: C,
Python, JavaScript, shell, JSON, YAML, SQL and Go; anything else plain). Posting
it is three steps:

1. Upload the text, `mime` `text/plain`.
2. **`ATTACH_MEDIA_SET`** with `media_kind` `2` (protocol 39). After the kind
   byte come `{ title: str, language: str, text: str }` in place of a video's
   facts: the title (1 to 120 bytes), the language (at most 16 bytes of `a`–`z`,
   `0`–`9`, `+`, `#`, `-`; empty for plain), and the text's first 64 KiB, for
   search. Answered `ATTACH_MEDIA_OK`, or `UNKNOWN_ATTACHMENT` as for a video,
   `MEDIA_TOO_LARGE` past 1 MiB, and `MEDIA_INVALID` for an upload that is not
   `text/plain`, already sent, untitled, a language outside those characters, or
   text that is not UTF-8.
3. `SEND` (or `SEND_REPLY`) with its id in the attachment list.

An attachment entry of kind `2` carries `{ title: str, language: str }` after the
kind byte, where a video's carries its facts. Search (`SEARCH`) finds a
message by its snippet's title or text: such hits follow the message hits on the
first page of a search with words and no filters, newest first, under the same
read gate, their excerpt "Snippet: <title>". Deleting the message, or the
storage sweep reclaiming the bytes, takes the snippet out of search.

The size cap is enforced at `UPLOAD_BEGIN`, whose `media_kind` says the upload
is a snippet, before any of its bytes are sent (`ATTACHMENT_TOO_LARGE`), and again
at `ATTACH_MEDIA_SET` when it is marked. Clients refuse a longer
text before uploading it, and offer a paste of more than 40 lines or 4 KiB as
one.

### 5.14f Image previews (ARCH-137)

A smaller copy of an image attachment, made by the daemon once at upload: the
**inline** rung (`1`), for a message in the timeline, and the **expanded** rung
(`2`), for the image viewer. Each is in its original's format: a JPEG's are JPEGs,
a PNG's PNGs, a WebP's WebPs. The original is always kept and is still fetched
with `DOWNLOAD_BEGIN`. Only a JPEG, PNG or WebP larger than the inline box gets
previews; a small image, a GIF, a HEIC, an animated image, and one the daemon
refused to decode (docs/details/IMAGE-PREVIEWS.md) get none.

Previews have **frames of their own**, rather than a rung field on
`DOWNLOAD_BEGIN` (a field changes an existing layout and bumps the version) or
the download's frames for the bytes (a preview and its original, both keyed by
the attachment id, could not be told apart). New frames need no version bump: a
peer that does not know them never sends them and is never sent them.

**`PREVIEW_BEGIN` (C → S), `0x0142`** `{ attachment_id: u64, rung: u8 }` — "this
rung of this attachment's preview". The gate is the download's
(`channel_read_access` on the attachment's channel, or everyone for an avatar),
because a preview shows the image. Then, in order:

1. **`PREVIEW_INFO` (S → C), `0x0143`** `{ attachment_id: u64, rung: u8,
   width: u32, height: u32, mime: str, total_size: u64 }` — once, before the
   bytes: the preview's own size in pixels and its type.
2. **`PREVIEW_CHUNK` (S → C), `0x0144`** `{ attachment_id: u64, rung: u8, seq: u32,
   data: bytes }` — sequential chunks, with the download path's backpressure.
3. **`PREVIEW_END` (S → C), `0x0145`** `{ attachment_id: u64, rung: u8 }` —
   everything sent.

One transfer at a time per connection, as for a download: a `PREVIEW_BEGIN`
while another transfer runs is refused `TRANSFER_PROTOCOL` and the running one is
left alone.

Refusals are `ERROR` frames carrying the attachment id in `context`, and the
connection stays open: **`PREVIEW_MISSING` 3077** when the attachment has no
preview at that rung — never made, or reclaimed under storage pressure — on which
a client **downloads the original** instead and shows nothing amiss;
**`PREVIEW_RUNG` 3078** for a rung that is neither `1` nor `2`; and the
download's own `FORBIDDEN`, `ATTACHMENT_GONE` and `UNKNOWN_ATTACHMENT`.

A client asks for the rung matching the size it will draw the image at (CLIENT.md,
"Image previews"): the inline rung in the timeline, the expanded one in the
viewer, the original only to save or open the file.

---

### 5.15 Incoming webhooks (REQ-170, ARCH-32/71)

An incoming webhook lets an uncontrolled third party post a message into one
channel over HTTP, with no user session. There are two surfaces: the
**binary-protocol frames** a client uses to mint a token, and the **HTTP
endpoint** the third party posts to.

**`CREATE_WEBHOOK` (C → S), `0x0059`** `{ channel_id: u64, label: str }` — an
authenticated member of `channel_id` mints a webhook (same post access as
sending). `label` is a human note. Fails (`ERROR`) with `UNKNOWN_CHANNEL` /
`NOT_A_MEMBER` if the caller can't post there, `CHANNEL_ARCHIVED` for an
archived channel, or `FORBIDDEN` for an encrypted DM.

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
`oc/1` is served by the HTTP/1.1 handler. The body is a `text/plain` message,
posted as sent; an `application/json` payload in the common chat-webhook
format — its `blocks`, or with none its `text`, then its `attachments`; or an
`application/x-www-form-urlencoded` body whose `payload` field holds that JSON.
How each part becomes the message, and its markup the message format's, is
ARCH-71. The text is capped at `MAX_BODY_SIZE`, cut at a character's end. The daemon hex-decodes the token, looks it up by hash, and posts
the text as a message **authored by the webhook's creator**, carrying the
webhook's **label as a display-name override** (the `author_name` field on
`BROADCAST`, so the post shows as e.g. "GitHub CI") — delivered to the channel's
members as an ordinary `BROADCAST` (§5.3) and included in backfill.
Responses: `200 {"ok":true,"message_id":N}` on success; `400` (empty or bad
body, or a malformed request); `403` (the channel is archived and therefore
read-only, REQ-035); `404` (unknown or disabled token); `405` (non-POST); `408`
(the request did not arrive whole within 10 seconds); `413` (a declared
`Content-Length` over `MAX_BODY_SIZE`, refused as soon as the headers are read,
before the body is); `429` (per-token rate limit, 60/min).

---

### 5.16 Notification preferences (REQ-130, REQ-131)

A user's notification settings are **server-authoritative and synced across their
devices**. `NOTIFY_PREFS` carries the per-channel **level** (REQ-130), beside the
user's default and per-channel mute; *when* notifications are allowed travels on
frames of its own — the recurring **schedule** (`SET_SCHEDULE`/`SCHEDULE`,
REQ-136), which replaced REQ-131's single do-not-disturb window, and
the manual **pause** (`SET_SNOOZE`/`SNOOZE`, REQ-278), both below. They are the config
that clients honor and that the push emitter (REQ-132/133, ARCH-85) consults;
quiet hours and a pause suppress *push*, not in-app unread (REQ-131).

**`SET_NOTIFY_PREF` (C → S), `0x0090`** `{ channel_id: u64, level: u8 }` — set the
level for a channel the caller can read: `0` all, `1` mentions-only, `2` none. An
absent pref falls back to the user's own default (`SET_NOTIFY_DEFAULT`, REQ-134),
not to a fixed "all". Refused with `ERROR NOT_A_MEMBER` for a
channel the caller can't access, or an invalid level.

**Opcode `0x0091` is RETIRED and carries no frame.** The recurring schedule
(`SET_SCHEDULE`, `0x00CC`) states the hours notifications are **allowed**; a
retired opcode is never redefined, because a number that means the reverse of
what a peer thinks it means is how two sides agree loudly and behave
differently.

**`LIST_NOTIFY_PREFS` (C → S), `0x0092`** (empty) — request the caller's full
settings.

**`NOTIFY_PREFS` (S → C), `0x0093`** `{ notify_default: u8, count: u16,
count × { channel_id: u64, level: u8, muted: u8, follow_threads: u8 } }` — the snapshot. It is sent both as the reply to `LIST_NOTIFY_PREFS` and, after any
`SET_*`, **pushed to every one of the user's connections** so a change on one
device updates the others. `follow_threads` (since version 37) is `1` when the
person follows every thread in the conversation (REQ-282).

**`SET_CHANNEL_THREAD_FOLLOW` (C → S), `0x011B`** `{ channel_id: u64, on: u8 }` —
follow every thread in a conversation the caller can read, or stop (REQ-282);
any `on` but `0` or `1` is malformed. Answered with `NOTIFY_PREFS` to each of the
caller's connections. With it on, every reply in the conversation counts as one
in a thread the person is in, for the push decision, the threads view
(`LIST_THREADS`) and `THREAD_REPLY`'s per-recipient `participant` byte, which the
clients' own decision reads -- one rule, `OC_THREAD_PARTICIPANT_SQL`. Mute still
silences it, and an explicit unfollow of one thread (`SET_THREAD_FOLLOW` off)
still wins for that thread. A new frame: no version change of its own.

**`SET_SNOOZE` (C → S), `0x00CA`** `{ minutes: u32 }` — **pause** notifications
for that many minutes from now; `0` ends the pause. More than a week (10,080
minutes) is cut to a week, so a bad client cannot silence somebody for a decade
by arithmetic. Slack's `dnd.setSnooze` takes
minutes for the same reason ours does: every preset the client offers is a
duration, and only the client knows the timezone that turns "until tomorrow" into
an instant. The daemon resolves it once and stores the absolute end.

**`SNOOZE` (S → C), `0x00CB`** `{ until_ms: u64 }` — when the pause ends, `0` for
none. Sent to **that user's own connections only**, after a `SET_SNOOZE` and
alongside every `NOTIFY_PREFS`. Other people are told the *fact* through
`PRESENCE_UPDATE`'s `dnd` byte and never the instant (REQ-122/278): a colleague
needs to know whether to write; when you are back is a movement report.

**`SET_TZ_OFFSET`** answers with a `SCHEDULE` (below) rather than silently: the
offset is what quiet hours are evaluated against, the client refreshes it from
the OS on every connect — so it lands *after* the `AUTH_OK` that seeded the net
thread's copy — and a presence badge computed against a stale offset is wrong for
the whole session.

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

A member who has set no schedule of their own follows the workspace's default
(`OPENCHIME_DEFAULT_QUIET_HOURS`). Their `SCHEDULE` is then that default,
resolved, with `0x80` (`OC_DND_FROM_DEFAULT`) or'd into `mode`, so a client can
say "Workspace default". A `SET_SCHEDULE` whose `mode` is `0x80` alone drops the
member's own schedule and returns them to the default. Any other mode sets their
own, which takes precedence from then on.

**`SET_TZ_OFFSET` (C → S), `0x00D6`** `{ tz_offset_min: i16 }` — the client's
UTC offset in minutes east, sent **on every connect** (ARCH-103). The offset the
schedule above is evaluated against is not a property of the schedule: it changes
when the user travels or when daylight saving turns over, neither of which
involves editing quiet hours. Carrying it only on `SET_SCHEDULE` meant it was
captured at the last edit and then trusted indefinitely, so this is its own frame
— refreshing the offset must not require, or rewrite, a schedule. The daemon
clamps it to −720…+840, the range real offsets occupy. It is answered with the
`SCHEDULE` the offset belongs to (above).

**`SET_KEYWORDS` (C → S), `0x00CE`** `{ count: u8, count × str }` and
**`SET_PRIORITY` (C → S), `0x00CF`** `{ count: u8, count × u64 }` — replace my
keyword list / my priority people wholesale (REQ-135). Wholesale because both
lists are short, a client editing one holds all of it, and a term is its own
identity — there is nothing an add/remove pair could name. **`ALERT_PREFS`
(S → C), `0x00D0`** `{ n_terms: u8, terms…, n_people: u8, people… }` returns both.

*One request, all of it.* `LIST_NOTIFY_PREFS` answers with `NOTIFY_PREFS`, then
`SNOOZE`, `SCHEDULE`, `ALERT_PREFS` and `MUTED_USERS`, in that order — so a client
never has to assemble its notification settings from five round trips or decide
what to show while half of them are outstanding. Both clients ask for it as soon
as they are signed in.

**`SET_USER_MUTE` (C → S), `0x0130`** `{ muted_id: u64, on: u8 }` — mute (`on` 1)
or unmute one person, for me alone. Nothing a muted person writes notifies me —
not a message, a reply, a reaction, a mention or a call, at any level, even when
they are also one of my priority people (`oc_notify_decide`'s `muted_author`, which
push and both clients ask alike) — and none of it counts as unread for me, in a
conversation or in a thread's count in `THREAD_LIST`, or appears in my activity.
Their reactions are no part of the counts I am sent: a replay's reaction entries
and `REACTION_UPDATED` give me each emoji's count without the people I have
muted. A direct message from them is still delivered. Muting
myself, nobody, the workspace's own sender or an id that is no one is ignored.
The muted person is sent nothing and sees no change. **`MUTED_USERS` (S → C),
`0x0131`** `{ count: u16, count × u64 }` — everyone I have muted, at most
`OC_MAX_MUTED_USERS` (256): answering `SET_USER_MUTE`, to every connection of mine,
and in the `LIST_NOTIFY_PREFS` answer. A client shows a muted person's messages,
and their rows in a who-reacted list (`REACTIONS`, which still names them),
folded to one line until revealed, one at a time, on that device alone.

*Why separate frames.* `NOTIFY_PREFS` ends in a repeated list, so anything added
to its fixed part shifts every entry after the first — the repeated-list trap that
forces a version bump. And the separation is the design, not just an encoding
convenience: the **pause** is manual and one-shot, the **schedule** (`0x00CC`) is
recurring and planned, cancelling one never cancels the other, and a pause only
ever *adds* silence — so the two can never disagree in a way needing a precedence
rule.

---

### 5.15a Outgoing webhooks (REQ-173)

An **outgoing webhook** sends a workspace's events to an `https` endpoint an
owner or admin names (docs/details/WEBHOOKS.md has what it receives, the
signing and the delivery). Every frame here is an owner's or an admin's; from
anyone else it is refused `FORBIDDEN`. All are new frames, so no version bump.

- **`CREATE_OUTGOING_WEBHOOK` (C → S), `0x0154`** `{ url: str, events: str,
  n_channels: u16, channels: [u64] }` — `events` is event names separated by
  single spaces; `channels` (at most 32) the conversations whose events it
  receives, none for only `user_change`. The daemon checks the endpoint first
  (`url_verification`) and saves the webhook only when it answers. Answered
  `OUTGOING_WEBHOOK_SECRET`.
- **`OUTGOING_WEBHOOK_SECRET` (S → C), `0x0155`** `{ webhook_id: u64, secret:
  str }` — the signing secret, sent once, to the connection that created it.
- **`LIST_OUTGOING_WEBHOOKS` (C → S), `0x0156`** `{}`, answered
- **`OUTGOING_WEBHOOK_LIST` (S → C), `0x0157`** `{ count: u16, entries: [ {
  webhook_id: u64, url: str, events: str, enabled: u8, disabled_reason: str,
  created_by: u64, created_at_ms: u64, n_channels: u16, channels: [u64] } ] }` —
  never a secret.
- **`SET_OUTGOING_WEBHOOK` (C → S), `0x0158`** `{ webhook_id: u64, enabled: u8
  }` — enable or disable it. Answered `OUTGOING_WEBHOOK_LIST`.
- **`DELETE_OUTGOING_WEBHOOK` (C → S), `0x0159`** `{ webhook_id: u64 }` — the
  webhook and its waiting deliveries go. Answered `OUTGOING_WEBHOOK_LIST`.

Refusals are `ERROR` frames: `FORBIDDEN` (not an owner or admin, or a DM or
group DM named), **`WEBHOOK_URL` 3085** (not `https`), **`WEBHOOK_EVENT` 3086**
(an event a plain webhook may not select), and **`WEBHOOK_UNVERIFIED` 3087**
(the endpoint did not answer the check).

### 5.16a Synced client settings

The daemon-side layer of the client config: portable UI prefs a frontend syncs
across its devices, keyed by a `client_type` bucket so a TUI's prefs stay
separate from a GUI's. The daemon is opaque about the keys/values — it
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

### 5.16b Self-service profile (REQ-240, ARCH-59)

A user editing their own account: renaming themselves and rotating their local
password. Both act on the **authenticated** user (`user_id` from the session, not
a field), so neither can touch another account.

**`SET_DISPLAY_NAME` (C → S), `0x0048`** `{ name: str }` — set your own
`users.display_name` (non-empty, ≤ 48 bytes). Answered with `PROFILE_UPDATED`,
or a non-fatal `ERROR`: `FORBIDDEN` for an empty or over-long name,
`GROUP_HANDLE_TAKEN` when the name is a group's handle (§5.16k).

**`SET_EMAIL` (C → S), `0x004F`** `{ email: str }` — a local account's own address
(REQ-240): one address of at most 254 bytes, or empty to remove it, stored
lower-cased. It is the account's, never a verified identity, so it satisfies no
join rule or invitation (AUTH.md §8.4). A provider's account keeps the address its
provider gives, refreshed at each sign-in, and is refused (`FORBIDDEN`), as is
something that is not one address. Answered with `PROFILE_INFO`.

**Changing a password** is not a frame: it is done on the daemon's own page
(`/account/password`, AUTH.md §8.10), whose check of the current password is a
sign-in's -- the same limiter, the same audit -- and which revokes every session
of the account, so each device signs in again. (`0x0049 CHANGE_PASSWORD` once
carried this in a frame; the number is retired.)

**`PROFILE_UPDATED` (S → C), `0x004A`** `{ user_id: u64, display_name: str }` — a
user's display name, after a rename. **Fanned to every authed connection** so all rosters update in
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
/ aged out / evicted under pressure, migration 0015):

    total_bytes (u64), avail_bytes (u64), attach_bytes (u64), attach_count (u64),
    reclaimed_orphan (u64), reclaimed_expired (u64), reclaimed_evicted (u64),
    last_reclaim_ms (u64), max_age_days (u64), reserve_bytes (u64),
    evict_enabled (u8), under_pressure (u8)

**`USAGE_REQ` (C → S), `0x0120`** `{ days: u16 }` — asks for the workspace's
usage over the last `days` UTC days, today included (REQ-043, the console's usage
figures). `days` is 1 to `OC_USAGE_MAX_DAYS` (90); 0 is taken as 1 and more as 90.
Owner and admin only: a member's request is refused with `ERROR FORBIDDEN`.

**`USAGE` (S → C), `0x0121`** — the figures, counted and never read from anything
anyone wrote:

    days (u16),
    n_days (u16), then per day, oldest first: day (u32), active (u32), messages (u32),
    n_weeks (u16), then per ISO week, oldest first: week (u32), active (u32),
    n_channels (u16), then per channel, busiest first:
        channel_id (u64), messages (u32), is_private (u8), name (str)

A `day` is days since the epoch in UTC; a `week` is an ISO week written `yyyyww`
(`202645`), its members counted once over the days of it inside the period.
`active` is how many members signed in, came back to a session, or used one that
day or week (migration 0072); `messages` how many messages people posted, the
workspace's own messages and call events not counted. The channels are the ten
(`OC_USAGE_TOP_CHANNELS`) with the most messages in the period, channels only,
never a direct message. A private channel's `name` is sent only to someone who is
in it, and is otherwise empty: a client shows "private channel". A frame with more
than 90 days, 15 weeks or 10 channels is malformed.

**`AUDIT_QUERY` (C → S), `0x0099`** `{ before_ms: u64, limit: u16, family: u8,
actor_id: u64, action: str }` — pages the audit log (REQ-251). `before_ms` pages
**backwards from a timestamp** rather than by offset, so a page boundary stays
stable as new entries arrive; `0` asks for the newest page. `limit` is the page
size: `0` means 50, and more than `OC_AUDIT_PAGE_MAX` (200) is cut to 200.
`family` (`0` any, else 1–4 as on `AUDIT_PAGE`), `actor_id` (`0` any) and
`action` (empty for any; an exact match, not a pattern) **narrow the page in the
daemon**, so a page holds only matching entries however sparse they are, and
paging back by `before_ms` keeps the filter. The three filter fields since
protocol 33.

**`AUDIT_PAGE` (S → C), `0x009A`** — a page of entries, newest first: time,
family (admin / account / security / moderation / MCP access), action, actor, target, outcome,
detail:

    count (u16), then count × { at_ms (u64), actor_id (u64), target_id (u64),
    family (u8 — 1 admin · 2 account · 3 security · 4 moderation · 5 MCP access),
    outcome (u8 — 1 ok, 0 denied/failed), actor_name (str), action (str),
    target (str), detail (str), channel_id (u64) }

`channel_id` is the channel the entry happened in, `0` for none: set for a
channel's own changes (`channel.*`) and for a moderated message or member in one
(`message.delete`, `message.restore`, `message.move`, where it is the channel the
message was moved to, and `channel.member.remove`), so an admin can go there;
for a message, `target_id` is the message. Entries written before SCHEMA.md
migration 0084 carry `0`. Appended in protocol 46.

It **never carries the secret involved** — that a password changed, never
the password; that an invite was redeemed, never the token (ARCH-79).

**The daemon's critical failures (REQ-263).** What the daemon cannot fix alone —
a TLS certificate it cannot obtain or renew, a CA asking for one to be replaced
now, one within a week of expiry — is kept for owners and admins (SCHEMA.md
migration 0051), one entry per failure while it holds. Nobody else is sent any of
it, and a member's request is refused with `ERROR FORBIDDEN`.

**`ALERTS_SUMMARY` (S → C), `0x00F6`** `{ unacked: u32, current: u32 }` — how
many entries nobody has acknowledged, and how many still hold. Sent to an owner
or admin in the sign-in sequence, right after `SNOOZE`, and to every
owner and admin connected whenever an entry is raised, cleared or acknowledged.

**`ALERTS_LIST` (C → S), `0x00F7`** — empty payload. Asks for the entries.

**`ALERTS` (S → C), `0x00F8`** `{ n: u16, n × { id: u64, first_ms: u64, last_ms:
u64, count: u32, current: u8, acked: u8, key: str, message: str } }` — newest
first, at most `OC_MAX_ALERTS` (100): what failed (`key`, stable, for example
`tls.obtain`), in the daemon's latest words (`message`, at most 400 bytes), how
many times between `first_ms` and `last_ms`, whether it still holds, and whether
it has been acknowledged.

**`ALERT_ACK` (C → S), `0x00F9`** `{ alert_id: u64 }` — acknowledge one entry,
or every one with `0`. Audited (`alert.ack`); answered by the `ALERTS_SUMMARY`
every owner and admin connected is sent. An entry still holding stays
acknowledged when it is raised again, until it stops and comes back.

---

### 5.16d Notification schedule, pause, and the notify default

Everything a user says about *when* they are notified, beyond §5.16's per-channel
level. Opcode `0x0091` is **retired and never reused** (§5.16, ARCH-103).

`SET_NOTIFY_DEFAULT` and `SET_MUTE` answer with a full `NOTIFY_PREFS` (`0x0093`)
to **all** of the user's connections, followed by `SNOOZE`, `SCHEDULE` and
`ALERT_PREFS`; the others answer, to all of them too, with the one frame they
changed (`SNOOZE`; `SCHEDULE`; `ALERT_PREFS`). That
three-frame trailer follows every `NOTIFY_PREFS`, not only a `LIST` request, so a
client always folds a complete picture rather than a delta.

`SET_TZ_OFFSET`, `SET_SNOOZE`/`SNOOZE`, `SET_SCHEDULE`/`SCHEDULE` and
`SET_KEYWORDS`/`SET_PRIORITY`/`ALERT_PREFS` are specified in §5.16.

**`SET_NOTIFY_DEFAULT` (C → S), `0x0077`** — Set my own fallback notification level, used by my channels that have no per-channel override. (REQ-134)

    level (u8)

A level above `2` (none) is refused with a non-fatal `ERROR MALFORMED_FRAME` (1003), and the default is left as it was.

**`SET_MUTE` (C → S), `0x006D`** — Mute or unmute one conversation for the calling user. (REQ-137)

    channel_id (u64), muted (u8)

**`SET_READ_CURSOR` (C → S), `0x006E`** — Set my read cursor for a channel deliberately, including BACKWARDS (mark unread). (REQ-235)

    channel_id (u64), message_id (u64)

`SET_READ_CURSOR` is deliberately **not** `CLIENT_ACK`: the ack path upserts
`MAX(existing, new)` so a replayed ack can never rewind anyone, and mark-unread
needs to move the cursor *backwards*. Once the new cursor is written, the daemon
sends a `READ_CURSOR` naming the actor to every connection the actor holds, the
one that asked included, so each of their devices moves to the new position and
counts its badge again. The channel's other members are not told: seen-by says a
member has seen a message, and that stays true.

`SNOOZE` is **self-only**. Other people learn *that* someone is not to be
disturbed, through the DND byte on `PRESENCE_UPDATE`, and never
when they are back (REQ-122). The instant is enforced on read: a stamp already in
the past reports as `0`, so no sweep and no shared clock are needed to agree a
pause is over.

**Keywords and priority people** (REQ-135, ARCH-103) are replaced wholesale
rather than edited, which keeps the ops idempotent and the caps enforceable.
Both lists cap at 64 (`OC_MAX_KEYWORDS`, `OC_MAX_PRIORITY`): the encoder sends
at most 64 and the daemon keeps the first 64, truncating rather than failing. The
daemon cuts each term to 63 bytes (`OC_KEYWORD_MAX` less its terminator) and
stores it lowercased; an empty term is dropped. A keyword hit is
**written into `mentions` with its own kind**, so the push query, the activity
feed and the reader's highlight all keep working unchanged — and it is matched by
`shared/mention.c`, never by SQL, so a client highlights exactly what the server
notified on. Priority people pierce a level and a pause but never a mute
(REQ-135/137).

**Email notifications** (REQ-365–367, ARCH-125/126) will add frames beside
these: reading and setting a person's mention email and digest, an owner's or
admin's workspace switches and member defaults, and telling a client the state
of each of the three levels so it can name the one that is off. They will be
specified in a subsection of §5.16 of their own as they are built; the design
they follow is [EMAIL.md](./EMAIL.md) §2.

### 5.16e Profile depth: custom status, title, timezone and avatar

**`SET_STATUS` (C → S), `0x006F`** — Set or clear my transient custom status. (REQ-241, REQ-122)

    emoji (str), text (str), expires_at (u64)

**`SET_PROFILE` (C → S), `0x00D8`** — Set my profile: full name, title, pronouns, phone, timezone and the voice I am read aloud in. (REQ-240, REQ-292)

    full_name (str), title (str), pronouns (str), phone (str), timezone (str), voice_id (str)

The whole profile in one frame, because it is edited on one screen and
committed by one button — a field-at-a-time wire would let Cancel mean "some of
it stuck". An empty field CLEARS that column (except `voice_id`, where empty keeps the
stored voice, so a client that knows nothing of read-aloud cannot clear it): the user deleted it, which is a
thing they did, and is a different fact from never having set it.

**`SET_AVATAR` (C → S), `0x0078`** — Point my avatar at an already-uploaded attachment, or clear it. (REQ-240; the image travels via the REQ-140 upload path)

    attachment_id (u64)

**`GET_PROFILE` (C → S), `0x0116`** — A member's whole profile, `user_id` 0 for mine; answered with `PROFILE_INFO` to this connection alone. Any member may ask for any member's (the roster is everyone's, REQ-030), but `phone`, the one field the roster does not carry, is filled only in one's own. `SET_PROFILE` replaces every field, so a client that changes one — the terminal client setting its voice — fetches first and sends the rest back as they were.

    user_id (u64)

**`PROFILE_INFO` (S → C), `0x0072`** — One person's complete profile — everything a roster shows about them — as the reply to any of SET_STATUS / SET_PROFILE / SET_AVATAR / SET_EMAIL / SET_PROFILE_FIELD_VALUE / GET_PROFILE. The reply to SET_PROFILE, SET_EMAIL and SET_PROFILE_FIELD_VALUE goes to every connection of the person who saved it, so their other devices show the change at once. (REQ-240, REQ-241)

    user_id (u64), display_name (str), email (str), status_emoji (str), status_text (str), status_expires (u64), title (str), timezone (str), avatar_id (u64), role (u8), full_name (str), pronouns (str), phone (str), voice_id (str),
    n_fields (u8), n_fields × { field_id (u64), value (str) },
    ooo_start (u64), ooo_end (u64), ooo_message (str)

The list (protocol 40) is the person's values for the workspace's own
profile fields, those they filled, at most 20; a client shows them in the order
`PROFILE_FIELDS` gives and ignores an id it does not know. The out-of-office
three after it (protocol 41) are as on `USER_LIST`.

**`SET_PROFILE_FIELD` (C → S), `0x0137`** — An owner or admin adds, renames or
deletes one of the workspace's profile fields (department, location, manager).

    field_id (u64), op (u8: 0 set, 1 delete), kind (u8: 0 text, 1 link), position (u16), label (str)

`field_id` 0 with op 0 adds a field; otherwise op 0 renames it, and moves it when
`position` is not 0. Its kind stays what it was made, because the values already
given were checked against it. Op 1 deletes it with every value anyone gave it.
`position` 0 puts a new field last. A label is 1–64
bytes; a workspace has at most 20 fields. Refused with `FORBIDDEN` to a member,
and `INVALID_PROFILE_FIELD` for a missing label, a kind outside the two, a
twenty-first field, or an unknown id. On success every signed-in connection is
sent `PROFILE_FIELDS`.

**`PROFILE_FIELDS` (S → C), `0x0138`** — The workspace's profile fields, in
`position` order: sent after sign-in, and to everyone when one changes.

    count (u8), count × { field_id (u64), kind (u8), position (u16), label (str) }

**`SET_PROFILE_FIELD_VALUE` (C → S), `0x0139`** — My value for one field; empty
clears it.

    field_id (u64), value (str)

At most 200 bytes, and a `link` field's value starts `https://`; anything else,
or a field the workspace does not have, is refused with
`INVALID_PROFILE_FIELD`. Answered with `PROFILE_INFO` to each of my connections.
A client shows a link's real address, as MARKDOWN.md has it for every link.

An **empty `text` on `SET_STATUS` clears all three fields together** — emoji,
text and expiry — because "no status" is one state rather than two, and a client
must not be able to leave an orphan emoji behind. `expires_at` is an absolute
instant, `0` meaning "until I change it", and the **daemon** enforces it on read:
a client that is not running cannot clear its own status.

`SET_AVATAR` takes an **attachment id, never bytes**, so the upload path's dedup,
size cap and reclamation all keep working. The id is validated rather than
trusted: it must exist, be finished (its bytes arrived) and not reclaimed, carry an
`image/*` MIME type, **and have been uploaded by the caller**. An unknown, unfinished,
reclaimed, non-image or another person's attachment is refused with the non-fatal
`UNKNOWN_ATTACHMENT` (3011), and the avatar is left as it was. The ownership check is
load-bearing — avatars are readable workspace-wide, so without it a member could point
their avatar at somebody else's private-channel attachment.

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

`state`: `0` pending · `1` sent · `2` failed (`fail_reason` says why) · `3` gone.

**`SCHEDULED_LIST` (S → C), `0x00C9`** — Terminator for a LIST_SCHEDULED stream, carrying how many SCHEDULED frames preceded it. (REQ-224, ARCH-102)

    count (u16)

`SET_DRAFT` upserts the draft for its conversation (`channel_id`,
`thread_root`), and an empty body deletes it — the same frame rather than a second op. A draft may be
**unaddressed** — `channel_id` 0 with a comma-separated `recipients` id list
beside it — because the New Message pane autosaves before a recipient is chosen
(REQ-229). An addressed draft requires read access to the channel — a public
channel, or a private one you belong to (`NOT_A_MEMBER` otherwise) — because a
draft is user content *about* a conversation, and storing one for a channel you
cannot see would report its existence back to you on every other device; a draft
in an encrypted DM is refused with `FORBIDDEN`. Bodies over
`OC_DRAFT_BODY_MAX` (16 KB) are **truncated, not refused**: no composer can type
one that long, so a frame that size is a bug or a probe, and losing its tail
beats losing the draft.

`DRAFT` is sent to the writer's **other** connections as a device sync — never
back to the connection that wrote it, which already has the text and would
otherwise be at risk of overwriting a composer still being typed in. A delete
arrives as the same frame with an empty body. A user holds at most one
unaddressed draft (`channel_id` 0); a new one replaces it. The surrogate `id`
names the row. `LIST_DRAFTS` streams newest
first, capped at 256 (`OC_MAX_DRAFTS`). Entries for private channels you have
since left are omitted but **not deleted**: leaving is reversible, so the draft waits for
you rather than being discarded.

*One frame for two jobs.* `DRAFT` is both the streamed list entry and the sync
push, exactly as `CLIENT_SETTINGS` doubles as snapshot and push — the client
folds it the same way whatever prompted it, instead of two frames that can drift.

A scheduled message is held in its own table and fired by a daemon
sweep on its own 15 s timer, delivered through the
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

**`RENAME_EMOJI` (C → S), `0x013D`** — Give a custom emoji a new name; the old one stays as an alias of the same image, so the messages and reactions that used it still show it. (REQ-072)

    name (str), new_name (str)

Allowed to the emoji's creator, an admin and an owner, as a delete is
(`FORBIDDEN` otherwise, and for a name that is no emoji). `new_name` follows the
names' rules (`MALFORMED_FRAME` otherwise) and must not be an emoji's or an
alias's already (`CHANNEL_EXISTS`, as `ADD_EMOJI` answers a taken name). The old
name becomes an alias of the emoji and the aliases it had come with it; past 8 on
the emoji or 256 in the workspace the rename is refused with `EMOJI_ALIAS_LIMIT`.
`ADD_EMOJI` is refused a name an alias holds. Answered, like an add, with
`EMOJI_LIST` to every connection.

**`EMOJI_LIST` (S → C), `0x007D`** — The whole custom-emoji catalogue — a reply to LIST_EMOJI and also the push after any add, rename or delete. (REQ-072)

    count (u16), then count repetitions of: name (str), attachment_id (u64), created_by (u64),
    created_at (u64), n_aliases (u8), n_aliases × alias (str)

`created_at` and the aliases are as of protocol 43. A client resolves `:alias:`
in a message or a reaction to the emoji the alias belongs to.

**`OPEN_GROUP_DM` (C → S), `0x0079`** — Open — or reopen — the group DM whose participant set is the caller plus the named users. (REQ-056)

    count (u16), then count repetitions of: user_id (u64)

**`MENTION_UNRESOLVED` (S → C), `0x00B3`** — Tell the sender alone which names in a just-sent message resolved to real people who are not in this channel, so the mention notified nobody. (REQ-287)

    channel_id (u64), message_id (u64), can_add (u8), is_private (u8), count (u16), then count repetitions of: user_id (u64), name (str)

**`LIST_FILE_CHANNELS` (C → S), `0x0073`** — Ask which channels hold files, and how many each holds.

    (empty)

**`FILE_CHANNELS` (S → C), `0x0074`** — One (channel_id, file count) pair per channel the caller can read that holds at least one file.

    count (u16), then count repetitions of: channel_id (u64), count (u32)

A custom emoji's **name is its identity**, flat and workspace-wide: a duplicate
is refused rather than replacing an image existing messages already refer to. A
group DM is not a second kind — it is the same `kind='dm'` channel under the same
participant-set key, which is why it needed no migration. `MENTION_UNRESOLVED`
goes to the **sender only**: nothing failed and the message was stored, so it is
a notice rather than an `ERROR`, and it carries `can_add`/`is_private` so the
client never offers an action that would fail.

### 5.16h Invite, session and webhook management

**`LIST_INVITES` (C → S), `0x004B`** — Ask for the outstanding (unconsumed, unexpired) tenant invites. (REQ-026)

    (empty)

**`INVITE_LIST` (S → C), `0x004C`** — The outstanding invites, identified by a server-side id — never the token. (REQ-026)

    count (u16), then count x { invite_id (u64), role (u8), created_at (u64), expires_at (u64), created_by (u64), uses (u16), used (u16) }

`created_at` is when the invite was made, ms since epoch UTC as `expires_at` is;
`0` for an invite made before the daemon recorded the time (SCHEMA.md §3bh).
`uses` is how many may redeem it (`0`: no limit) and `used` how many have (REQ-026);
an invite leaves the list when its last use is taken.

**`REVOKE_INVITE` (C → S), `0x004D`** — Revoke one outstanding invite by its id. (REQ-026)

    invite_id (u64)

**`INVITE_REVOKED` (S → C), `0x004E`** — Ack for REVOKE_INVITE, carrying the id so the client can drop that row without re-listing. (REQ-026)

    invite_id (u64)

**`REVOKE_SESSION` (C → S), `0x008E`** `{ session_id: u64 }` — sign one of your
own sessions out (REQ-182): its row goes, any connection on it closes, and the
answer is `SESSION_LIST` without it. A session id that is not the caller's
changes nothing.

**`LIST_SESSIONS` (C → S), `0x0075`** — Ask for the caller's own active sessions. (REQ-182)

    (empty)

**`SESSION_LIST` (S → C), `0x0076`** — The caller's unexpired sessions, so a device can be identified before it is revoked. (REQ-182)

    count (u16), then count x { session_id (u64), created_at (u64), last_seen (u64), expires_at (u64), current (u8), device_label (str) }

**`SET_WEBHOOK_STATE` (C → S), `0x006B`** — Enable or disable one incoming webhook. (REQ-170)

    webhook_id (u64), disabled (u8)

**`ROTATE_WEBHOOK` (C → S), `0x006C`** — Mint a new token for an existing webhook, invalidating the old one. (REQ-170)

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

    root_id (u64), channel_id (u64), root_author (u64), root_at (u64), last_reply_at (u64), reply_count (u32), unread (u32), following (u8), preview (str), action (u8)

`action` is `1` when the root is an action (REQ-058, §5.16l): `preview` is then its action text.

**`SET_THREAD_FOLLOW` (C → S), `0x00D4`** — Follow or unfollow one thread explicitly. (REQ-062, ARCH-104)

    root_id (u64), channel_id (u64), on (u8)

**`MARK_THREAD_READ` (C → S), `0x00D5`** — Mark a thread's replies read up to a reply id. (REQ-062, ARCH-104)

    root_id (u64), up_to (u64)

Unread replies are counted against a **per-thread cursor**, not the channel's:
the channel cursor advances when the channel is read and says nothing about a
thread inside it, and threads are deliberately outside the main scroll (REQ-060).
The follow and read acks return the one row that changed rather than a fresh
list, so opening a thread does not cost the whole view.

### 5.16j Forwards and link unfurls (REQ-057, REQ-222, ARCH-105, ARCH-117)

**`FORWARD` (S → C), `0x00D9`** — what a forwarded message points at (REQ-057),
fanned to the channel's members immediately behind the `BROADCAST` it describes.

    message_id (u64), channel_id (u64), src_channel (u64), src_message (u64),
    src_author (u64), src_excerpt (str), n_attach (u16),
    src_attach_name (str), src_action (u8)

`src_action` is `1` when the original was an action (REQ-058, §5.16l): the
excerpt is then its action text, and a card reads "`<src_author>` `<excerpt>`".

`message_id`/`channel_id` name the forward itself; the rest describe the
original. Every field is **resolved by the daemon**, never taken from the
sender, and all of it is a **snapshot** taken when the forward was sent —
editing the original afterwards does not rewrite what was forwarded.
**Replayed on backfill (§6.2) and on history paging (§6.3)**, for the reason the
reaction, pin and unfurl replays exist: the reference travels only on this
frame, a client stores nothing (ARCH-88), and a replay that omitted it would
lose the attribution on every reload.

`n_attach` counts the original's files and `src_attach_name` is the **first
one's name** — the card says "report.txt and 2 more" rather than listing a wall
of them, and clicking the card opens the original. The files themselves stay
with the original, because one attachment belongs to one message and copying
would mean either a shared `storage_key` — which breaks the orphan model
reclamation counts on (ARCH-77/78) — or duplicated bytes.

**`UNFURL` (S → C), `0x00D7`** — a URL's fetched preview, arriving after the
BROADCAST it belongs to — whenever the daemon's fetch completes, or never.

    message_id (u64), channel_id (u64), url (str), title (str), descr (str)

There is no request frame: the daemon extracts a committed message's URLs with
the **shared address scanner** (`shared/url.c` — the same rules the client
autolinks by, so an unfurl can never sit under text a client does not render as
a link), fetches the first 3 off the hot path behind an SSRF gate, and fans the
result to the channel's connected members. **Replayed on backfill (§6.2) and on
history paging (§6.3)**, because an unfurl travels only on this frame and a
replay that omitted it would silently lose every preview on reload — the defect
class the reaction and pin replays exist to prevent. An **edit drops the
message's stored unfurls** and re-fetches from the new body, so a removed URL's
preview cannot be replayed. Always on — there is no switch. Adding the frame
needed no protocol-version bump; a peer that does not know it never expects it.

### 5.16k User groups (REQ-307-309, ARCH-114)

A **group** is a named set of people with an `@handle`. Owners and admins manage
groups; everyone signed in sees them, since a composer offers them and a profile
lists them. A handle uses the mention scanner's characters (`[A-Za-z0-9._-]`), is
1–48 long, does not start with a digit or `.`, `-` or `_`, and is compared without
case. It shares one namespace with display names and the broadcast words: a group
cannot take `here`, `channel`, `everyone` or anyone's display name, and a person
cannot rename themselves to a group's handle (`GROUP_HANDLE_TAKEN`). A group holds
people only, at most 1024; a workspace holds at most 256 groups.

**`LIST_GROUPS` (C → S), `0x00E8`** — Ask for every group. The answer is one `GROUP_INFO` per group, then `GROUPS_END`. A group the answer does not name no longer exists. A client need not ask at sign-in: the daemon follows a session's first `USER_LIST` with the same answer, unasked.

    (empty)

**`CREATE_GROUP` (C → S), `0x00E9`** — Owner/admin. Answered, and announced to everyone, with the new group's `GROUP_INFO`.

    handle (str), name (str), description (str)

**`UPDATE_GROUP` (C → S), `0x00EA`** — Owner/admin: a group's handle, name and description, whole.

    group_id (u64), handle (str), name (str), description (str)

**`DELETE_GROUP` (C → S), `0x00EB`** — Owner/admin. The group leaves every channel it was in, and takes with it the membership it gave. Announced to everyone as `GROUP_DELETED`.

    group_id (u64)

**`GROUP_ADD_MEMBERS` (C → S), `0x00EC`** / **`GROUP_REMOVE_MEMBERS` (C → S), `0x00ED`** — Owner/admin: people in or out, at most 256 per frame. Each person added gains every channel the group is in. Each person removed loses those channels they are in only through it. An id that is nobody, or somebody removed from the workspace, is ignored.

    group_id (u64), count (u16), then count x { user_id (u64) }

**`GROUP_INFO` (S → C), `0x00EE`** — One group, whole. It is a list entry, and it is pushed to everyone signed in on any change.

    group_id (u64), handle (str), name (str), description (str), created_by (u64), count (u16), then count x { user_id (u64) }

**`GROUP_DELETED` (S → C), `0x00EF`** — Pushed to everyone signed in.

    group_id (u64)

**`GROUPS_END` (S → C), `0x00F0`** — Terminator of a `LIST_GROUPS` answer.

    count (u16)

**`CHANNEL_ADD_GROUP` (C → S), `0x00F1`** / **`CHANNEL_REMOVE_GROUP` (C → S), `0x00F2`** — Any member of the channel (not a DM), as with inviting a person. At most 32 groups per channel. The group's members are in the channel through it: added to the group later, they are in; taken out, they are out, unless they were also added directly. Answered like `INVITE_TO_CHANNEL`: the channel's `CHANNEL_INFO` to the actor, then its `CHANNEL_GROUPS`.

    channel_id (u64), group_id (u64)

**`CHANNEL_GROUPS` (S → C), `0x00F3`** — The groups a channel has.
- Sent right after every `CHANNEL_INFO` of a channel (not a DM), to the same people, none included.
- Sent after the channel list, for each channel.
- Sent to every member when the channel's groups change.

    channel_id (u64), count (u16), then count x { group_id (u64) }

**`CHANNEL_VIA_GROUP` (S → C), `0x00F4`** — Sent right after the `MEMBERS` terminator of every `LIST_MEMBERS` answer. It names the channel's members who are in it **only** through a group, which are the ones leave and remove refuse.

    channel_id (u64), count (u16), then count x { user_id (u64) }

**`MENTION_UNRESOLVED_MORE` (S → C), `0x00F5`** — To the sender only, right after `MENTION_UNRESOLVED`, when a message could not reach more people than that notice lists (a group larger than its eight names). It gives the total.

    channel_id (u64), message_id (u64), total (u16)

**Who gains or loses a channel by any of these** is told as an invitee and a
leaver are:
- A person who gained a channel gets its `CHANNEL_INFO` (joined 1) and its `CHANNEL_GROUPS`.
- A person who lost one gets its `CHANNEL_INFO` with joined 0. So does someone removed with `REMOVE_FROM_CHANNEL`.

**Leaving and removing.** Leave and remove clear someone's direct membership,
and a group the channel has keeps them in. Someone in a channel **only** through a
group cannot leave it, and cannot be removed from it alone. `LEAVE_CHANNEL` and
`REMOVE_FROM_CHANNEL` are refused with `MEMBER_VIA_GROUP`. They leave when they
leave the group, or when the group leaves the channel.

**`@handle` in a message** reaches every member of the group who can read the
channel, each under their own notification level, schedule and pause, as a
personal mention does. Members who cannot read it are listed to the sender in
`MENTION_UNRESOLVED` (§5.16g), up to its eight names, followed by
`MENTION_UNRESOLVED_MORE` with the total when there are more. A handle wins a clash
with a person's name. Only an identity provider's name at first sign-in can make
one.

### 5.16l Actions (REQ-058, ARCH-115)

**`ACTION` (S → C), `0x00FC`** — whether a message is an action: one that begins
`/me`, one or more spaces, then text, and reads "Ada Starr is away".

    message_id (u64), channel_id (u64), actor_id (u64), actor_name (str),
    text_start (u32), text_len (u32)

The body stays what was typed; this is the daemon's record beside it
(`actions`, SCHEMA.md §3au). `text_start` and `text_len` are **bytes** of the
body that are the action text. `actor_name` is a webhook's label, or empty for
the actor's current name, so a rename follows. **`text_len` 0 means "not an
action"**, which is how an edit or a restore unmakes one.

**Every live message frame is followed by exactly one `ACTION`** — `BROADCAST`
(a send, a webhook post, a call event), `THREAD_REPLY`, `MSG_EDITED` and
`MSG_RESTORED` — action or not, with only the message's own `FORWARD` allowed
between them. A client holds the message frame, its attachments and its forward
until that `ACTION` arrives and hands them on together, so nothing that shows a
message — a toast, a preview — ever sees it without its answer. **A replay sends
only the actions there are**: backfill (§6.2) and history (§6.3) after their
forwards, and `LIST_THREAD` (§5.10) before its `THREAD` terminator, so a client
releases a replayed message unpaired at the next frame and applies the action
when it comes.

Every excerpt the daemon builds of a message says whether it is an action, and
is then cut from the action text: `CHANNEL_LIST`'s `preview_action`,
`SEARCH_RESULTS`, `ACTIVITY_ENTRY`, `THREAD_SUMMARY`, `PINNED_MSG`, `SAVED_MSG`
and `FORWARD`'s `src_action`. A client never decides for itself whether text is
an action.

### 5.16m Summaries (REQ-310–313, ARCH-116)

Only when the daemon sent the **`summarize`** capability (`CAPABILITIES`, §5.14b): summaries are
off unless the operator turns them on. It is sent while the model is still
coming up, too — a request then waits in the queue until it is ready — and a
daemon whose model did not come up stops sending it, answering whatever waited
UNAVAILABLE.

**`SUMMARIZE` (C → S), `0x00FD`** — summarize a channel or DM the person can read.

    req_id (u32), channel_id (u64), scope (u8), start_ms (u64), end_ms (u64)

`scope` 0 **unread** (from the message last read to now), 4 **today** (the
reader's today, up to now), 3 **since yesterday** (the reader's yesterday and
today, up to now), 1 **last 7 days** (the reader's last seven days, today
included), 2 **custom range** (`[start_ms, end_ms)`; ignored otherwise). Days
are the reader's, by their time-zone offset. A daemon reads a scope it does not
know as unread.

**`SUMMARY` (S → C), `0x00FE`** — the answer, once, to one `SUMMARIZE`.

    req_id (u32), status (u8), summary_id (u64), channel_id (u64),
    start_ms (u64), end_ms (u64), version (str), body (lstr)

`status` 0 **ok**: `body` is the summary as JSON, in the client shape of
SUMMARIES.md §3, and `start_ms`/`end_ms` the span it covers. Its size is set by
the standard (SUMMARIES.md §1).
1 **unavailable** (summaries are off, or too many are being made), 2
**forbidden** (the person cannot read the conversation), 3 **failed**; `body` is
then a sentence saying why. A summary not yet made is made before the answer,
which on a small machine may take minutes; the connection carries on meanwhile.
`summary_id` changes whenever the summary is rebuilt — after a message in the
span is sent, edited, deleted or restored — and never names another.
4 **cancelled**: the asker cancelled it (`SUMMARY_CANCEL`); `body` says so.

A request is its asker's, not their connection's: it outlives the connection and
the daemon. Its `SUMMARY` goes to the connection that asked while that
connection still watches it; a request nobody watches — its asker detached
(`SUMMARY_DETACH`), or their connection closed, or the daemon restarted — is
made all the same and told as a notice (`SUMMARY_READY`). The same span asked
for again by the same person joins the request already waiting, and the new
asker watches it.

**`SUMMARY_QUEUED` (S → C), `0x0100`** — where a `SUMMARIZE` that is being made
is in the daemon's queue.

    req_id (u32), position (u16)

`position` is how many requests are ahead of it; 0, it is being made now;
`0xFFFF`, summaries are still coming up (the model being fetched or loaded) and
it waits for them. Sent
when the request joins the queue and again whenever the queue moves, until its
`SUMMARY`. A request answered from a stored summary gets none. A new frame, so no
version bump: a client that does not know it waits for the `SUMMARY` as before.

**`SUMMARY_DETACH` (C → S), `0x0101`** `{ req_id: u32 }` — stop watching this
connection's request `req_id` ("notify me when ready", or the summary closed
while it was made). It goes on, and is told as a notice when made.

**`SUMMARY_CANCEL` (C → S), `0x0102`** `{ req_id: u32 }` — cancel this
connection's request `req_id`: out of the queue, or, being made, stopped at its
next pause. Answered `SUMMARY` status 4; nothing of it is kept or told. One
already answered is left as it was.

**`SUMMARY_READY` (S → C), `0x0103`** — a summary made while its asker was not
watching: a notice.

    notice_id (u64), channel_id (u64), scope (u8), status (u8),
    start_ms (u64), end_ms (u64), made_at_ms (u64)

`status` as `SUMMARY`'s (0 made, else not). Sent to every connection the person
has when it is made, and again on every sign-in (after `AUTH_OK`) until the
notice is seen. A summary given live to the connection watching it is seen at
once. A notice is kept until seen, then a day (SUMMARIES.md §5).

**`SUMMARY_OPEN` (C → S), `0x0104`** `{ req_id: u32, notice_id: u64 }` — the
notice's summary as it was made, answered as `SUMMARY` to `req_id`: status 0 and
the body, or the status and sentence of one not made; status 1, "This summary is
no longer kept.", for a notice gone or not the person's. The notice is seen.

**`SUMMARY_DISMISS` (C → S), `0x0105`** `{ notice_id: u64 }` — the notice seen,
unopened.

A notice is in the person's Activity (`LIST_ACTIVITY`, the involved filter) as kind 4 **summary**: `message_id` the notice, `channel_id` its
conversation, `at` when it was made, `text` its scope in digits, `action` its
status. The five frames are new, so no version bump; kind 4 is a new value in an
existing field, which a client that does not know it shows as it shows any
unknown kind.

### 5.16n Polls (REQ-225, ARCH-119)

A poll is a message whose **body is its question**, with two to ten options fixed
when it is made (`polls`, `poll_options`, `poll_votes`: SCHEMA.md §3bd). It goes
anywhere a message goes — a channel, a DM, a group DM, a thread — but an
encrypted DM, whose options would sit in the clear. An edit (§5.5) corrects the
question and cannot reach the options. Deleting the message deletes the poll; a
restore brings back the question as an ordinary message. Only a daemon that sent
**`polls`** in `CAPABILITIES` (§5.14b) takes these frames.

`flags` is a bit set: `0x01` **MULTI** (a voter may pick any number of options),
`0x02` **ANONYMOUS** (who picked what is never sent), and, on `POLL` only, `0x04`
**HAS_MINE** (`mine` is the recipient's own selection).

**`POST_POLL` (C → S), `0x010F`** — make a poll.

    channel_id (u64), idempotency_token (16 bytes), parent_id (u64),
    question (lstr), flags (u8), close_at_ms (u64),
    count (u8), count × option (str)

`parent_id` 0 posts into the channel; otherwise the poll is a reply in that
thread (§5.10). `close_at_ms` 0 means it never closes. It is answered exactly as
`SEND` (or, with a parent, `SEND_REPLY`) is, with the same rate, idempotency and
permission rules: `SEND_ACK`, then the message to the conversation with its
`ACTION` (§5.16l) and then its `POLL`. Refused with `INVALID_POLL` when it has
fewer than two options or more than ten, an empty option or one past 100 bytes,
no question, files or a forward, or a deadline already past; with `FORBIDDEN` in
an encrypted DM. A count above ten is malformed.

**`VOTE` (C → S), `0x0110`** — my whole selection, replacing whatever I had.

    channel_id (u64), message_id (u64), count (u8), count × option (u8)

Option indexes count from 0. No options withdraws the vote. Refused, to the voter
alone, with `INVALID_POLL` for more than one pick on a single-choice poll, an
index the poll does not have, or the same one twice; `POLL_CLOSED` past the
deadline; `UNKNOWN_MESSAGE` for a message that is not a poll. The `ERROR`'s
context is the message id. Votes share the reaction rate window and notify
nobody.

**`POLL` (S → C), `0x0111`** — a poll's definition and tallies.

    message_id (u64), channel_id (u64), flags (u8), close_at_ms (u64),
    voters (u32), mine (u16), count (u8), count × { label (str), votes (u32) }

`voters` counts people, not picks. `mine` has a bit per option and means
something only with `HAS_MINE`. Sent live after the message's `ACTION`, and on
every vote: to the conversation without anyone's selection, and to the voter's
own connections with theirs. **A client keeps the selection it holds when a
`POLL` arrives without `HAS_MINE`.** Replayed with the asker's selection on
backfill (§6.2) and history (§6.3) after the actions, and by `LIST_THREAD`
(§5.10) before its `THREAD` terminator. No frame announces a deadline passing:
past `close_at_ms` a client draws the poll closed and the daemon refuses votes.

**`LIST_POLL_VOTERS` (C → S), `0x0112`** `{ channel_id: u64, message_id: u64 }` —
who picked what, for anyone who can read the conversation.

**`POLL_VOTERS` (S → C), `0x0113`** — the answer, ordered by option then person.

    message_id (u64), count (u16), count × { option (u8), user_id (u64) }

An anonymous poll is refused with `FORBIDDEN` by the daemon: it keeps who voted,
because one vote each needs it, and this is the one frame that could carry it.
The list stops at 4,096 entries.

**`EDIT_POLL` (C → S), `0x0114`** — the author changes a poll.

    channel_id (u64), message_id (u64), flags (u8), close_at_ms (u64),
    count (u8), count × option (str)

It carries the options, `MULTI` and `ANONYMOUS`, and the deadline **as they are
now to be, all of them**; the question is the body, changed with `EDIT`. Until
anyone has voted, anything may change. Once anyone has, the options and flags
must be sent as they stand, or it is refused with `POLL_HAS_VOTES`: changing
them would change what the cast votes meant. The deadline may always change —
put back, brought forward, `0` for never, or a time already past to close the
poll now. Refused with `FORBIDDEN` to anyone but the author, and `INVALID_POLL`
for options out of bounds. On success every member gets a `POLL` without anyone's
selection, keeping the one they hold. Adding these frames needed no
protocol-version bump.

### 5.16o To-do lists

A to-do list is a message whose **body starts `/todo`** followed by a title; each
following line that is not blank is a task, a leading `-` or `*` ignored, and
the text after a line's first `: ` its description. The daemon reads the body
when it stores the message and keeps the list in its own rows from then on
(`todo_lists`, `todo_tasks`: SCHEMA.md §3bx); the body stays the author's plain
text, so a client that draws no list still shows it readably. It goes anywhere
a message goes, by `SEND` or `SEND_REPLY`: a channel, a DM, a group DM, a
thread. An encrypted DM's body is not the daemon's to read, so it makes no list.

Limits: a title of at most 250 bytes, a task name of at most 250, a description
of at most 1,000, and at most 200 tasks. A send past one is refused with
`TODO_INVALID`, and one naming a task twice with `TODO_DUPLICATE`; the message is
not stored. **A to-do message cannot be edited** (`EDIT` is refused with
`TODO_NO_EDIT`): its title is renamed and its tasks added instead. Deleting the
message takes its list from every reader; a restore sends it back as it stood.
Nothing in an archived channel's lists changes (`CHANNEL_ARCHIVED`).

A task is named by its place in the list, from 0. Tasks are only ever added at
the end, never removed, reordered or changed, so a place always names the same
task.

**`TODO_ADD` (C → S), `0x0122`** — add a task, for anyone who can read the
message.

    channel_id (u64), message_id (u64), name (str), description (str)

Spaces and tabs are trimmed from both ends. Refused with `TODO_INVALID` for a
blank name or one past its limit, `TODO_DUPLICATE` for a name the list already
has, `TODO_FULL` at 200 tasks.

**`TODO_MARK` (C → S), `0x0123`** — mark a task done or not done.

    channel_id (u64), message_id (u64), task (u16), done (u8)

`done` is the state wanted, not a toggle: a task already in it is left as it is,
who marked it included, so two people marking the same task at once agree.
Refused with `TODO_INVALID` for a task the list does not have. A `done` above 1
is malformed.

**`TODO_RENAME` (C → S), `0x0124`** — the author renames the list.

    channel_id (u64), message_id (u64), title (str)

Refused with `FORBIDDEN` to anyone but the list's author, and `TODO_INVALID`
for a blank title or one past 250 bytes.

Each of the three is refused with `UNKNOWN_MESSAGE` for a message that is not a
list, is deleted, or the sender cannot read. A refusal goes to the sender alone
as an `ERROR` whose context is the message id.

**`TODO` (S → C), `0x0125`** — a list's whole state, a range of its tasks a
frame.

    message_id (u64), channel_id (u64), author_id (u64), title (str),
    total (u16), first (u16), count (u8),
    count × { name (str), description (str), done (u8), done_by (u64), done_at_ms (u64) }

The frame carries tasks `[first, first + count)` of `total`, at most 40 a frame;
a list is sent as its frames in order from `first` 0, which replaces whatever a
client held for it. `done_by` and `done_at_ms` are 0 for a task not done. Sent
live after the message's `ACTION` and on every change, to the whole
conversation; after `MESSAGE_RESTORED` for a restored list; replayed by backfill
(§6.2) and history (§6.3) after the polls, by `LIST_THREAD` (§5.10) before its
`THREAD` terminator, and ahead of `SEARCH_RESULTS` for each result that is a
list, so every one carries the list's current state. Adding these frames needed
no protocol-version bump.

### 5.16p Agents and their tokens (REQ-321, REQ-322)

An **agent** is an account that is not a person (docs/details/MCP.md §3.1): a
`users` row of kind `agent`, with a name and nothing a person signs in with,
which acts only through a **token**. Every frame here is an owner's or an
admin's; from anyone else it is refused `FORBIDDEN`. All are new frames, so no
version bump.

A token is 32 random bytes, sent as `ocpt_` and the bytes in base64url. It is
**sent once**, in `AGENT_TOKEN` to the connection that minted it, and stored only
as its SHA-256: no frame lists it again. `scopes` is names from the scope
catalog (`shared/scopes.c`, docs/plans/APPS.md §4) separated by single spaces;
the daemon keeps them in the catalog's order. An agent holds at most 10 tokens.

- **`CREATE_AGENT` (C → S), `0x0146`** `{ name: str, token_name: str, scopes:
  str }` — an agent, and its first token. Answered `AGENT_TOKEN`, and the agent's
  `USER_LIST` entry, flagged `OC_USER_AGENT`, to every connection. It takes no
  seat in the registered-user cap.
- **`AGENT_TOKEN` (S → C), `0x0147`** `{ agent_id: u64, token_id: u64, token:
  str, scopes: str }` — a token minted, the only time it is sent.
- **`REMOVE_AGENT` (C → S), `0x0148`** `{ agent_id: u64 }` — its tokens and its
  channel memberships go in one transaction, and its row is marked removed;
  its messages stay under its name. Answered `USER_UPDATED` (removed) to every
  connection, and each channel it was in is told as for a member leaving.
- **`MINT_AGENT_TOKEN` (C → S), `0x0149`** `{ agent_id: u64, name: str, scopes:
  str }` — another token. Answered `AGENT_TOKEN`.
- **`LIST_AGENT_TOKENS` (C → S), `0x014A`** `{ agent_id: u64 }`, answered
- **`AGENT_TOKEN_LIST` (S → C), `0x014B`** `{ agent_id: u64, count: u16,
  entries: [ { token_id: u64, name: str, scopes: str, created_by: u64,
  created_at_ms: u64, last_used_at_ms: u64, last_used_from: str } ] }` — each
  token's name, who minted it, when, its scopes, and when and from where it was
  last used (0 and empty for never; written at most once a minute).
- **`REVOKE_AGENT_TOKEN` (C → S), `0x014C`** `{ agent_id: u64, token_id: u64 }` —
  the token's row is deleted, and the next request with it is refused.
  Answered `AGENT_TOKEN_LIST`.

Refusals are `ERROR` frames: `FORBIDDEN` (not an owner or admin, or no such
agent), **`UNKNOWN_SCOPE` 3079** (a scope outside the catalog), **`TOO_MANY_TOKENS`
3080** (the agent already holds 10), and `INVALID_CHANNEL` for a name that is
empty or too long.

**Channels.** An owner or admin adds an agent to a channel with
`INVITE_TO_CHANNEL` and removes it with the channel's member removal, as for a
person, and must be in the channel themselves. Anyone else is refused
`FORBIDDEN`. An agent is never put in a DM, a group DM or a user group.

### 5.16q A person's AI clients (AUTH.md §8.13)

A person connects their own AI client to the MCP endpoint by approving it in
their signed-in app: the daemon's authorize page shows the client's name and an
eight-letter code, and the person enters the code here. The connection then
acts as them, and each one is listed and revoked here. All are new frames;
`via` on `BROADCAST` and `THREAD_REPLY` is the layout change of version 51.
Every frame is refused `FORBIDDEN` to a visitor, an agent or a removed account,
and `CONNECTIONS_OFF` 3084 while `OPENCHIME_MCP_CONNECTIONS` is off. No frame
carries a token.

- **`CONNECTION_CODE` (C → S), `0x014D`** `{ code: str }` — the code as the
  person typed it (either case, a dash or spaces anywhere). Answered
  `CONNECTION_REQUEST`, or `UNKNOWN_CODE` 3081 for a code that is not pending:
  unknown, past its ten minutes, or already acted on.
- **`CONNECTION_REQUEST` (S → C), `0x014E`** `{ code: str, client_name: str,
  redirect_uri: str }` — what is asking: the client's name, and where it will
  return the person, for them to check before approving.
- **`CONNECTION_DECIDE` (C → S), `0x014F`** `{ code: str, approve: u8,
  lifetime_days: u32 }` — approve (`1`), with the connection's lifetime: `0`
  for never, `30`, `90` or `365` (any other is `BAD_LIFETIME` 3083), or decline
  (`0`, lifetime ignored). Answered `CONNECTION_DECIDED`, or `UNKNOWN_CODE`.
- **`CONNECTION_DECIDED` (S → C), `0x0150`** `{ approved: u8, client_name: str }`.
- **`LIST_CONNECTIONS` (C → S), `0x0151`** `{ all: u8 }` — one's own (`0`), or
  everyone's (`1`, an owner's or admin's; anyone else `FORBIDDEN`). Answered
- **`CONNECTION_LIST` (S → C), `0x0152`** `{ all: u8, count: u16, entries: [ {
  connection_id: u64, user_id: u64, user_name: str, client_name: str,
  created_at_ms: u64, last_used_at_ms: u64, expires_at_ms: u64, lifetime_days:
  u32 } ] }` — the connections in force, newest first, at most 500;
  `last_used_at_ms` 0 for never, `expires_at_ms` 0 for no end.
- **`REVOKE_CONNECTION` (C → S), `0x0153`** `{ connection_id: u64, all: u8 }` —
  one's own, or with `all` anyone's as an owner or admin. Its tokens go at once:
  the client's next request is refused. Answered `CONNECTION_LIST` in the same
  scope, or `UNKNOWN_CONNECTION` 3082 for one that is not there or not the
  caller's.

---

### 5.17 Calls (REQ-150-152, REQ-160, REQ-161, REQ-301-306, ARCH-73, ARCH-86/87, ARCH-113)

Audio is **server-relayed** (no P2P/ICE, ARCH-18): the media flows over UDP to
the daemon's relay, which runs in its event loop and is handed each
participant's token as they join, and a call is *set up* over this TCP
protocol. **A call is an object of its own** ([CALLS.md](./CALLS.md) §1): a row
in `calls` (SCHEMA.md §3bz), whose id is its `call_id` in every frame below, with
a subject, invitees and participants of its own and no tie to a conversation but
the one message it leaves in the conversation it was started from, if any. Any
number of calls run at once. What only a running call has — connections, slots,
tokens, device keys, the epoch — is net-thread memory; what happened is written
to the record as it happens. CALLS.md is the design, including the end-to-end
encryption this section carries the keys for.

`CALL_START`, `CALL_JOIN`, `CALL_INVITE`, `CALL_SUBJECT`, `CALL_SHARE`, `CALL_KEY`, `CALL_CAMERA` and
`CALL_VIDEO_SUB` together are limited to 60 per 10 seconds per connection; one
past the limit is refused with a non-fatal `SEND_RATE_LIMITED`. `CALL_LEAVE`,
`CALL_DECLINE` and `CALL_END` are not limited: each changes the caller's own
state once, and refusing one would strand them in the call.

A **participant** is a user on one connection, with a **slot** (0-255, the low
byte of every SFrame KID it sends under) and its **device key** (an X25519
public key). A connection is in at most one call, and a user in a call from one
device: joining from a second moves them. The **epoch** starts at 1 and goes up
by one on every join and every leave. An **invitee** is someone asked to the
call, the starter first; being one is what lets a person join the call and be
told about it, and it lasts — declining or leaving does not undo it. The cap is
ten (`OC_CALL_MAX`; in development mode `OPENCHIME_DEV_CALL_MAX` sets it, 2 to
32), announced on `WORKSPACE_INFO` as `call_max`, and counts participants plus
invitations not yet taken or declined.

**`CALL_START` (C → S), `0x00AE`** `{ from_channel: u64, subject: str,
device_key: 32 bytes, codecs: u8, n: u16, n × { user_id: u64 } }` — start a call
and join it. `subject` is required: blank, or longer than 200 bytes, is refused
with `ERROR CALL_SUBJECT` (3075). `from_channel` is the conversation the call is
started from, `0` for none; the starter must be able to read it (else
`ERROR NOT_A_MEMBER`), and it then gets **one message** about the call — a
`BROADCAST` of kind `1` with the call's `call_id`, authored by the starter, whose
body reads "alice started a call: Q3 pricing review" — which notifies nobody;
when the call is renamed or ends the daemon rewrites that body (a `MSG_EDITED`
to the conversation's members) and once it is over it says how long the call
lasted. The users named are asked, those of them with an account that is on and
is not a visitor, at most as many as fit beside the starter; the conversation's
membership does not matter. `codecs` is the video codecs the starter can
decode, as bits: `1` VP9 (ARCH-87); a client with no video, `0`. The call's row
is made, and the starter put in it, as for `CALL_JOIN` below. A `CALL_JOIN`,
`CALL_LEAVE` or another `CALL_START` from the same connection before the answer
overtakes it: its call is made with nobody in it, and ends at once. A relay that
has exited and could not be restarted refuses a start with
`ERROR CALL_UNAVAILABLE` (3025); so is one when the daemon holds as many calls as
it can. On a public demo (AUTH.md §8.12) a start or join past the day's call
limit is refused with `ERROR DEMO_LIMIT` (3037), and a call ends for everyone —
`CALL_STATE` with `ended` set — twenty minutes after it started
(`OC_DEMO_CALL_MAX_MS`, `daemon/demolimits.h`).

**`CALL_JOIN` (C → S), `0x00A0`** `{ call_id: u64, device_key: 32 bytes,
codecs: u8 }` — join a call. Only its invitees may: anyone else is refused with
`ERROR NOT_A_MEMBER` ("not invited to this call"), and a call that is not
running with `ERROR NOT_IN_CALL` (3030). A call already at the cap is refused
with `ERROR CALL_FULL` (3028). A connection already in another call leaves it
first; one in this call is taken out and put back, with a fresh token and slot
— how a client whose address changed is relayed again.

**`CALL_JOINED` (S → C, to the joiner), `0x00A2`** `{ call_id: u64, udp_port:
u16, token: bytes, slot: u8, epoch: u32, starter: u64, started_at: u64, n: u16,
n × participant }`, where a participant is `{ user_id: u64, slot: u8,
device_key: 32 bytes, codecs: u8, flags: u8 }` — `flags` bit 0 (`AWAY`) marking
one whose connection went and whose seat is held for its rejoin (below), bit 1
(`CAMERA`) one whose camera is on (`CALL_CAMERA`), bit 2 (`RECORDER`) the
Recorder (below) — the joiner's private media
endpoint (the relay's `udp_port` and a bearer `token`), its slot, and the call
as it stands. The token is opaque to the client and 16 to 32 bytes long: 16
random bytes behind the daemon's `OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX`, if one
is set, and `udp_port` is `OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT` where that is
set (AUDIO.md §4). The client then speaks UDP directly to the relay at the host
it connected to, over IPv4 when the host has an IPv4 address and over IPv6 when
that is all it has: `token ‖ seq(u16) ‖ stream(u8) ‖ payload` to it,
`sender_user_id(u64) ‖ seq(u16) ‖ stream(u8) ‖ payload` from it
(`daemon/audio.h`). Every non-empty payload is an SFrame ciphertext (CALLS.md
§5.4); an empty one is a keep-alive. `stream` says which of the sender's streams
the packet is on — `0` audio (with its mute state and the viewers' and watchers'
control packets), `1` a shared screen, `2` a camera — in the clear, so the
relay can act on it, and authenticated as the SFrame metadata, so the relay
cannot change it unnoticed. A packet naming any other stream, or too short to
name one, is dropped. The relay forwards a camera's packets only while its
sender's camera is on, and only to the participants whose subscription takes it
(`CALL_VIDEO_SUB`); audio and a share go to everyone.

**`CALL_ROSTER` (S → C, to the other participants), `0x00A3`** `{ call_id: u64,
epoch: u32, n: u16, n × participant }` — on every join and leave, and when a
participant's connection goes (it is then marked `AWAY`), each in a new epoch;
and, to every participant, when a camera goes on or off, in the same epoch —
nobody came or went, so nobody rekeys. A device that finds its own slot gone
from the roster is out of the call: its user moved to another device if the
roster names its user elsewhere, else the relay swept it.

**`CALL_LEAVE` (C → S), `0x00A1`** `{ call_id: u64 }` — leave the call; naming
another call does nothing. The last one out ends the call. It also overtakes a
`CALL_START` from this connection still being made.

**`CALL_INVITE` (C → S), `0x00A4`** `{ call_id: u64, n: u16, n × { user_id: u64
} }` — a participant asks more people: each with an account that is on and is
not a visitor. Participants plus invitations past the cap: `ERROR CALL_FULL`,
and nobody is added. Not a participant: `ERROR NOT_IN_CALL`.

**`CALL_DECLINE` (C → S), `0x00A5`** `{ call_id: u64 }` — an invitee says no.
A decline from someone not invited does nothing. The decliner stays an invitee:
they may still join, and are still told.

**`CALL_END` (C → S), `0x00A6`** `{ call_id: u64 }` — the starter ends the call
for everyone. Anyone else in it: `ERROR NOT_CALL_STARTER` (3029); no such call,
or one the sender was never asked to: `ERROR NOT_IN_CALL`.

**`CALL_SUBJECT` (C → S), `0x00AF`** `{ call_id: u64, subject: str }` — rename
the call. Anyone in it may; the rename is on its timeline, with who and when,
and every invitee is told by `CALL_STATE`. A blank subject, or one longer than
200 bytes: `ERROR CALL_SUBJECT`. Not a participant: `ERROR NOT_IN_CALL`.

**`CALL_STATE` (S → C), `0x00A7`** `{ call_id: u64, subject: str, starter: u64,
started_at: u64, from_channel: u64, chat_channel: u64, flags: u8, ended: u8, np:
u16, np × { user_id: u64 }, ni: u16, ni × { user_id: u64 }, sharer: u64, rec_by: u64 }` — a call
as Calls home lists it: its subject, the conversation it was started from (`0`
none), its own chat (below), `flags` bit 0 set while it is being recorded, who is
in it (the people: never the Recorder), who
is asked and has not yet joined or declined, who is sharing a screen (`0`
nobody), and who started the recording going on (`0` while it is not recorded). Sent to the call's **invitees — the starter and every participant among
them — and to nobody else**, on every change to its subject, participants,
invitations or sharer, and at sign-in, after `SNOOZE`, once for each running
call the user is asked to. `ended = 1` is the last word on a call.

**`CALL_KEY` (C → S), `0x00A8`** `{ call_id: u64, epoch: u32, n: u16, n × {
recipient: u64, sealed: bytes } }` — a participant's media key for the epoch,
sealed to each other participant's device key (64 bytes: the HPKE `enc` and the
sealed 16-byte key with its tag, CALLS.md §5.3). The daemon forwards each copy,
unread, to its recipient if both are in the call and the epoch is the current
one; a copy for an earlier epoch is dropped, since its sender makes one for the
current epoch from the roster it is about to receive. The sender is not in the
call: `ERROR NOT_IN_CALL`.

**`CALL_KEY_FOR` (S → C), `0x00A9`** `{ call_id: u64, epoch: u32, sender: u64,
sealed: bytes }` — one sealed key, from its sender.

**The call's chat** is a conversation the call owns: a DM in kind, made with the
call, whose members are its invitees — each one asked later becomes a member
when asked — and which is in no conversation list (`CHANNEL_LIST`, the files'
conversations). Everything said in it goes by the ordinary frames (`SEND`,
`HISTORY_REQUEST`, `SEARCH`, uploads) on `chat_channel`, under the ordinary
membership rules, so it reaches the call's people and nobody else; its messages
notify as a group DM's do.

**Invitations notify** (REQ-302): the client decides a toast from `CALL_STATE`
through the shared evaluator with the invitation as a mention — under the
conversation the call was started from, if any, so its mute applies — and the
daemon pushes to the invitee's phones on the same terms, with `"kind":"call"`
in the contentless payload (§5.16).

**Missed calls** (REQ-304) leave no line anywhere since version 49: a call that
ends with nobody but its starter having joined is a missed call in its record,
and Calls home shows it as one. Missed-call lines stored before then are
ordinary kind-`1` messages with `call_id` `0`, and read as they always did.

**`CALL_RECORD` (C → S), `0x00BA`** `{ call_id: u64, on: u8 }` — start (`on = 1`)
or stop recording the call (CALLS.md §8). Anyone in it may do either. Where
recording is not offered — the daemon was built without it (`CALLREC=1`), an
admin turned it off (`OPENCHIME_CALL_RECORDING`), it is a public demo, or the
recordings already take the space they may (`OPENCHIME_CALL_RECORDING_MAX_MB`) —
a start is refused with `ERROR RECORDING_UNAVAILABLE` (3076); `CAPABILITIES`
names `call-recording` exactly where it is offered, and a client shows Record
only then. Not a participant: `ERROR NOT_IN_CALL`. A start while it records, or a
stop while it does not, changes nothing.

A recording is made by the **Recorder**, which the daemon runs and which joins the
call as a participant of its own: on the roster with `RECORDER` set, user id
`0xFFFFFFFFFFFFFF00` (no account's), a slot, and a device key made for this
recording and held in memory only; it sends no audio or video, and it is not
counted toward the cap. It comes in at a new epoch, so every device seals its key
to it as to anyone in the call — a `CALL_KEY` entry naming it is opened by the
daemon, which is the Recorder, and forwarded to nobody — and it can open nothing
said before; it seals a key of its own to each participant (`CALL_KEY_FOR` from
its user id), under which it sends the viewers' control packets — a NACK, a
keyframe request — as any viewer does. While it is in the call `CALL_STATE` has
`flags` bit 0 set and the timeline has who started it. Stopped — by anyone, or by
the call ending — it leaves at a new epoch, so nothing said from then is sealed
to it, and the recording is processed (CALLS.md §8): its page shows it as
processing, then ready, with an attachment to play it from, or failed.

**`CALL_HISTORY` (C → S), `0x00B4`** `{ before_ms: u64, limit: u16 }` — a page
of the calls this user was asked to, newest first: those started before
`before_ms` (`0` from the newest), at most `limit` (1–50; `0` or more is 50).
Answered with **`CALL_HISTORY_PAGE` (S → C), `0x00B5`** `{ n: u16, n × { call_id:
u64, subject: str, starter: u64, started_at: u64, ended_at: u64, end_reason: u8,
from_channel: u64, chat_channel: u64, n_people: u16, flags: u8, nf: u8, nf × {
user_id: u64 } }, more: u8 }` — `nf` (at most 3) of who took part, first joined first;
`ended_at` `0` and `end_reason` `0` while it runs, else `1` the last person left,
`2` ended for everyone, `3` the daemon stopped; `n_people` how many took part;
`flags` as this user saw it: bit 0 took part, bit 1 missed (asked, never joined
nor declined, and it is over), bit 2 declined, bit 3 asked to it while it runs
and not yet answered, bit 4 it has a recording ready, bit 5 it has a transcript;
`more` `1` when older calls follow. Calls home is built from it and from
`CALL_STATE`.

**`CALL_DETAIL` (C → S), `0x00B6`** `{ call_id: u64, line_from: u32 }` — one
call's page. Only those asked to the call get it — the starter, participants,
and invitees whether they joined or not; to anyone else, an admin included, it is
`ERROR NOT_A_MEMBER` ("not invited to this call"), whether the call exists or
not. Answered with **`CALL_PAGE` (S → C), `0x00B7`** `{ call_id, subject, starter,
started_at, ended_at, end_reason, from_channel, chat_channel` (as above)`, n:
u16, n × { user_id: u64, joined_at: u64, left_at: u64 }` — the stays, a rejoin
within the held seat one stay, at most 128 — `, n: u16, n × { user_id: u64,
invited_by: u64, invited_at: u64, declined_at: u64 }` — the invitees, at most
128 — `, n: u16, n × { at: u64, user_id: u64, kind: u8, detail: str }` — the
timeline, at most 128: `kind` `1` camera on, `2` camera off, `3` share started,
`4` share stopped, `5` recording started, `6` recording stopped, `7` renamed
(`detail` the new subject) — `, n: u16, n × { id: u64, state: u8, started_by:
u64, started_at: u64, stopped_at: u64, duration_ms: u32, bytes: u64,
attachment_id: u64 }` — the recordings, at most 16: `state` `1` recording, `2`
processing, `3` ready, `4` failed; a ready one is fetched and played through its
`attachment_id`, an attachment of the call's chat — `, has_transcript: u8,
language: str, n_lines_total: u32, line_from: u32, n: u16, n × { id: u64,
speaker: u64, start_ms: u32, end_ms: u32, text: str } }` — the transcript's
lines from `line_from`, at most 200 and 24 KiB of text a page; a client asks
again from where the page stopped for the rest. Times on the page are ms since
the epoch, a line's from the call's start. Any list past its cap is malformed.

**`CALL_DELETE` (C → S), `0x00B8`** `{ call_id: u64, what: u8, id: u64 }` —
delete the call's transcript (`what` `1`), an admin's to do, or one of its
recordings (`what` `2`, `id` the recording's), its starter's or an admin's, once
it is ready or failed. Anyone else: `ERROR FORBIDDEN`; nothing there:
`ERROR NOT_IN_CALL`. Done, every connection of those asked to the call, and the
deleter's, is sent **`CALL_DELETED` (S → C), `0x00B9`**, the same three fields;
a recording's bytes go with it.

**Transcripts are searched** with `SEARCH` (§5.11): a plain text search's first
page carries, after the messages and snippets it finds, the transcript lines it
matches in the calls the searcher was asked to — each a `SEARCH_RESULTS` entry
with `message_id` `0`, `call_id` and `line_id` set, `channel_id` the call's chat,
`author_id` the speaker and `server_time` when it was said — which a client
opens as the call's page at that line. A filtered search, or a later page, is the
messages' alone.

**The transcript function.** The daemon stores a call's transcript through one
function (`oc_dbwriter_call_transcript`, `daemon/dbwriter.h`): its language and
its lines, replacing any it had. Nothing calls it yet; what makes transcripts
will.

**The record** (SCHEMA.md §3bz): the call's row when it starts and when it ends
— how: the last person left, it was ended for everyone, or the daemon stopped
with it open, which a daemon starting over such a row records; each
participant's stay, from a join to a leave, a rejoin within the held seat being
the same stay; each invitation and decline; and the timeline — cameras on and
off, shares started and stopped, renames.

**Loss and rejoin (REQ-152).** A participant is dropped on `CALL_LEAVE`, and when
the relay's silence sweep drops it (the relay runs in the daemon's event loop,
which takes the participant out as a leave would; a client keeps alive every 5 s,
so only the vanished are swept). A swept participant whose connection is still
open is sent a `CALL_ROSTER` without it, as a device its user moved away from is,
so it knows it is out. Each drop is a new epoch for the rest, who rekey.

A **TCP disconnect holds the seat** rather than dropping it: the participant's
token is revoked, so nothing reaches it, but it stays in the roster and the call
for 15 s (`OC_CALL_REJOIN_GRACE_MS`), marked `AWAY` in a `CALL_ROSTER` to the
rest in a new epoch. A `CALL_JOIN` from the same user on a new connection takes
the seat back — a fresh token and slot, a new epoch — and past
the grace the seat is dropped as a leave would be. A call whose participants all
lose their connection at once is therefore still there when they come back. A
sharer that disconnects stops sharing at once.

A participant whose connection goes, who leaves or who rejoins has its camera
off; a rejoin turns it on again with `CALL_CAMERA`.

A client rejoins by itself (CALLS.md §4); a rejoin is an ordinary `CALL_JOIN`,
and a client closing on purpose sends `CALL_LEAVE` first so no seat is held for
it.

**`CALL_SHARE` (C → S), `0x00AA`** `{ call_id: u64, on: u8 }` — start
(`on = 1`) or stop sharing a screen in the call (REQ-161). **One sharer per
call**: a start takes over from whoever was sharing, who learns it from the
`CALL_STATE` that follows and stops. A stop from someone not sharing changes
nothing. Not a participant: `ERROR NOT_IN_CALL`. A sharer who leaves,
disconnects, is swept by the relay or rejoins is no longer the sharer. Each
change is a `CALL_STATE`, and on the call's timeline.

**`CALL_MEDIA` (both ways), `0x00AB`** — call media over this connection, for a
participant whose network passes no UDP to the relay. Offered when the daemon
names `calls-tcp` in `CAPABILITIES`; a client does not send it otherwise. The
frame carries exactly what a relay datagram carries, without the token (the
connection is authenticated and in one call):

| Direction | Payload |
|---|---|
| C → S | `{ seq: u16, stream: u8, ct: bytes }` |
| S → C | `{ sender: u64, seq: u16, stream: u8, ct: bytes }` |

A participant is on whichever transport its latest packet came by — a
`CALL_MEDIA` puts it on the connection, a datagram puts it back on UDP — and the
relay forwards each packet to each other participant by that participant's own
transport, so one call can hold both. An empty `ct` is a keepalive, answered to
its sender alone, on the transport it came by. Media never costs a connection: a
`CALL_MEDIA` for a connection with 256 KiB or more already waiting to be written
is dropped, and the loss is recovered as UDP loss is (FEC and concealment for
audio, resends and keyframes for a share). A client probes UDP first, and falls
back when the relay does not answer (AUDIO.md §4).

**`CALL_CAMERA` (C → S), `0x00AC`** `{ call_id: u64, on: u8 }` — turn this
participant's camera on (`on = 1`) or off in the call (REQ-160). The relay
carries its camera stream only while it is on, and a change is a `CALL_ROSTER`
to every participant, marking it `CAMERA`, in the same epoch, and on the call's
timeline. Not a participant: `ERROR NOT_IN_CALL`.

**`CALL_VIDEO_SUB` (C → S), `0x00AD`** `{ call_id: u64, all: u8, n: u16,
n × { user_id: u64 } }` — whose cameras the relay sends this participant: every
one (`all = 1`, and nobody named), or the `n` named (at most 32; more is
malformed). A participant starts at every one, and at every one again on a
rejoin; a view sends what it shows. Not a participant: `ERROR NOT_IN_CALL`.

**Video on the wire (ARCH-86/87)** — a shared screen and the cameras — rides the
same relay and SFrame packets, on their own streams; the codec is a **wire
contract** — `codecs` above, VP9 mandatory, a new bit for a successor.
Fragments, NACKs, keyframe requests and rate reports travel inside the
encryption ([CALLS.md](./CALLS.md) §5.4, §6, §7).

### 5.18 Push device tokens (REQ-132/133, ARCH-85)

**`REGISTER_DEVICE_TOKEN` (C → S), `0x00B0`** `{ platform: u8, token: str }` —
register the caller's mobile push token (`platform` 0 = APNs, 1 = FCM). Upserts on
`(user, token)`. A token is 1–511 bytes of letters, digits, `_`, `-`, `:` and `.`,
as APNs and FCM issue them; anything else is refused whole, never cut short. The daemon owns this registry; central's push gateway stores
nothing (REQ-041). Answered with `DEVICE_TOKEN_ACK`.

**`UNREGISTER_DEVICE_TOKEN` (C → S), `0x00B1`** `{ token: str }` — drop the caller's
registration of a token (logout / token rotation). Answered with `DEVICE_TOKEN_ACK`.

**`DEVICE_TOKEN_ACK` (S → C), `0x00B2`** `{ ok: u8, code: u16 }` — `ok=1` on success;
on failure `code` is the reason (`OC_ERR_INVALID_DEVICE_TOKEN` for a token no device
issues (§8.2) or an unknown platform). A committed SEND then drives contentless APNs/FCM delivery to
registered tokens via the emitter (ARCH-85), gated by each recipient's level + DND.

---

### 5.19 Encrypted DMs (PRIVATE-DM.md)

An encrypted DM is a DM channel marked `e2e` (`CHANNEL_INFO` and `CHANNEL_LIST`,
§5.7) that joins two devices, end to end. The `PDM_*` frames carry its handshake,
a Noise XX exchange the daemon routes and cannot read, and the opener's first
ratchet message; everything said in it then goes as ordinary frames (§5.1–5.13)
whose bodies are ciphertext. The `pdm` field of these frames is the channel id.
A client sends none of these to a daemon whose `CAPABILITIES` lack `pdm`, since
an unknown frame closes the connection. Limits: a handshake message is 1–256
bytes, a header exactly 40, a ciphertext 16–16,400; a frame outside them is
malformed. `PDM_OPEN` and `PDM_HS` together are limited to 30 per 10 seconds per
connection; one past it is refused with a non-fatal `SEND_RATE_LIMITED`.

| Frame | Payload |
|---|---|
| `PDM_OPEN` | `peer u64`, `hs bytes` |
| `PDM_OPENED` | `pdm u64`, `peer u64` |
| `PDM_INVITE` | `pdm u64`, `from u64`, `hs bytes` |
| `PDM_HS`, `PDM_HS_FOR` | `pdm u64`, `hs bytes` |
| `PDM_SEND`, `PDM_MSG` | `pdm u64`, `header bytes`, `ct bytes` |
| `PDM_END` | `pdm u64`, `reason u8` |

- **Opening.** `PDM_OPEN` to someone else who is signed in makes a new DM channel
  between the two (`e2e=1`, never the pair's ordinary DM or an earlier encrypted
  one). The opener is sent `PDM_OPENED` with its id, then its `CHANNEL_INFO`
  (`e2e=1`); each of the peer's connections gets `PDM_INVITE`. Anything else
  (oneself, nobody signed in, 16 encrypted DMs already open for either person,
  the daemon already carrying 256, or a channel that could not be made) is `PDM_END` with id 0 and reason 5.
- **Answering.** The first of the peer's devices to send `PDM_HS` for it takes it;
  the daemon binds that device's session, sends it the channel's `CHANNEL_INFO`
  (`e2e=1`), tells the peer's other devices `PDM_END` reason 2, and relays the
  message to the opener as `PDM_HS_FOR`. The opener's `PDM_HS` (handshake 3)
  goes the other way. `PDM_END` from a device still holding the invitation
  declines it (reason 6 to the opener).
- **The first ratchet message.** `PDM_SEND` from a bound device reaches the other
  as `PDM_MSG`. Clients use it once, for the opener's first, empty ratchet
  message, which lets the other side's ratchet send. It counts against the
  message rate and, in demo mode, the daily message limit.
- **Messages.** `SEND`, `SEND_REPLY`, `EDIT`, `DELETE`, `REACT`, `TYPING` and read
  cursors work as in any DM. A body is `oc-e2e:1:` followed by the base64 of the
  40-byte ratchet header and the AES-GCM ciphertext; reaction emoji are not
  encrypted. While the conversation is live, what is said in it goes only to its
  two bound connections, and a `SEND`, `SEND_REPLY`, `EDIT`, `DELETE` or `REACT`
  from any other connection is refused with `FORBIDDEN` (a `TYPING` is dropped).
  The daemon reads no body in it: no search, mentions, keyword hits, actions,
  unfurls, summaries (`SUMMARY` answers unavailable), read-aloud, voice input or
  forwards, and every excerpt (list preview, activity, threads, pins, saved) is
  empty. A `SEND` or `SEND_REPLY` with attachments or a forward, `UPLOAD_BEGIN`,
  `CREATE_WEBHOOK`, `SCHEDULE_MESSAGE`, `SET_DRAFT`, `PIN` and `SAVE_ITEM` are refused
  with `FORBIDDEN`.
- **Away and back.** A bound device whose connection drops keeps its place for its
  session: its `PDM_*` frames wait (at most 64) and are delivered when a
  connection signs in with the same session, which catches up on the channel as
  any client does. A side away 24 hours, an invitation unanswered for 24 hours,
  or a full queue ends the DM (reason 3). A fresh sign-in is another session and
  is given nothing of it.
- **Ending.** `PDM_END` from a bound device ends it for both; the other device is
  told reason 1, or 4 when the sender says it no longer holds the session (a
  restarted app answers frames for a DM it does not know with reason 4). A frame
  naming a DM that is gone, or one the sender is not bound to, is answered with
  `PDM_END` reason 5.

Reasons: 1 closed, 2 taken by another device, 3 gone, 4 lost, 5 refused, 6 declined.
The session is held in memory only. When it ends its channel is closed
(`archived_at_ms` set), so a later write is `CHANNEL_ARCHIVED`; a daemon restart
closes every encrypted DM; and the storage maintenance pass deletes one closed
more than 24 hours ago, with every row about it (PRIVATE-DM.md §2).

### 5.20 Blocks and DM privacy

A block is its blocker's alone, and nothing tells the person blocked
(SCHEMA.md §3br). While it holds, the blocked person can start no direct
conversation naming the blocker: `OPEN_DM` and `OPEN_GROUP_DM` are refused with
the non-fatal `DM_REFUSED`, and `PDM_OPEN` with `PDM_END` reason 5. In a direct
conversation the two already share, what the blocked person sends is stored and
reaches every other member, but never the blocker: not live, not by push, not in
backfill or history, not in unread counts or activity, and not after the block
is lifted. Channels are untouched. DM privacy `nobody` refuses every new direct
conversation naming that person the same way; one that exists carries on.

**`SET_BLOCK` (C → S), `0x0132`** `{ blocked_id: u64, on: u8 }` — block (`on` 1) or
unblock (0) one person. Oneself, the workspace's own sender, or nobody is
ignored. Answered with `BLOCKS`.

**`BLOCKS` (S → C), `0x0133`** — whom I block, and my DM privacy.

    dm_privacy (u8: 0 anyone, 1 nobody), count (u16), count × user_id (u64)

Sent to each of my connections after `SET_BLOCK` or `SET_DM_PRIVACY`, and with
the answer to `LIST_NOTIFY_PREFS`. At most 256 people; a frame with more is
malformed.

**`SET_DM_PRIVACY` (C → S), `0x0136`** `{ dm_privacy: u8 }` — who may start a
new direct conversation with me. Answered with `BLOCKS`.

**`LIST_BLOCKS` (C → S), `0x0134`** — empty; every block in the workspace, for an
owner or admin (`FORBIDDEN` otherwise).

**`BLOCK_LIST` (S → C), `0x0135`** — the answer, newest first, at most 500.

    count (u16), count × { blocker_id (u64), blocked_id (u64), since_ms (u64) }

Adding these frames needed no protocol-version bump.

### 5.21 Out of office (REQ-122)

**`SET_OUT_OF_OFFICE` (C → S), `0x013A`** — my absence, apart from the custom
status, which shows beside it.

    start_ms (u64), end_ms (u64), message (str)

In force while `start_ms` ≤ now < `end_ms`. An `end_ms` of 0, or one already
past, clears it (start, end and message all go). Refused with the non-fatal
`INVALID_OUT_OF_OFFICE` for an end not after its start or a message over 280
bytes. On success the person's connections are sent `PROFILE_INFO`, and every
signed-in connection a `USER_LIST` of one, their row: everyone's marks and
composer notices follow it. Nothing is sent when it ends; a client compares the
times with its clock. While it is in force a client marks the person "Out of
office until <date>" wherever they appear, and someone writing them a DM or
mentioning them is told so, with the message, before sending.

### 5.16r Release notes (CLIENT.md, "What's new")

A client asks its own daemon for a build's release notes, and the daemon asks
central for them with its workspace credential (AUTH.md §3): no client reaches
central, and no request crosses origins.

```
RELEASE_NOTES_GET (0x015A), C → S
  build_id  str     the build whose notes: 1–64 bytes of [A-Za-z0-9._-]

RELEASE_NOTES (0x015B), S → C
  build_id  str     as asked
  status    u16     central's answer: 200 with the notes, 404 when the build is no release
  body      bytes   central's body as it gave it, at most 60 000 bytes
```

Only a signed-in connection may ask; a build id that is not a plain token is
refused as `MALFORMED`. The daemon fetches with a signed `GET` to
`<OPENCHIME_ENROLL_URL's origin>/v1/workspace/releases/build/<build id>` off the
event loop, one fetch per build id at a time, and keeps an answer 6 hours, a
failure 5 minutes, and at most 32 build ids. A box not connected to the
OpenChime services, or one central did not answer, gets
**`RELEASE_NOTES_UNAVAILABLE` 3088**; the client then offers the notes page.
New frames: no version bump.

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
back. For `count = 0`, the same rule applies to every channel the user belongs
to; a channel left out of a non-empty list is not replayed.

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
caller's *member* channels and sends each its newest page.

### 6.2 Replay and `BACKFILL_DONE` (server → client), msg_type `0x0031`

In response, the server streams ordinary `BROADCAST` frames (§5.3) for every
top-level message with `message_id > after_message_id` in each requested channel
(thread replies are not replayed: they come with `LIST_THREAD`, and their parents
carry `THREAD_META`), in
ascending `message_id` order per channel, then sends a single `BACKFILL_DONE`
to mark the catch-up complete.

The replay goes out a slice at a time, as the connection drains, and **nothing
else is written to that connection until it is complete**: a `BROADCAST` of a
new message, a presence change or any other frame raised while a replay is in
progress follows its `BACKFILL_DONE`, in the order it was raised. So the replay
stays ascending and whole, and a client's cumulative `CLIENT_ACK` never covers
an id it has not yet been sent. A connection that stops reading while frames
wait behind a replay is closed once those pass the output cap, as it would be
without one.

The replay also carries the **reaction state** of the messages it sends: after
the `BROADCAST` frames (each followed by `THREAD_META` when it has replies),
the pins (`PIN_UPDATED`) and the requester's saved marks (`SAVED_UPDATED`), the
server emits one `REACTION_UPDATED` (§5.9) per
(message, emoji) with the aggregate count. A `BROADCAST` has no room for
reactions, so without this a client that keeps no local cache (ARCH-88) lost
every reaction the moment it reloaded. The `user_id` on those frames is the
requesting user whenever they are one of the reactors, so the client can render
its own reactions as such without a second round trip.

After the reactions come the link previews, forwards and actions of the messages
replayed, then their **polls** (§5.16n): one `POLL` per poll, carrying the
requesting user's own selection, so a reload shows what they picked; then their
**to-do lists** (§5.16o): each list's `TODO` frames, as the list stands now.

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

The response uses §6.2's replay encoding: `BROADCAST` frames in **ascending** id
order (the order the client's high-water dedup expects) — each with its
attachments, reply counts and author-name overrides — followed by the same
side-state a reconnect carries, in this order: `PIN_UPDATED` for each pinned
message, `SAVED_UPDATED` for the requester's own saved marks, `REACTION_UPDATED`
per (message, emoji), the matching `UNFURL`, `FORWARD` and `ACTION` frames, one
`POLL` per poll (with the requester's selection), each to-do list's `TODO`
frames (§5.16o), then `BACKFILL_DONE`.

That list is the point of the path, not a detail of it. A client stores nothing
(ARCH-88), so state which travels only on a live fan-out is lost on any reload
that is not a reconnect — which is every scroll-back and every permalink. The
side-state is built by the **same filler the reconnect replay uses**, so the two
paths cannot drift apart.

On this path `more` means **"older messages exist above this page"**, which is
how a client knows to stop asking rather than retrying at the top of a channel
forever.

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
| `str`   | `u16` byte-length `n`, then `n` bytes of UTF-8 (no NUL terminator). `n <= 65535`. Used for short strings (client info, error messages). |
| `lstr`  | `u32` byte-length `n`, then `n` bytes of UTF-8. Used for message bodies, which may exceed a `u16` length (a body's `n <= MAX_BODY_SIZE`), and for tokens (the JWT in `AUTH`, the device token). |
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
| `context` | bytes| Optional correlation data. For a failed `SEND`, this is the 16-byte `idempotency_token`; for other requests, the id the section names (a message, attachment or segment id, big-endian); otherwise empty (`n = 0`) or a short label. |
| `message` | str  | Human-readable detail, for logs/display.                             |

### 8.2 Reason code table (REQ-111)

Codes are grouped by range so a client can categorize an unrecognized code.

| Code   | Name                  | Stage      | Fatal | Meaning                                                        |
|--------|-----------------------|------------|-------|----------------------------------------------------------------|
| `1001` | `VERSION_TOO_OLD`     | handshake  | yes   | Client's `max_version` < server minimum. Show "please update." |
| `1002` | `VERSION_TOO_NEW`     | handshake  | yes   | Client's `min_version` > server maximum. Operator must upgrade the daemon; do **not** prompt the user to update the app. |
| `1003` | `MALFORMED_FRAME`     | any        | see note | A `HELLO` that does not decode (fatal, as `REJECT`); non-fatal for a custom-emoji name out of bounds (`ADD_EMOJI`/`DELETE_EMOJI`), a group DM of fewer than three people, or a `SET_NOTIFY_DEFAULT` level out of range. Any other frame that does not decode closes the connection without an `ERROR`. |
| `1004` | `FRAME_TOO_LARGE`     | any        | yes   | Reserved: a `length` implying a frame larger than `MAX_FRAME_SIZE`. The daemon closes the connection without sending it. |
| `1005` | `UNEXPECTED_MSG_TYPE` | any        | yes   | A frame not valid in the current state: a non-`HELLO` first frame (sent as `REJECT`, since no version is negotiated yet) or a post-auth type this version does not define (sent as `ERROR`). A `SEND` before `AUTH_OK` gets the more specific `AUTH_REQUIRED` instead. |
| `1006` | `VERSION_MISMATCH`    | post-hello | yes   | A post-handshake frame carried a `version` other than the negotiated one. Distinct from `1001`/`1002`, which answer a `HELLO` and precede a session: this says the peer agreed and then sent something else. The `ERROR` itself is stamped with the negotiated version, so it is readable by the peer being disconnected. |
| `2001` | `AUTH_REQUIRED`       | post-hello | yes   | A messaging frame arrived before successful `AUTH`.            |
| `2002` | `AUTH_INVALID_TOKEN`  | auth       | on `AUTH` | JWT failed signature/audience/expiry validation (REQ-023).     |
| `2003` | `AUTH_RATE_LIMITED`   | auth       | on `AUTH` | Too many attempts from this address (or, for a password on the sign-in page, for this account), or too many direct sign-ins under way from this address (REQ-191). |
| `2004` | `USER_LIMIT`          | auth       | yes   | Workspace at its registered-user cap (`OPENCHIME_MANAGED_MAX_USERS`); a new user cannot be created. An existing user still logs in. Also sent, non-fatal, to an owner or admin whose `ENABLE_USER` would take a seat past the cap. |
| `2005` | `AUTH_NOT_ALLOWED`    | auth       | yes   | A valid identity that no join rule or invite admits (AUTH.md §8.4). An identity the workspace already knows still signs in. |
| `2006` | `AUTH_SOURCE_UNAVAILABLE` | auth   | on `AUTH` | `AUTH_BEGIN` named a source this deployment does not offer, or one it cannot reach; or a password came in a frame, where the daemon takes one only on its pages (AUTH.md §8.10). |
| `2007` | `AUTH_PENDING` | auth   | no    | A device code not approved yet (§4.2c). |
| `2008` | `AUTH_SLOW_DOWN` | auth   | no    | A device-code poll sooner than the interval; the interval grows by five seconds. |
| `2009` | `AUTH_EXPIRED` | auth   | no    | A device code that ran out, was collected, or never was. |
| `2010` | `AUTH_DENIED` | auth   | no    | A device code refused on the page. |
| `2011` | `AUTH_USE_PROVIDER` | auth | yes | A sign-in by emailed code for a person who signs in here with a provider; they sign in with that provider instead (AUTH.md §8.4). |
| `2012` | `AUTH_SETUP_REQUIRED` | auth | — | Pages only: a local account with no second step where `OPENCHIME_MFA=required` (AUTH.md §8.6). |
| `3001` | `BODY_TOO_LARGE`      | messaging  | no    | A `SEND` or `SEND_REPLY` body over `MAX_BODY_SIZE`; nothing is stored, and the token is in `context` (§5.1). |
| `3002` | `NOT_A_MEMBER`        | messaging  | no    | Sender is not a member of the target channel (REQ-031).        |
| `3003` | `UNKNOWN_CHANNEL`     | messaging  | no    | `channel_id` does not exist in this tenant.                    |
| `3004` | `SEND_RATE_LIMITED`   | messaging  | no    | Per-connection rate exceeded (REQ-190): sends; also reactions (§5.9), poll votes, encrypted-DM (`PDM_*`) frames, voice-input segments and call signalling (§5.17). |
| `3005` | `FORBIDDEN`           | admin/msg  | no    | The actor may not perform the action — role (ARCH-60, §6) or not the message's author (§5.5/5.6). |
| `3006` | `LAST_OWNER`          | admin      | no    | Would remove or demote the tenant's last owner (REQ-030).      |
| `3007` | `UNKNOWN_MESSAGE`     | messaging  | no    | `EDIT`/`DELETE` names a message not in the channel, or already tombstoned (§5.5/5.6). |
| `3008` | `INVALID_CHANNEL`     | messaging  | no    | `CREATE_CHANNEL` name empty or over 64 bytes (§5.7). |
| `3009` | `INVALID_REACTION`    | messaging  | no    | `REACT` emoji empty or over 32 bytes (§5.9). |
| `3010` | `ATTACHMENT_TOO_LARGE`| messaging  | no    | Upload exceeds `MAX_ATTACHMENT_SIZE` (REQ-140). |
| `3011` | `UNKNOWN_ATTACHMENT`  | messaging  | no    | No such attachment, or not finalized (REQ-141). |
| `3012` | `STORAGE_FULL`        | messaging  | no    | Upload refused: below the DB reserve (REQ-216). |
| `3013` | `ATTACHMENT_GONE`     | messaging  | no    | Reclaimed by age or storage pressure (REQ-215/217), or not in the object store. |
| `3014` | `INVALID_DEVICE_TOKEN`| messaging  | no    | `REGISTER_DEVICE_TOKEN` had a token no device issues (empty, over 511 bytes, or a character outside letters, digits, `_`, `-`, `:`, `.`) or an unknown platform (REQ-132). |
| `3015` | `TRANSFER_PROTOCOL`   | messaging  | no    | Out-of-order/oversized chunk or bad transfer state. |
| `3016` | `UNKNOWN_WEBHOOK`     | webhook    | no    | No such (or disabled) incoming webhook token (REQ-170). |
| `3017` | `CHANNEL_EXISTS`      | channel    | no    | `CREATE_CHANNEL` name already taken, compared case-insensitively (REQ-040). |
| `3018` | `TOO_MANY_PINS`       | channel    | no    | The channel already holds 100 pins (REQ-230, ARCH-90). |
| `3019` | `CHANNEL_ARCHIVED`    | channel    | no    | The channel is archived and read-only (REQ-035). Returned by `SEND`, `SEND_REPLY`, `UPLOAD_BEGIN`, `CREATE_WEBHOOK`, `EDIT_POLL` and free-talk voice input, and by `EDIT`, `DELETE`, `REACT` and a vote in a closed encrypted DM. A webhook post into an archived channel is refused with HTTP `403`, not an `ERROR` frame. |
| `3020` | `INVALID_MESSAGE`     | messaging  | no    | Nothing to send — an empty body on a scheduled message (REQ-224). |
| `3021` | `MEDIA_INVALID`       | attachment | no    | `ATTACH_MEDIA_SET` refused: kind, bounds, type, already sent, or poster (§5.14a). |
| `3022` | `MEDIA_TOO_LARGE`     | attachment | no    | The video exceeds the workspace's video cap, `WORKSPACE_INFO`'s `max_video_size` (§5.14a). |
| `3023` | `NOT_RENDERABLE`      | read-aloud | no    | The message has nothing to say aloud (§5.14b, REQ-294). |
| `3024` | `TTS_UNAVAILABLE`     | read-aloud | no    | Read-aloud is off, its queue is full, the connection is past 60 renderings a minute, or the render failed (§5.14b). |
| `3025` | `CALL_UNAVAILABLE`    | calls      | no    | The audio relay exited and could not be restarted, so a `CALL_JOIN` is refused rather than answered with a media port nothing listens on (§5.17). |
| `3026` | `SEGMENT_TOO_LONG`    | voice input | no   | A segment's `sample_count` exceeds the cap `STT_INFO` announced (§5.14c, REQ-298). |
| `3027` | `STT_UNAVAILABLE`     | voice input | no   | Voice input is off, too many segments are waiting or more than 60 arrived in a minute, the queue is full, or recognition failed (§5.14c, REQ-300). |
| `3028` | `CALL_FULL`           | calls      | no    | A join, a start's invitations or a `CALL_INVITE` would put the call past the cap (§5.17, REQ-305). |
| `3029` | `NOT_CALL_STARTER`    | calls      | no    | `CALL_END` from someone other than the call's starter (§5.17). |
| `3030` | `NOT_IN_CALL`         | calls      | no    | No call in that conversation, or the sender is not in it (`CALL_INVITE`, `CALL_END`, `CALL_KEY`, `CALL_SHARE`, `CALL_CAMERA`, `CALL_VIDEO_SUB`). |
| `3031` | `INVITE_UNREDEEMABLE` | admin      | no    | `INVITE_USER` carried an email where no sign-in source could ever spend an invite bound to one: the deployment offers no provider sign-in, neither the relay nor a direct connection (§5.8). |
| `3032` | `GROUP_HANDLE_TAKEN`  | groups     | no    | A group, a person or a broadcast word already answers to that `@name`: a create, an update, or a rename (`SET_DISPLAY_NAME`) (§5.16k). |
| `3033` | `UNKNOWN_GROUP`       | groups     | no    | No such group. |
| `3034` | `MEMBER_VIA_GROUP`    | groups     | no    | `LEAVE_CHANNEL` or `REMOVE_FROM_CHANNEL` for someone in the channel only through a group (§5.16k). |
| `3035` | `INVALID_GROUP`       | groups     | no    | A handle, name or description out of bounds, or a group, a workspace or a channel at its cap (§5.16k). |
| `3036` | `SIGNUP_LIMITED`      | auth       | no    | Ten accounts were made from this address in the last hour; the sign-up page says so (AUTH.md §2). Not sent as a frame. |
| `3037` | `DEMO_LIMIT`          | demo       | no    | A public demo's daily limit was reached (AUTH.md §8.12); `message` says which, in a sentence for the person. Sent as `ERROR`, or as a transfer's error for an upload. |
| `3038` | `INVALID_POLL`        | messaging  | no    | A poll's question or options out of bounds, or a vote naming options the poll does not have, or two on a single-choice poll (REQ-225). |
| `3039` | `POLL_CLOSED`         | messaging  | no    | The poll is past its deadline and takes no more votes (REQ-225). |
| `3040` | `POLL_HAS_VOTES`      | messaging  | no    | An `EDIT_POLL` changing a poll's options or settings after someone voted; only its deadline can change now (REQ-225). |
| `3041` | `EDIT_WINDOW_CLOSED`  | messaging  | no    | An author's `EDIT`, or `DELETE` of their own message, past the window the workspace's configuration sets (`OPENCHIME_EDIT_WINDOW`, `OPENCHIME_DELETE_WINDOW`, which clients are told as `edit_window` and `delete_window`). An admin's or owner's delete of someone else's message is never limited (REQ-051, REQ-052). |
| `3042` | `WORKSPACE_SENDER`    | messaging  | no    | A message, reply or file sent into a DM with the workspace's own sender (`USER_LIST` `flags`), whose DMs nobody reads. |
| `3043` | `CANNOT_MOVE`         | messaging  | no    | A `MOVE_MESSAGE` that cannot be made: into or out of a DM or encrypted DM, a thread root with replies into a thread, into a thread that is itself a reply, or to where it already is (REQ-053). |
| `3044` | `TODO_NO_EDIT`        | messaging  | no    | An `EDIT` of a message that carries a to-do list; its title is renamed and its tasks added instead (§5.16o). |
| `3045` | `TODO_INVALID`        | messaging  | no    | A to-do list's title, or a task's name or description, blank or past its limit; or a task the list does not have (§5.16o). |
| `3046` | `TODO_DUPLICATE`      | messaging  | no    | A task named as one already in the list, or a `/todo` body naming one twice (§5.16o). |
| `3047` | `TODO_FULL`           | messaging  | no    | A `TODO_ADD` to a list that already holds 200 tasks (§5.16o). |
| `3070` | `DM_REFUSED`          | messaging  | no    | An `OPEN_DM` or `OPEN_GROUP_DM` naming someone who blocks the asker, or a new one naming someone whose DM privacy is `nobody`. It says neither, so a block is never announced. |
| `3071` | `INVALID_PROFILE_FIELD` | admin    | no    | A profile field without a label, of an unknown kind or id, past the 20 a workspace may have; or a value over 200 bytes, for a field it does not have, or for a link not starting `https://`. |
| `3072` | `INVALID_OUT_OF_OFFICE` | profile  | no    | An out-of-office whose end is not after its start, or whose message is over 280 bytes. |
| `3073` | `EMOJI_ALIAS_LIMIT`   | emoji      | no    | A `RENAME_EMOJI` past the 8 old names an emoji keeps, or the 256 a workspace does. |
| `3074` | `EXPORT_UNAVAILABLE`  | attachment | no    | An `EXPORT_GET` where export is not running (not built in, or `OPENCHIME_H264_EXPORT` off), or the person's three or the daemon's queue are full (§5.14d). |
| `3075` | `CALL_SUBJECT`        | call       | no    | A call started, or renamed, with a subject that is blank or longer than 200 bytes (§5.17). |
| `3076` | `RECORDING_UNAVAILABLE` | call     | no    | A `CALL_RECORD` start where recording is not offered: not built in, turned off by an admin, a public demo, or the recordings' space used up (§5.17). |
| `3077` | `PREVIEW_MISSING` | transfer | no    | A `PREVIEW_BEGIN` for a rung the attachment has no preview at: never made, or reclaimed. The client downloads the original instead (§5.14f). |
| `3078` | `PREVIEW_RUNG` | transfer | no    | A `PREVIEW_BEGIN` naming a rung that is neither 1 nor 2 (§5.14f). |
| `3079` | `UNKNOWN_SCOPE` | admin | no    | A token minted with a scope outside the catalog (§5.16p). |
| `3080` | `TOO_MANY_TOKENS` | admin | no    | A token minted for an agent that already holds 10 (§5.16p). |
| `3081` | `UNKNOWN_CODE` | account | no    | A `CONNECTION_CODE` or `CONNECTION_DECIDE` whose code no AI client is waiting on: unknown, past its ten minutes, or already acted on (§5.16q). |
| `3082` | `UNKNOWN_CONNECTION` | account | no    | A `REVOKE_CONNECTION` for a connection that is not in force, or not the caller's to revoke (§5.16q). |
| `3083` | `BAD_LIFETIME` | account | no    | A `CONNECTION_DECIDE` approving with a lifetime other than 0, 30, 90 or 365 days (§5.16q). |
| `3084` | `CONNECTIONS_OFF` | account | no    | Any §5.16q frame while `OPENCHIME_MCP_CONNECTIONS` is off. |
| `3085` | `WEBHOOK_URL` | admin | no    | An outgoing webhook's URL is not `https` (§5.15a). |
| `3086` | `WEBHOOK_EVENT` | admin | no    | An outgoing webhook named an event it may not select (§5.15a). |
| `3087` | `WEBHOOK_UNVERIFIED` | admin | no    | The endpoint did not answer the `url_verification` check (§5.15a). |
| `3088` | `RELEASE_NOTES_UNAVAILABLE` | client | no | The box is not connected to the OpenChime services, or central did not answer (§5.16r). |
| `9001` | `INTERNAL`            | any        | maybe | Server-side failure; `fatal` indicates whether the connection survives. |

Handshake-stage codes (`1001`/`1002`, and `1003`/`1005` for a bad or missing
`HELLO`) are delivered via `REJECT`, which carries the same `code`; the other codes are delivered via `ERROR`.
A code marked **on `AUTH`** is fatal as the answer to `AUTH` — another try is on
a new connection — and non-fatal as the answer to `AUTH_BEGIN` or the device-code
frames (§4.2a, §4.2c).

---

## 9. Message type registry

**Kept by hand in step with `shared/protocol.h`** — every `OC_MSG_*` the codec
defines, in opcode order, 239 of them. The sections above specify the payload layouts; this
table is the index and the authority on which values are taken.

**Every opcode carries exactly one message type.** `scripts/test/fast/opcodes.sh`
(run by `make test` and CI) fails on any duplicate, with no exception list, so
this table cannot silently gain a shared value.

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
| `0x0015` | `AUTH_BEGIN` | C → S | pre-auth: start a browser sign-in |
| `0x0016` | `AUTH_REDIRECT` | S → C | the authorize URL the daemon built |
| `0x0017` | `AUTH_CONTINUE` | S → C | a first step that is correct but not sufficient |
| `0x0018` | `AUTH_DEVICE_BEGIN` | C → S | pre-auth: ask for a device code |
| `0x0019` | `AUTH_DEVICE` | S → C | the user code, where to enter it, and the device code |
| `0x001A` | `AUTH_DEVICE_POLL` | C → S | pre-auth: has the code been approved? |
| `0x001B` | `AUTH_DEVICE_TOKEN` | S → C | approved: the token to present on `AUTH` |
| `0x001C` | `WORKSPACE_ADDRESS` | S → C | after `WORKSPACE_INFO`: the workspace's address now |
| `0x001D` | `RESET_CREDENTIAL` | C → S | owner/admin: a one-time link to set a local account's password |
| `0x001E` | `CREDENTIAL_RESET` | S → C | the reset link's token |
| `0x001F` | `ENABLE_USER` | C → S | owner/admin: bring a removed member back |
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
| `0x0032` | `THREAD_META` | S → C | a parent's reply count and repliers (backfill, live) |
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
| `0x003F` | `FILES` | S → C | terminator of a LIST_FILES response, saying whether more remain |
| `0x0040` | `LIST_USERS` | C → S | tenant user enumeration |
| `0x0041` | `USER_LIST` | S → C |  |
| `0x0042` | `SET_ROLE` | C → S | (ARCH-60, REQ-030) |
| `0x0043` | `INVITE_USER` | C → S | (REQ-033, tenant-level; owner/admin) |
| `0x0044` | `REMOVE_USER` | C → S | (REQ-033, tenant-level; owner/admin) |
| `0x0045` | `USER_UPDATED` | S → C | ack/push for SET_ROLE, REMOVE_USER and ENABLE_USER |
| `0x0046` | `INVITE_CREATED` | S → C | the minted invite token |
| `0x0047` | *(retired)* | — | was `REDEEM_INVITE`, a password in a frame; sign-up is a page (AUTH.md §8.10). Never reused. |
| `0x0048` | `SET_DISPLAY_NAME` | C → S | change your own display name (REQ-240) |
| `0x0049` | *(retired)* | — | was `CHANGE_PASSWORD`; the password page. Never reused. |
| `0x004A` | `PROFILE_UPDATED` | S → C | a user's display name changed (also the self ack) |
| `0x004B` | `LIST_INVITES` | C → S | outstanding invites (owner/admin) |
| `0x004C` | `INVITE_LIST` | S → C | the list, by server-side id (never tokens) |
| `0x004D` | `REVOKE_INVITE` | C → S | revoke one by id |
| `0x004E` | `INVITE_REVOKED` | S → C | ack |
| `0x004F` | `SET_EMAIL` | C → S | a local account's own address, unverified; answered with `PROFILE_INFO` |
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
| `0x0070` | `SET_PRESENCE` | C → S | set own presence (REQ-120) |
| `0x0071` | `PRESENCE_UPDATE` | S → C | a user's presence changed |
| `0x0072` | `PROFILE_INFO` | S → C | a user's full profile (also a push) |
| `0x0073` | `LIST_FILE_CHANNELS` | C → S |  |
| `0x0074` | `FILE_CHANNELS` | S → C | (channel_id, count) pairs |
| `0x0075` | `LIST_SESSIONS` | C → S |  |
| `0x0076` | `SESSION_LIST` | S → C | never the tokens |
| `0x0077` | `SET_NOTIFY_DEFAULT` | C → S | the global fallback level (REQ-134) |
| `0x0078` | `SET_AVATAR` | C → S | point my avatar at an uploaded attachment (REQ-240) |
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
| `0x008B` | `MARK_ALL_READ` | C → S | advance every membership's cursor (§5.4) |
| `0x008C` | `GET_CHANNEL_DESCRIPTION` | C → S | a channel's long-form description (§5.4) |
| `0x008D` | `CHANNEL_DESCRIPTION` | S → C | the answer, and the fan-out of a change |
| `0x008E` | `REVOKE_SESSION` | C → S | sign one of your own sessions out; answered with `SESSION_LIST` |
| `0x0090` | `SET_NOTIFY_PREF` | C → S | set a channel's notification level (REQ-130) |
| `0x0091` | *(retired)* | — | was `SET_DND`, a single quiet window; replaced by `SET_SCHEDULE` (`0x00CC`, §5.16). Never reused. |
| `0x0092` | `LIST_NOTIFY_PREFS` | C → S | request all notification settings |
| `0x0093` | `NOTIFY_PREFS` | S → C | the notify default + per-channel levels and mutes (also a sync push) |
| `0x0094` | `SET_CLIENT_SETTING` | C → S | upsert a synced client setting (empty value = delete) |
| `0x0095` | `LIST_CLIENT_SETTINGS` | C → S | request a client_type bucket's settings |
| `0x0096` | `CLIENT_SETTINGS` | S → C | a bucket snapshot (also a device-sync push) |
| `0x0097` | `STORAGE_STATUS_REQ` | C → S | owner/admin: ask for storage usage (REQ-214) |
| `0x0098` | `STORAGE_STATUS` | S → C | usage + policy + what maintenance reclaimed |
| `0x0099` | `AUDIT_QUERY` | C → S | owner/admin: page the audit log (REQ-251) |
| `0x009A` | `AUDIT_PAGE` | S → C | a page of entries, newest first |
| `0x00A0` | `CALL_JOIN` | C → S | join a call this user is asked to, with the device key and the codecs it decodes (REQ-150/301) |
| `0x00A1` | `CALL_LEAVE` | C → S | leave the call |
| `0x00A2` | `CALL_JOINED` | S → C | to the joiner: UDP endpoint/token, slot, epoch, starter, roster with device keys |
| `0x00A3` | `CALL_ROSTER` | S → C | to participants: roster and epoch changed |
| `0x00A4` | `CALL_INVITE` | C → S | a participant invites more people (REQ-302) |
| `0x00A5` | `CALL_DECLINE` | C → S | an invitee declines |
| `0x00A6` | `CALL_END` | C → S | the starter ends the call for everyone |
| `0x00A7` | `CALL_STATE` | S → C | a call as Calls home lists it, to its invitees, on every change and at sign-in (REQ-303) |
| `0x00A8` | `CALL_KEY` | C → S | a participant's media key, sealed to each other participant (ARCH-113) |
| `0x00A9` | `CALL_KEY_FOR` | S → C | one sealed media key, from its sender |
| `0x00AA` | `CALL_SHARE` | C → S | start or stop sharing a screen in the call; a start takes over (REQ-161) |
| `0x00AB` | `CALL_MEDIA` | both | call media over the connection when UDP cannot reach the relay (`calls-tcp`) |
| `0x00AC` | `CALL_CAMERA` | C → S | turn this participant's camera on or off in the call (REQ-160) |
| `0x00AD` | `CALL_VIDEO_SUB` | C → S | whose cameras the relay sends this participant (REQ-160) |
| `0x00AE` | `CALL_START` | C → S | start a call with a subject and invitees, from a conversation or none, and join it (CALLS.md §1) |
| `0x00AF` | `CALL_SUBJECT` | C → S | rename the call (CALLS.md §1) |
| `0x00B0` | `REGISTER_DEVICE_TOKEN` | C → S | register a mobile push token (REQ-132) |
| `0x00B4` | `CALL_HISTORY` | C → S | a page of this user's calls, newest first (CALLS.md §1) |
| `0x00B5` | `CALL_HISTORY_PAGE` | S → C | the page |
| `0x00B6` | `CALL_DETAIL` | C → S | one call's page, for those asked to it |
| `0x00B7` | `CALL_PAGE` | S → C | the call's page: people, times, timeline, recordings, transcript |
| `0x00B8` | `CALL_DELETE` | C → S | delete a call's transcript (an admin) or a recording (its starter, an admin) |
| `0x00B9` | `CALL_DELETED` | S → C | it is gone, to those asked to the call |
| `0x00BA` | `CALL_RECORD` | C → S | start or stop recording the call (CALLS.md §8) |
| `0x00B1` | `UNREGISTER_DEVICE_TOKEN` | C → S | drop a push token (logout / token change) |
| `0x00B2` | `DEVICE_TOKEN_ACK` | S → C | register/unregister acknowledged |
| `0x00B3` | `MENTION_UNRESOLVED` | S → C | to the sender only: which mentioned names reached nobody (REQ-287) |
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
| `0x00D6` | `SET_TZ_OFFSET` | C → S | minutes east of UTC, refreshed on every connect |
| `0x00D7` | `UNFURL` | S → C | a URL's fetched preview (REQ-222); also replayed on backfill |
| `0x00D8` | `SET_PROFILE` | C → S | my profile fields (REQ-240) |
| `0x00D9` | `FORWARD` | S → C | what a forwarded message points at (REQ-057); also replayed on backfill |
| `0x00DA` | `ATTACH_MEDIA_SET` | C → S | mark an upload as a video message, with its poster (REQ-162), or a snippet, with its title and language (REQ-226) |
| `0x00DB` | `ATTACH_MEDIA_OK` | S → C | accepted |
| `0x00DC` | `TTS_INFO` | S → C | read-aloud availability and voices (REQ-291/295) |
| `0x00DD` | `AUDIO_GET` | C → S | the speech of one message (REQ-291) |
| `0x00DE` | `AUDIO_INFO` | S → C | its duration and size |
| `0x00DF` | `AUDIO_CHUNK` | S → C | a slice of the rendering |
| `0x00E0` | `AUDIO_END` | S → C | the whole rendering is sent |
| `0x00E1` | `CAPABILITIES` | S → C | the features this daemon offers, by name |
| `0x00E2` | `VOICE_PREVIEW_GET` | C → S | hear a voice say the preview sentence (REQ-292) |
| `0x00E3` | `STT_INFO` | S → C | voice input's model, language and segment cap (REQ-300) |
| `0x00E4` | `STT_BEGIN` | C → S | a segment of speech: its mode, target and length (REQ-296) |
| `0x00E5` | `STT_CHUNK` | C → S | a slice of its samples |
| `0x00E6` | `STT_END` | C → S | the whole segment is sent |
| `0x00E7` | `STT_TEXT` | S → C | what was said, and the message it was posted as (REQ-297) |
| `0x00E8` | `LIST_GROUPS` | C → S | every user group (REQ-307) |
| `0x00E9` | `CREATE_GROUP` | C → S | owner/admin |
| `0x00EA` | `UPDATE_GROUP` | C → S | owner/admin |
| `0x00EB` | `DELETE_GROUP` | C → S | owner/admin |
| `0x00EC` | `GROUP_ADD_MEMBERS` | C → S | owner/admin |
| `0x00ED` | `GROUP_REMOVE_MEMBERS` | C → S | owner/admin |
| `0x00EE` | `GROUP_INFO` | S → C | one group, whole; pushed to all on a change |
| `0x00EF` | `GROUP_DELETED` | S → C | pushed to all |
| `0x00F0` | `GROUPS_END` | S → C | terminator of a LIST_GROUPS answer |
| `0x00F1` | `CHANNEL_ADD_GROUP` | C → S | give a channel a group (REQ-309) |
| `0x00F2` | `CHANNEL_REMOVE_GROUP` | C → S | take it away |
| `0x00F3` | `CHANNEL_GROUPS` | S → C | the groups a channel has |
| `0x00F4` | `CHANNEL_VIA_GROUP` | S → C | after a member list: who is in only through a group |
| `0x00F5` | `MENTION_UNRESOLVED_MORE` | S → C | how many a message could not reach in all (REQ-308) |
| `0x00F6` | `ALERTS_SUMMARY` | S → C | owner/admin: the daemon's critical failures, counted (REQ-263) |
| `0x00F7` | `ALERTS_LIST` | C → S | owner/admin: ask for them |
| `0x00F8` | `ALERTS` | S → C | the entries, newest first |
| `0x00F9` | `ALERT_ACK` | C → S | owner/admin: acknowledge one, or all |
| `0x00FA` | `RESTORE` | C → S | take back a delete you made, while it is held |
| `0x00FB` | `MSG_RESTORED` | S → C | a deleted message, whole again (`BROADCAST`'s layout) |
| `0x00FC` | `ACTION` | S → C | whether a message is an action (REQ-058); after every live message frame, and replayed |
| `0x00FD` | `SUMMARIZE` | C → S | summarize a channel or DM over a span (REQ-310) |
| `0x00FE` | `SUMMARY` | S → C | the answer to one `SUMMARIZE`: the summary as JSON, or why not |
| `0x00FF` | `ERROR` | S → C | §8.1 |
| `0x0100` | `SUMMARY_QUEUED` | S → C | where one `SUMMARIZE` is in the queue: how many are ahead of it |
| `0x0101` | `SUMMARY_DETACH` | C → S | stop watching a request: told as a notice when made |
| `0x0102` | `SUMMARY_CANCEL` | C → S | cancel a request |
| `0x0103` | `SUMMARY_READY` | S → C | a summary made while its asker was not watching (a notice) |
| `0x0104` | `SUMMARY_OPEN` | C → S | a notice's summary, as it was made |
| `0x0105` | `SUMMARY_DISMISS` | C → S | a notice seen, unopened |
| `0x0106` | `SERVER_BUILD` | S → C | after `WORKSPACE_INFO`: the daemon's build id |
| `0x0107` | `PDM_OPEN` | C → S | start an encrypted DM with a person, with handshake 1 (§5.19) |
| `0x0108` | `PDM_OPENED` | S → C | to the opener: the encrypted DM's id (its channel) |
| `0x0109` | `PDM_INVITE` | S → C | to the peer's devices: the invitation, with handshake 1 |
| `0x010A` | `PDM_HS` | C → S | handshake 2 (the peer's device) or 3 (the opener) |
| `0x010B` | `PDM_HS_FOR` | S → C | that handshake message, to the other device |
| `0x010C` | `PDM_SEND` | C → S | one ratchet message: header and ciphertext (the opener's first, empty one) |
| `0x010D` | `PDM_MSG` | S → C | that message, to the other device |
| `0x010E` | `PDM_END` | both | the encrypted DM ended, and why |
| `0x010F` | `POST_POLL` | C → S | make a poll: a message whose body is its question (REQ-225) |
| `0x0110` | `VOTE` | C → S | my whole selection on a poll, replacing what I had |
| `0x0111` | `POLL` | S → C | a poll's definition and tallies; after its message's `ACTION`, on every vote, and replayed |
| `0x0112` | `LIST_POLL_VOTERS` | C → S | who picked what on a poll |
| `0x0113` | `POLL_VOTERS` | S → C | the answer; refused for an anonymous poll |
| `0x0114` | `EDIT_POLL` | C → S | the author changes a poll: options and settings until the first vote, the deadline always |
| `0x0115` | `DEACTIVATE_SELF` | C → S | deactivate my own account, as a removal does (REQ-033) |
| `0x0116` | `GET_PROFILE` | C → S | a member's whole profile, 0 for mine; answered with `PROFILE_INFO`, the phone in one's own only |
| `0x0117` | `SET_COLLAPSED` | C → S | fold a message away for me, or open it again |
| `0x0118` | `COLLAPSED_UPDATED` | S → C | to each of my connections: a message folded or opened; replayed in backfill |
| `0x0119` | `WORKSPACE_SETTINGS` | S → C | read-only: the workspace's settings clients act on, after `WORKSPACE_INFO` |
| `0x011A` | `READ_RECEIPTS` | both | whether I share my read receipts: set or ask, and told |
| `0x011C` | `MOVE_MESSAGE` | C → S | move a message, or a thread whole, to another channel or into a thread (REQ-053) |
| `0x011D` | `MESSAGE_MOVED` | S → C | to the readers of both channels, once each: a message moved |
| `0x011E` | `FIND_AT_TIME` | C → S | the message at a moment in a conversation or one thread of it (jump to a date) |
| `0x011F` | `FOUND_AT_TIME` | S → C | its answer: the first at or after the moment, else the last before |
| `0x0120` | `USAGE_REQ` | C → S | owner/admin: the workspace's usage over the last days (REQ-043) |
| `0x0121` | `USAGE` | S → C | active members by day and week, messages by day, the busiest channels |
| `0x0122` | `TODO_ADD` | C → S | add a task to a to-do list (§5.16o) |
| `0x0123` | `TODO_MARK` | C → S | mark a task done or not done: the state wanted, not a toggle |
| `0x0124` | `TODO_RENAME` | C → S | the author renames a to-do list |
| `0x0125` | `TODO` | S → C | a to-do list's state, a range of its tasks a frame; live, replayed and with search results |
| `0x0130` | `SET_USER_MUTE` | C → S | mute or unmute one person, for me alone |
| `0x0131` | `MUTED_USERS` | S → C | to my connections: everyone I have muted, on change and with `LIST_NOTIFY_PREFS` |
| `0x0132` | `SET_BLOCK` | C → S | block or unblock one person, for me alone (§5.20) |
| `0x0133` | `BLOCKS` | S → C | to each of my connections: whom I block, and who may start a DM with me |
| `0x0134` | `LIST_BLOCKS` | C → S | owner or admin: every block in the workspace |
| `0x0135` | `BLOCK_LIST` | S → C | the answer: blocker, blocked and since, per block |
| `0x0136` | `SET_DM_PRIVACY` | C → S | who may start a new direct conversation with me: anyone or nobody |
| `0x0137` | `SET_PROFILE_FIELD` | C → S | owner or admin: add, rename or delete a workspace profile field |
| `0x0138` | `PROFILE_FIELDS` | S → C | the workspace's profile fields: after sign-in, and to everyone on a change |
| `0x0139` | `SET_PROFILE_FIELD_VALUE` | C → S | my value for one profile field; empty clears it; answered with `PROFILE_INFO` |
| `0x013A` | `SET_OUT_OF_OFFICE` | C → S | my out-of-office span and message; an end of 0 or past clears it (§5.21) |
| `0x013B` | `CHANNEL_ADD_MANAGER` | C → S | a manager, admin or owner names a member of the channel a manager (§5.7) |
| `0x013C` | `CHANNEL_REMOVE_MANAGER` | C → S | and takes the role from any manager, the creator included |
| `0x013D` | `RENAME_EMOJI` | C → S | a custom emoji's new name; the old one stays as an alias (§5.16g) |
| `0x013E` | `EXPORT_GET` | C → S | the playable copy of a video message (§5.14d) |
| `0x013F` | `EXPORT_INFO` | S → C | its duration and size (§5.14d) |
| `0x0140` | `EXPORT_CHUNK` | S → C | a slice of the copy (§5.14d) |
| `0x0141` | `EXPORT_END` | S → C | the whole copy is sent (§5.14d) |
| `0x0142` | `PREVIEW_BEGIN` | C → S | a rung of an image attachment's preview (§5.14f) |
| `0x0143` | `PREVIEW_INFO` | S → C | the preview's dimensions, type and size (§5.14f) |
| `0x0144` | `PREVIEW_CHUNK` | S → C | a slice of the preview (§5.14f) |
| `0x0145` | `PREVIEW_END` | S → C | the whole preview is sent (§5.14f) |
| `0x0146` | `CREATE_AGENT` | C → S | an agent, and its first token (§5.16p) |
| `0x0147` | `AGENT_TOKEN` | S → C | a token minted, shown once (§5.16p) |
| `0x0148` | `REMOVE_AGENT` | C → S | an agent, its tokens and memberships (§5.16p) |
| `0x0149` | `MINT_AGENT_TOKEN` | C → S | another token for an agent (§5.16p) |
| `0x014A` | `LIST_AGENT_TOKENS` | C → S | an agent's tokens (§5.16p) |
| `0x014B` | `AGENT_TOKEN_LIST` | S → C | the list, never a token (§5.16p) |
| `0x014C` | `REVOKE_AGENT_TOKEN` | C → S | revoke one token (§5.16p) |
| `0x0154` | `CREATE_OUTGOING_WEBHOOK` | C → S | an outgoing webhook (§5.15a) |
| `0x0155` | `OUTGOING_WEBHOOK_SECRET` | S → C | its signing secret, shown once (§5.15a) |
| `0x0156` | `LIST_OUTGOING_WEBHOOKS` | C → S | the workspace's outgoing webhooks (§5.15a) |
| `0x0157` | `OUTGOING_WEBHOOK_LIST` | S → C | the list, never a secret (§5.15a) |
| `0x0158` | `SET_OUTGOING_WEBHOOK` | C → S | enable or disable one (§5.15a) |
| `0x0159` | `DELETE_OUTGOING_WEBHOOK` | C → S | delete one (§5.15a) |
| `0x015A` | `RELEASE_NOTES_GET` | C → S | a build's release notes (§5.16r) |
| `0x015B` | `RELEASE_NOTES` | S → C | central's answer: status and body (§5.16r) |

## 10. Connection state machine

```
             HELLO
   (new) ─────────────▶ AWAIT_WELCOME
                             │ WELCOME + AUTH_CHALLENGE
                             ▼
                        AWAIT_AUTH ──── AUTH ───▶ (verify: oidc token or
                             │                     session token; a password
                             │                     in a frame is refused)
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
- In `AWAIT_AUTH` the client may also send `AUTH_BEGIN`, `AUTH_DEVICE_BEGIN` and
  `AUTH_DEVICE_POLL` (§4.2a, §4.2c); their refusals are non-fatal.
- A frame invalid for the current state yields `UNEXPECTED_MSG_TYPE` (fatal):
  as `REJECT` before the handshake, as `ERROR` afterwards. The pre-auth case is
  the deliberate exception — a messaging frame before `AUTH_OK` answers
  `AUTH_REQUIRED`, which says the same thing more precisely.
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
| REQ-130/136/278 | `SET_NOTIFY_PREF`/`SET_NOTIFY_DEFAULT`/`SET_MUTE`/`SET_SCHEDULE`/`SET_SNOOZE`/`LIST_NOTIFY_PREFS` → `NOTIFY_PREFS`; server-authoritative settings synced to all the user's devices (§5.16, ARCH-72/103). Opcode `0x0091` is retired and never reused. |

Related decisions in ARCHITECTURE.md: ARCH-41 (handshake &
version negotiation), ARCH-42 (primitive field encodings), ARCH-43 (message id
allocation), ARCH-44 (idempotency mapping), ARCH-45 (client-side dedup),
ARCH-46 (reconnect backfill shape), ARCH-47 (error frame & code table).
