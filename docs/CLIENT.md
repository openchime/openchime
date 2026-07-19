# OpenChime — Client

The client's architecture and how it's built. Cross-referenced from
ARCHITECTURE.md (ARCH-11, **ARCH-74**, and the superseded ARCH-12/62/63/65) and
PROTOCOL.md (the wire flow the core drives).

**Direction (ARCH-74).** The client is **one shared C app-core with a native UI
per platform** — the *tdlib* model — chosen for real native feel over a
custom-rendered single UI. The earlier raylib + `gfx.h` + HarfBuzz plan is
dropped; there is no shared rendering layer, because each platform's OS shapes
its own text and provides its own widgets.

**Status.** Building the app-core + a **TUI** frontend first (the fastest usable,
CI-testable client). Native GUIs (Win32/WinUI, AppKit, Android, a web DOM UI)
and mobile follow.

---

## 1. Layers

```
shared/        wire contract: protocol, tls, framebuf, sock   (daemon + client)
client/core/   the app-core — frontend-agnostic, headless-testable C:
               net thread + session + (later) SQLite store + the view-model
               (channels, messages, roster, presence, unread) + reducers
client/tui/    terminal frontend over the core                (first frontend)
[client/win, client/mac, ...]   native GUIs over the same core (later)
```

The **app-core is the one shared asset.** It holds *all* logic and state; a
frontend is pure view + input — it renders the view-model and emits intents. Get
this boundary right and each new frontend is small; get it wrong and you rewrite
the client per platform. The core lives in the same repo and links the daemon's
exact `shared/` wire source, so client and server can't drift (the same reason
`tests/e2e_client.c` reuses `shared/protocol.c`).

## 2. The app-core (`client/core/`)

- **`net.c` — the network thread.** A blocking-socket TLS connection (the client
  is one connection on its own thread, so no epoll): `dial` → `oc_tls_handshake`
  → `HELLO`/`WELCOME` → `AUTH_CHALLENGE` → `AUTH` → `AUTH_OK`, then a serve loop
  that interleaves sending queued intents with reading + dispatching server
  frames. Lifted from `tests/e2e_client.c`. A process normally has one oc_client
  (one net thread), but a multi-client harness (the headless test) spins several
  concurrently; that is safe because the vendored mbedTLS is built with
  **`MBEDTLS_THREADING`** (`scripts/build_mbedtls.sh`), so its internal shared
  state is mutex-protected.
- **`queue.c` — two thread-safe queues** (mutex+condvar FIFO, the daemon's
  net↔dbwriter shape): **intents in** (send, backfill, react, edit, delete,
  typing) and **events out** (connected, auth-ok, message, channel, presence,
  reaction, edit, delete, typing, disconnected, error).
- **`event.h` — the core's wire-agnostic vocabulary:** the `oc_ev` (net→UI) and
  `oc_cmd` (UI→net) types crossing the queues.
- **`model.c` — the view-model + reducers.** `oc_model` holds the connection
  state, the channel/DM list, a per-channel message buffer (with the ARCH-45
  high-water dedup mark) carrying per-message reaction aggregates and
  edited/deleted flags, roster/presence, an ephemeral (expiring) typing table,
  and per-channel unread + read-marker counts. `oc_model_apply` folds one `oc_ev`
  into that state. A frontend owns an `oc_model`, drains events each tick, applies
  them, and renders — the "read events at frame start" shape, fed by the net
  thread. Single-threaded on the frontend, so no locking.
- **`client.c` — the facade.** `oc_client_start(host, port, cred)` /
  `oc_client_tick()` (drain + apply) / `oc_client_model()` / `oc_client_send()` /
  `oc_client_backfill()` / `oc_client_mark_read()` / `oc_client_react()` /
  `oc_client_edit()` / `oc_client_delete()` / `oc_client_typing()` /
  `oc_client_stop()`. A frontend uses only this.

