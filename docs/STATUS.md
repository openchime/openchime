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
| 031 channel membership, public/private read-post | ✅ | Public auto-joins the poster; private is members-only. |
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
| 131 do-not-disturb schedule | ✅ | `SET_DND` stores a daily UTC minutes-of-day window on `users`; governs push, not in-app unread (ARCH-72). |
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
| 201 no local storage in any client | 🔵 ✅ | **Built** (ARCH-88). A client writes **no files at all**: the session token, TOFU pin and workspace book are one credential per workspace in the OS credential store (Credential Manager / libsecret, enumerated for the book), cached history is gone, and the outbox is in memory. Works because the read cursor is server-side (REQ-090) — a cursorless `BACKFILL_REQUEST`, or an explicit cursor of 0, resumes from it. `third_party/sqlite`, `-lsqlite3` and the borrowed `daemon/migrate.c` are gone from every client target; the Win32 binary went 2.9 MB → **1.9 MB**. Verified on Windows: sign-in, restart, silent reconnect and history all working with an empty `%LOCALAPPDATA%`. **Costs:** no offline history, a queued message dies with the process (WIN-59 warns on quit), and no keyring means no persistence at all. The rule covers client *state*; the TUI's user-authored `~/.config/openchime/config` is deliberately still a file. |
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
| Search | `search`, `close_search` | ✅ | Rail → New (+) → Search messages…; results jump to the channel, not the matched message. |
| Channel management | `create_channel`, `join_channel`, `leave_channel` | ✅ | Rail → New (+) → New channel (name only); sidebar right-click Join / Leave / Mark as read. |
| Attachments: download | `download` | ✅ | Right-click → Download (native Save dialog). |
| Attachments: upload | `upload` | ✅ | Composer "+" button + drag-drop anywhere. |
| Notifications / DND | `set_notify_pref`, `set_dnd` | 🔨 | Channel menu level + rail → profile avatar → Do not disturb… (raw `HH:MM-HH:MM` prompt). **`list_notify_prefs` is not wired** — there is no review screen for the synced prefs (the TUI has one). |
| Self-service profile | `set_display_name`, `change_password` | ✅ | Rail → profile avatar → Change display name… / Change password… (one-line prompts; no confirm field). |
| Webhooks | `webhooks`, `create_webhook`, `delete_webhook` | ✅ | Channel menu → Webhooks… / Create webhook…; overlay, click-to-delete. |
| Storage / audit (admin) | `storage_status`, `audit_query` | ✅ | Rail → workspace menu → Storage usage / Audit log overlays (owner/admin only). |
| Settings sync | `set_client_type`, `set_setting`, `list_settings` | ✅ | Identifies the `gui` bucket + lists on connect. |
| Read receipts (seen-by) | model `readers[]` | ✅ | "✓ Seen by …" footer under the transcript. |
| Logout | `logout` | ✅ | Rail → workspace menu → Sign out / Sign out everywhere; window closes on the drop. |
| Manual reconnect | `reconnect` | ✅ | Rail → workspace menu → Reconnect now (no banner or countdown). |
| Multiple workspaces | one `oc_client` per ws + switcher | 🔨 | **Rail switcher UI built** — the workspace avatar at the top of the rail (`open_switcher`/`switch_workspace`, winmain.c) — remembered workspaces + "Add a workspace…". It **stop/reconnects a single `oc_client`**, so the remaining piece is the TUI's **N-concurrent-client** model (background receive + "N elsewhere" unread), not the switcher affordance. |

> **Depth caveat:** this table tracks whether each engine feature is *reachable*; it does **not** measure how developed each screen/dialog is. For the full four-way (Slack vs Pumble vs TUI vs Win32) surface-depth gap analysis — including underdeveloped screens and a recommended build order — see [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md).

### The shell (the "nav epic")

The app menu that once hung off the window is gone; the GUI is now organized
around a **global left-nav rail** (Slack-shaped, Lucide icons stroked as Direct2D
path geometry — VENDORS.md). Reading top to bottom:

- the **workspace avatar** (its initial) — opens the workspace switcher;
- six **primary views** — Home, DMs, Activity, Files, Later, and Admin
  (owner/admin only), overflowing into a "More" flyout when the window is short;
