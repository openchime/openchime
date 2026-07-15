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
| 010 instance+email+DNS resolution | 🔵 ⛔ | Client-side; skeleton client connects to a raw host:port, no SRV/`.well-known`. |
| 011 distinct resolution-failure error | 🔵 ⛔ | Client-side. |
| 020 two-mode auth, mode advertised | ✅ / 🔵 | Daemon: local + OIDC verify, `AUTH_CHALLENGE` advertises the mode. OIDC *browser flow + PKCE* is client-side (⛔). |
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
| 091 client dedup on high-water | 🔵 ✅ | Implemented in the skeleton client. |
| 092 in-channel order = accept order | ✅ | Ascending, tenant-monotonic `message_id`. |
| 093 idempotent retry | ✅ | Persisted `(channel, token) → id`. |
| 094 RPO 15s (Litestream) | ✅ | Replication to object storage + restore-on-boot (compose). |
| 100 auto-reconnect w/o re-auth | ✅ / 🔵 | Daemon accepts `session`-token reconnect; robust client auto-reconnect is partial. |
| 101 backfill on reconnect | ✅ | Per-channel cursors → replayed messages + `BACKFILL_DONE` (+ `THREAD_META`). |
| 102 offline outbox | 🔵 ⛔ | Client SQLite store not built. |
| 110 reject unsupported version pre-parse | ✅ | |
| 111 VERSION_TOO_OLD/NEW reason codes | ✅ | |

### 4–7. Presence, Notifications, Attachments, Media, Integrations

| REQ | Status | Notes |
|-----|--------|-------|
| 120 presence | ✅ | In-memory net-thread state; `SET_PRESENCE`/`PRESENCE_UPDATE` + auth-time online snapshot (ARCH-67). |
| 121 typing indicator | ✅ | Member-scoped `TYPING`/`TYPING_UPDATE` relay; ~6s client-side expiry resolves the window (ARCH-68). |
| 130 per-channel notification level | ⛔ | |
| 131 do-not-disturb schedule | ⛔ | |
| 132 APNs/FCM push | ⛔ | |
| 133 self-host push gateway | ⛔ | |
| 140 file attachments (object storage) | ⛔ | |
| 141 attachment access control | ⛔ | `[needs ARCH decision]`. |
| 150–152 server-relayed audio | ⛔ | |
| 160 video | ➖ | Deliberate scope exclusion. |
| 170 incoming webhooks | ⛔ | Only `/healthz` HTTP exists; ALPN demux to an HTTP handler is designed, not built. |
| 171 CA-signed cert for webhooks | ⛔ | Tied to webhooks. |

### 8. Security Posture

| REQ | Status | Notes |
|-----|--------|-------|
| 180 encrypted transport, no plaintext fallback | ✅ | TLS-only. |
| 181 daemon-controlled session + expiry | ✅ | `sessions` table, daemon-set TTL. |
| 182 session revocation / logout | ✅ | `LOGOUT` deletes rows + drops the connection. |
| 183 TOFU self-signed pinning | ✅ | Webhook CA-cert exception (REQ-171) not built (webhooks not built). |
| 190 per-connection send rate limit | ✅ | Per-connection fixed window (30 sends / 3s) on `SEND`/`SEND_REPLY`; excess → non-fatal `SEND_RATE_LIMITED`. |
| 191 failed-auth rate limit (account + source) | ✅ | Fixed-window, checked before PBKDF2. |

### 9–10. Platform, Infrastructure

| REQ | Status | Notes |
|-----|--------|-------|
| 200 Linux/Win/macOS/iOS/Android clients | 🟡 | Windows `.exe` cross-compiles (skeleton UI); other platforms ⛔. Daemon is Linux-only (epoll/eventfd). |
| 210 lean/standard memory profile | ❔ | Plausible; not measured. |
| 211 low-hundreds concurrent connections | ❔ | epoll loop, `OC_NETLOOP_MAX_FD=4096`; concurrency is correctness-tested (8-client load test) but the low-hundreds ceiling is not benchmarked. |

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

None — the robustness backlog is clear. Remaining unknowns are quantitative, not
correctness: REQ-210/211's exact memory/connection-count profile is still
un-benchmarked (the load test proves concurrency correctness at small N, not the
low-hundreds-connection ceiling), and there is no periodic large-scale soak.

---

## Summary

The **daemon is a feature-complete v1 chat core**: two-mode auth with
daemon-owned sessions and revocation, roles + full tenant administration,
public/private channels, messaging with edit/delete, reactions, threads, and
full-text search — all reachable over the wire and tested end-to-end.

The **largest missing surfaces** are (a) a real **client** beyond the Phase-1
skeleton (the only consumer of all the above), and (b) whole **feature families
not yet begun**: notifications/push, attachments, audio, and webhooks.
Presence/typing (REQ-120/121) is now built and tested end-to-end.

The **server-robustness backlog is cleared** (see Resolved above): SEND-flood
rate limiting, the per-connection output-buffer cap, idempotency-map pruning,
reads decoupled onto a read-only connection, server-side delivery accounting, a
per-IP connection throttle, TLS-identity persistence across restore, truncation
signals, the first-owner setup token, and a codec fuzzer + concurrency load
test. What remains is **not** hardening but **scope**: a real client, the
unbuilt feature families (notifications/push, attachments, audio, webhooks), and
a quantitative capacity benchmark for REQ-210/211.