**Headless-testable.** `tests/test_client_core.c` starts the daemon's netloop
in-process (like the itest) and drives real `oc_client`s against it — connect +
auth, channel-list populate, send round-trip, unread + mark-read, history
backfill, display names, reaction aggregates, edit/delete, and typing — so the
whole reducer set is CI-covered with no UI.

## 3. Frontends

Each frontend is thin: create a client, loop { `oc_client_tick`; render the
model; translate input to intents }, stop.

- **TUI (`client/tui/`, first):** a terminal cell grid — a channel sidebar (with
  unread counts; presence dots planned), a scrolling wrapped message pane, and an
  input line — built on
  **termbox2** (the cell grid + input) and **utf8proc** (Unicode width +
  grapheme breaking), both **MIT**, both vendored as committed single-file source
  (ARCH-75). The one property that matters to a chat client is **correct
  wide-character / emoji width**, so emoji reactions (REQ-070/071) and wrapped
  messages don't corrupt the layout — utf8proc's `utf8proc_charwidth` + UAX #29
  segmentation (with bundled UCD tables) gives us that without hand-rolling.
  **notcurses was rejected on licensing** — it hard-depends on libunistring
  (LGPL); termbox2 + utf8proc keep the whole client permissive. **The TUI is
  text-only and never renders graphics — no images, ever.** Built on the host
  like the daemon (`make tui`), zero transitive dependencies.

  **Interaction model — modeless (weechat/irssi/Slack), not modal.** The input
  line is always ready to type a message (Enter sends); actions are
  `/`-commands (`/join`, `/dm`, `/react`, `/thread`, `/search`, …) and
  navigation is Ctrl/Alt chords (switch buffer, scroll, quick-jump). A chat app's
  reflex is "type and hit Enter"; a modal/vim scheme taxes every message with an
  `i` first, so it's rejected. **Loop:** termbox2 on the main thread, draining
  `oc_client` events each iteration and polling input with a short timeout
  (`tb_peek_event`, the "read events at frame start" shape); a wakeup fd on the
  client queue is a later optimization if idle cost ever matters. **Layout (v2,
  the lazygit/k9s idiom):** a header bar (workspace · you · presence · connection ·
  unread), three **bordered, titled panels** — Channels │ Messages │ Members
  (the active one drawn in a bright border) — a status line, the always-ready
  composer, and a **context keybinding hint bar**; `?` (or `/help`) opens a full
  help overlay. Read-only overlays (thread, search, roster, who-reacted,
  notification prefs, webhooks) render inside the Messages panel. Everything is
  drawn cell-by-cell, re-laid-out on resize, measuring glyph width with utf8proc.
  **Navigation mode** (Esc from the composer, or Tab on an empty composer): a
  panel is focused (bright border); Tab cycles Channels / Messages / Members,
  j/k move a highlighted selection, Esc returns to the composer. On the Messages
  panel single keys act on the *selected* message — Enter/`t` thread, `r` react
  (👍; the picker comes later), `e` edit (prefills the composer), `x` delete,
  `w` who-reacted; on Members, Enter opens a DM. **Command palette** (`:` on an
  empty composer, or Ctrl-K): a fuzzy-filtered popup over every command plus
  jump-to-channel / open-DM entries — Enter runs a parameterless command, prefills
  the composer for one needing an argument (autocomplete then helps), or jumps.
  **Emoji picker:** `r` on a selected message opens a filterable emoji popup
  (type a shortcode name, Enter reacts with the real Unicode emoji, toggling).
  This completes the v2 redesign — panels + focus, navigation mode, autocomplete,
  command palette, and the picker — all frontend-only over the unchanged app-core.
  **Mouse + config:** the TUI enables mouse input (click a channel/member to
  select, wheel to scroll) and reads machine-local prefs from
  `~/.config/openchime/config` (`client/tui/config.c`, XDG, created with commented
  defaults on first run): `mouse`, `members_panel` (off/on/auto), panel widths,
  `time` (12/24h), and a default `workspace`. **Layered config:** the portable
  prefs (everything but `workspace`) also sync through the daemon's per-`(user,
  client_type)` settings bucket (a `client_settings` key/value table; wire
  `SET_CLIENT_SETTING` / `LIST_CLIENT_SETTINGS` / `CLIENT_SETTINGS`), layered
  *over* the machine-local file — a value in the daemon bucket wins, else the
  file default stands. The core exposes it as `oc_client_set_setting` /
  `oc_client_list_settings`, folding each snapshot into the model
  (`oc_model_setting`); the `tui` bucket is separate from a future `gui` one, so
  frontends never step on each other. `/set <key>
  <value>` (`mouse on|off`, `members off|on|auto`, `time 12h|24h`,
  `channels-width N`, `members-width N`, `reset <key>`) writes a key, which
  round-trips back and applies live; the daemon fans the change to your other
  logged-in TUIs so they update in place. Which server you connect to
  (`workspace`) is deliberately machine-local and never synced.
  **Storage report (REQ-214):** `/storage` opens an admin overlay showing free
  space, live attachment usage, the active retention/eviction policy, and how
  much maintenance has reclaimed by reason — with pressure and any evictions
  called out in red, since an operator should not have to read carefully to
  notice the daemon deleted files nobody approved individually. Owner/admin
  only; the daemon refuses a member's request rather than sending zeros. An
  attachment reclaimed by age or pressure returns `OC_ERR_ATTACHMENT_GONE`,
  which the core renders as "no longer available" rather than a generic
  transfer error (REQ-215).
  **Multiple workspaces (REQ-012–015):** the TUI holds **one `oc_client` per
  signed-in workspace** (`g_ws`, capped at `MAX_WS`) and ticks *all* of them every
  frame, rendering only the active one — so a workspace you aren't looking at
  keeps receiving and counting unread, and the header shows an "N elsewhere"
  badge. `^W` (or `/workspaces`) opens the **switcher**: each remembered
  workspace with its connection dot, account, and unread count, plus an
  always-present **"+ Log in to new workspace"** row; `d` forgets a closed one.
  The list comes from the store's **workspace book** (§5) unioned with the open
  sessions, so a workspace with no running client is still offered — selecting it
  reconnects silently on its stored session token, falling back to the login box
  pre-filled from the book. Each session carries its own focused channel, scroll,
  and half-typed message, restored on switch-back, and nothing crosses between
  workspaces: separate connection, credentials, model, and cached history.
  **Composer autocomplete:** as you type, a live suggestion strip offers
  context-aware completions — slash commands, `#channel` and `@user` names (from
  the model's channel list + roster), command arguments (channels for
  `/join`/`/leave`, users for `/dm`/`/role`/`/remove`), and `:emoji:` shortcodes
  from a bundled table (inserting the real Unicode emoji). **Tab** accepts the
  first candidate and cycles the rest; on an empty composer Tab still switches
  channel.
  **Build order:** the lean core loop landed first (sidebar, focus/switch, history
  backfill on open, live messages + display names, send, unread, scrollback,
  reflow, per-nick colors), then each of the following surfaced one engine feature
  already on the wire —
  - **reactions** — emoji aggregates with a `[n]` "you reacted" marker; `/react
    <emoji>` toggles on the last message (exercising the wide-char/emoji
    correctness that justified the toolkit).
  - **edit/delete** — `/edit`, `/delete`; an `(edited)` marker + `[message
    deleted]` tombstone.
  - **typing indicators** — throttled `TYPING` while composing; `✎ X is typing…`
    on the status line.
  - **threads** — `/thread` opens a message's thread in place of the channel
    (Enter then posts a reply), with a `↳ N replies` marker on the parent;
    `/close` exits.
  - **search** — `/search <query>` overlays matching messages (channel, author,
    snippet).
  - **channel management** — `/create`, `/join`, `/leave`, `/list`; non-joined
    public channels show dimmed with a `+`.
  - **roster + presence** — `/who` overlays the tenant roster with
    online/away/offline dots + roles; `/away` and `/online` set your own presence.
  - **direct messages** — `/dm <name>` opens a 1:1 DM, titled `@peer` in the
    sidebar (the daemon reports the DM peer in `CHANNEL_INFO` — a small protocol
    addition).
  - **logout** — `/logout` revokes this session server-side and quits once the
    connection drops.
  - **who-reacted** — `/reactions` overlays the full reactor list of the last
    message, each reactor paired with the emoji they used (REQ-071).
  - **notification prefs + DND** — `/prefs` overlays the DND window + per-channel
    levels; `/notify all|mentions|none` sets the focused channel; `/dnd HH:MM
    HH:MM | off` sets the do-not-disturb window (REQ-130/131; each SET returns a
    full sync that the model folds in).
  - **read receipts / seen-by (REQ-090)** — the core now sends a `CLIENT_ACK`
    whenever the focused channel's read marker advances; the daemon fans each
    member's read cursor to the others as `READ_CURSOR`, and the TUI renders a
    dim "✓ seen by …" footer under the last message naming everyone (bar you)
    who has read up to it.
  - **self-service profile (REQ-020)** — `/profile` opens a modal with your name,
    role, id, and presence; `/nick <name>` renames you (the daemon fans a
    `PROFILE_UPDATED` so every roster — and your own header — updates live);
    `/passwd <old> <new>` rotates your local password (the server verifies the old
    one, and a wrong one shows an error).
  - **admin / user management** — `/role <name> owner|admin|member`, `/invite
    [admin|member]` (mints a tenant token, shown once atop the roster), `/remove
    <name>` disables a user (REQ-030/033, owner/admin only; a `USER_UPDATED` folds
    each change into the roster).
  - **webhook management** — `/webhook` overlays the focused channel's incoming
    webhooks; `/webhook create <label>` mints one (the 32-byte token is shown
    once atop the overlay, like `/invite`); `/webhook rm <id>` deletes one
    (REQ-170; CREATE/LIST/DELETE_WEBHOOK, a WEBHOOK_DELETED drops the row).
  - **attachments** — `/upload <path>` streams a local file through the daemon
    (UPLOAD_BEGIN → CHUNKs within the advertised window → END → OK) and links it
    into a message; `/download <id> [path]` saves an attachment by id. The net
    thread runs one transfer at a time as a state machine over the frame stream;
    received messages carry attachment metadata (id, filename, mime, size),
    rendered as a `📎 name (size) #id` line — the id is what you `/download`.
    Text-only, so files are never rendered inline (REQ-140/141).

  With attachments surfaced, **every engine feature now on the wire is reachable
  from the TUI**; the remaining client work is the later native GUIs.