- a bottom cluster — **New (+)**, **Alerts**, and the **profile avatar**.

Three custom Direct2D dropdowns replace the old native app menu: the **workspace
menu** (invite, preferences, storage/audit, reconnect, sign out), the **profile
menu** (presence, DND, display name, password), and the **New menu** (channel,
DM, upload, search). The channel column gained a header with settings + compose
buttons and a **"Find a conversation"** filter box (a native `EDIT` that
substring-filters channel names).

**Only Home and DMs render the chat shell**, and they render it identically —
`VIEW_DMS` has no DM-specific behaviour yet. **Activity, Files, Later, and
Notifications are `draw_stub_view` placeholders** ("coming soon"), as is the
workspace menu's **Preferences** item (a `MessageBox`). They are reachable dead
ends, and they map onto REQ-139 (activity feed), REQ-143 (files browser),
REQ-231 (saved items), and REQ-261 (preferences hub) respectively.

**Feature parity is essentially complete** — all 27 features are reachable, 25
of them fully; two are 🔨. Since this table was written, sign-in was rebuilt
(WIN-2), the failure surface landed (WIN-1), and the client became stateless
(ARCH-88) — so the depth backlog below is the live picture:

- **Notification prefs** — the per-channel level and the DND window are settable,
  but `list_notify_prefs` is never called, so there is no review screen for the
  server-synced prefs.
- **Multiple workspaces** — the rail switcher UI exists, but Win32 switches by
  stop/reconnecting a single `oc_client`, so a background workspace does not
  receive or accrue unread ("N elsewhere"). This is the one genuinely unbuilt
  *capability* versus the TUI's N-concurrent-client model.

