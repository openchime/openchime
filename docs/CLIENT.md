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
CI-testable client). Native GUIs (Win32, GTK, AppKit, Android, a web DOM UI)
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
  unread counts and presence dots), a scrolling wrapped message pane, and an
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
  like the daemon (`make tui`), zero transitive dependencies. The widget/
  formatting layer above termbox2 is the in-tree **`tuikit`** toolbox (ARCH-83,
  [TUIKIT.md](./TUIKIT.md)): panels, modal lists, a command palette, text
  prompts, and a 256-color theme. The menu/screen-driven redesign built on it
  has shipped and replaced the slash-command UX.

  **Interaction model — modeless and menu-driven, not slash-command-driven.**
  The input line is always ready to type a message (Enter sends only); every
  action is reached through discoverable UI — pane action menus (Enter on a
  selected message/member/channel), modal prompt dialogs, and a global action
  launcher (Ctrl+K or `:`). Navigation is Esc/Tab between the composer and the
  panels plus a few Ctrl chords (Ctrl+F search, Ctrl+W workspaces, Ctrl+R
  reconnect, Ctrl+Q quit). There are no slash commands. A chat app's
  reflex is "type and hit Enter"; a modal/vim scheme taxes every message with an
  `i` first, so it's rejected. **Loop:** termbox2 on the main thread, draining
  `oc_client` events each iteration and polling input with a short timeout
  (`tb_peek_event`, the "read events at frame start" shape); a wakeup fd on the
  client queue is a later optimization if idle cost ever matters. **Layout (v2,
  the lazygit/k9s idiom):** a header bar (workspace · you · presence · connection ·
  unread), three **bordered, titled panels** — Channels │ Messages │ Members
  (the active one drawn in a bright border) — a status line, the always-ready
  composer, and a **context keybinding hint bar**; `?` opens a full
  help overlay. Read-only overlays (thread, search, roster, who-reacted,
  notification prefs, webhooks) render inside the Messages panel. Everything is
  drawn cell-by-cell, re-laid-out on resize, measuring glyph width with utf8proc.
  **Navigation mode** (Esc from the composer, or Tab on an empty composer): a
  panel is focused (bright border); Tab cycles Channels / Messages / Members,
  j/k move a highlighted selection, Esc returns to the composer. On the Messages
  panel, Enter opens an action menu (Reply in thread / Add reaction / Download /
  Edit / Delete / Who reacted); single-key accelerators act directly: `t`
  thread, `r` react (opens a filterable emoji picker), `e` edit (prefills the
  composer), `x` (or `d`) delete, `w` who-reacted. On Members, Enter opens a
  member menu (Message / Make admin / Make member / Remove); `n` starts a new
  DM. **Action launcher** (`:` on an empty composer, or Ctrl+K): a
  fuzzy-filtered, sectioned list of every action — New channel, Leave,
  Notifications, Webhooks, Upload, Search, New DM, Set away/online, Change
  display name/password, DND, Profile, Invite, Storage usage, Audit log, Switch
  workspace, Help, Log out. Enter runs a parameterless action immediately, or
  opens a modal prompt dialog for one needing an argument.
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
  frontends never step on each other. Synced prefs are read-only from the TUI: it
  pulls the daemon bucket on connect and layers it over the machine-local
  `~/.config/openchime/config` file, but the interactive `/set` writer was
  removed with the slash-command UX. Edit the config file to change local prefs.
  (The core still exposes `oc_client_set_setting`; no frontend surface currently
  calls it.) Which server you connect to
  (`workspace`) is deliberately machine-local and never synced.
  **Storage report (REQ-214):** the launcher's "Storage usage" opens an admin overlay showing free
  space, live attachment usage, the active retention/eviction policy, and how
  much maintenance has reclaimed by reason — with pressure and any evictions
  called out in red, since an operator should not have to read carefully to
  notice the daemon deleted files nobody approved individually. Owner/admin
  only; the daemon refuses a member's request rather than sending zeros. An
  attachment reclaimed by age or pressure returns `OC_ERR_ATTACHMENT_GONE`,
  which the core renders as "no longer available" rather than a generic
  transfer error (REQ-215). The message list shows the same thing **in place**:
  the attachment metadata carries a `reclaimed` flag, so a tombstoned file
  renders dimmed as `📎 name — no longer available` with no download id, instead
  of offering a fetch that is guaranteed to fail.
  **Audit log (REQ-251):** the launcher's "Audit log" opens an admin overlay of
  administrative/security actions, newest first, with denials/failures in red;
  owner/admin only.
  **Multiple workspaces (REQ-012–015):** the TUI holds **one `oc_client` per
  signed-in workspace** (`g_ws`, capped at `MAX_WS`) and ticks *all* of them every
  frame, rendering only the active one — so a workspace you aren't looking at
  keeps receiving and counting unread, and the header shows an "N elsewhere"
  badge. `Ctrl+W` (or launcher "Switch workspace") opens the **switcher**: each remembered
  workspace with its connection dot, account, and unread count, plus an
  always-present **"+ Log in to new workspace"** row; `d` forgets a closed one.
  The list comes from the store's **workspace book** (§5) unioned with the open
  sessions, so a workspace with no running client is still offered — selecting it
  reconnects silently on its stored session token, falling back to the login box
  pre-filled from the book. Each session carries its own focused channel, scroll,
  and half-typed message, restored on switch-back, and nothing crosses between
  workspaces: separate connection, credentials, model, and cached history.
  **Composer autocomplete:** as you type, a live suggestion strip offers
  context-aware completions — `#channel` names, `@user` names, and `:emoji:`
  shortcodes (inserting the real Unicode emoji). Tab accepts the first and
  cycles; on an empty composer Tab enters navigation mode.
  **Build order:** the lean core loop landed first (sidebar, focus/switch, history
  backfill on open, live messages + display names, send, unread, scrollback,
  reflow, per-nick colors), then each of the following surfaced one engine feature
  already on the wire —
  - **reactions** — emoji aggregates with a `[n]` "you reacted" marker; `r` on a
    selected message (the emoji picker) or the message menu's "Add reaction"
    toggles a reaction (exercising the wide-char/emoji correctness that justified
    the toolkit).
  - **edit/delete** — the message menu (or `e`/`x` on a selected message); an
    `(edited)` marker + `[message deleted]` tombstone.
  - **typing indicators** — throttled `TYPING` while composing; `✎ X is typing…`
    on the status line.
  - **threads** — the message menu's "Reply in thread" (or `t`) opens a message's
    thread in place of the channel (Enter then posts a reply), with a `↳ N
    replies` marker on the parent; Esc exits.
  - **search** — Ctrl+F (or the launcher's "Search messages") overlays matching
    messages (channel, author, snippet).
  - **channel management** — `n` in the Channels pane creates a channel; the
    channel menu's Join/Open/Leave manage membership; non-joined public channels
    show dimmed with a `+`.
  - **roster + presence** — the Members panel lists the tenant roster with
    online/away/offline dots + roles; the launcher's "Set away/online" sets your
    own presence.
  - **direct messages** — `n` in the Members pane (or a member menu's "Message")
    opens a 1:1 DM, titled `@peer` in the sidebar (the daemon reports the DM peer
    in `CHANNEL_INFO` — a small protocol addition).
  - **logout** — the launcher's "Log out" revokes this session server-side and
    quits once the connection drops.
  - **who-reacted** — `w` on a selected message (or the message menu's "Who
    reacted") overlays the full reactor list, each reactor paired with the emoji
    they used (REQ-071).
  - **notification prefs + DND** — the launcher's "Notifications" overlays the DND
    window + per-channel levels; the channel menu's "Notify: all|mentions|none"
    sets the focused channel; the launcher's "Do not disturb" sets the
    do-not-disturb window (REQ-130/131; each SET returns a full sync that the
    model folds in).
  - **read receipts / seen-by (REQ-090)** — the core now sends a `CLIENT_ACK`
    whenever the focused channel's read marker advances; the daemon fans each
    member's read cursor to the others as `READ_CURSOR`, and the TUI renders a
    dim "✓ seen by …" footer under the last message naming everyone (bar you)
    who has read up to it.
  - **self-service profile (REQ-020)** — the launcher's "Your profile" opens a
    modal with your name, role, id, and presence; "Change display name" renames
    you (the daemon fans a `PROFILE_UPDATED` so every roster — and your own header
    — updates live); "Change password" rotates your local password (the server
    verifies the old one, and a wrong one shows an error).
  - **admin / user management** — a member menu's "Make admin/Make member/Remove"
    (or the launcher's "Invite a user", which mints a tenant token shown once atop
    the roster) manage users (REQ-030/033, owner/admin only; a `USER_UPDATED`
    folds each change into the roster).
  - **webhook management** — the channel menu's "Webhooks" overlays the focused
    channel's incoming webhooks; "Create webhook" mints one (the 32-byte token is
    shown once atop the overlay, like an invite) — REQ-170, CREATE/LIST_WEBHOOK.
    **Deleting a webhook is not surfaced**: the core exposes
    `oc_client_delete_webhook` and the daemon handles `DELETE_WEBHOOK`, but the
    overlay is read-only and its header still shows a stale `/webhook` slash-command
    hint left over from the removed slash UX (both are known bugs —
    [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §3.6).
  - **attachments** — the launcher's "Upload a file" streams a local file through
    the daemon (UPLOAD_BEGIN → CHUNKs within the advertised window → END → OK) and
    links it into a message; the message menu's "Download file" saves an
    attachment by id. The net thread runs one transfer at a time as a state
    machine over the frame stream; received messages carry attachment metadata
    (id, filename, mime, size), rendered as a `📎 name (size) #id` line.
    Text-only, so files are never rendered inline (REQ-140/141).

  With attachments surfaced, **nearly every capability the app-core exposes is
  reachable from the TUI** — the two `oc_client_*` calls with no TUI surface are
  `delete_webhook` (above) and `logout(OC_LOGOUT_ALL)` (the TUI only revokes its
  own session). `oc_client_set_setting` also has no caller in any frontend.

  Separately, several frames the **daemon** speaks reach **no client** yet:
  `CALL_JOIN`/`CALL_LEAVE` (the audio client, REQ-150–152),
  `REGISTER_DEVICE_TOKEN`/`UNREGISTER_DEVICE_TOKEN` (exercised only by
  `tests/demo_client.c`, so no shipped client can populate the push registry —
  ARCH-85), `INVITE_TO_CHANNEL`, `REMOVE_FROM_CHANNEL`, `REDEEM_INVITE` (the
  missing signup/first-owner flow, REQ-268), and `TRANSFER_CANCEL`. None is
  implemented in `client/core`, so this is core work, not frontend work.
- **Windows (next):** **Win32 in pure C** over the core — **Direct2D/DirectWrite
  (+ WIC)** for the custom surfaces (message transcript, sidebar, rails) and
  **native controls for the hard bits** (RichEdit composer, EDIT search, Win32
  menus, dialogs). No WinUI, no .NET, no C++, no cross-platform toolkit
  (ARCH-80/82). The **Windows TUI** already shipped and validated the core port —
  pthreads, DNS SRV, sockets — via a termbox2-API layer over the Windows Console
  API (ARCH-81, done), so the GUI proceeds against a trusted core. Two earlier
  GUI drafts were built and rejected (ARCH-82): a comctl32 GUI (too dated) and a
  self-rendered Clay+raylib GUI (non-native, perpetually lags). **The GUI is
  affordance-driven — buttons/menus/dialogs/drag-drop, never slash commands**
  (those belong to the TUI).
- **Linux GUI (later):** **GTK in pure C** — GTK is the native Linux toolkit and
  a C library (ARCH-80). Distributed as an AppImage/Flatpak, not a static binary
  (GTK cannot cleanly static-link).
- **macOS/iOS (later):** AppKit/UIKit over the core. **Objective-C, not pure C** —
  there is no C-native GUI on modern macOS (ARCH-80); Obj-C is a C superset, so
  the UI shell calls the core with no FFI.
- **Android (later):** Android views (Kotlin) over the core.
- **Web (later):** a DOM UI — WASM can't use native desktop widgets; the core
  compiles to WASM and drives a JS/TS view.

## 4. The wire layer (reused, already tested)

The core links `shared/protocol.c` (every `oc_encode_*`/`oc_decode_*` for both
directions exists), `shared/tls.c` (client TLS + TOFU), `shared/framebuf.c`
(reassembly), and `shared/sock.h` (POSIX/Winsock shim). The wire sequence is
PROTOCOL.md §3–§6 and the §10 state machine. **TOFU pinning (ARCH-10) is built:**
the first connect to a remote workspace records the cert's SHA-256 in the client
store (`workspace_state.tls_pin`, §5) and every later connect enforces an exact
match; a genuine change is reported distinctly ("the server's security certificate
has changed") rather than as an unreachable host. **Loopback is deliberately not
pinned** — a `127.0.0.1` connection has no MITM vector, and pinning it only fires
false alarms as local dev daemons re-self-sign (`client/core/net.c:is_loopback`,
[TLS.md](./TLS.md)).

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
brief status line. A graceful logout (the launcher's "Log out") or a fatal
reject (bad version, expired session) ends the loop instead of retrying. The net thread tracks its own
per-channel high-water for the reconnect cursors; a headless test bounces the
daemon and asserts the client re-auths, keeps its history, and can send again.
While backing off, the status line counts the wait down (`connection lost —
reconnecting in Ns… (^R to retry now)`), and `oc_client_reconnect` (bound to
`Ctrl+R` in the TUI) cuts the current sleep short to retry immediately.

**Cross-restart reconnect is built too (via the §5 store).** The net thread
pre-loads a still-valid stored token and pins the stored fingerprint, so the
*first* connect after a relaunch already uses `OC_AUTH_SESSION` — no password
prompt. A rejected token (expired/revoked) is dropped and, if a password is
still held, retried once with it; logging out clears the stored token.

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
  webhook management, attachments, and audit log (see §3 for where each lives).
  **Nearly every capability the app-core exposes is reachable from the TUI** —
  §3 lists the two that are not (webhook delete, log-out-everywhere) and the
  daemon frames no client reaches yet.
- **Also shipped since:** the **local store** and reconnect/offline (REQ-100/101/102
  — silent session-token reconnect across restarts, persisted TOFU pin, cached
  history, offline outbox, workspace book), **multiple workspaces** (REQ-012–015),
  DNS workspace resolution (REQ-010/011), the local **Sign in** dialog, and the
  **Windows TUI** (ARCH-81). See §5–§6.
- **Next — Windows GUI depth (P0).** The Win32 GUI surfaces all 27 engine
  features (ARCH-82) but many surfaces are thin; the prioritized gaps are
  [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §5, now each carrying a REQ:
  the global error/toast + connection-status surface (**REQ-263**), search that
  navigates to the matched message (REQ-080), the sidebar overhaul — DM section +
  grouping + search (**REQ-267**), composer autocomplete + emoji picker
  (**REQ-265**), the in-app settings/preferences hub (**REQ-261**), the
  other-user profile viewer (**REQ-266**), a command palette (**REQ-260**), and
  the **N-concurrent-workspace model** (REQ-012–015 — the rail switcher UI is
  already built; what's missing is holding N clients at once with background
  unread, as the TUI does). Inline image rendering (**REQ-142**) is a D2D-native
  win once those land.
- **Next — auth completeness.** The **OIDC browser flow + PKCE + loopback
  courier** — the one remaining piece of REQ-020 and what makes SSO usable at all
  (there is no SAML by design, **REQ-027**); plus first-run onboarding
  (**REQ-268**).
- **Next — audio client.** Opus encode/decode + UDP to the sidecar — the deferred
  half of REQ-150/151 ([AUDIO.md](./AUDIO.md)).
- **Then — broader competitor parity (non-video).** The rest of the parity
  backlog now specced in REQUIREMENTS.md §§1–16 and tracked in
  [STATUS.md](./STATUS.md): message actions (forward **REQ-057**, mark-unread
  **REQ-235**, unread nav **REQ-236**, mute **REQ-137**), notification depth
  (global level **REQ-134**, keywords **REQ-135**, OS toast **REQ-138**, activity
  feed **REQ-139**), channel management (rename **REQ-036**, browse/join
  **REQ-038**, files browser **REQ-143**), workspace admin (**REQ-042/043**),
  themes (**REQ-262**), polls (**REQ-225**), and the rest. Priorities are
  CLIENT_GAP_ANALYSIS.md §5.
- **Then — remaining platforms + screenshare.** The other native GUIs (GTK,
  AppKit), a web DOM UI, and mobile — and, gated behind the audio client,
  **screenshare** (REQ-161, [VIDEO.md](./VIDEO.md)).
