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
| 024 local accounts | 🟡 | PBKDF2 ✅, invite-token creation + redeem ✅, failed-auth rate-limit ✅. **First-owner one-time setup token ⛔** — bootstrap is via `OC_BOOTSTRAP_USERS` env instead. |
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
| 050 message → channel/DM, author, server time | ✅ / ⛔ | Channel messaging ✅. **Direct messages ⛔** — schema has `kind='dm'` but no create/open op. |
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
| 090 at-least-once + ack msg type | 🟡 | Fan-out to connected members ✅; missed messages recovered via backfill. **`CLIENT_ACK` is decoded then ignored — no server-side delivery cursor.** |
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
| 120 presence | ⛔ | No presence frames/state. |
| 121 typing indicator | ⛔ | `[needs ARCH decision — expiry window]`. |
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
| 190 per-connection send rate limit | ⛔ | **Reason code exists; no enforcement.** See robustness backlog. |
| 191 failed-auth rate limit (account + source) | ✅ | Fixed-window, checked before PBKDF2. |

### 9–10. Platform, Infrastructure

| REQ | Status | Notes |
|-----|--------|-------|
| 200 Linux/Win/macOS/iOS/Android clients | 🟡 | Windows `.exe` cross-compiles (skeleton UI); other platforms ⛔. Daemon is Linux-only (epoll/eventfd). |
| 210 lean/standard memory profile | ❔ | Plausible; not measured. |
| 211 low-hundreds concurrent connections | ❔ | epoll loop, `OC_NETLOOP_MAX_FD=4096`; not load-tested. |

---

## Server-robustness backlog

Correctness/hardening gaps **in already-shipped features** — the work to make
the server production-robust before adding more features. Priority is a rough
ordering, not a commitment.

1. **REQ-190 — no SEND rate limit (High).** A single authenticated client can
   flood `SEND` unbounded, amplified by broadcast fan-out to every channel
   member. Add a per-connection fixed-window limiter (reuse `ratelimit.c`),
   answering excess with the existing `SEND_RATE_LIMITED` code.
2. **Unbounded per-connection output buffer (High).** `out_append` grows
   `out_cap` to fit; a client that stops reading while the server fans out
   broadcasts grows the buffer without bound → single-client memory-exhaustion
   DoS. Cap the pending-output size and drop/disconnect a stuck consumer.
3. **`sent_messages` never pruned (Medium).** ARCH-44 specifies ~24h pruning;
   unbounded growth otherwise. Add a periodic delete job on the writer thread.
4. **Single writer thread blocks on reads (Medium).** Search, backfill, and list
   ops run on the one DB-writer thread alongside all writes; a heavy query
   stalls every send/auth for that tenant. Consider a separate read-only
   connection (WAL readers don't block the writer) for query jobs.
5. **`CLIENT_ACK` ignored — no delivery accounting (Medium).** Backfill relies
   entirely on client-supplied cursors; the server keeps no per-client delivery
   cursor, so there's no server-side delivery state to reason about or prune.
6. **No connection/accept throttle (Medium).** Fixed `OC_NETLOOP_MAX_FD` with no
   per-IP connection cap → connection-exhaustion DoS.
7. **Litestream restore ↔ TOFU cert (Medium).** Restore-on-boot can change the
   self-signed cert, tripping every client's pin (open item in TLS.md). Decide:
   persist the cert in the replica, or a client re-pin flow.
8. **Silent truncation signals (Low).** Thread/search/backfill cap at fixed
   limits with no "more available" flag; a client can't tell it was truncated.
9. **No load/stress/fuzz coverage (Low–Medium).** REQ-210/211 unverified; the
   codec has no fuzzer. A load test + a frame fuzzer would validate the
   memory/concurrency claims and parser robustness.
10. **First-owner setup token (Low).** REQ-024 specifies a one-time setup token;
    today the first owner comes from `OC_BOOTSTRAP_USERS`. Functional, but not
    the spec'd air-gapped bootstrap.

---

## Summary

The **daemon is a feature-complete v1 chat core**: two-mode auth with
daemon-owned sessions and revocation, roles + full tenant administration,
public/private channels, messaging with edit/delete, reactions, threads, and
full-text search — all reachable over the wire and tested end-to-end.

The **largest missing surfaces** are (a) a real **client** beyond the Phase-1
skeleton (the only consumer of all the above), and (b) whole **feature families
not yet begun**: presence/typing, notifications/push, attachments, audio, and
webhooks.

Before building more, the **robustness backlog** above hardens the existing
server — most importantly send rate-limiting (REQ-190) and the unbounded
per-connection output buffer, which are the two clearest denial-of-service
vectors in the current build.