Next per the agreed sequencing is the **depth pass**. The numbered work list is
[WIN32_BACKLOG.md](./WIN32_BACKLOG.md) (`WIN-1`…`WIN-54`); its ordering rationale
is [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §5. It leads with the
error/toast surface (REQ-263), navigable search (REQ-080), and the sidebar
overhaul (REQ-267).

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

The **client** is a shared, frontend-agnostic **C app-core** (ARCH-74) with a
**termbox2 + utf8proc TUI** (ARCH-75) as the reference frontend — **every engine
feature on the wire is reachable from it** — plus a **local store** (SQLite today;
being removed from all clients per ARCH-88/REQ-201) giving
silent session-token reconnect, a persisted TOFU pin, cached history, and an
offline outbox (REQ-100/101/102 met client-side). The remaining client work is
scope, not hardening: the incomplete **Windows GUI** depth pass, the later native
GUIs (GTK/AppKit) + web/mobile, the **OIDC browser flow**, the **audio client**
(Opus/UDP), and **screenshare** (REQ-161).

The forward feature scope is the Sections 11–14 table and the **Non-video
competitor-parity backlog** table below (REQ-026…275), reconciled
against [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md); the prioritized client
build order lives in that document's §5 and the [CLIENT.md](./CLIENT.md) §8 roadmap.

### 11–14. Rich Text, Retrieval, Profiles, Compliance

*Mostly forward scope; tracked here so the omission is visible rather than implied.*

| REQ | Status | Notes |
|-----|--------|-------|
| 221 @mentions | ✅ | **Built (ARCH-89, migration 0021):** the body stays plain UTF-8 with the literal `@name`; `shared/mention.c` is the single scanner both daemon and client link, so highlight and notify cannot drift. The daemon resolves each name against the *channel's* membership and stores `(message, user, kind, span)` in `mentions`; `@here`/`@channel`/`@everyone` are broadcasts. The Win32 client accent-colours mention spans and tints a row that names you. Limitation: `@here` is treated as a plain broadcast for push, because presence is not visible to the push worker's read-only connection (ARCH-66). |
| 220, 222–231 rich text, threads-in-place, pins, saved items | ⛔ | Forward scope; none backed by an ARCH decision yet. |
| 240–242 profiles, timezone, status text | ⛔ | Forward scope. |
| 250 opt-in retention policy | ⛔ | Distinct from REQ-217's max attachment age (built) — 250 also ages out *messages*, which nothing does today. Still `[needs ARCH decision]`. |
| 251 audit log | ✅ | **Built** (ARCH-79, migration 0016): four families — admin, account, security, moderation — recorded on the writer, read via `AUDIT_QUERY`/`AUDIT_PAGE` (0x0099/0x009A) and the TUI's Audit log action (Ctrl+K → Audit log), owner/admin gated against the user's *current* role. Never records the secret involved. Verified live: role change, invite, and password change all appear with the acting user, and a member promoted mid-session immediately gains access. |
| 251a bounded audit log | ✅ | Aged out by the ARCH-78 maintenance pass, `OPENCHIME_AUDIT_MAX_DAYS` (default 365). |
| 251b per-family cap | ✅ | **The cap is applied per family**, so a flood of attacker-controlled `auth.failed` entries cannot age out administrative history — without this the audit log becomes an evidence-shredder. Rate-limited attempts are dropped silently rather than audited, so a throttled spray produces no rows at all. Regression-tested with a 200-entry security flood against a surviving admin entry. |
| 252 legal hold + export | ⛔ | Forward scope; `[needs ARCH decision]`. |
| 253 SCIM provisioning | ⛔ | Federated function (ARCH-76); central-service concern. |

---

## Non-video competitor-parity backlog (REQ-026…275)

The non-video features Slack/Pumble ship that OpenChime lacked a requirement for
are now specced (REQUIREMENTS.md §§1–16, added from
[CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md)). All are **forward scope** —
none is built beyond what its cross-referenced note says — and most still need an
ARCH decision. Tracked here so the target-state contract and the reality stay
reconciled. The **prioritized build order** (which of these to do first) lives in
CLIENT_GAP_ANALYSIS.md §5 and the CLIENT.md §8 roadmap, not here.

| REQ | Status | Notes |
|-----|--------|-------|
| 026 shareable invite links + invite mgmt | ⛔ | Daemon has single-use invite tokens (REQ-024/033); link/expiry/revoke/pending-list unbuilt. |
| 027 SSO scope (OIDC-via-central only; no SAML) | ➖ | Design exclusion (ARCH-55); recorded, not code. The daemon already only verifies the central ES256 JWT. |
| 036 rename channel | ⛔ | No rename op; channels have a fixed name today. |
| 037 guest accounts | ⛔ | Role model is owner/admin/member (REQ-030); no channel-scoped guest. |
| 038 browse/join public-channel directory | ⛔ | No directory listing; joining needs a known channel. |
| 042 workspace settings + branding | ⛔ | No tenant name/icon/default-channels/join-policy surface. |
| 043 admin console (member table, bulk, analytics) | ⛔ | Admin ops exist per-action (roles/invite/remove/storage/audit); no console. |
| 057 forward / quote-share a message | ⛔ | Not on the wire. |
| 062 followed-threads view + follow/unfollow | ⛔ | Threads work (REQ-060); no follow-state or aggregated view. |
| 073 configurable quick reactions | 🔵 ⛔ | Reactions built (REQ-070); quick-set is a per-user client pref, unbuilt. |
| 122 surface DND/OOO/custom-status in presence | ⛔ | Presence built (REQ-120); status not projected into it. |
| 134 global (default) notification level | ⛔ | Per-channel level built (REQ-130); no workspace default. |
| 135 keyword / priority-people alerts | ⛔ | The mention→notify path it extends is now built (REQ-221/ARCH-89); keyword and priority-people matching itself is not. |
| 136 notification schedule / quiet hours | ⛔ | Single daily DND window built (REQ-131); richer schedule unbuilt. |
| 137 mute channel/DM (suppress + de-emphasize) | ⛔ | Distinct from level=none; sidebar de-emphasis + unread exclusion unbuilt. |
| 138 OS toast + sounds + badges | 🔵 ⛔ | Per-client rendering of the notify decision (ARCH-72); no client does OS toast yet. |
| 139 activity feed / notification inbox | ⛔ | No aggregated view in any client. Migration 0021 now indexes mentions by user, so the mentions half has a query behind it. |
| 142 inline image/thumbnail rendering | 🔵 ⛔ | Graphical-frontend only (TUI exempt, ARCH-75); Win32 shows attachment lines only. |
| 143 files browser (channel Files tab) | ⛔ | Attachment metadata exists (migration 0009); no files-listing view. |
| 176 third-party API / SDK | ⛔ | No public programmatic surface; wire is the custom binary protocol (ARCH-6). |
| 177 email-to-channel ingestion | ⛔ | Needs out-of-daemon inbound-mail; unbuilt. |
| 184 MFA / 2FA (local mode) | ⛔ | Local auth is password-only (ARCH-59); no second factor. |
| 192 IP allowlist / access restriction | ⛔ | Per-IP throttle exists in the accept loop; no allowlist. |
| 225 native polls | ⛔ | Not a message type today. |
| 226 code/text snippets | ⛔ | Fenced code blocks pending (REQ-220); no first-class snippet object. |
| 235 mark message/conversation unread | ⛔ | Read cursor exists (REQ-090/095); no mark-unback op. |
| 236 new-message divider + jump-to-unread | 🔵 ⛔ | Client-side over the read cursor; TUI has unread markers, no divider/jump. |
| 237 all-unreads / unreads-only views | ⛔ | Per-channel unread tracked (REQ-014); no aggregate view. |
| 238 mark-all-read / catch-up | ⛔ | No bulk cursor-advance. |
| 254 data import / migration | ⛔ | No importer; distinct from compliance export (REQ-252). |
| 260 command palette / quick switcher | 🔵 ✅ | TUI Ctrl+K (ARCH-83); **Win32 built** (WIN-11): Ctrl+K subsequence palette over a 20-action catalogue plus every conversation as "Go to", dispatching through the same menu codes the menus use. |
| 261 in-app settings/preferences hub | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-9): appearance, time format, members pane, date dividers, desktop notifications and quick reactions, applied live and synced through the `gui` settings bucket. TUI remains file-only. |
| 262 theme/appearance selection | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-26): dark / light / match-system, applied on the next frame and persisted. TUI still has no in-app toggle. |
| 263 error/toast + connection-status surface | 🔵 🟡 | TUI partial (status line). **Win32 built** (WIN-1): a transient toast stack for in-session failures + a connection banner (reason + Retry now); runtime-verified. **Win32 sign-in rebuilt** (WIN-2): a two-step in-window view reporting DNS and auth failures inline, retryable, with sign-out returning to it. The reconnect countdown now ticks (WIN-55). |
| 264 keyboard-shortcut reference | ✅ | TUI `?` overlay; **Win32 built** (WIN-25): Ctrl+/ sheet generated from one KEYMAP table so it cannot drift from the handlers. |
| 265 composer autocomplete + emoji picker | ✅ | **Win32 built** (WIN-7/8): @/#/:emoji popover plus a searchable category picker, both over a shared core catalogue (`client/core/complete.[ch]`) so the two frontends complete identically. |
| 266 other-user profile viewer | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-10): avatar, name, live presence, role, and Message. Rich fields remain REQ-240. |
| 267 sidebar org parity (DM section, sections, search) | ✅ | **Win32 built** (WIN-5/6): collapsible Channels/DMs with per-section sort+filter, scrolling, presence avatars, built from the shared `oc_model_sidebar` so TUI and GUI cannot drift. |
| 268 first-run onboarding (signup/first-owner UI) | 🔵 ✅ GUI / ⛔ TUI | **Win32 built** (WIN-32): "Have an invite? Create an account" redeems an invite on the sign-in card, creating the account and signing in together. |
| 269 keyboard-only operation + accessibility | 🟡 ⛔ | Win32 is keyboard-operable in the ordinary paths (composer, completion, Alt+Up/Down conversation movement, Ctrl+K, Ctrl+/, F6) but exposes **no accessibility surface**: it answers no `WM_GETOBJECT`, so a screen reader sees one blank window. A self-drawn UI gets nothing for free. |
| 270 GIF/sticker pickers | ➖ | Explicit exclusion (app/webhook territory). |
| 271 Canvas / collaborative docs | ➖ | Explicit exclusion. |
| 272 Lists / tables / boards | ➖ | Explicit exclusion. |
| 273 Clips / async voice-video messages | ➖ | Explicit exclusion (cf. REQ-160). |
| 274 Slack Connect / cross-org shared channels | ➖ | Explicit exclusion (island model, ARCH-4/REQ-040). |
| 275 first-party bot / MCP server | ➖ | Explicit exclusion beyond the app platform (REQ-172). |
