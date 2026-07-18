# OpenChime — Implementation Status

A reconciliation of [REQUIREMENTS.md](./REQUIREMENTS.md) against what is actually
built and tested in the tree. The requirements doc is the *target-state
contract* (written in present-perfect, as if finished); this doc records the
*current reality*. Update it whenever a requirement's status changes.

**Legend**

| Mark | Meaning |
|------|---------|
| ✅ | Implemented in the daemon and covered by tests (unit + in-process integration, and usually the compose e2e). |
| 🟡 | Partial — the server side exists but something material is missing (often the client half, or an accounting detail). |
| 🔵 | Client-side work; the daemon has no part to play, or its part is done and the gap is the client. |
| ⛔ | Not started. |
| ➖ | Satisfied by design/topology, or a deliberate scope exclusion — no code to write. |
| ❔ | Plausibly met but unverified (no measurement/test). |

Test surface backing the ✅ items: one in-process test binary (`make test` —
codec, migrations, auth/jwt/roles/ratelimit, dbwriter handlers, and an
end-to-end `itest_netloop` that drives the real epoll server + writer thread
over TLS) plus a compose-based black-box e2e (`make integration`).

---

## Status by requirement

### 1. Tenancy, Identity, Access

| REQ | Status | Notes |
|-----|--------|-------|
| 010 workspace+email+DNS resolution | 🔵 ✅ | **Built** (`client/core/resolve.c`): a workspace normalizes (bare name → configured DNS suffix; a dotted domain passes through) then resolves via SRV (`_openchime._tcp.<domain>`) with an A-record fallback at 443 (an explicit `:port` on the workspace pins it, skipping SRV). The TUI takes `<workspace>` (resolved) or a raw `<host> <port>` (dev). The optional `.well-known` half is not consulted yet; the email is OIDC-only (unused here), as specified. |
| 011 distinct resolution-failure error | 🔵 ✅ | Resolution failure returns a distinct status (`OC_RESOLVE_NOT_FOUND` / `_BAD_WORKSPACE`) → the TUI prints "workspace not found — does not resolve in DNS" and exits, kept apart from "could not reach the server" (connect) and "auth failed" (login). |
| 020 two-mode auth, mode advertised | ✅ / 🔵 | Daemon: local + OIDC verify, `AUTH_CHALLENGE` advertises the mode. **Local mode now has a client login UI:** the TUI shows a modal Sign-in box (workspace/username/masked password/remember-me) when no cred + no stored token, resolving on submit and retrying on failure. OIDC *browser flow + PKCE* is still client-side (⛔). |
| 021 Entra/Google providers | ➖ | Lives in the central relay service (out of this repo); the daemon only verifies the re-issued token. |
| 022 Apple Sign-In / no Facebook | ➖ | Central service concern. |
| 023 verified credential per mode | ✅ | ES256 pinned (alg+iss+aud+exp), or PBKDF2 password. |
| 024 local accounts | ✅ | PBKDF2, invite-token creation + redeem, failed-auth rate-limit, and a first-run one-time owner setup token (or `OC_BOOTSTRAP_USERS`). |
| 025 OIDC central relay | ➖ / ✅ | Relay is out-of-repo; daemon trusts the re-issued token. |
| 030 roles + ≥1-owner invariant | ✅ | |
| 031 channel membership, public/private read-post | ✅ | Public auto-joins the poster; private is members-only. |
| 032 edit/delete own + admin moderation delete | ✅ | `deleted_by` distinguishes self vs moderator. |
| 033 tenant + channel invite/remove | ✅ | Tenant invite/remove owner/admin-gated; channel invite/remove any member. |
| 040 per-tenant isolation | ➖ | One process + one DB per tenant; no shared query surface. |
| 041 no shared runtime in message path | ➖ | Only shared dependency is the login-time OIDC relay (OIDC mode only). |

### 2. Messaging

