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
| 012 remembered workspaces | 🔵 ✅ | **Built** (`client/core/store.c`, migration 5): a `workspace_book` table holds every workspace signed into on this device — typed address, account, last-used stamp — ordered most-recently-used. `oc_store_workspace_forget` also erases that workspace's session token (SQLite column *or* keyring), TOFU pin, cached history, and outbox, so "forget" leaves nothing behind. Headless-tested (ordering, label preservation on re-login, full erase). |
| 013 workspace switcher | 🔵 ✅ | **Built** (TUI): `^W` / `/workspaces` opens a switcher listing remembered workspaces with connection dot, account, and unread count, plus an always-present "+ Log in to new workspace" row (`d` forgets a closed one). Selecting a closed workspace reconnects on its stored token, else opens the login box pre-filled from the book. PTY-smoke verified across two live daemons. |
| 014 several workspaces at once | 🔵 ✅ | **Built** (TUI): one `oc_client` per workspace (`MAX_WS`), all ticked every frame, only the active one rendered — a background workspace keeps receiving and counting unread, surfaced as an "N elsewhere" header badge and a per-row unread count in the switcher. PTY-smoke verified: sitting on workspace B, a message posted into A by another user appears as A's unread. Each workspace keeps its own connection, credentials, model, and cache. |
| 015 per-workspace view state | 🔵 ✅ | **Built** (TUI): focused channel, scroll offset, and a half-typed composer live on the session and are saved/restored across a switch. |
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
| 040 per-tenant isolation | ➖ | One process + one DB per tenant; no shared query surface. Identical in all three deployment models (ARCH-76). |
| 041 no shared runtime in message path | ➖ | Holds in all three deployment models (ARCH-76): no OpenChime-operated service is ever in the message/data path. Federated deployments additionally depend on the project for OIDC, push, directory, SCIM, DNS name, and packages — all identity/notification/discovery/provisioning metadata, never message content. Stand-alone depends on none of them. |

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
| 094 ack after local commit; RPO is a deployment property | ✅ / ➖ | The daemon acks only after the local WAL commit (✅). Off-box replication and any stated RPO are a deployment concern (ARCH-3): hosted is implemented in the `openchime-saas` repo, self-hosted is the operator's own backup. Nothing to build here. |
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
| 133 push is a federated function | ⛔ | Tied to REQ-132. Push requires the project's gateway (the mobile clients are signed under the project's developer accounts), so it is available in the self-hosted federated and hosted models and **absent in self-hosted stand-alone** (ARCH-76/ARCH-16). |
| 140 file attachments (object storage) | 🟡 | **Built + tested end-to-end:** proxied chunked upload/download over the wire (ARCH-69), `attachments` migration 0009, frames §5.14, and **message-linking** — a SEND references uploaded attachments (self-describing optional list), the BROADCAST carries their metadata inline, and backfill re-attaches them on reconnect. Thread replies carry attachments too (SEND_REPLY/THREAD_REPLY + LIST_THREAD). Two blob backends behind the ARCH-70 vtable: local-FS (default) and S3-compatible, now selected by whether `OPENCHIME_S3_*` credentials are configured rather than an explicit flag. The S3 backend speaks **HTTPS with CA + hostname verification** and is **verified against a real provider (Tigris)** — multi-chunk streaming round-trips byte-for-byte, sizes and delete semantics correct, and bad certificates (expired / self-signed / untrusted-root / wrong-host) rejected. Previously it was plain-HTTP only, so it could not reach any public provider. Blob I/O now runs on the **ARCH-69 transfer worker pool** (`daemon/xferpool.c`), off the net thread, so a slow S3 endpoint no longer stalls the event loop. One job per transfer in flight gives chunk ordering and backpressure (the connection stops being read while a job is out); the handle travels with the job so a client disconnecting mid-transfer cannot leak or double-free it. Covered by test_xferpool (ordering, concurrency, cleanup; clean under TSan and ASan) and an itest that abandons uploads mid-stream. **Surfaced in the TUI** (`/upload <path>`, `/download <id> [path]`): the net thread runs one transfer at a time as a state machine over the frame stream, respecting the upload window; the headless test round-trips a multi-chunk blob. |
| 141 attachment access control | ✅ | Proxied bytes → download authorized by the ordinary channel-read check on the attachment's channel; no signed URLs (ARCH-69). Verified over the wire (cross-user fetch allowed; non-member refused) and in dbwriter units. |
| 150–152 server-relayed audio | ✅ (server) / ⛔ (client) | **Server built + tested end-to-end** (ARCH-73): `CALL_JOIN`/`CALL_LEAVE` + per-channel ephemeral roster, per-join tokens, forked UDP relay sidecar, disconnect/rejoin (REQ-152). **The client half does not exist** — no `CALL_*` in `client/core`, no Opus, no UDP media path, no audio device layer, no echo cancellation. Designed in [AUDIO.md](./AUDIO.md): huddle model (1:1 is the degenerate case), client-side mixing of N streams (forced by the server never decoding), duplex audio engine at 16 kHz/20 ms, and AEC behind a processor vtable with an ERLE test harness. |
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
| 212 messaging survives storage exhaustion | ✅ | **Built** (`daemon/storage.c`): a configured reserve (`OPENCHIME_DB_RESERVE_MB`, default 256) belongs to SQLite and is never spent on attachments; past it uploads are refused (216) while messaging is untouched. |
| 213 reclaim orphaned/aborted blobs | ✅ | **Built** — this closes the leak where `oc_blob_delete` had no caller and blob storage grew monotonically. The maintenance pass sweeps attachments never linked to a message (past a grace window) and hands their keys to the transfer pool. Verified on a live daemon: seeded orphan row + blob, pass fired on its timer, row tombstoned and the bytes gone from disk. |
| 214 surface storage usage to admins | ✅ | **Built**: `STORAGE_STATUS_REQ`/`STORAGE_STATUS` (0x0097/0x0098) carry usage, the active policy, and cumulative reclamation counts; `/storage` in the TUI renders them, flagging pressure and any evictions in red. Owner/admin only, checked in the writer against the user's **current** role so a demotion takes effect mid-session. PTY-smoke verified: an owner sees real numbers, a member is refused. |
| 215 automatic oldest-first eviction | ✅ | **Built**: oldest-first under pressure, default-on, `OPENCHIME_EVICT=off` disables, grace window via `OPENCHIME_EVICT_GRACE_HOURS` (default 24), tombstone keeps the row so the message stays readable. **Auditable** — migration 0015 records *why* each blob went (orphan / expired / evicted), so the trail is a query against a table we already keep rather than a second growing log; the counts surface in `/storage` (REQ-214). **Rendered** — a download of a reclaimed attachment returns `OC_ERR_ATTACHMENT_GONE` and the client says "no longer available (reclaimed by the server's storage policy)" instead of a generic transfer error. |
| 216 refuse uploads below the floor | ✅ | **Built**: `UPLOAD_BEGIN` is rejected with `OC_ERR_STORAGE_FULL` when free space is under the reserve — at declaration, before a byte moves, off the cached sample so it costs no syscall. |
| 217 max attachment age | ✅ | **Built**: `OPENCHIME_ATTACH_MAX_AGE_DAYS` (default 0 = keep forever). Expiry runs every pass regardless of pressure, tombstoning as 215 does. Headless-tested that a 90-day-old attachment expires under a 30-day policy while a 2-day-old one does not. |
| 218 periodic maintenance pass | ✅ | **Built** (`maybe_run_maintenance`): runs off the net loop's 500 ms tick, gated to `OPENCHIME_MAINT_INTERVAL_MS` (default 5 min), so a quiet box is maintained too — deliberately unlike `maybe_prune_idem`, which only fires when writes happen. Work splits by thread: the writer selects and tombstones rows, the transfer pool deletes bytes (it can block on S3). Bounded by `OPENCHIME_MAINT_BATCH` (default 64) per pass. |

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
  survives a restore onto a new box. Tested (round-trip + end-to-end fingerprint check).
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