- **Windows (later):** Win32/WinUI (C++/WinRT or C#) over the C core.
- **macOS/iOS (later):** AppKit/UIKit (Swift) over the core.
- **Android (later):** Android views (Kotlin) over the core.
- **Web (later):** a DOM UI — WASM can't use native desktop widgets; the core
  compiles to WASM and drives a JS/TS view.

## 4. The wire layer (reused, already tested)

The core links `shared/protocol.c` (every `oc_encode_*`/`oc_decode_*` for both
directions exists), `shared/tls.c` (client TLS + TOFU), `shared/framebuf.c`
(reassembly), and `shared/sock.h` (POSIX/Winsock shim). The wire sequence is
PROTOCOL.md §3–§6 and the §10 state machine. **TOFU pinning (ARCH-10):** Phase 1
trusts the presented cert (`pin=NULL`); persisting + pinning the fingerprint
arrives with the store phase.

## 5. Local store

The core bundles SQLite (`client/core/store.c`) and reuses the daemon's
migration-runner (`oc_migrate`) with its own client migration set. **Built so
far:**
- `workspace_state` (one row per `"host:port"`) — the **session token + expiry**
  (ARCH-58) and the **per-workspace TOFU pin** (ARCH-10), so a relaunched client
  reconnects silently with the token — no password — against the pinned cert.
  **The session token prefers the OS keyring:** the core exposes an abstract
  `oc_secret` get/put/del vtable (`client/core/secret.h`, no keyring library in
  the core), and the store routes the token through it when set — so on a desktop
  the token lives in the platform secret store, not the plaintext SQLite file.
  The TUI supplies a **libsecret** backend (`client/tui/secret_backend.c`, Secret
  Service → GNOME Keyring/KWallet); when there's no keyring (headless / no D-Bus)
  the backend returns NULL and the token falls back to the SQLite column. Only the
  token goes to the keyring — the (public) pin + cache stay in SQLite. Windows'
  Credential Manager / macOS Keychain slot behind the same vtable later.
- `cached_message` — **cached history** per channel (ARCH-45/46). Every BROADCAST
  is written through as it arrives (edits/deletes update the row); on startup the
  net thread replays the cache into the model *before connecting*, so history
  shows instantly (proven offline: a headless test and a PTY smoke both load it
  with the daemon down). The replay also seeds the net thread's per-channel
  high-water, so the first `BACKFILL_REQUEST` resumes from the last cached id
  instead of refetching from 0, and replayed history is marked read (not stale
  "unread"). Reactions/attachments on cached messages aren't cached yet — they
  re-appear only for messages the backfill re-sends.

- `outbox` — the **offline outbox** (REQ-102). Every send is recorded here (with
  its idempotency token) before it goes out, removed on the `SEND_ACK`, and
  resent on reconnect — so a message composed while disconnected (or in flight
  when the connection dropped, or still queued when the app closed) survives, and
  the daemon's idempotency dedups any partial delivery. Proven end-to-end: a
  headless test and a PTY smoke both compose a message with the daemon down, and
  a later run flushes it.

- `workspace_book` — the **workspace book** (REQ-012), one row per workspace this
  device has signed into: the address the user typed (friendlier than the
  resolved `"host:port"` key), the account used, and a last-used stamp for
  most-recently-used ordering. It backs the switcher, so returning to a workspace
  never means retyping its address. `oc_store_workspace_forget` removes the row
  *and* that workspace's session token, TOFU pin, cached history, and outbox — so
  "forget" leaves nothing of it on disk.

The store is owned by the net thread (one connection, one thread); an unusable
path just disables persistence (in-memory only). The TUI puts it at
`$OPENCHIME_STATE` or `$HOME/.local/state/openchime/state.db`. The three
`workspace_book` calls are the one exception to net-thread ownership: they are
safe from a second `oc_store` handle on the same file (WAL + busy timeout, and
they run at login/logout rather than on the message path), which is how the
switcher lists workspaces that have no running client.

Note migrations are forward-only, so the historical `instance_*` spellings
persist in migrations 1–3; migration 4 renames them to `workspace_*`, and a
fresh store lands on the same schema an upgraded one does.

With the outbox, **the store + reconnect/offline work (REQ-100/101/102) is
complete.** Keychains for the token are noted future hardening; optimistic local
echo of an offline-composed message (showing it before the reconnect delivers it)
is a later UI refinement.

## 6. Auth + reconnect/offline

**In-session auto-reconnect is built (REQ-100/101).** The net thread runs one
connection after another in a loop: it captures the `session_token` from
`AUTH_OK`, and on an unexpected drop it silently re-authenticates with that token
(`OC_AUTH_SESSION` — no password) under exponential backoff, then
`BACKFILL_REQUEST`s each known channel from its last-seen message id to recover
anything missed while offline. The in-memory model is preserved across the blip
(replays dedup on the high-water mark), so a reconnect is invisible beyond a
brief status line. A graceful `/logout` or a fatal reject (bad version, expired
session) ends the loop instead of retrying. The net thread tracks its own
per-channel high-water for the reconnect cursors; a headless test bounces the
daemon and asserts the client re-auths, keeps its history, and can send again.
While backing off, the status line counts the wait down (`connection lost —
reconnecting in Ns… (^R to retry now)`), and `oc_client_reconnect` (bound to
`^R` in the TUI) cuts the current sleep short to retry immediately.

**Cross-restart reconnect is built too (via the §5 store).** The net thread
pre-loads a still-valid stored token and pins the stored fingerprint, so the
*first* connect after a relaunch already uses `OC_AUTH_SESSION` — no password
prompt. A rejected token (expired/revoked) is dropped and, if a password is
still held, retried once with it; `/logout` clears the stored token.

**The offline outbox (REQ-102) is built** (see §5): the net thread records each
send in the store before delivery, resends the outbox on reconnect, and clears a
row on its `SEND_ACK` — so an offline-composed send goes out on the next
connection, deduped by the daemon.

**Workspace resolution is built (REQ-010/011,** `client/core/resolve.c`**).** A
user-typed workspace — a full domain (`chat.acme.com`) or a bare
name (`acme`, which gets the configured `$OPENCHIME_SUFFIX`
appended) — resolves by plain DNS: SRV (`_openchime._tcp.<domain>`) first, then
the domain's A record at 443. A resolution failure is a distinct status, so the
TUI tells "workspace not found" apart from "could not reach the server" (connect)
and "auth failed" (login). The TUI accepts `<workspace>` (resolved) or a raw
`<host> <port>` (dev/local); an explicit `:port` on the workspace
(`chat.acme.com:9000`) pins the port and skips SRV. The optional `.well-known`
metadata half is not consulted yet.

**The local login box is built (REQ-020 local mode).** With no credential and no
stored session token, the TUI shows a modal **Sign in** dialog — workspace /
username / masked password / *Remember me* — that resolves the workspace on submit
(inline "not found"), then connects; an auth failure keeps the box up with the
reason and refocuses the password to retry. *Remember me* gates whether the
session token persists (the store) or stays session-only. A returning user with a
stored token skips the box entirely (silent reconnect). Cross-cutting: the model
gained a sticky `last_error` (not overwritten by "disconnected") so the flow can
tell auth-failure from unreachable.

**Still to build:** **OIDC** via the system browser to central's authorize URL
with a loopback `127.0.0.1` redirect catching the ES256 JWT
(`ASWebAuthenticationSession` on iOS/macOS) — its client half only, since the
central relay is out of repo.

## 7. Build

The core + TUI build on the host (a `make tui` target linking `client/core/*.c` +
`client/tui/*.c` + `shared/*` + `third_party/utf8proc/utf8proc.c`, with
`termbox2.h` compiled `TB_IMPL` in one TU), like the daemon — no cross-compile,
no container, no external build system. termbox2 and utf8proc are **vendored as
committed single-file source** in `third_party/` (like jsmn, pinned to termbox2
v2.5.0 / utf8proc v2.11.3), both MIT, so local and CI share identical sources with
zero transitive dependencies (ARCH-75). Native GUIs build with their platform
toolchains over the core; release artifacts come from CI/CD, never a dev machine.

## 8. Roadmap

- **Now:** app-core + termbox2 TUI shipped. On top of the lean core loop
  (sidebar, backfill on open, send, display names, unread, scrollback), the TUI
  surfaces: reactions, edit/delete, typing, threads, search, channel management,
  roster + presence, DMs, logout, who-reacted, notification prefs/DND, admin,
  webhook management, and attachments (see §3 for the commands). **Every engine
  feature on the wire is now reachable from the TUI.**
- **Next:** store + reconnect/offline; auth completeness (local + OIDC); and the
  **audio client** (Opus encode/decode + UDP to the sidecar — the deferred half
  of REQ-150/151).
- **Then:** native desktop GUIs (Windows/macOS), a web DOM UI, and mobile.