| REQ | Status | Notes |
|-----|--------|-------|
| 050 message → channel/DM, author, server time | ✅ | Channel messaging + **direct messages** (`OPEN_DM`, `kind='dm'` channels; messaging/backfill/search via the membership path). |
| 055 self-DM (notes to self) | ✅ | `OPEN_DM` with self as target -> a single-participant DM; idempotent, members-only. |
| 051 edit + "edited" marker | ✅ | `edited_at_ms`; original time/position kept. Marker rendering is client-side. |
| 052 delete tombstone | ✅ | Body nulled; id/author/timestamps kept; reactions cleared. |
| 053 no retention cutoff | ✅ | No message pruning of any kind. |
| 054 ~64KB body cap | ✅ | Enforced in the codec. |
| 060 threads (reply count, recent repliers) | ✅ | Reply fan-out, counts live + on backfill; repliers via `LIST_THREAD`. |
| 061 thread notifications | ⛔ | Needs notification config (REQ-130). |
| 070 emoji reactions (toggle, no stack) | ✅ | Composite PK enforces one-per-emoji-per-user. |
| 071 aggregate counts + reactor identities | ✅ | |
| 080 full-text search (FTS5, member-scoped) | ✅ | External-content FTS5; edit re-indexes; tombstones excluded. |

### 3. Delivery and Reliability

| REQ | Status | Notes |
|-----|--------|-------|
| 090 at-least-once + ack msg type | ✅ | Fan-out to connected members; `CLIENT_ACK` advances a per-(user,channel) delivery cursor (migration 0007); backfill recovers misses. |
| 091 client dedup on high-water | 🔵 ✅ | Implemented in the app-core view-model (per-channel high-water mark). |
| 092 in-channel order = accept order | ✅ | Ascending, tenant-monotonic `message_id`. |
| 093 idempotent retry | ✅ | Persisted `(channel, token) → id`. |
| 094 RPO 15s (Litestream) | ✅ | Replication to object storage + restore-on-boot (compose). |
| 100 auto-reconnect w/o re-auth | ✅ | Daemon accepts `session`-token reconnect; **the client net thread auto-reconnects** — captures the AUTH_OK token, silently re-auths with `OC_AUTH_SESSION` (no password) under backoff on a drop, preserving the in-memory model. **Now also across process restarts:** a client SQLite store (`client/core/store.c`) persists the session token + TOFU pin per workspace, so a relaunch reconnects silently against the pinned cert. Headless-tested (daemon bounce; wrong-password client rides the stored token). |
| 101 backfill on reconnect | ✅ | Per-channel cursors → replayed messages + `BACKFILL_DONE` (+ `THREAD_META`). **The client drives it on reconnect**, backfilling each channel from its last-seen id (net-thread high-water); replays dedup on the model mark. **Now cursor-backed by the store:** cached messages seed the high-water at startup, so even a first backfill after relaunch resumes from the last cached id, not 0. |
| 102 offline outbox | 🔵 ✅ | **Built.** The client store's `outbox` table records each send (with its idem token) before delivery; the net thread resends the outbox on reconnect and clears a row on its `SEND_ACK`, so a message composed offline (or in flight at a drop, or queued at app-close) goes out on the next connection, deduped by the daemon. Headless-tested + PTY-smoked (compose with the daemon down; a later run flushes it). |
| 110 reject unsupported version pre-parse | ✅ | |
| 111 VERSION_TOO_OLD/NEW reason codes | ✅ | |

### 4–7. Presence, Notifications, Attachments, Media, Integrations

