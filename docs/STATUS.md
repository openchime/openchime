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

For the **Win32 GUI** there is a third surface: `scripts/gui_smoke.sh` drives the
running client through its test hook and asserts **116 behavioural invariants** —
what is in each column, which native children are shown, what covers the window,
modal commit-vs-cancel, the composer's editing primitives, group DMs, custom
emoji, avatars, sidebar sections and channel visibility. It is not in CI (the
daemon is Linux-only and GitHub's Windows runners cannot host it, WIN-81), so it
is a local pre-push gate. A ✅ on a client row generally means it passed there,
not that it was eyeballed.

> **The gate was flaky and is now deterministic (2026-07-31, WIN-87/88).** It
> used to fail ~60% of runs for reasons that were never product defects, and
> worse, it asserted *states* rather than transitions — so "Esc closes the
> palette" passed when the palette never opened, inflating the pass count on
> exactly the runs where something was broken. It now waits on the state it is
> about to assert, owns its own daemon on port 9500, and refuses to run if the
> workspace it reaches is not the fixture. **Six consecutive clean runs**, and it
> is up to **123 checks** with the group-DM picker covered.

**The numbered Win32 backlog (WIN-1 … WIN-84) is closed**, and an adversarial
review on 2026-07-30 found no defect in any of it. Work found since — including
two avatar-consistency defects found by looking at the running client rather
than by any assertion — is the open list in
[WIN32_BACKLOG.md](./WIN32_BACKLOG.md). The ⛔ rows below are what remains at the
requirement level.

---

## Status by requirement

### 1. Tenancy, Identity, Access

| REQ | Status | Notes |
|-----|--------|-------|
| 010 workspace+email+DNS resolution | 🔵 ✅ | **Built** (`client/core/resolve.c`): a workspace normalizes (bare name → configured DNS suffix; a dotted domain passes through) then resolves via SRV (`_openchime._tcp.<domain>`) with an A-record fallback at 443 (an explicit `:port` on the workspace pins it, skipping SRV). The TUI takes `<workspace>` (resolved) or a raw `<host> <port>` (dev). The optional `.well-known` half is not consulted yet; the email is OIDC-only (unused here), as specified. |
| 011 distinct resolution-failure error | 🔵 ✅ | Resolution failure returns a distinct status (`OC_RESOLVE_NOT_FOUND` / `_BAD_WORKSPACE`) → the TUI prints "workspace not found — does not resolve in DNS" and exits, kept apart from "could not reach the server" (connect) and "auth failed" (login). |
| 012 remembered workspaces | 🔵 ✅ | **Built** (`client/core/store.c`, migration 5): a `workspace_book` table holds every workspace signed into on this device — typed address, account, last-used stamp — ordered most-recently-used. `oc_store_workspace_forget` also erases that workspace's session token (SQLite column *or* keyring), TOFU pin, cached history, and outbox, so "forget" leaves nothing behind. Headless-tested (ordering, label preservation on re-login, full erase). |
| 013 workspace switcher | 🔵 ✅ | **Built** (TUI): **Ctrl+W** (or the action menu's 'Switch workspace') opens a switcher listing remembered workspaces with connection dot, account, and unread count, plus an always-present "+ Log in to new workspace" row (`d` forgets a closed one). Selecting a closed workspace reconnects on its stored token, else opens the login box pre-filled from the book. PTY-smoke verified across two live daemons. |
| 014 several workspaces at once | 🔵 ✅ | **Built** (TUI): one `oc_client` per workspace (`MAX_WS`), all ticked every frame, only the active one rendered — a background workspace keeps receiving and counting unread, surfaced as an "N elsewhere" header badge and a per-row unread count in the switcher. PTY-smoke verified: sitting on workspace B, a message posted into A by another user appears as A's unread. Each workspace keeps its own connection, credentials, model, and cache. |
| 015 per-workspace view state | 🔵 ✅ | **Built** (TUI): focused channel, scroll offset, and a half-typed composer live on the session and are saved/restored across a switch. |
| 020 two-mode auth, mode advertised | ✅ / 🔵 | Daemon: local + OIDC verify, `AUTH_CHALLENGE` advertises the mode. **Local mode now has a client login UI:** the TUI shows a modal Sign-in box (workspace/username/masked password/remember-me) when no cred + no stored token, resolving on submit and retrying on failure. OIDC *browser flow + PKCE* is still client-side (⛔). |
| 021 Entra/Google providers | ➖ | Lives in the central relay service (out of this repo); the daemon only verifies the re-issued token. |
| 022 Apple Sign-In / no Facebook | ➖ | Central service concern. |
| 023 verified credential per mode | ✅ | ES256 pinned (alg+iss+aud+exp), or PBKDF2 password. |
| 024 local accounts | ✅ | PBKDF2, invite-token creation + redeem, failed-auth rate-limit, and a first-run one-time owner setup token (or `OPENCHIME_BOOTSTRAP_USERS`). **Registered-user cap enforced** (`OPENCHIME_MAX_USERS`): new-user creation refused at the cap across redeem/register/bootstrap/OIDC-JIT with `ERROR USER_LIMIT`; active-user count (removed members free a seat). |
| 025 OIDC central relay | ➖ / ✅ | Relay is out-of-repo; daemon trusts the re-issued token. **Enrollment client built** (ARCH-84, `daemon/enroll.c`): the daemon generates its keypair + audience, emits an `oce1.` code, and calls out to activate; the enrolled audience feeds OIDC. Tested (`tests/test_enroll.c`). |
| 030 roles + ≥1-owner invariant | ✅ | |
| 031 channel membership, public/private read-post | ✅ | Public auto-joins the poster; private is members-only. `LIST_MEMBERS` (ARCH-91) now lets a client read a **channel's** roster — until it existed, frontends showed the tenant roster beside a channel name, which was wrong for any workspace with more than one channel. |
| 032 edit/delete own + admin moderation delete | ✅ | `deleted_by` distinguishes self vs moderator. |
| 033 tenant + channel invite/remove | ✅ | Tenant invite/remove owner/admin-gated; channel invite/remove any member. |
| 040 per-tenant isolation | ➖ | One process + one DB per tenant; no shared query surface. Identical in all three deployment models (ARCH-76). |
| 041 no shared runtime in message path | ➖ | Holds in all three deployment models (ARCH-76): no OpenChime-operated service is ever in the message/data path. Federated deployments additionally depend on the project for OIDC, push, directory, SCIM, DNS name, and packages — all identity/notification/discovery/provisioning metadata, never message content. Stand-alone depends on none of them. |

### 2. Messaging

| REQ | Status | Notes |
|-----|--------|-------|
| 050 message → channel/DM, author, server time | ✅ | Channel messaging + **direct messages** (`OPEN_DM`, `kind='dm'` channels; messaging/backfill/search via the membership path). |
| 055 self-DM (notes to self) | ✅ | `OPEN_DM` with self as target -> a single-participant DM; idempotent, members-only. **Idempotency is now structural** (migration 0019): a DM's participant set is a unique `dm_key`, so a duplicate conversation cannot be represented — previously any deletion of a membership row (as `remove_user` did) stranded the DM and the next open created a second one. `remove_user` now deletes a departing user's DM channels outright. |
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
| 130 per-channel notification level | ✅ | `SET_NOTIFY_PREF`/`LIST_NOTIFY_PREFS` → `NOTIFY_PREFS`; server-authoritative level (all/mentions/none) in `notification_prefs` (migration 0012), synced to all the user's devices (ARCH-72). Since REQ-221 the `mentions` level is *honoured* on the push path, not merely stored. |
| 131 recurring daily DND window | ✅ ⤳ | **Absorbed by REQ-136 (2026-07-31).** Built and working — `SET_DND` stores a daily UTC minutes-of-day window on `users`, wraps past midnight, and gates both push and the client's desktop toasts. But Slack has **one** recurring mechanism, not two: *Every day* with a single start/end **is** this case. The target state is REQ-136's per-weekday schedule, which replaces these two columns rather than sitting beside them. |
| 132 APNs/FCM push | ✅ (daemon emitter) | **Built (ARCH-85, `daemon/push.c`):** the daemon owns a `device_tokens` registry (migration 0018, `REGISTER_DEVICE_TOKEN` frame); a committed SEND fans a notify decision to an off-hot-path worker that selects recipients (members − author, level=ALL, not DND, with a token), signs the batch with the enrollment key (a freshness-windowed request signature the gateway verifies), and POSTs a **contentless** ping to the control-plane gateway, pruning stale tokens it returns. Tested (`tests/test_push.c`, incl. a fake-gateway round-trip). APNs/FCM transport + creds are the gateway's (control-plane repo). **MENTIONS level deferred** (needs REQ-221). |
| 133 push is a federated function | ✅ | Emitter is gated on `OPENCHIME_PUSH_URL` **and** an active enrollment (ARCH-85), so push is available in the self-hosted federated and hosted models and **absent in self-hosted stand-alone** (ARCH-76/ARCH-16). The gateway holds the project's Apple/Google credentials; the daemon never does. |
| 140 file attachments (object storage) | 🟡 | **Built + tested end-to-end:** proxied chunked upload/download over the wire (ARCH-69), `attachments` migration 0009, frames §5.14, and **message-linking** — a SEND references uploaded attachments (self-describing optional list), the BROADCAST carries their metadata inline, and backfill re-attaches them on reconnect. Thread replies carry attachments too (SEND_REPLY/THREAD_REPLY + LIST_THREAD). Two blob backends behind the ARCH-70 vtable: local-FS (default) and S3-compatible, now selected by whether `OPENCHIME_S3_*` credentials are configured rather than an explicit flag. The S3 backend speaks **HTTPS with CA + hostname verification** and is **verified against a real provider (Tigris)** — multi-chunk streaming round-trips byte-for-byte, sizes and delete semantics correct, and bad certificates (expired / self-signed / untrusted-root / wrong-host) rejected. Previously it was plain-HTTP only, so it could not reach any public provider. Blob I/O now runs on the **ARCH-69 transfer worker pool** (`daemon/xferpool.c`), off the net thread, so a slow S3 endpoint no longer stalls the event loop. One job per transfer in flight gives chunk ordering and backpressure (the connection stops being read while a job is out); the handle travels with the job so a client disconnecting mid-transfer cannot leak or double-free it. Covered by test_xferpool (ordering, concurrency, cleanup; clean under TSan and ASan) and an itest that abandons uploads mid-stream. **Surfaced in the TUI** (upload via the action launcher's 'Upload a file'; download via the message action menu): the net thread runs one transfer at a time as a state machine over the frame stream, respecting the upload window; the headless test round-trips a multi-chunk blob. |
| 141 attachment access control | ✅ | Proxied bytes → download authorized by the ordinary channel-read check on the attachment's channel; no signed URLs (ARCH-69). Verified over the wire (cross-user fetch allowed; non-member refused) and in dbwriter units. |
| 150–152 server-relayed audio | ✅ (server) / ⛔ (client) | **Server built + tested end-to-end** (ARCH-73): `CALL_JOIN`/`CALL_LEAVE` + per-channel ephemeral roster, per-join tokens, forked UDP relay sidecar, disconnect/rejoin (REQ-152). **The client half does not exist** — no `CALL_*` in `client/core`, no Opus, no UDP media path, no audio device layer, no echo cancellation. Designed in [AUDIO.md](./AUDIO.md): huddle model (1:1 is the degenerate case), client-side mixing of N streams (forced by the server never decoding), duplex audio engine at 16 kHz/20 ms, and AEC behind a processor vtable with an ERLE test harness. |
| 160 camera video | ➖ | Deliberate scope exclusion, now **narrowed** to camera video + general playback (ARCH-86). Screenshare is split out as REQ-161. |
| 161 screenshare | ⛔ | **Designed, not started** ([VIDEO.md](./VIDEO.md)). No code in any client or the daemon. Decisions taken: it rides the existing opaque relay with **no server-side codec** (ARCH-86); **VP9 via libvpx** is the single mandatory baseline because the relay cannot transcode and a call may mix platforms (ARCH-87); the **TUI is permanently exempt** (ARCH-75, renders no graphics). Unresolved before any build: fragmentation past the 1400-byte datagram cap, loss recovery, rate control, and a codec field on `CALL_JOINED`. **Sequenced behind the audio client** (REQ-150–152), which is itself at phase 0. |
| 170 incoming webhooks | ✅ | ALPN-demuxed HTTP handler on the proto port; `POST /webhook/<token>` posts as the creator with the webhook's label as a display-name override (ARCH-71, migrations 0010–0011). Client mints tokens via `CREATE_WEBHOOK`; hashed storage; JSON/plain body; per-token rate limit; list/delete management (LIST_WEBHOOKS/DELETE_WEBHOOK). Tested end-to-end, and surfaced in the TUI (channel menu → Webhooks / Create webhook). |
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
| 201 no local storage in any client | 🔵 ✅ | **Built** (ARCH-88). A client writes **no files at all**: the session token, TOFU pin and workspace book are one credential per workspace in the OS credential store (Credential Manager / libsecret, enumerated for the book), cached history is gone, and the outbox is in memory. Works because the read cursor is server-side (REQ-090) — a cursorless `BACKFILL_REQUEST`, or an explicit cursor of 0, resumes from it. `third_party/sqlite`, `-lsqlite3` and the borrowed `daemon/migrate.c` are gone from every client target; dropping SQLite took the Win32 binary from 2.9 MB to 1.9 MB at the time. (It has since grown with the features added after: `make windows-gui` produces **3.7 MB unstripped, 1.2 MB stripped**, measured 2026-07-30. The Makefile does not strip, so 3.7 MB is what ships today — quote a measurement, not the 1.9 MB historical figure.) Verified on Windows: sign-in, restart, silent reconnect and history all working with an empty `%LOCALAPPDATA%`. **Costs:** no offline history, a queued message dies with the process (WIN-59 warns on quit), and no keyring means no persistence at all. The rule covers client *state*; the TUI's user-authored `~/.config/openchime/config` is deliberately still a file. |
| 200 Linux/Win/macOS/iOS/Android clients | 🟡 | Client pivoted to **one shared C app-core + native UI per platform** (ARCH-74, tdlib model — supersedes the raylib/Windows-cross-compile plan). The app-core (`client/core/`: net thread, queues, view-model + reducers, `oc_client` facade) is **built and headless-tested** (`tests/test_client_core.c` drives it against an in-process daemon; `make core` compile-check, linked into `make test`). First frontend is a **TUI** — rebuilt menu/screen-driven on the in-tree `tuikit` toolbox (ARCH-83) with a 256-color theme and a Ctrl+K command palette; the slash-command UX (ARCH-75) is gone — with: connect + local auth, channel sidebar + unread, live messages + history backfill, display names, per-nick colors, scrollback, send, with reactions, edit/delete, typing indicators, threads, search, channel + DM management, presence + roster, who-reacted, notification prefs + DND, admin (roles/invite/remove), webhook management, attachments, and logout — all reached through context menus and the command palette (Ctrl+K). Nearly every capability the app-core exposes is reachable from the TUI — the exceptions (webhook delete, log-out-everywhere) and the daemon frames no client reaches yet are listed in [CLIENT.md](./CLIENT.md) §3; native GUIs pending. The **Windows GUI** rendering stack is settled — **Win32 + Direct2D/DirectWrite + RichEdit**, pure C (ARCH-82); two first-draft GUIs (comctl32, and a self-rendered Clay+raylib) were built, rejected as dated / non-native, and **have been removed** (the old self-rendered `client/gui` tree and the vendored Clay/raylib are deleted — the current native GUI lives at `client/gui/win32/` and is unrelated). Native AppKit/Android/DOM/WASM frontends still pending. Daemon is Linux-only (epoll/eventfd). |
| 210 lean/standard memory profile | ✅ | **Measured** (`Scripts/bench.sh`): ~5 MB baseline + **~50 KB RSS per idle connection**, so a few hundred connections sit in ~15–30 MB and low-thousands stay within the 256 MB lean profile. Message round-trip **p50 ~2–3 ms, p90 ~80 ms, p99 ~130 ms** at 32 concurrent senders. (An earlier *p99 ~20–40 ms* was a harness artifact and was corrected 2026-07-19 — see [BENCHMARK.md](./BENCHMARK.md).) |
| 211 low-hundreds concurrent connections | ✅ | **Measured**: hundreds of concurrent pinned-TLS connections held in a small fraction of the lean profile. Connection *setup* is bounded by the 600k-iteration PBKDF2 auth on the single writer at **~2 logins/s** (≈500 ms each), so a burst of simultaneous logins queues there; steady-state is cheap. `OC_NETLOOP_MAX_FD=4096`. See [BENCHMARK.md](./BENCHMARK.md) — an earlier ~6–7 logins/s figure was a harness artifact (a 10 s read timeout silently dropping most connections) and was corrected 2026-07-19. |
| 212 messaging survives storage exhaustion | ✅ | **Built** (`daemon/storage.c`): a configured reserve (`OPENCHIME_DB_RESERVE_MB`, default 256) belongs to SQLite and is never spent on attachments; past it uploads are refused (216) while messaging is untouched. |
| 213 reclaim orphaned/aborted blobs | ✅ | **Built** — this closes the leak where `oc_blob_delete` had no caller and blob storage grew monotonically. The maintenance pass sweeps attachments never linked to a message (past a grace window) and hands their keys to the transfer pool. Verified on a live daemon: seeded orphan row + blob, pass fired on its timer, row tombstoned and the bytes gone from disk. |
| 214 surface storage usage to admins | ✅ | **Built**: `STORAGE_STATUS_REQ`/`STORAGE_STATUS` (0x0097/0x0098) carry usage, the active policy, and cumulative reclamation counts; the TUI's Storage action (Ctrl+K → Storage usage) renders them, flagging pressure and any evictions in red. Owner/admin only, checked in the writer against the user's **current** role so a demotion takes effect mid-session. PTY-smoke verified: an owner sees real numbers, a member is refused. |
| 215 automatic oldest-first eviction | ✅ | **Built**: oldest-first under pressure, default-on, `OPENCHIME_EVICT=off` disables, grace window via `OPENCHIME_EVICT_GRACE_HOURS` (default 24), tombstone keeps the row so the message stays readable. **Auditable** — migration 0015 records *why* each blob went (orphan / expired / evicted), so the trail is a query against a table we already keep rather than a second growing log; the counts surface in the TUI's Storage action (REQ-214). **Rendered** — a download of a reclaimed attachment returns `OC_ERR_ATTACHMENT_GONE` and the client says "no longer available (reclaimed by the server's storage policy)" instead of a generic transfer error. |
| 216 refuse uploads below the floor | ✅ | **Built**: `UPLOAD_BEGIN` is rejected with `OC_ERR_STORAGE_FULL` when free space is under the reserve — at declaration, before a byte moves, off the cached sample so it costs no syscall. |
| 217 max attachment age | ✅ | **Built**: `OPENCHIME_ATTACH_MAX_AGE_DAYS` (default 0 = keep forever). Expiry runs every pass regardless of pressure, tombstoning as 215 does. Headless-tested that a 90-day-old attachment expires under a 30-day policy while a 2-day-old one does not. |
| 218 periodic maintenance pass | ✅ | **Built** (`maybe_run_maintenance`): runs off the net loop's 500 ms tick, gated to `OPENCHIME_MAINT_INTERVAL_MS` (default 5 min), so a quiet box is maintained too — deliberately unlike `maybe_prune_idem`, which only fires when writes happen. Work splits by thread: the writer selects and tombstones rows, the transfer pool deletes bytes (it can block on S3). Bounded by `OPENCHIME_MAINT_BATCH` (default 64) per pass. |

---

## Windows GUI feature parity (ARCH-82)

The **TUI is the reference client** — nearly every capability the app-core
exposes is reachable from it ([CLIENT.md](./CLIENT.md) §3, which also lists the
two that are not). This table tracks the
native Win32 GUI (`client/gui/win32/`) toward the same bar. The contract is the
`oc_client_*` facade (`client/core/client.h`): each row is a feature the GUI must
surface as an **affordance** (button / menu / dialog — never a slash command).
Legend: ✅ done · 🔨 in progress · ⛔ not started.

| Feature | Core intent(s) | GUI | Notes |
|---|---|---|---|
| Connect + local login | `start_secure` | ✅ | Two-step in-window sign-in view (WIN-2): workspace → DNS resolve → username/password + Remember me, errors inline and retryable. Defaults to the hosted `<name>.openchime.io` form with **Advanced options** for a full address. Dev args and a stored session token still skip it. |
| Channel list + switch | `list_channels`, `backfill`, `mark_read` | ✅ | Sidebar, click to switch, auto-select first. |
| Live messages + history | model render | ✅ | Grouped transcript, avatars, times, wheel scroll. |
| Send | `send` | ✅ | RichEdit composer, Enter sends / Shift+Enter newline. |
| Edit / delete | `edit`, `delete` | ✅ | Message right-click; inline edit in the composer. |
| Reactions (toggle) | `react` | ✅ | Right-click → React (emoji submenu). |
| Who reacted | `list_reactions` / `close_reactions` | ✅ | Message menu → Who reacted; reactor overlay. |
| Typing indicator | `typing` | ✅ | Sends while composing; renders "X is typing…" in the header. |
| Direct messages | `open_dm` | ✅ | Left-click a member. |
| Roster + presence | `list_users`, `toggle_roster` | ✅ | Members pane with presence dots + roles. |
| Set own presence | `set_presence` | ✅ | Rail → profile avatar → Set status: Online / Away. |
| Admin: roles / remove | `set_role`, `remove_user` | ✅ | Member right-click, role-gated. |
| Admin: invite | `invite_user` | ✅ | Rail → workspace menu → Invite people as member / as admin (token shown once, owner/admin only). |
| Threads | `open_thread`, `reply`, `close_thread` | ✅ | Message menu → Reply/Open thread; overlay + reply composer. |
| Search | `search`, `close_search` | ✅ | Rail → New (+) → Search messages… or Ctrl+F; a result jumps to the matched message and flashes it (WIN-3). **Paged** (WIN-38): `SEARCH` carries a `before_id` **keyset** cursor — not an offset, so a message posted mid-paging cannot make a row repeat or vanish — and "Load more results" appears only when the server says there is more. **Operators** (WIN-39): `from:` / `in:` / `has:file\|link\|image` / `after:` / `before:`, parsed in `shared/searchq.c` so the client's filter line and the daemon's WHERE clause cannot disagree. |
| Channel management | `create_channel`, `join_channel`, `leave_channel`, `list_members`, `update_channel` | ✅ | Rail → New (+) → New channel (name + public/private); sidebar right-click Join / Leave / Mark as read; the members pane lists the **channel's** roster (REQ-031). The **About** tab sets the topic (any member) and renames or archives (owner/admin) — REQ-034/035/036. |
| @mentions | shared `oc_mention_scan` + notify level | ✅ | Accent-coloured, semi-bold spans; a message naming you tints its row and gets an accent bar; the `mentions` notify level is evaluated with the same scanner the daemon resolves with (REQ-221). |
| Pins | `pin`, `list_pins`, `close_pins` | ✅ | Message menu → Pin/Unpin to channel; a "Pinned by …" marker above the message; the **Pins** tab lists them, jumps to one, or unpins (REQ-230). |
| Channel files | `list_files`, `close_files` | ✅ | The **Files & links** tab: name, uploader, size, date, download, jump-to-message (REQ-143). The workspace-wide form (`channel_id 0`) fills the rail's **Files** view, naming the channel each file came from and jumping to it, with a name search and type/ownership filters. Its channel column is exact rather than inferred from the 200-row page (WIN-82, `LIST_FILE_CHANNELS`). |
| Attachments: download | `download` | ✅ | Right-click → Download (native Save dialog), or the Files tab. |
| Attachments: upload | `upload` | ✅ | Composer "+" button + drag-drop anywhere. |
| Notifications / DND | `set_notify_pref`, `set_dnd`, `list_notify_prefs` | ✅ | Channel menu level + rail → profile avatar → Do not disturb… (an on/off check plus validated From/To fields, WIN-13 — typed `HH:MM`, no time picker and no weekday schedule, REQ-136) + a **Notifications pane** (WIN-12) that calls `list_notify_prefs` and edits the per-channel level. |
| Self-service profile | `set_display_name`, `change_password` | ✅ | Rail → profile avatar → Change display name… / Change password… (one dialog each; the password form has a confirm field, WIN-20). |
| Webhooks | `webhooks`, `create_webhook`, `delete_webhook` | ✅ | Channel **About** tab → Webhooks… (channel-scoped admin belongs with the channel, ARCH-94); also the channel menu. Overlay, click-to-delete. |
| Storage / audit (admin) | `storage_status`, `audit_query` | ✅ | Rail → **Admin** (Storage · Audit log tabs, refetched on entry, owner/admin only); also on the workspace menu. |
| Settings sync | `set_client_type`, `set_setting`, `list_settings` | ✅ | Identifies the `gui` bucket + lists on connect. |
| Read receipts (seen-by) | model `readers[]` | ✅ | "✓ Seen by …" footer under the transcript. |
| Logout | `logout` | ✅ | Rail → workspace menu → Sign out / Sign out everywhere; window closes on the drop. |
| Manual reconnect | `reconnect` | ✅ | Three ways: the **connection dot** beside the workspace name (filled = live, hollow = not; click to retry, WIN-64), the connection banner with the reason, a live countdown and "Retry now" (WIN-1/WIN-55), and the workspace menu's Reconnect now. |
| Multiple workspaces | one `oc_client` per ws + switcher | ✅ | **Built** (WIN-29). Up to `WS_MAX` clients are held in a slot array (`g_wss`) and **every one is ticked each frame** (`WM_TIMER`/`TIMER_TICK`), not just the one on screen, so a background workspace drains events, accrues unread and can raise a notification. Per-workspace view state (selection, scroll, backfill set) swaps on switch; unread elsewhere surfaces as a rail badge (`ws_unread_elsewhere`) and per-row counts in the switcher. A no-argument launch signs in to every remembered workspace holding a session token (WIN-57). |

> **Depth caveat:** this table tracks whether each engine feature is *reachable*; it does **not** measure how developed each screen/dialog is. For the full four-way (Slack vs Pumble vs TUI vs Win32) surface-depth gap analysis — including underdeveloped screens and a recommended build order — see [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md).

### The shell (the "nav epic")

The app menu that once hung off the window is gone; the GUI is now organized
around a **global left-nav rail** (Slack-shaped, Lucide icons stroked as Direct2D
path geometry — VENDORS.md). Reading top to bottom:

- the **workspace avatar** (its initial) — opens the workspace switcher;
- six **primary views** — Home, DMs, Activity, Files, Later, and Admin
  (owner/admin only), overflowing into a "More" flyout when the window is short;
- a bottom cluster — **New (+)** and the **profile avatar**.

Three custom Direct2D dropdowns replace the old native app menu: the **workspace
menu** (invite, preferences, storage/audit, reconnect, sign out), the **profile
menu** (presence, DND, display name, password), and the **New menu** (channel,
DM, upload, search). The channel column gained a header with settings + compose
buttons and a **"Find a conversation"** filter box (a native `EDIT` that
substring-filters channel names).

**Home and DMs both render the chat shell**, but they are no longer the same
thing: **DMs is a person-centric index** — every workspace member as a row, with
existing conversations first and everyone else below, picking one opening (or
creating) the DM. It also surfaces **self-DM** (REQ-055), which the engine has
supported all along with no client able to reach it.

**No rail view is a stub any more.** Activity (REQ-139), Files (REQ-143) and
Later (REQ-231) were the last three and all now render real surfaces — verified
2026-07-30 against a running client: Activity has All/Mentions/Reactions/Threads
filters over a real feed, Files has a channel column with a name search and
type/ownership filters, Later lists saved items by channel. `draw_stub_view`
survives only as the `default:` arm of the view switch. Admin is likewise a
developed pane (Storage · Audit log · Invites).

**Every tracked engine feature is reachable**, and since this table was written
the Win32 GUI has moved from trailing the TUI to leading it: sign-in was rebuilt
(WIN-2), the failure surface landed (WIN-1), the client became stateless
(ARCH-88), and @mentions, pins, the channel Files tab and the per-channel roster
(REQ-221/230/143/031) shipped here first.

**The depth pass is done.** Every item that opened it — the error/toast and
connection surface (REQ-263), navigable search with paging and operators
(REQ-080), the sidebar overhaul (REQ-267), the preferences hub (REQ-261), the
command palette (REQ-260), inline images (REQ-142) — has landed, along with
mark-unread, mute, star, forward, copy-link/permalink, invite management with
revoke, the active-session list, group DMs, custom emoji, avatars and the
N-concurrent-workspace model. What is left in this client is the short open list
in [WIN32_BACKLOG.md](./WIN32_BACKLOG.md):

**Everything startable in this client is now closed** (2026-07-31). The avatar
defects (WIN-85/86), the flaky harness and its daemon-selection trap (WIN-87/88),
the typed group-DM picker (WIN-93) and the group shown as "user" in the DMs index
(WIN-95) are all fixed, verified on Windows and covered by the smoke. Two product
bugs the reworked harness then caught are fixed with them: the composer could
reach **negative width** and vanish on a default window at 150% DPI, and an
unrelated server error mid-transfer desynchronised the client's transfer slot
from the daemon's, so every later upload failed with an opaque `transfer error`.

What remains is **blocked, not deferred** — each needs a daemon requirement or a
product decision, and none is startable in `client/gui/win32/`:

- ~~**WIN-60** — the unreproduced crash while typing.~~ **Closed 2026-07-31.**
  All three occurrences predate the composer rewrite (WIN-80/ARCH-98) that
  replaced the RichEdit child wholesale the next morning, so they happened in
  code that no longer exists. The crash filter is verified working (a deliberate
  fault produces the report and a minidump) and has seen nothing since, across a
  targeted composer stress run and repeated smoke runs. A recurrence is new
  information and a new id.
- ~~**WIN-89 / REQ-269** — accessibility.~~ **Built 2026-07-31 (ARCH-99).** A UIA
  provider over the self-drawn UI, a real system caret, TextPattern on both the
  transcript and the composer, and announcements. The custom controls were not
  walked back. Verified by a real UIA client walking the tree from outside the
  process (`scripts/uia_probe.ps1`).
- ~~**WIN-90 / WIN-96 / REQ-220** — rich text and its toolbar.~~ **Built
  2026-07-31 (ARCH-100).** The parser is `client/core/richtext.c` — shared, so
  the TUI inherits it when it catches up — Win32 renders its spans as DirectWrite
  ranges, and a seven-button toolbar over the composer plus Slack's chords
  (Ctrl+B/I, Ctrl+Shift+X/C/7/8/9) write the same delimiters you could type.
- **WIN-91** — drafts across a restart. ARCH-88 leaves only server storage, so
  this needs a decision on where a draft lives *and* on whether a half-typed
  message should sync to your other devices.
- **WIN-92 / REQ-278** — "pause notifications until…". Needs the daemon half
  (`dnd_until_ms`) built first.
- **WIN-94 / REQ-135/136** — the per-weekday schedule and keyword alerts. Same:
  daemon first, and the client half is small.

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
- ✅ **Codec fuzzer + concurrency load test.** A deterministic fuzzer runs 45k
  iterations by default (30k random + 15k framed, overridable via
  `OC_FUZZ_RANDOM_ITERS`/`OC_FUZZ_FRAMED_ITERS`) of random/framed bytes through
  `oc_parse_frame` + every decoder
  (clean under ASan/UBSan); an 8-client concurrent-send load test exercises the
  accept path, writer, and fan-out under contention.

**Open**

None — the robustness backlog is clear. REQ-210/211 are now benchmarked
(`Scripts/bench.sh` + `tests/bench_load.c`): ~50 KB RSS per idle connection, so
low-hundreds of connections fit in tens of MB of the 256 MB lean profile, at
p50 ~2–3 ms message round-trip (p99 ~130 ms at 32 concurrent senders). The one
measured bottleneck is connection *setup* throughput — the 600k-iteration PBKDF2
auth on the single writer, at **~2 logins/s** — a correctness-preserving cost,
not a memory limit; session-token reconnect (ARCH-58) skips PBKDF2 entirely,
which is what makes a thundering-herd reconnect tolerable. There is no periodic
large-scale soak yet. Both figures here were corrected on 2026-07-19 after the
harness itself proved to be the limit; see [BENCHMARK.md](./BENCHMARK.md).

---

## Summary

The **daemon is a feature-complete v1 chat core**, all reachable over the wire
and tested end-to-end: two-mode auth with daemon-owned sessions and revocation,
roles + full tenant administration, public/private channels and DMs, messaging
with edit/delete, reactions, threads, full-text search, presence/typing,
notification settings (level + DND), attachments (proxied chunked transfer + both
blob backends), incoming webhooks, the storage-maintenance tiers, the audit log,
federated enrollment, and the mobile-push emitter (ARCH-85). Server-relayed audio
(REQ-150–152) is built server-side (signaling + forked UDP relay sidecar). The
**server-robustness backlog is cleared** (see Resolved above) and the capacity
profile (REQ-210/211) is benchmarked ([BENCHMARK.md](./BENCHMARK.md)). What remains
on the daemon is not hardening but the follow-ups noted per-REQ above (e.g. the
webhook CA cert REQ-171). The MENTIONS push level is no longer pending — REQ-221
closed it.

The **client** is a shared, frontend-agnostic **C app-core** (ARCH-74) with two
frontends over it: a **termbox2 + utf8proc TUI** (ARCH-75) and the **native Win32
GUI** (ARCH-82). The TUI was the reference client and reached *every* engine
feature on the wire until the July 2026 work; **the Win32 GUI now leads it by
more than twenty features**, not the four this section claimed until 2026-07-30.
Everything in this list exists on the wire and in the app-core and is surfaced
only in the GUI, so closing it is TUI work alone:

- **Messages:** @mentions (221), pins (230), saved items / Later (231), forward
  (057), copy-link and permalink navigation (232), mark-unread (235), the unread
  divider and jump-to-unread (236), mark-all-read (238), inline images (142 —
  *the one permanent exemption*, ARCH-75 renders no graphics).
- **Channels:** the per-channel member roster (031), topic (034), archive (035),
  rename (036), visibility change (036a), browse/join directory (038), the files
  listing (143), mute (137), starred conversations and user-defined sections (234).
- **People and presence:** group DMs (056), custom status with expiry (122),
  profile depth incl. avatars (240/241), the other-user profile viewer (266).
- **Notifications:** the global default level (134), the activity feed (139).
- **Shell and account:** the preferences hub (261), theme/appearance (262),
  first-run onboarding / invite redeem (268), configurable quick reactions (073),
  custom emoji (072), the active-session list (182), webhook *delete*, and
  log-out-everywhere.

The app-core carries all of it, which is what makes this a catch-up rather than a
build. See [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §4 for the same
list framed as gaps.

The core's **store keeps nothing on disk** (ARCH-88/REQ-201 — done, not in
progress: `client/core/store.c` references SQLite zero times). One credential per
workspace in the OS credential store carries the session token, the TOFU pin and
the workspace book, which is why silent reconnect and a persisted pin survive a
restart; history comes from the server's own read cursor and the outbox lives in
RAM (REQ-100/101/102 met client-side, at the cost of no offline history and a
queued message dying with the process).

The remaining client work is scope, not hardening, and the frontend order is
fixed: **all of Win32, then the TUI, then GTK, then macOS.** So the **Win32 open
list** ([WIN32_BACKLOG.md](./WIN32_BACKLOG.md) — now four items, each waiting on
a daemon requirement), then the **TUI catch-up** below, then the later native
GUIs (GTK/AppKit) + web/mobile. Alongside those, independent of frontend order:
the **OIDC browser flow**, the **audio client** (Opus/UDP), and **screenshare**
(REQ-161).

The forward feature scope is the Sections 11–14 table and the **Non-video
competitor-parity backlog** table below (REQ-026…275), reconciled
against [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md); the prioritized client
build order lives in that document's §5 and the [CLIENT.md](./CLIENT.md) §8 roadmap.

### 11–14. Rich Text, Retrieval, Profiles, Compliance

*Mostly forward scope; tracked here so the omission is visible rather than implied.*

| REQ | Status | Notes |
|-----|--------|-------|
| 221 @mentions | ✅ | **Built (ARCH-89, migration 0021):** the body stays plain UTF-8 with the literal `@name`; `shared/mention.c` is the single scanner both daemon and client link, so highlight and notify cannot drift. The daemon resolves each name against the *channel's* membership and stores `(message, user, kind, span)` in `mentions`; `@here`/`@channel`/`@everyone` are broadcasts. The Win32 client accent-colours mention spans and tints a row that names you. Limitation: `@here` is treated as a plain broadcast for push, because presence is not visible to the push worker's read-only connection (ARCH-66). |
| 230 pins | ✅ 🔵 | **Built (ARCH-90, migration 0022):** a pin is channel state keyed on the message — any member may pin or unpin (including someone else's), capped at 100 per channel, listed newest-first with each message's body so the view opens in one round trip. Pin state is replayed on backfill, so it survives a reconnect. Win32 shows a "Pinned by …" marker inline and a Pinned list in the channel header. **TUI has none of this yet.** |
| 231 saved items ("Later") | ✅ 🔵 | **Built (ARCH-95, migration 0025):** `saved_items` keyed (user, message) — private, the mirror of a pin. Win32 fills the **Later** rail view, with "Save for later" in the message menu and Remove in the list. **TUI: none.** |
| 220 rich text | ✅ 🔵 | **Built 2026-07-31 (WIN-90, ARCH-100, [MARKDOWN.md](./MARKDOWN.md)):** `client/core/richtext.c` parses the dialect — Slack-compatible inline emphasis (`*bold*`, `_italic_`, `~strike~`, backticks, `>`), extended with the ordered/unordered lists Slack's markup has no syntax for — and returns spans over the unchanged plain-UTF-8 body, so search and the mention byte offsets keep addressing the same text. In `client/core/` and never the daemon: formatting needs no server knowledge, unlike mentions (ARCH-89). Ambiguity is where the work is, and it is pinned down by `tests/test_richtext.c`: `2 * 3 * 4` is arithmetic, `a_variable_name` is an identifier, `snake__case__here` is one too, an unclosed delimiter never restyles the rest of the message, code suppresses everything inside it, and `\*` writes a literal asterisk. Win32 renders it as DirectWrite ranges beside the existing mention highlighting — in the transcript the delimiters are *removed*, in the composer they stay visible but faint, because there the text is what will be sent. Slack's `<URL\|label>` links and HTML-entity escaping are deliberately **not** adopted: both are API-layer artifacts that would put `&amp;` in the FTS5 index. **Authoring** (WIN-96): a formatting toolbar over the composer and Slack's chords wrap the selection in those same delimiters — never a rich-text model, because ARCH-100 §5 requires everything the toolbar produces to be expressible in text. It toggles off on a second press, trims the selection to its non-space core (a delimiter beside a space is not a delimiter), and marks every line a selection touches for the block forms, numbering `1. 2. 3.`. **The TUI does not render or author it yet** — the parser is shared and waiting for it. |
| 222–229 unfurls, threads-in-place, etc. | ⛔ |
| 240–242 profiles, timezone, status text | ✅ | **Built** (WIN-47/53, migrations 0027): title, timezone, custom status with expiry, and **avatar images** (2026-07-30) — an attachment id, so upload/caps/dedup/reclaim are the existing paths. Pronouns are the one field not built. Building avatars exposed that the reclaim sweep would have deleted every one of them. |
| 250 opt-in retention policy | ⛔ | Distinct from REQ-217's max attachment age (built) — 250 also ages out *messages*, which nothing does today. Still `[needs ARCH decision]`. |
| 251 audit log | ✅ | **Built** (ARCH-79, migration 0016): four families — admin, account, security, moderation — recorded on the writer, read via `AUDIT_QUERY`/`AUDIT_PAGE` (0x0099/0x009A) and the TUI's Audit log action (Ctrl+K → Audit log), owner/admin gated against the user's *current* role. Never records the secret involved. Verified live: role change, invite, and password change all appear with the acting user, and a member promoted mid-session immediately gains access. |
| 251a bounded audit log | ✅ | Aged out by the ARCH-78 maintenance pass, `OPENCHIME_AUDIT_MAX_DAYS` (default 365). |
| 251b per-family cap | ✅ | **The cap is applied per family**, so a flood of attacker-controlled `auth.failed` entries cannot age out administrative history — without this the audit log becomes an evidence-shredder. Rate-limited attempts are dropped silently rather than audited, so a throttled spray produces no rows at all. Regression-tested with a 200-entry security flood against a surviving admin entry. |
| 252 legal hold | ⛔ | Narrowed 2026-07-30: export → REQ-276, DLP → REQ-277. `[needs ARCH decision]`. |
| 276 compliance capture | ⛔ | Scoped 2026-07-30, two mechanisms: vendor push (Global Relay EML over **SMTP journaling**; file-drop first) and our own documented pull API. Gaps held open: extract schema, credential model, user→email mapping. |
| 277 DLP at send time | ⛔ | Scoped 2026-07-30. Pre-post **webhook**: what it returns is what is stored, so nothing is redacted after delivery. Reference SSN redactor in the test suite. Open: fail-open vs fail-closed, contract, signing. |
| 253 SCIM provisioning | ⛔ | Federated function (ARCH-76); central-service concern. |

---

## Non-video competitor-parity backlog (REQ-026…275)

The non-video features Slack/Pumble ship that OpenChime lacked a requirement for
are now specced (REQUIREMENTS.md §§1–16, added from
[CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md)). This started as pure forward
scope; **much of it has since shipped** — read each row's status mark, not the
section heading. What remains unbuilt is mostly either an explicit exclusion (➖)
or still waiting on an ARCH decision. Tracked here so the target-state contract
and the reality stay reconciled. The **prioritized build order** lives in
CLIENT_GAP_ANALYSIS.md §5 and the CLIENT.md §8 roadmap, not here.

| REQ | Status | Notes |
|-----|--------|-------|
| 026 shareable invite links + invite mgmt | ⛔ | Daemon has single-use invite tokens (REQ-024/033); link/expiry/revoke/pending-list unbuilt. |
| 027 SSO scope (OIDC-via-central only; no SAML) | ➖ | Design exclusion (ARCH-55); recorded, not code. The daemon already only verifies the central ES256 JWT. |
| 034 channel topic / description | ✅ 🔵 | **Built (ARCH-93, migration 0024):** a `topic` column set via `UPDATE_CHANNEL`; **any member** may set it, empty clears it, 250-byte cap. Win32 shows it on the header's second line and edits it in the About tab. **TUI has none of this.** |
| 035 archive channel | ✅ 🔵 | **Built (ARCH-93, migration 0024):** `archived_at_ms` non-NULL is the flag. Read-only enforced in `channel_post_access`, so send, threaded reply, upload and webhook post all return `CHANNEL_ARCHIVED`; hidden from the list for non-members, kept (flagged) for members; reversible. Owner/admin. Win32: header badge, About-tab toggle with a confirm, read-only composer. **TUI: none.** |
| 036 rename channel | ✅ 🔵 | **Built (ARCH-93):** owner/admin; the **id is untouched**, so membership, history and cursors follow it. |
| 036a change channel visibility | ✅ | **Built 2026-07-30** (`OC_CHUP_PRIVATE` / `OC_CHUP_PUBLIC`): two ops rather than a toggle, so two admins cannot flip it twice. No membership surgery either way. The directions are asymmetric and the product says so — public→private narrows; private→public discloses the whole history and cannot be undone by flipping back. |
| 037 guest accounts | ⛔ | Role model is owner/admin/member (REQ-030); no channel-scoped guest. |
| 038 browse/join public-channel directory | ✅ 🔵 | **Win32 built** (WIN-54a): "Browse channels" lists every public channel with its topic, unjoined first, Join or Open. No daemon work — `LIST_CHANNELS` always returned them with a `joined` flag; what was missing was somewhere to see them. |
| 042 workspace settings + branding | ⛔ | No tenant name/icon/default-channels/join-policy surface. |
| 043 admin console (member table, bulk, analytics) | ⛔ | Admin ops exist per-action (roles/invite/remove/storage/audit); no console. |
| 056 group direct messages | ✅ | **Built 2026-07-30** (`OPEN_GROUP_DM`): a group DM is a DM with more than two participants — same kind, same `dm_key` identity, same membership and read-access code, so no migration and no second path that could disagree. Reopening the same set returns the same conversation. Titled by its people from one renderer in the core. |
| 072 custom emoji | ✅ | **Built 2026-07-30** (migration 0029): name is the identity, image is an attachment, catalogue fanned to everyone on change. Drawn wherever an emoji is — chips, picker, reactor list — and inline in message bodies over a transparent shortcode. Reclaim excludes them, the trap avatars taught. |
| 081 search operators (`from:` `in:` `has:` dates) | ✅ | **Built** (WIN-39, `shared/searchq.c`): one parser both frontends and the daemon share. `has:link` was silently matching nothing until 2026-07-30 — bodies are BLOBs and LIKE on a BLOB is false for every pattern. |
| 234 starred conversations + custom sections | ✅ | **Built** (WIN-41/83): both halves, in `client_settings` so nothing is stored locally (ARCH-88). A conversation appears exactly once; caps refuse rather than evict. |
| 057 forward / quote-share a message | ✅ 🔵 | **Win32 built** (WIN-51): forwards carry the quoted body, attachment name and a permalink; `oc_send` already carried `attach_ids`, so no new op. |
| 062 followed-threads view + follow/unfollow | ⛔ | Threads work (REQ-060); no follow-state or aggregated view. |
| 073 configurable quick reactions | ✅ 🔵 | **Win32 built** (WIN-28): six shortcodes in Preferences, resolved through the shared catalogue so an unknown name drops rather than renders wrong. |
| 122 surface DND/OOO/custom-status in presence | ✅ 🔵 | **Built** (WIN-53, migration 0027): custom status with expiry, shown beside names in the member pane and profile. The rail's own avatar carries presence AND a quiet-hours badge (2026-07-30). |
| 134 global (default) notification level | ✅ | **Built 2026-07-30** (migration 0028, `SET_NOTIFY_DEFAULT`): `users.notify_default`, carried on every NOTIFY_PREFS sync so no client guesses it, and honoured by push as `COALESCE(np.level, u.notify_default)`. Fixed two push bugs on the way: the fallback was hardcoded to ALL, and `np.muted` was never consulted. |
| 135 keyword / priority-people alerts | ⛔ | **Specified 2026-07-31.** Keywords are part of the **mentions** level, not a switch of their own (Slack's middle level is literally "Mentions & keywords"), matched **case-insensitively and exactly** — "deploy" ≠ "deployment" — phrases allowed, surfacing in the activity feed as a mention rather than a fourth kind. **Keywords fire in threads**, a deliberate divergence: Slack's don't, which reads as a limitation rather than a decision. **Priority people pierce a level and a pause, never a mute** — mute is the strongest "do not hear from this" (REQ-137) and a VIP overriding it would make it unreliable when someone is reaching for it. Slack documents only the pause case; the other two are ours. |
| 136 recurring notification schedule | ⛔ | **Specified 2026-07-31 and now the ONLY recurring mechanism** — it absorbs REQ-131. Slack's shape: *Every day* / *Weekdays* / *Custom*, Custom carrying an independent start and end **per weekday**. Stored against the user's **local** calendar day (REQ-241), never UTC — in UTC a "Friday evening" becomes Saturday for much of the world. Replaces the two minutes-of-day columns rather than supplementing them; no live deployments, so the change need not preserve values. |
| 278 pause notifications until an instant | ⛔ | **Fully specified 2026-07-31 against Slack's own API, not its help pages.** `dnd.info` separates the **schedule** (`dnd_enabled`, `next_dnd_*` — REQ-131/136) from the **pause** (`snooze_*` — this row); `dnd.setSnooze` takes *minutes from now*, `dnd.endSnooze` and `dnd.endDnd` cancel each independently. A pause only ever **adds** silence, so no precedence rule is needed. **The fact is public, the timing is not** — every `snooze_*` field is self-only in Slack, so others learn *that* you are not to be disturbed, never when you are back (REQ-122). VIPs pierce a pause (REQ-135); **the sender override is a deliberate divergence** — the one place we do not match Slack, because a do-not-disturb any sender can pierce is a weaker promise than it reads. Storage: `dnd_until_ms` on `users`, 0 = ended. |
| 279 workspace default DND hours | ⛔ | **Scoped 2026-07-31.** Owner/admin sets a default for members who have not configured their own; **any member can override or disable it** — a non-overridable default would be an availability policy, not a preference. Blocked on there being no tenant-settings surface at all (REQ-042). |
| 280 no email notifications | ➖ | **Decided 2026-07-31 — an exclusion, not a gap.** No digest, no mention mail, no outbound SMTP, in any deployment model: it would oblige every self-hoster to run a mail relay and would put message content into a third-party mail system, against REQ-040/041. **Stated consequence:** with push federated-only (REQ-133), a **self-hosted stand-alone deployment has no out-of-app notification path at all**. |
| 281 one notify evaluator + precedence | ⛔ | **Scoped 2026-07-31, and the highest-value item in this section.** Nine inputs feed one boolean; REQ-134 already exposed two silent push defects of exactly this kind (fallback hardcoded to `ALL`; `np.muted` never consulted) and three more inputs are about to join. Wants a **pure** evaluator, one implementation shared by the push worker and every client (the ARCH-89 mention-scanner argument), and a **truth table** rather than case-by-case tests. Open: the precedence order itself, incl. whether priority people pierce a pause. |
| 282 follow every thread in a channel | ⛔ | **Scoped 2026-07-31.** Per-conversation, distinct from following one thread (REQ-062) — following threads individually requires having already seen the thread, which is the thing being missed. Storage falls out naturally as a column on `notification_prefs` beside `level` and `muted`. |
| 283 reminders + saved-item due dates | ⛔ | **Scoped 2026-07-31 — the only true capability gap in notifications, the rest are knobs.** In-app and push delivery only, never email (REQ-280); surfaces as an activity-feed entry rather than a bot DM, since there is no bot (REQ-275). Cheaper than it looks: `saved_items` (migration 0025) needs one `remind_at_ms`, and the ARCH-78 maintenance pass is a delivery sweep that already exists. Open: sweep granularity — the pass defaults to 5 min, which is a different promise from firing on the minute. |
| 284 what the unread badge counts | ⛔ | **Scoped 2026-07-31.** A requirement about *meaning*, not three settings: nothing currently states what a number on a conversation represents, which is how two clients disagree about the same count and both look right. Default is the quieter reading (badge = what was worth notifying). |
| 285 call-start notification | ⛔ | **Scoped 2026-07-31.** A missed message is read later; a missed call is missed. No new call machinery — `CALL_JOIN` on an empty roster is already the "started" event (ARCH-73) — so it is a notify decision over state the daemon keeps. Ships with the audio client. |
| 286 desktop close-to-tray / flash | ⛔ | **Scoped 2026-07-31, because the behaviour already exists with no contract.** Win32 ships a tray icon and balloons while `WM_CLOSE` quits, so closing the window ends notifications today and nothing said whether that was intended. Default should be keep-running-in-tray. Client preference (ARCH-92); the default is a product call. |
| 287 mentioning a non-member is never silent | ✅ 🔵 | **Built 2026-07-31.** New `MENTION_UNRESOLVED` frame (0x00B3) to the **sender only** — not an ERROR, since nothing failed and the message was stored. The daemon distinguishes a typo from a real person in the wrong channel, and answers `can_add`/`is_private` itself so the client never offers an action that would fail. Win32 opens a confirmation offering to add them; a **private** channel states that adding discloses the full history (cf. REQ-036a) instead of making it one click; a DM or archived channel degrades to a notice. **TUI: none yet.** |
| 288 public-channel mention notifies non-members | ✅ 🔵 | **Built 2026-07-31.** A mention in a **public** channel now resolves against the whole roster and reaches the person's **activity feed**, even though they are not a member; a **private** channel is unchanged and stays silent. Three gates, not one: `store_mentions` resolves it, the activity query accepts membership **OR** a public channel — that second one is where it actually lands, and without it the row is stored and nobody ever sees it — and push is deliberately **left membership-gated**, so this never rings a phone about a channel somebody never joined. Verified with two accounts: bob sees "Mention in #townsquare" without being a member, and sees nothing at all for a private one. |
| 137 mute channel/DM (suppress + de-emphasize) | ✅ | **Built** (WIN-40, migration 0026): a `muted` column distinct from level — level decides whether the daemon notifies, mute also de-emphasises the sidebar row and drops the badge. Push honours it as of 2026-07-30. |
| 138 OS toast + sounds + badges | 🔵 ⛔ | Per-client rendering of the notify decision (ARCH-72); no client does OS toast yet. |
| 139 activity feed / notification inbox | ✅ 🔵 | **Built (ARCH-95):** a union of three queries — mentions of you, reactions to your messages, replies under your threads — excluding your own actions and gated on current membership. No maintained table: the rows were already stored and indexed. `users.activity_seen_ms` is a watermark for "what is new", deliberately not per-item read state. Win32 renders it as a list-and-detail view (ARCH-94): the feed is the second column, the conversation stays in the middle, and clicking an item shows the thread from that message down — reaching it through fetch-around (ARCH-96) however old it is. Filter by All / Mentions / Reactions / Threads. **TUI: none.** **Extended 2026-07-31 (WIN-97):** Slack's Activity also answers *what have I not read* — **Unreads / DMs / Channels** — which ours does not. Now in scope for REQ-139: one query with three predicates (messages past the `delivery_cursors` read cursor, filtered by channel kind or notify level), not three features. Slack's saved custom views stay out. |
| 142 inline image/thumbnail rendering | ✅ 🔵 | **Win32 built** (WIN-17): WIC decode + D2D, click to expand, save-on-hover. TUI exempt (ARCH-75). Screenshots could not show them until 2026-07-30 — the capture used its own render target, so every capture of this app was a picture with the pictures missing. |
| 143 files browser (channel + workspace) | ✅ 🔵 | **Built (ARCH-91, migration 0023):** `LIST_FILES` streams a channel's shared files newest-first, or (channel 0) every channel the caller can read. Pending uploads excluded; reclaimed rows listed and flagged rather than hidden. Win32 surfaces it twice: the channel's **Files & links** tab, and a workspace-wide **Files** view on the rail that names the channel each file came from and jumps to it. Both filter by type (All / Images / Documents / Other), client-side over `mime`, and say so when the 200-row cap is hit. **TUI has none of this.** |
| 176 third-party API / SDK | ⛔ | No public programmatic surface; wire is the custom binary protocol (ARCH-6). |
| 177 email-to-channel ingestion | ⛔ | Needs out-of-daemon inbound-mail; unbuilt. |
| 184 MFA / 2FA (local mode) | ⛔ | Local auth is password-only (ARCH-59); no second factor. |
| 192 IP allowlist / access restriction | ⛔ | Per-IP throttle exists in the accept loop; no allowlist. |
| 223 drafts | ⛔ | **Decided 2026-07-31 (ARCH-101), not built.** Server-stored in a `drafts` table keyed `(user_id, channel_id, thread_root)` with its own ops — deliberately not `client_settings`, which is partitioned per frontend and would leave a GUI draft invisible in the TUI. `thread_root` is in the key from day one (0 = the channel) so thread drafts cost a client change later, not a migration. Last-writer-wins, made survivable by two client rules: only write a draft you changed, and never overwrite a composer being typed in. Kept on archive and on leaving (both reversible); deleted on send, channel deletion and user removal. Sidebar marks a conversation holding one, matching Slack; not counted as unread. In scope for compliance capture (REQ-276); DLP still redacts at send (REQ-277). |
| 225 native polls | ⛔ | Not a message type today. |
| 226 code/text snippets | ⛔ 🔵 | **Half of it built:** fenced code blocks parse and render since REQ-220 (2026-07-31), in the Win32 client only. What 226 still lacks is a first-class snippet *object* — a titled, named body you can share and open on its own rather than a fenced run inside a message. |
| 235 mark message/conversation unread | ✅ | **Built** (WIN-52, migration 0027): a DISTINCT op that may move the cursor backwards — the ack path stays monotonic by construction (`MAX(message_id, excluded)`), which is deliberate replay safety, not an oversight. |
| 236 new-message divider + jump-to-unread | ✅ 🔵 | **Win32 built**: an unread divider in the transcript plus an "N new" jump in the header. TUI has markers only. |
| 237 all-unreads / unreads-only views | ⛔ | Per-channel unread tracked (REQ-014); no aggregate view. |
| 238 mark-all-read / catch-up | ✅ 🔵 | **Win32 built** (WIN-33): a loop over the existing CLIENT_ACK — cumulative per channel, so no new op was needed. A true bulk op stays open as REQ-238's server half. |
| 254 data import / migration | ⛔ | No importer; distinct from compliance export (REQ-252). |
| 260 command palette / quick switcher | 🔵 ✅ | TUI Ctrl+K (ARCH-83); **Win32 built** (WIN-11): Ctrl+K subsequence palette over a 20-action catalogue plus every conversation as "Go to", dispatching through the same menu codes the menus use. |
| 261 in-app settings/preferences hub | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-9, rebuilt WIN-78): two panes — Appearance / Messages / Notifications / Advanced — with explicit commit (Save/Cancel), reached from the You menu and Ctrl+, as well as the workspace menu. |
| 262 theme/appearance selection | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-26, extended WIN-78): **System is the default** and follows the desktop live (WM_SETTINGCHANGE), incl. the title bar. A **colour scheme** is a PAIR — nav rail + accent, both per mode — and the selected-row tint derives from the accent. Plus text size, message density and per-window zoom (ARCH-97 keeps the three multipliers apart). |
| 263 error/toast + connection-status surface | 🔵 🟡 | TUI partial (status line). **Win32 built** (WIN-1): a transient toast stack for in-session failures + a connection banner (reason + Retry now); runtime-verified. **Win32 sign-in rebuilt** (WIN-2): a two-step in-window view reporting DNS and auth failures inline, retryable, with sign-out returning to it. The reconnect countdown now ticks (WIN-55). |
| 264 keyboard-shortcut reference | ✅ | TUI `?` overlay; **Win32 built** (WIN-25): Ctrl+/ sheet generated from one KEYMAP table so it cannot drift from the handlers. |
| 265 composer autocomplete + emoji picker | ✅ | **Win32 built** (WIN-7/8): @/#/:emoji popover plus a searchable category picker, both over a shared core catalogue (`client/core/complete.[ch]`) so the two frontends complete identically. |
| 266 other-user profile viewer | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-10, moved to the context pane by ARCH-94): avatar, name, live presence, role, and Message — beside the transcript rather than over it, with one level of back to the member list. Rich fields remain REQ-240. |
| 267 sidebar org parity (DM section, sections, search) | ✅ | **Win32 built** (WIN-5/6/41/83): Starred, **user-defined sections** (8 × 32, appear-once with Starred winning), Channels and DMs, each with its own sort/filter/collapse, plus find. An expanded empty section says so in italics. |
| 268 first-run onboarding (signup/first-owner UI) | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-32): "Have an invite? Create an account" redeems an invite on the sign-in card, creating the account and signing in together. |
| 269 keyboard-only operation + accessibility | ✅ 🔵 | Win32 is keyboard-operable in the ordinary paths (composer, completion, Alt+Up/Down conversation movement, Ctrl+K, Ctrl+/, F6). The accessibility half was absent — no `WM_GETOBJECT`, no system caret — and is **built (ARCH-99, 2026-07-31)**: a UIA provider over the self-drawn UI (transcript as a navigable message list, sidebar as conversations, composer as editable text), a real system caret, and UIA events for arriving messages and failures. **The custom controls stay** — accessibility is implemented for them, not obtained by reverting to native controls. |
| 270 GIF/sticker pickers | ➖ | Explicit exclusion (app/webhook territory). |
| 271 Canvas / collaborative docs | ➖ | Explicit exclusion. |
| 272 Lists / tables / boards | ➖ | Explicit exclusion. |
| 273 Clips / async voice-video messages | ➖ | Explicit exclusion (cf. REQ-160). |
| 274 Slack Connect / cross-org shared channels | ➖ | Explicit exclusion (island model, ARCH-4/REQ-040). |
| 275 first-party bot / MCP server | ➖ | Explicit exclusion beyond the app platform (REQ-172). |
