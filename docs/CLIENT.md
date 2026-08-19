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
               net thread + session + credential store + the view-model
               (channels, messages, roster, presence, unread) + reducers
client/tui/    terminal frontend over the core                (first frontend)
client/gui/win32/  the native Win32 GUI over the same core    (in progress)
client/shared/ assets shared by graphical frontends (baked Lucide icon paths)
[client/gui/gtk, client/gui/mac, ...]  further native GUIs    (later)
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
- **`client.c` — the facade.** The lifecycle is `oc_client_start` /
  `start_secure` / `start_stored`, then `oc_client_tick()` (drain + apply),
  `oc_client_model()` and `oc_client_stop()`; the messaging core is
  `send` / `backfill` / `history` / `history_around` / `mark_read` / `react` /
  `edit` / `delete` / `typing`. **`client/core/client.h` is the authoritative
  list** — it declares over a hundred entry points, covering threads, pins, saved
  items, activity, search with operators and paging, channel and member
  management, files, drafts, scheduling, custom emoji, profile and status,
  notification settings, admin and storage. This document does not enumerate
  them; read the header. A frontend uses only this facade.

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
  the arrow keys move a highlighted selection (`j`/`k` were dropped with the
  tuikit redesign), Esc returns to the composer. On the Messages
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
  select, wheel to scroll) and reads machine-local prefs from a
  hand-edited config file (`client/tui/config.c`, created with commented defaults
  on first run): `$XDG_CONFIG_HOME/openchime/config` — else
  `~/.config/openchime/config` — on Linux/macOS, and
  `%LOCALAPPDATA%\openchime\config` on Windows, so each platform uses its own
  convention and the two TUI builds share one code path: `mouse`, `members_panel` (off/on/auto), panel widths,
  `time` (12/24h), and a default `workspace`. **Layered config:** the portable
  prefs (everything but `workspace`) also sync through the daemon's per-`(user,
  client_type)` settings bucket (a `client_settings` key/value table; wire
  `SET_CLIENT_SETTING` / `LIST_CLIENT_SETTINGS` / `CLIENT_SETTINGS`), layered
  *over* the machine-local file — a value in the daemon bucket wins, else the
  file default stands. The core exposes it as `oc_client_set_setting` /
  `oc_client_list_settings`, folding each snapshot into the model
  (`oc_model_setting`); the `tui` bucket is separate from the `gui` one the
  Win32 client identifies as, so
  frontends never step on each other. The TUI pulls the daemon bucket on connect
  and layers it over the machine-local `~/.config/openchime/config` file. Most
  display prefs are read-only there — the interactive `/set` writer went with the
  slash-command UX, so editing the file is how they change — but the TUI **does**
  write one thing back: its sidebar sort, filter and collapse state, persisted
  through `oc_client_set_setting`. Which server you connect to
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
    hint left over from the removed slash UX (both are known bugs, and
    tracked).
  - **attachments** — the launcher's "Upload a file" streams a local file through
    the daemon (UPLOAD_BEGIN → CHUNKs within the advertised window → END → OK) and
    links it into a message; the message menu's "Download file" saves an
    attachment by id. The net thread runs one transfer at a time as a state
    machine over the frame stream; received messages carry attachment metadata
    (id, filename, mime, size), rendered as a `📎 name (size) #id` line.
    Text-only, so files are never rendered inline (REQ-140/141).

  **The TUI reaches a minority of the app-core today.** It was the reference
  client until the July 2026 engine and GUI work; since then the core has roughly
  doubled and the TUI has not followed, so **most `oc_client_*` entry points have
  no TUI caller** — pins, saved items, the activity feed, the channel member and
  file listings, mute, the notification default, group DMs, custom emoji, status
  and profile fields, drafts, scheduling, threads-follow, the People directory,
  search operators and paging, and webhook delete among them. The frontend order
  (all of Win32 first) makes that an accepted consequence rather than a defect;
  the itemised list is the [issue tracker](https://github.com/openchime/openchime/issues). `oc_client_set_setting`, once
  recorded here as callerless, **is called by both frontends** — the TUI persists
  its sidebar sort/filter/collapse through it and the Win32 client its
  preferences.

  Separately, three frames the **daemon** speaks reach **no client**:
  `CALL_JOIN`/`CALL_LEAVE` (the audio client, REQ-150–152),
  `REGISTER_DEVICE_TOKEN`/`UNREGISTER_DEVICE_TOKEN` (exercised only by
  `tests/demo_client.c`, so no shipped client can populate the push registry —
  ARCH-85), and `TRANSFER_CANCEL`. None is implemented in `client/core`, so those
  are core work rather than frontend work. `INVITE_TO_CHANNEL`,
  `REMOVE_FROM_CHANNEL` and `REDEEM_INVITE` **are** implemented
  (`oc_client_channel_invite` / `_channel_kick` / `_redeem_invite`) and the Win32
  client surfaces all three.
- **Windows (`client/gui/win32/`):** **Win32 in pure C** over the
  core — **Direct2D/DirectWrite
  (+ WIC)** for the custom surfaces (message transcript, sidebar, rails,
  **the composer**, and every menu) and **native controls where they still earn
  their keep**: `EDIT` boxes (find, search, files-search, sign-in, palette, the
  form fields), the file picker, and the window frame. No WinUI, no .NET, no C++,
  no cross-platform toolkit (ARCH-80/82). The composer and the context menus were
  native once and are not any more — ARCH-98 took them back, so **`RichEdit`,
  `TrackPopupMenu`, `CreatePopupMenu` and `AppendMenuW` appear nowhere in the
  client**. The **Windows TUI** already shipped and validated the core port —
  pthreads, DNS SRV, sockets — via a termbox2-API layer over the Windows Console
  API (ARCH-81, done), so the GUI proceeds against a trusted core. Two earlier
  GUI drafts were built and rejected (ARCH-82): a comctl32 GUI (too dated) and a
  self-rendered Clay+raylib GUI (non-native, perpetually lags). **The GUI is
  affordance-driven — buttons/menus/dialogs/drag-drop, never slash commands**
  (those belong to the TUI).

  **Shell layout.** A **global left-nav rail** (Lucide icons stroked as D2D path
  geometry) holds the workspace avatar → switcher, six primary views (Home, DMs,
  Activity, Files, Later, Admin — the last owner/admin-gated, with a "More"
  overflow flyout), and a bottom cluster of New (+) / profile avatar.
  Three custom D2D dropdowns replace the old native app menu — **workspace**
  (invite, preferences, storage/audit, reconnect, sign out / sign out
  everywhere), **profile** (presence, DND, display name, password), and **New**
  (channel, DM, upload, search). Right of the rail sits the channel column
  (header with settings + compose buttons, a "Find a conversation" filter, then
  the channel list), the transcript, the self-drawn composer, and an optional
  members pane. **Home, DMs and Activity** render that chat shell
  (`shell_visible()`), and Home and DMs are not the same: the second column holds
  channels in one and conversations in the other (`sidebar_kind()`), and DMs shows
  an index until a conversation is picked. **Activity, Files, Later, Drafts,
  Threads, People, Admin and Preferences are all developed surfaces** — none is a
  placeholder — answering REQ-139, REQ-143, REQ-231, REQ-223/224/228, REQ-062,
  REQ-289 and REQ-261. Per-feature status is the marker on each
  requirement in [REQUIREMENTS.md](./REQUIREMENTS.md); open work is in the
  [issue tracker](https://github.com/openchime/openchime/issues).
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
the first connect to a remote workspace records the cert's SHA-256 in that
workspace's OS-credential blob (`oc_store_save_pin`, §5) and every later connect
enforces an exact match; a genuine change is reported distinctly ("the server's security certificate
has changed") rather than as an unreachable host. **Loopback is deliberately not
pinned** — a `127.0.0.1` connection has no MITM vector, and pinning it only fires
false alarms as local dev daemons re-self-sign (`client/core/net.c:is_loopback`,
[TLS.md](./TLS.md)).

## 5. Local store

The core **stores nothing locally beyond credentials** (ARCH-88/REQ-201). There is
no database and no file: `client/core/store.c` is a thin front for the OS
credential store, holding one entry per workspace with the session token, the
TOFU pin, and the book fields (typed address, account, last-used). Because there
is one credential per workspace, **enumerating the credential store is the
workspace book** (`oc_secret_each`), and "forget" is a single delete. Cached
history is gone and the offline outbox lives in memory on the net thread. With no
OS credential store, nothing persists at all.

  **There is no plaintext fallback.** Where no store exists — headless, no D-Bus,
  a locked keychain — `oc_store_save_session`/`load_session` simply do nothing, so
  that machine keeps no session and the user signs in again next launch. Opening a
  store also **erases any `state.db` an older build left behind**, so upgrading
  costs one re-sign-in and one re-TOFU rather than leaving a plaintext credential
  on disk. macOS Keychain slots behind the same vtable.

**One credential per workspace holds three things**, in a flat versioned blob
(`SEC_VER`) rather than a schema — there is no database, so there are no tables
and no migrations:

- the **session token**, which is what makes silent reconnect across a restart
  work (REQ-100);
- the **TOFU pin** (REQ-183), which is not secret but is integrity-sensitive:
  rewriting a pin is how a man-in-the-middle is accepted, so it lives where the
  token does rather than in a file anyone can edit;
- the **workspace book** fields (REQ-012) — the address the user typed, the
  account, and a last-used stamp. Because there is one credential per workspace,
  **enumerating the credential store is the book** (`oc_secret_each`), and
  "forget" is a single delete that leaves nothing behind.

**Cached history does not exist**, and the **offline outbox is in memory** on the
net thread for the life of the process (ARCH-88). Every send is recorded there
with its idempotency token before it goes out and cleared on its `SEND_ACK`, so a
message composed while disconnected, or in flight when the connection dropped, is
resent on reconnect and deduped by the daemon — but **a message still queued when
the process exits is lost**, which is why a frontend warns on quit while the
outbox is non-empty.

The store is owned by the net thread. An unusable store just disables persistence
and the client runs in memory. The TUI resolves `$OPENCHIME_STATE`, else
`$HOME/.local/state/openchime/state`, purely as an on/off flag — nothing is
written at that path, and resolving it deletes a pre-ARCH-88 `state.db` if one is
there.

**Reconnect and offline (REQ-100/101/102) are complete**, at the stated cost:
no offline reading of history, and a queued message dies with the process.

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
reconnecting in Ns…`; the TUI appends its own `(Ctrl+R to retry now)`, since a
keybinding belongs to a frontend and not the shared core), and
`oc_client_reconnect` (bound to
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

## Native children over Direct2D (Win32)

A native child window — the find/search/files-search/palette boxes, the sign-in
fields, the form fields — composites **above** the Direct2D output. There is no
z-order to lose and nothing can be drawn over one. (The composer was one of these
until the composer was rewritten; it is part of the scene now, which is what removed a whole class of
this bug rather than managing it.) A child left visible while the
surface it belongs to is not drawn therefore appears as a bare control floating
over whatever *is* drawn, which reads as corruption rather than as a bug.

This recurred four times before it was fixed as a class, so the rule is written
down: **every native child's visibility is decided in `layout_natives()`**, from
three shared predicates —

- `sidebar_kind()` — what the second column currently holds (channels · DMs ·
  activity). Anything that depends on the column's *content* asks this, not
  `view_has_sidebar()`, which only says whether a column exists. Conflating the
  two is what let the find box leak into three separate views.
- `main_is_conversation()` — whether the middle column is something you can type
  into. The DMs view has a sidebar but shows an index until you pick someone.
- `window_is_covered()` — whether a modal, the palette, the lightbox, a menu or
  the sign-in card owns the whole window.

**And the drawing must ask the same question as the control.** Hiding the
composer while still painting its box and buttons produced an input you could not
type into, which is its own defect — the rule outlived the RichEdit that taught
it, and `main_is_conversation()` now decides both the field's rect and its focus.

A new view or overlay has to name itself in one of those predicates. It cannot
silently inherit another surface's children.

**Two traps this fix walked into itself, both worth keeping in mind.**

*A predicate whose default is a real answer.* `sidebar_kind()` first returned
`CHANNELS` as its fallback, which handed that answer to every view its author had
not thought about — so the find box appeared over the Files view's filter chips.
It now returns `NONE` for a view with no second column, and `NONE` is the enum's
zero.

*The harness could not see the bug.* `gui_drive.sh shot` used to render Direct2D
only, so a stray native child was **invisible to it** — which is why this class
kept reaching the user rather than being caught in verification. **That is fixed:
`shot` now composites the children on top of the scene** (see "Seeing the whole
window" below), so a screenshot shows what the user sees. The dump reports each
child's `IsWindowVisible` alongside the three predicates as well, because a
boolean is a better assertion than an image when what you want to know is
*whether* a control is shown. Read `re=` with the self-drawn composer in mind: the composer has no
child window any more, so that field reports whether its **rect is non-empty**,
not a child's visibility. Every other flag is still an `IsWindowVisible`:

```
natives re=1 find=1 ffind=0 srch=0 pick=0 pal=0 si_ws=0 sbkind=1 conv=1 covered=0
```

The rule earned its keep immediately: the Files view's own "Search files" box
(`g_ffind`) was added next, gated on `g_view == VIEW_FILES` rather than on any
predicate that merely happened to be true there, and checked across all six
views from the dump before it was shown to anybody.

**And the same principle applies to painted chrome.** Hiding the RichEdit under
the Files/Pins/About tabs still left the composer's box, buttons and send arrow
DRAWN there — an input you cannot type into, which is the thing hiding the child
was meant to prevent. `main_is_conversation()` now decides both.

## Check CI after pushing

Both workflows run on every push. `ci` includes a black-box end-to-end run against
the containerised daemon, and it is the only thing that proves the wire actually
works end to end — the unit suite links the shared modules directly and never
performs a handshake.

It went red for **eleven consecutive pushes** on 2026-07-29 because
`tests/e2e_client.c` sent a hard-coded HELLO range of `{1, 1}` while
`OC_PROTOCOL_VERSION` had been bumped to 2, and nobody looked. `make test` was
green the whole time, which is exactly why a green local suite is not a substitute
for reading the run. Push, then check.

## Shortcuts belong in the message loop, not a window proc

`SHORTCUTS[]` in winmain.c drives **both** the sheet (Ctrl+/) and the keys, and
`accel_dispatch()` runs from the message loop before any window sees the message.

This is not tidiness. The chords used to be handled in the main window's
`WM_KEYDOWN`, and the main window almost never has focus — **the composer does**.
A native child consumes what it does not recognise, so Ctrl+K, Ctrl+F and Ctrl+/
were dead whenever the caret was in the message box, which is to say always, while
the shortcut sheet advertised them. The find box, search box, palette box, picker
and sign-in fields were the same black hole; each had to re-implement any chord it
wanted to let through, and F6's two ends duly drifted apart.

- **A new global shortcut goes in `SHORTCUTS[]`** with an `ACC_*` action. It then
  works from every focus and appears in the sheet, because they read the same rows.
- **Focus-specific keys stay in their procs** and are `ACC_NONE` rows in the table:
  Enter sends only in the composer; Esc means something different in every surface.
- **Shortcuts are suppressed while a modal is open** — a modal owns the window, and
  opening a second surface behind it would leave two things claiming the screen.
  Esc and Enter reach it through `modal_key`.
- Test with `gui_drive.sh key ctrl+k` (also `alt+shift+up`, `ctrl+slash`, `esc`).
  The verb really holds the modifiers via `SetKeyboardState`, because
  `accel_dispatch` reads `GetKeyState`; posting a bare key proves nothing. Before
  it existed, **no chord in the app was testable at all**.

## Modals: one frame, explicit commit

Every modal is drawn by `modal_frame()` (winmain.c) from an `oc_modal_spec`. The
frame owns the scrim, the card, the title bar with its close button, the footer
rule and the button row; a caller draws only content and decides none of that.
Before it there were **five** dialog idioms in one product: four D2D cards each
computing its own geometry, six middle-column panes borrowing the modal header
(including its "Esc to close" caption, which is not a modal concept), sixteen
native GDI popups and four MessageBoxes. All five are now one:

- the four cards are `oc_modal_spec`s,
- the six panes use **`pane_header()`** instead — its own header with a real ✕ and
  no modal furniture, because a pane is not a modal: it fills the middle column and
  the rest of the app stays live beside it,
- the sixteen GDI popups are `form_dialog()` **on the frame** (below),
- and the MessageBoxes are `confirm()`. Exactly one native message box survives, in
  `WM_CLOSE`: quit has to be answered before the window goes away, and our own
  frame needs a message loop that is about to end.

**Explicit commit, never live-apply.** A form modal declares `snapshot`/`restore`
and the frame copies its values on open, so `Save` commits and `Cancel`/`✕`/`Esc`
put them back — all three meaning the same thing. Live-apply makes Cancel a lie:
it either does nothing, or has to undo changes nobody recorded. The old
Preferences had no buttons at all and expected Esc, which is the dead end that
prompted this.

Not every modal is a form. Workspaces performs immediate irreversible actions and
Shortcuts is a reference sheet, so neither snapshots and both carry one dismissing
button. Where the setting lives on the SERVER — per-channel notification levels —
`restore` re-sends the snapshot rather than writing a local, and only for rows
that actually changed.

### `form_dialog()` — our frame, the platform's fields

`form_dialog(owner, title, fields, n)` is the generic typed form (`FF_TEXT`,
`FF_PASSWORD`, `FF_CHECK`, `FF_CHOICE`) behind every typed form in the client —
topics, renames, webhooks, invites, quick reactions, sign-up, keywords, priority
people, the custom pause time, profile fields, sections and more. It used to be a native
popup with its own window class, STATIC labels, BUTTONs, a `WS_CAPTION` title bar
and the stock shell font. The justification was that the platform's focus, tab order
and IME handling beat matching the palette — and **half of that still holds**:

- the **text fields are native `EDIT`s** and always will be, so caret, selection,
  IME, clipboard and undo remain the platform's problem, not ours;
- the **chrome is ours**, because sixteen grey Windows-95 boxes in the middle of a
  themed app were never worth it — and none of them was dismissible the way every
  other sheet is, screenshot-comparable, or reachable by the harness.

The fields are children of the **main** window, positioned by `layout_natives()`
from rects the painter recorded — the arrangement the sign-in card already uses.
Two consequences worth knowing before touching it:

- **The form's EDITs do not consult `window_is_covered()`.** They are part of the
  modal; the modal is what covers the window.
- **Esc and Enter are answered by the field**, in `form_edit_proc`. A single-line
  EDIT eats both, and a key sent straight to the focused child never reaches the
  message loop — so handling them in the loop left the frame's
  Enter-commits/Esc-cancels dead in the one modal where you are always typing. The
  smoke caught that.

It stays **synchronous**, by a nested message loop, because every caller reads
the answer on the next line. That loop is the one the old popup ran and it already
ran from inside `on_click`, so the re-entrancy is not new.

**Two rules the frame cannot enforce for you:**

- **Open through `modal_enter()`**, so the snapshot is always taken and the
  transient overlays are closed. The command palette and emoji picker each claim
  *every* click while open, and `layout_natives` hides their boxes whenever
  something covers the window — so a palette left open behind a modal is invisible
  and eats every click meant for the card. That was a live bug found by the smoke.
- **A click inside the card that matches no control stops there.** It used to fall
  through to the shell underneath, so a stray click in a modal's empty space could
  change channel behind the dimmed card.

## Preferences is two-paned, and appearance applies live

Categories left (**Appearance / Messages / Notifications / Advanced**), one pane's
rows right. A row's hit-boxes exist only while its category is showing — the click
router and the painter share `g_pref_cat` so they cannot disagree — and the sheet
reopens on the pane you left it on.

**Appearance applies live and reverts on Cancel.** A colour, a text size and a
density are their own preview; you cannot judge any of them from a label. The
snapshot therefore covers the accent, the text size, the density, the zoom step and
the DPI, and `prefs_restore` re-applies each through the one path that knows what it
costs (`scale_apply` rebuilds every DirectWrite format; `dpi_set` drops the render
target, the brushes and the thumbnail cache).

**A colour scheme is a PAIR** — the nav rail and the accent — because they are seen
together: an accent chosen against a fixed rail could fight it, and that looked like a
bug rather than a choice. Each scheme carries both colours for both modes, and the
selected-row tint is derived from the accent rather than being a palette constant, so
a plum scheme does not leave a blue selection behind. Every rail is dark in both
modes on purpose: rail icons and labels are near-white, so a light rail would need its
own foreground set and a second contrast problem to keep solved.

**The three scale multipliers stay separate (ARCH-97).** Text size is a preference
and follows the account. Zoom (Ctrl+`=` / Ctrl+`-` / Ctrl+`0`) is this window only
and is deliberately **not** persisted — a temporary magnification following you to
another machine is not what you asked for. DPI belongs to the display. `g_text_scale`
is the product of the first two, so nothing downstream has to know there are two
inputs; anything that caches a font size of its own (the RichEdit's CHARFORMAT, the
placeholder HFONT, the form fields' HFONT) is rebuilt by `scale_apply`.

## Two standing UI rules

**No "…" on a command.** Not in menus, not on buttons, not in cue banners. The
convention means "this opens something", but the app is affordance-driven — nearly
every command opens something — so it decorated almost everything and distinguished
nothing. Ellipses survive only where they mean *continuation*: "Loading…",
"Reconnecting…", "alice is typing…", and the `:…:` placeholder for an emoji whose
image has not arrived.

**An expanded section that is empty says so**, in italics — the app's only italic
(ARCH-97 names weights, not styles) precisely so it cannot be read as a conversation
called "Empty". It says "No matches" instead when a find filter is what emptied it,
because "you have none of these" and "none of yours match" are different answers and
an empty list gives the same silence to both.

## The composer is ours (ARCH-98)

There is no RichEdit and **no child window**. The field is drawn into the Direct2D
scene by `ed_draw()`, and the **main window** owns the keyboard while `g_ed_focus` is
set. Consequences worth knowing before touching it:

- **`layout_composer()` computes a rect, it does not move a control.** `g_ed_box` is
  both what `ed_draw` paints into and what `ed_hit` tests against, so what you click
  is what you see. When the middle column is not a conversation the rect is emptied
  **and focus is dropped** — keys must not go to a field nobody can see.
- **Every mutation goes through `ed_begin_edit()`**, which pushes an undo snapshot.
  Undo is whole-text snapshots rather than a journal: a message is at most 4000 UTF-16
  units, so a snapshot is 8KB, and it cannot get out of step with the buffer.
- **`ed_changed()` is the one post-edit path** — it re-measures the box, fires the
  typing indicator (which used to ride `EN_CHANGE`), refreshes the completion popover
  and resets the caret blink. A key handler that mutates text and skips it will look
  almost right.
- **The IME composition string is spliced into the layout at the caret**, not appended
  to the buffer: that is what makes it wrap and measure like real text, and therefore
  what makes the candidate window land in the right place. Hit-testing subtracts its
  length again, because it is in the layout and not in the text.
- **Anything that replaces `g_body` must call `ed_invalidate_layout()`** (see
  `scale_apply`). A stale layout is not visibly broken, which is worse than broken.

## Screenshots can see images (and once could not)

`test_shot` renders the scene into its own DC render target. A D2D bitmap belongs to
the target that created it, so the capture used to switch images off entirely — which
means **every screenshot ever taken of this app was a picture with the pictures
missing**, including the inline-image feature whose whole point is the
picture. That cost an hour of diagnosing a working avatar as broken.

The decoded PBGRA pixels are now kept beside each cached bitmap, and a capture creates
its own bitmaps from them for its own target (released when the shot ends). Only the
*fetch* is still suppressed during a capture, so driving the harness cannot generate
transfer traffic.

**If you add another cached GPU resource, keep the bytes too.** Anything that lives
only as a render-target-owned object is invisible to every screenshot, and therefore
unverifiable.

## Run the GUI smoke before pushing Win32 chrome

`scripts/gui_smoke.sh` drives the client through the test hook and asserts a
**dynamic count** — it prints the total it ran, which was **249** on 2026-08-02.
Do not quote a fixed number: the suite grows, and helpers like `fitcheck` assert
in loops. It covers, for each view, what is in the second column
(`sidebar_kind`), whether the middle one is typeable (`main_is_conversation`),
which native children are shown, and whether anything covers the window — plus
the search overlay, the Pins tab, the Preferences modal, the command palette, the
generic form (including that Esc does **not** commit and Enter does), a pane
header's ✕, the composer cue naming the open conversation, the notification
schedule and keyword editors, the pause, threads, the People directory, the
Activity unread filters, and a **chrome-fit matrix across DPI × zoom × text
size** (ARCH-97).

Every one of those is a boolean, and booleans belong in a script. Three bugs
reached the user in a day for want of this, one of them a regression,
all of them chrome. It is verified to catch them: reintroducing Files
returning `SBK_CHANNELS` — fails exactly two checks, the column kind and the leaked
find box.

**A new view or overlay is added to the matrix in that script as well as to the
predicates in `winmain.c`.** That is the point of it.

It is not in CI, and that gap is honest: the daemon is epoll-based so it is
Linux-only, and GitHub's Windows runners cannot host it (no Linux containers). A
hosted smoke needs a self-hosted Windows box. Until then, run it and read it — the
same discipline as reading CI.

## Seeing the whole window (Win32 harness)

`scripts/gui_drive.sh shot <name>` produces one image of the entire application,
Direct2D surface **and** native children. Getting there took two dead ends, both
worth recording so nobody re-walks them:

| Route | D2D content | Native children |
|---|---|---|
| Re-render the scene into a DC target | ✓ | **✗** |
| `PrintWindow(PW_RENDERFULLCONTENT)` | **✗ — blank** | ✓ |

The first misses children because they are separate windows. The second returns a
blank client area because our `WM_PAINT` renders through a D2D **HWND** target
straight to the screen and never touches the HDC Windows supplies — measured, not
assumed: the capture was white with the RichEdit's placeholder floating in it.

So `test_shot` does what the window does: render the scene, then walk
`EnumChildWindows` and blit each visible child on top, at its real position.
`EnumChildWindows` rather than a list of handles, because a list is one more place
a seventh child must be registered, and forgetting is this area's recurring
failure. Each child is asked twice — `PrintWindow` first, then `WM_PRINTCLIENT` —
because neither works for every control class: `PrintWindow` on a *child* returned
an empty box for the RichEdit, while `WM_PRINTCLIENT` is what a control
implements for this purpose.

**A control that paints itself must handle `WM_PRINTCLIENT` too.** The composer's
cue text was drawn only to `GetDC(hwnd)`, so it existed on screen and nowhere
else — the one string in the composer that users read could not be checked by any
capture. `re_proc` now overlays it on both `WM_PAINT` and `WM_PRINTCLIENT`. Any
future self-painted control needs the same pair.

## Typography (graphical clients) — ARCH-97

The platform owns the **family**, we own the **scale**. Full reasoning is in
ARCH-97; what a frontend author needs is the table and two rules.

| Token | DIP | Weight | Used for |
|---|---|---|---|
| `display` | 17 | 600 | view + workspace titles |
| `title` | 15 | 600 | channel header, author names |
| `body` | 15 | 400 | message text **and** the composer |
| `ui` | 14 | 400 / 600 | controls, list rows, buttons |
| `meta` | 12.5 | 400 | timestamps, sublabels, chips, counts |
| `micro` | 10 | 600 | rail labels |

**Rule one: name the role, never the size.** The Win32 client's formats were
`g_hdr`, `g_small`, `g_time`, `g_ava`, `g_rail` — five names describing a size or
a single call site, which is why `g_small` accumulated 208 uses covering
timestamps, chips, counts, hints and section headers with no way to change one
without changing all of them. They are now `g_display`, `g_title`, `g_body`,
`g_ui`/`g_ui_b`, `g_meta`/`g_meta_w`/`g_meta_r`, `g_avatar`, `g_micro` — the same
tokens the other graphical clients will declare, so a reviewer moving between
them reads one vocabulary.

**Rule two: two weights.** Regular 400 and Semibold 600 in all chrome. Bold 700
belongs to markdown `**strong**` in message bodies and nowhere else, so weight
carries one meaning in the UI and a different, deliberate one in content.

Sizes are DIPs and are multiplied by the user's **text size** factor when the
formats are built (`fonts_build`); system DPI and window zoom are applied
separately, for the reasons ARCH-97 gives. Rebuilding the formats is the only way
to change a DirectWrite size, so any preference that moves the scale must call
`fonts_build` and then force a relayout.

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
- **Done — Windows GUI depth.** The Win32 GUI surfaces every engine feature
  (ARCH-82) and its depth backlog is **closed**: the error/toast + connection
  surface (**REQ-263**), search with jump-to-match, keyset paging and operators
  (REQ-080), the sidebar overhaul (**REQ-267**), composer autocomplete + emoji
  picker (**REQ-265**), the preferences hub (**REQ-261**), the other-user profile
  viewer (**REQ-266**), the command palette (**REQ-260**), inline images
  (**REQ-142**), channel rename/topic/archive/visibility
  (REQ-034/035/036/036a), the activity feed (**REQ-139**), permalinks and
  jump-in-context (**REQ-232**/ARCH-96), mark-unread/mute/star
  (REQ-235/137/234), profile depth with avatars and custom status
  (REQ-240/241/122), the global notify default (REQ-134) and the
  **N-concurrent-workspace model** (REQ-012–015) are all built.
  **Accessibility (REQ-269) is built** — a UIA provider over the self-drawn UI, a
  real system caret and UIA events (**ARCH-99**), with an `AutomationId` and an
  `InvokePattern` on every actionable element (**REQ-290**), verified
  from outside the process by `scripts/uia_probe.ps1`. Rich text and its toolbar
  are built too (**REQ-220**, ARCH-100: a shared parser in `client/core/` plus
  DirectWrite ranges). The items that once waited on a daemon requirement —
  drafts, scheduled send, the notification schedule, keywords and priority
  people, the pause, cross-channel threads, the People directory — all landed
  with their server halves on 2026-08-01/02. Open work is in the
  [issue tracker](https://github.com/openchime/openchime/issues).
- **The frontend order is fixed: all of Win32, then the TUI, then GTK, then
  macOS.** Win32 is the reference client and it is finished *first* — including
  the items now waiting on a daemon requirement, which are Win32 work waiting on
  their other half, not work deferred behind another frontend. The TUI being
  behind is an accepted consequence of that order, not a reason to reorder it.
- **Then — TUI catch-up.** The two frontends have swapped places: the TUI was
  the reference client and is now behind the GUI by **more than twenty** features,
  every one of which already exists in the app-core — @mentions (REQ-221), pins
  (REQ-230), saved items (REQ-231), the channel files listing (REQ-143), the
  per-channel roster (REQ-031), channel management (REQ-034/035/036/038), mute and
  star (REQ-137/234), mark-unread (REQ-235), the activity feed (REQ-139), in-app
  preferences and themes (REQ-261/262), profile depth and custom status
  (REQ-240/241/122), group DMs (REQ-056), custom emoji (REQ-072), rich text
  (REQ-220), and more. Because the frontend order puts all of Win32 first, the
  TUI's gap is a consequence of that order rather than tracked work; it is
  deliberately not tracked as an issue.
- **Next — auth completeness.** The **OIDC browser flow + PKCE + loopback
  courier** — the one remaining piece of REQ-020 and what makes SSO usable at all
  (there is no SAML by design, **REQ-027**); plus first-run onboarding
  (**REQ-268**).
- **Next — audio client.** Opus encode/decode + UDP to the sidecar — the deferred
  half of REQ-150/151 ([AUDIO.md](./AUDIO.md)).
- **Then — the rest of the specified scope.** REQUIREMENTS.md §§1–16 carries it,
  each requirement marked with whether it is built; whatever is not built and
  matters is an issue in the [tracker](https://github.com/openchime/openchime/issues).
- **Then — remaining platforms + screenshare.** The other native GUIs (GTK,
  AppKit), a web DOM UI, and mobile — and, gated behind the audio client,
  **screenshare** (REQ-161, [VIDEO.md](./VIDEO.md)).