| REQ | Status | Notes |
|-----|--------|-------|
| 120 presence | ✅ | In-memory net-thread state; `SET_PRESENCE`/`PRESENCE_UPDATE` + auth-time online snapshot (ARCH-67). |
| 121 typing indicator | ✅ | Member-scoped `TYPING`/`TYPING_UPDATE` relay; ~6s client-side expiry resolves the window (ARCH-68). |
| 130 per-channel notification level | ✅ | `SET_NOTIFY_PREF`/`LIST_NOTIFY_PREFS` → `NOTIFY_PREFS`; server-authoritative level (all/mentions/none) in `notification_prefs` (migration 0012), synced to all the user's devices (ARCH-72). |
| 131 do-not-disturb schedule | ✅ | `SET_DND` stores a daily UTC minutes-of-day window on `users`; governs push, not in-app unread (ARCH-72). |
| 132 APNs/FCM push | ⛔ | Deferred mobile-push milestone: needs the push transport + per-message notify decision. Settings (REQ-130/131) that gate it are built. |
| 133 self-host push gateway | ⛔ | Tied to REQ-132. |
| 140 file attachments (object storage) | 🟡 | **Built + tested end-to-end:** proxied chunked upload/download over the wire (ARCH-69), `attachments` migration 0009, frames §5.14, and **message-linking** — a SEND references uploaded attachments (self-describing optional list), the BROADCAST carries their metadata inline, and backfill re-attaches them on reconnect. Thread replies carry attachments too (SEND_REPLY/THREAD_REPLY + LIST_THREAD). Two blob backends behind the ARCH-70 vtable: local-FS (default) and S3/MinIO (`OPENCHIME_BLOB_BACKEND=s3`, SigV4-signed, verified end-to-end against MinIO). **Surfaced in the TUI** (`/upload <path>`, `/download <id> [path]`): the net thread runs one transfer at a time as a state machine over the frame stream, respecting the upload window; the headless test round-trips a multi-chunk blob. |
| 141 attachment access control | ✅ | Proxied bytes → download authorized by the ordinary channel-read check on the attachment's channel; no signed URLs (ARCH-69). Verified over the wire (cross-user fetch allowed; non-member refused) and in dbwriter units. |
| 150–152 server-relayed audio | ✅ (server) | **Built + tested end-to-end** (ARCH-73): `CALL_JOIN`/`CALL_LEAVE` + per-channel ephemeral roster on the net thread; a forked UDP relay sidecar (`daemon/audio_sidecar.c`) that the daemon drives over a Unix socket (AUTHORIZE/REVOKE) and advertises via `udp_port`+token in `CALL_JOINED`. The sidecar relays opaque Opus payloads to a call's participants — no libopus server-side. Disconnect drops-and-re-rosters + REVOKEs, with a UDP silence sweep (REQ-152). An itest joins two clients and relays audio between them over real UDP. **Client-side** Opus encode/decode + UDP I/O is Phase-2 client work. |
| 160 video | ➖ | Deliberate scope exclusion. |
| 170 incoming webhooks | ✅ | ALPN-demuxed HTTP handler on the proto port; `POST /webhook/<token>` posts as the creator with the webhook's label as a display-name override (ARCH-71, migrations 0010–0011). Client mints tokens via `CREATE_WEBHOOK`; hashed storage; JSON/plain body; per-token rate limit; list/delete management (LIST_WEBHOOKS/DELETE_WEBHOOK). Tested end-to-end, and surfaced in the TUI (`/webhook` create/list/rm). |
| 171 CA-signed cert for webhooks | ⛔ | Endpoint currently served on the daemon's TOFU cert; on-demand CA/ACME issuance (ARCH-34) is the remaining infra follow-up. |

### 8. Security Posture

| REQ | Status | Notes |
|-----|--------|-------|
| 180 encrypted transport, no plaintext fallback | ✅ | TLS-only. |
| 181 daemon-controlled session + expiry | ✅ | `sessions` table, daemon-set TTL. |
| 182 session revocation / logout | ✅ | `LOGOUT` deletes rows + drops the connection. |
| 183 TOFU self-signed pinning | ✅ | Client-daemon TLS is TOFU-pinned, and **the client now persists the pin** (`workspace_state.tls_pin` in the client store): first connect trusts + records the cert SHA-256, every later connect/restart enforces it. The webhook CA-cert exception (REQ-171) is not yet built, so the webhook endpoint currently reuses the TOFU cert. |
| 190 per-connection send rate limit | ✅ | Per-connection fixed window (30 sends / 3s) on `SEND`/`SEND_REPLY`; excess → non-fatal `SEND_RATE_LIMITED`. |
| 191 failed-auth rate limit (account + source) | ✅ | Fixed-window, checked before PBKDF2. |

### 9–10. Platform, Infrastructure