### 11–14. Rich Text, Retrieval, Profiles, Compliance

*Mostly forward scope; tracked here so the omission is visible rather than implied.*

| REQ | Status | Notes |
|-----|--------|-------|
| 220–231 rich text, threads-in-place, pins, saved items | ⛔ | Forward scope; none backed by an ARCH decision yet. |
| 240–242 profiles, timezone, status text | ⛔ | Forward scope. |
| 250 opt-in retention policy | ⛔ | Distinct from REQ-217's max attachment age (built) — 250 also ages out *messages*, which nothing does today. Still `[needs ARCH decision]`. |
| 251 audit log | ✅ | **Built** (ARCH-79, migration 0016): four families — admin, account, security, moderation — recorded on the writer, read via `AUDIT_QUERY`/`AUDIT_PAGE` (0x0099/0x009A) and the TUI's `/audit`, owner/admin gated against the user's *current* role. Never records the secret involved. Verified live: role change, invite, and password change all appear with the acting user, and a member promoted mid-session immediately gains access. |
| 251a bounded audit log | ✅ | Aged out by the ARCH-78 maintenance pass, `OPENCHIME_AUDIT_MAX_DAYS` (default 365). |
| 251b per-family cap | ✅ | **The cap is applied per family**, so a flood of attacker-controlled `auth.failed` entries cannot age out administrative history — without this the audit log becomes an evidence-shredder. Rate-limited attempts are dropped silently rather than audited, so a throttled spray produces no rows at all. Regression-tested with a 200-entry security flood against a surviving admin entry. |
| 252 legal hold + export | ⛔ | Forward scope; `[needs ARCH decision]`. |
| 253 SCIM provisioning | ⛔ | Federated function (ARCH-76); central-service concern. |