| REQ | Status | Notes |
|-----|--------|-------|
| 200 Linux/Win/macOS/iOS/Android clients | 🟡 | Client pivoted to **one shared C app-core + native UI per platform** (ARCH-74, tdlib model — supersedes the raylib/Windows-cross-compile plan). The app-core (`client/core/`: net thread, queues, view-model + reducers, `oc_client` facade) is **built and headless-tested** (`tests/test_client_core.c` drives it against an in-process daemon; `make core` compile-check, linked into `make test`). First frontend is a **termbox2 + utf8proc TUI** (ARCH-75, `make tui`) with: connect + local auth, channel sidebar + unread, live messages + history backfill, display names, per-nick colors, scrollback, send, **reactions** (`/react`), **edit/delete**, **typing indicators**, **threads**, **search**, **channel + DM management**, **presence + roster** (`/who`, `/away`), **who-reacted** (`/reactions`), **notification prefs + DND** (`/prefs`, `/notify`, `/dnd`), **admin** (`/role`, `/invite`, `/remove`), **webhook management** (`/webhook` create/list/delete), **attachments** (`/upload`, `/download`), and **logout**. Every engine feature on the wire is now reachable from the TUI; native Win32/AppKit/Android/DOM frontends pending. Daemon is Linux-only (epoll/eventfd). |
| 210 lean/standard memory profile | ✅ | **Measured** (`Scripts/bench.sh`): ~5 MB baseline + **~50 KB RSS per idle connection**, so a few hundred connections sit in ~15–30 MB and low-thousands stay within the 256 MB lean profile. Message round-trip p50 ~4 ms, p99 ~20–40 ms under concurrency. |
| 211 low-hundreds concurrent connections | ✅ | **Measured**: hundreds of concurrent pinned-TLS connections held in a small fraction of the lean profile. Connection *setup* is bounded by the 600k-iteration PBKDF2 auth on the single writer (~6–7 logins/s), so a burst of simultaneous logins queues there; steady-state is cheap. `OC_NETLOOP_MAX_FD=4096`. |

---

## Server-robustness backlog

Correctness/hardening gaps **in already-shipped features** — the work to make
the server production-robust before adding more features. Priority is a rough
ordering, not a commitment.

**Resolved**

- ✅ **REQ-190 — SEND rate limit.** A per-connection fixed-window limiter (30
  sends / 3s) on `SEND`/`SEND_REPLY` in the net thread, answering excess with a
  non-fatal `SEND_RATE_LIMITED` before the send becomes a job. Tested over the
  wire.
- ✅ **Per-connection output-buffer cap.** `out_append` now bounds the pending
  output at 1 MiB; a client that stops reading while the daemon fans out is
  dropped rather than growing daemon memory without limit (delivery stays
  recoverable via reconnect + backfill). Tested over the wire.
- ✅ **`sent_messages` pruning (ARCH-44).** The writer runs a time-gated delete of
  idempotency rows older than 24h (at most once/hour), bounding the table.
  Tested (an aged token re-allocates; a recent one still dedups).
- ✅ **Reads decoupled from the writer thread (ARCH-66).** Backfill, search, and
  the `LIST_*` ops run on a third thread with its own `query_only` WAL
  connection, so a heavy query no longer stalls sends/auth. All existing read
  tests now exercise that path.
- ✅ **Server-side delivery accounting (REQ-090).** `CLIENT_ACK` now advances a
  per-(user,channel) cursor (migration 0007); a cursorless backfill resumes each
  member channel from that cursor. Tested.
- ✅ **Per-IP connection throttle.** The accept loop caps concurrent connections
  per source IP (`OPENCHIME_MAX_CONNS_PER_IP`, default 256) and closes excess at
  accept. Tested with a tiny-cap loop.
- ✅ **TLS identity persisted across restore (ARCH-66b).** The self-signed cert+key
  live in the replicated DB (migration 0008), restored on boot so the TOFU pin
  survives restore-on-boot. Tested (round-trip + end-to-end fingerprint check).
- ✅ **Truncation signals.** `BACKFILL_DONE`/`SEARCH_RESULTS`/`THREAD` carry a
  more/truncated flag so a client knows to page.
- ✅ **First-owner setup token (REQ-024).** First run in local mode with no owner
  mints a one-time owner invite and logs its token (redeemed to create the
  owner); reuses the invite path. Tested.
- ✅ **Codec fuzzer + concurrency load test.** A deterministic fuzzer runs 180k
  iterations of random/framed bytes through `oc_parse_frame` + every decoder
  (clean under ASan/UBSan); an 8-client concurrent-send load test exercises the
  accept path, writer, and fan-out under contention.

**Open**

None — the robustness backlog is clear. REQ-210/211 are now benchmarked
(`Scripts/bench.sh` + `tests/bench_load.c`): ~50 KB RSS per idle connection, so
low-hundreds of connections fit in tens of MB of the 256 MB lean profile, at
p50 ~4 ms message round-trip. The one measured bottleneck is connection *setup*
throughput (600k-iteration PBKDF2 auth on the single writer, ~6–7 logins/s) — a
correctness-preserving cost, not a memory limit. There is no periodic
large-scale soak yet.

---

## Summary

The **daemon is a feature-complete v1 chat core**: two-mode auth with
daemon-owned sessions and revocation, roles + full tenant administration,
public/private channels, messaging with edit/delete, reactions, threads, and
full-text search — all reachable over the wire and tested end-to-end.

The **client** is a shared, frontend-agnostic **C app-core** (ARCH-74) — network
thread, view-model, and reducers, driven headlessly against the in-process daemon
in CI — with a **termbox2 + utf8proc TUI** (ARCH-75) as the first frontend. The
TUI does live messaging with history backfill, display names, unread, reactions,
edit/delete, typing, threads, search, channel/DM management, presence + roster,
who-reacted, notification prefs + DND, admin (roles/invite/remove), webhook
management, attachments (`/upload`/`/download`), and logout — **every engine
feature on the wire is now reachable from the TUI**. The app-core also has a
**local SQLite store** (`client/core/store.c`) that makes it durable across
restarts: silent session-token reconnect (auto after a drop, and after a
relaunch), a persisted TOFU pin, cached history that shows instantly + seeds the
backfill cursor, and an **offline outbox** that resends messages composed while
disconnected — i.e. the reconnect/offline requirements (REQ-100/101/102) are now
met client-side. The remaining client work is the later native GUIs and the
fuller auth UX (OIDC, DNS resolution). The remaining server-side gap is **mobile
push transport** (REQ-132/133). **Server-relayed audio** (REQ-150–152) is now
built end-to-end — call signaling + a forked UDP relay sidecar — with only the
client-side Opus/UDP left (Phase-2 client work). Presence/typing (REQ-120/121) is
built and tested end-to-end, as are **notification settings** (REQ-130/131:
per-channel level + DND, server-authoritative and device-synced; the push
delivery they gate is the deferred piece). **Attachments** (REQ-140/141) are
built end-to-end — proxied chunked transfer, access control, message-linking, and
both blob backends — and now **surfaced in the TUI** (`/upload`/`/download`), so
there is no attachment work left on either side.

The **server-robustness backlog is cleared** (see Resolved above): SEND-flood
rate limiting, the per-connection output-buffer cap, idempotency-map pruning,
reads decoupled onto a read-only connection, server-side delivery accounting, a
per-IP connection throttle, TLS-identity persistence across restore, truncation
signals, the first-owner setup token, and a codec fuzzer + concurrency load
test. What remains is **not** hardening but **scope**: a real client and the
unbuilt server surface (mobile push transport; audio's server side is now built).
The capacity profile
(REQ-210/211) is now benchmarked — see [BENCHMARK.md](./BENCHMARK.md)
(`Scripts/bench.sh`). Incoming webhooks
(REQ-170) are built end-to-end
(ARCH-71), pending only the CA-signed cert (REQ-171). Attachments (REQ-140/141)
are built end-to-end — proxied chunked transfer, access control, and message-linking
— including thread-reply attachments and both the local-FS and S3/MinIO blob backends.
