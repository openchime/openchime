# OpenChime — Windows GUI Backlog

The execution list for `client/gui/win32/` (ARCH-82). Every item below is a
gap between the shipped Win32 client and the bar set by the TUI, the reference
clients, or a `REQ-NNN`.

**Where this sits.** [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) is the
*analysis* (what is thin, versus Slack and Pumble); [STATUS.md](./STATUS.md)'s
parity table is the *feature reachability* tracker. This document is the *work
list* derived from both — one row per shippable branch.

**Ids.** Items are `WIN-1` … `WIN-59`, numbered once and **stable**: an id is
never renumbered or reused, so a commit or branch can cite it. Ordering is *not*
priority — the **Pri** column is, and it may change. Section membership may also
change as blockers clear (an item moves from §3 to §1 without changing its id).

**Sections.**
- **§1 Ready now** — buildable today with no change outside `client/gui/win32/`.
- **§2 Ready now, with a small app-core change** — the wire already carries it;
  `client/core` just doesn't expose it yet.
- **§3 Blocked** — needs daemon/protocol work (usually an ARCH decision) first.
- **§4 Not planned here** — out of scope for this client, recorded so the
  omission is deliberate rather than forgotten.

**Pri:** P0 core parity · P1 important · P2 nice-to-have.
**Size:** S ≈ a sitting · M ≈ a day · L ≈ multi-day / architectural.

---

## §1 Ready now — pure Win32

| # | Item | Pri | Size |
|---|---|---|---|
| ~~**WIN-1**~~ | ~~**Global error/toast + connection banner (REQ-263).**~~ **DONE.** A toast stack (bottom-right, 6 s, click to dismiss) for in-session failures, and a connection banner under the header — reason + **Retry now** — while unauthenticated. The two are split by kind so no message appears twice, and the transcript's centred `last_error` is gone. Runtime-verified on Windows: the banner against a dead endpoint, and a live `send rate exceeded` toast driven against a real daemon. **Caveat:** the banner shows the core's `reconnecting in Ns…` text, which the net thread emits *once per backoff* — it is not a ticking countdown. A live one needs the core to expose a retry deadline; filed as WIN-55. | P0 | M |
| ~~**WIN-2**~~ | ~~**Login dialog error display + remember-me.**~~ **DONE — rebuilt entirely** as a two-step, Slack-shaped, in-window sign-in view (REQ-263/020/010). Step 1 takes a workspace and resolves it by DNS, defaulting to the **hosted** case — you type `acme` and the field shows a fixed `.openchime.io` chip — with **Advanced options** revealing a full domain / `host:port` field for self-hosted. Step 2 takes username + password with **Remember me**. Errors are inline and retryable; the password is cleared and refocused on failure. Sign-out now returns here instead of quitting. The ~140-line pre-window GDI popup is deleted and *Add a workspace…* routes to the same view. Runtime-verified: DNS failure, bad password, success, remember-me both ways, sign-out round-trip, and both bypass paths. | P0 | M |
| ~~**WIN-3**~~ | ~~**Search results that navigate (REQ-080).**~~ **DONE.** Clicking a hit arms a jump on its `message_id`: the transcript selects the channel, scrolls that message a third of the way down the pane and flashes it for 1.6 s. Query terms are tinted in the snippets (case-insensitive substring, matching what the daemon's search actually does). A jump the transcript never resolves within 4 s toasts "older than the loaded history" rather than silently doing nothing. Also fixed two bugs found here: `text_width()` measured one character short, and the result timestamp used a 16-byte buffer for a 17-byte `strftime`, so **every result showed a blank time**. Runtime-verified on Windows: searched a seeded needle, clicked the hit, landed on the flashed message. | P0 | M |
| ~~**WIN-4**~~ | ~~**Search overlay input + truncation notice.**~~ **DONE.** The modal prompt is gone: the overlay owns a native query box (Enter searches, Esc closes), so refining never closes and reopens the pane. Results scroll with a thumb, and a count line reads "N results for …" — plus "more exist; narrow the query" when the set was capped. That flag was being **discarded at decode** (`oc_decode_search_results(..., NULL)`); it is now carried to `oc_model.search_truncated`, and the client's own 64-entry decode cap reports as truncation too. True paging stays WIN-38 — there is no cursor on the wire. | P1 | M |
| ~~**WIN-5**~~ | ~~**Sidebar DM section + Public/Private grouping (REQ-267).**~~ **DONE.** Channels and Direct messages are separate collapsible sections built by a *shared core helper* (`oc_model_sidebar`), so the TUI and the GUI cannot drift. Each section header carries a kebab with Sort (A–Z / recent / unread) and Filter (all / unread / active), persisted in the synced `client_settings` bucket. DMs render an initial avatar with a presence dot rather than an `@` sigil, and a self-DM shows the account name with a dimmed "you". | P0 | M |
| ~~**WIN-6**~~ | ~~**Sidebar scrolling.**~~ **DONE.** The list scrolls (wheel + thumb) with rows clipped to the pane, so nothing past the fold is unreachable. **Caveat:** `oc_model_sidebar` is still called with a 512-row buffer — scrolling is unbounded in practice but the row build is not literally uncapped. | P0 | M |
| ~~**WIN-7**~~ | ~~**Composer autocomplete for `@user` / `#channel` / `:emoji:` (REQ-265).**~~ **DONE.** A popover above the composer filters as you type; Up/Down move, Tab or Enter inserts, Esc dismisses, and a click picks. The matching rules were **lifted out of the TUI into the core** (`client/core/complete.[ch]`), so both frontends complete a token identically instead of holding two copies of the logic (ARCH-74). Runtime-verified on Windows for all three triggers. Real mention *semantics* remain WIN-45. | P0 | M |
| ~~**WIN-8**~~ | ~~**Composer emoji picker (REQ-265).**~~ **DONE.** A searchable, category-sectioned picker over the shared core catalogue (~190 entries with shortcodes and keywords), reachable from a composer button and from the message menu's React → More…. The six quick reactions stay inline as one-click affordances rather than being the only choice. Reaction chips are now real bordered chips with colour glyphs and a "mine" state. **Caveat:** curated, not literally all of Unicode — a picker over every emoji is unusable and the shortcodes are what get typed. | P1 | M |
| ~~**WIN-9**~~ | ~~**Settings / Preferences hub (REQ-261).**~~ **DONE.** The "coming soon" MessageBox is a real pane: Appearance (dark / light / match system), time format, members-pane default, and date dividers, each a segmented control that applies **live** — a preference you must restart to see is indistinguishable from one that did nothing. Written through `oc_client_set_setting` into the `gui` bucket, which was the core call with no caller in any frontend; being server-synced they follow the account to another machine, which is the only option left now a client writes no files (ARCH-88). Notification *behaviour* is deliberately absent: per-channel level and DND have their own surfaces, and a global default is REQ-134 (blocked). Runtime-verified including persistence across a restart. | P0 | M |
| ~~**WIN-10**~~ | ~~**View another user's profile (REQ-266).**~~ **DONE.** A profile pane with the avatar, display name, live presence and role, opened by clicking a members-pane row or from its context menu — including your own. Clicking a member used to jump straight into a DM, which made viewing someone impossible and was the more destructive of the two actions; **Message** is now a button on the pane. Richer fields stay REQ-240 / WIN-47, and the pane shows what the roster actually knows rather than placeholders for them. | P0 | S |
| ~~**WIN-11**~~ | ~~**Command palette / Ctrl+K (REQ-260).**~~ **DONE.** Ctrl+K opens a subsequence-matching palette over a 20-action catalogue plus every channel and DM as "Go to". The catalogue dispatches through the **same `menu_dispatch` codes the menus use**, so the palette cannot offer an action the menus lack or run it differently. Up/Down, Enter, Esc, and click. Runtime-verified including quick-switch. | P1 | M |
| ~~**WIN-12**~~ | ~~**Notification-prefs review screen.**~~ **DONE.** A pane listing every channel and DM with All / Mentions / None editable in place, and the current DND window at the top. `oc_client_list_notify_prefs` had no caller at all. Verified end-to-end: setting a level persists server-side and re-renders. | P1 | S |
| ~~**WIN-13**~~ | ~~**DND time pickers.**~~ **DONE.** An on/off checkbox plus separate From/To fields, pre-filled from the current window, validated (00:00–23:59) with a toast on bad input instead of silently doing nothing. Turning it off says so. | P1 | S |
| ~~**WIN-14**~~ | ~~**New-message divider + jump-to-unread (REQ-236).**~~ **DONE.** A red "New" separator above the first unread message, and an "N new ↑" pill in the header that scrolls to it (reusing WIN-3's jump-and-flash). The marker is **snapshotted on entry**, because entering a channel acks it — read live, the divider vanished in the frame it appeared. Cleared when you send. Runtime-verified with a second user. | P1 | M |
| ~~**WIN-15**~~ | ~~**Per-reply actions inside threads + thread scrollbar.**~~ **DONE.** `draw_msglist` gained a mode so the thread pane records hit-boxes and scrolls with its own offset (opening a thread no longer disturbs where the transcript was). The full message menu works on a reply — `show_msg_menu` now resolves ids in `thread_msgs` as well as the channel, without which the menu simply never appeared, and it routes react/delete to the thread's channel. No thread item on a reply, since there are no nested threads (REQ-060). | P1 | M |
| ~~**WIN-16**~~ | ~~**Transcript paging past the 600-message cap.**~~ **DONE**, and it needed a wire addition. `BACKFILL_REQUEST`'s cursor only points forward, and §6.1 makes a zero cursor mean "the newest page", so a client could reach the newest page of a channel and **no further, permanently** — there is no local history to fall back on (ARCH-88). New additive opcode `HISTORY_REQUEST` (0x0034, PROTOCOL.md §6.3) pages backwards; the response reuses the §6.2 shape. `channel_append` also had to stop rejecting ids at/below the high-water mark, which silently discarded every paged-in message; it now dedups by lookup and inserts older messages at their sorted position. Scroll-to-top requests the previous page, one in flight at a time, and stops when the server reports nothing above. Render cap raised 600→2000. Verified: 60 → all 243 messages by scrolling, then no further requests. | P1 | M |
| ~~**WIN-17**~~ | ~~**Inline image thumbnails (REQ-142).**~~ **DONE.** Common image types render inline, scaled to fit and never upscaled, with the filename kept on the placeholder so an image whose bytes never arrive is not reduced to a nameless grey box. Two upstream defects had to be fixed first: every upload declared `application/octet-stream` (so nothing downstream could tell an image from a zip — REQ-142 was impossible regardless of client quality), and the basename split only on `/` (so a Windows upload declared its whole path as the filename). The bytes are fetched **into memory** (`oc_client_fetch_attachment` → `OC_EV_ATTACHMENT_DATA`, bounded at 8 MB) and decoded with WIC — no temp file, since a scratch file for rendering is exactly the cache ARCH-88 removed. **Click-to-expand is not implemented**; the message menu's Download is still how you get the full file. | P1 | M |
| ~~**WIN-18**~~ | ~~**OS toast notifications (REQ-138).**~~ **DONE.** `Shell_NotifyIconW` with `NIF_INFO` rather than WinRT: the client is pure C (ARCH-82) and `ToastNotificationManager` needs a C++/WinRT projection plus a registered AppUserModelID, while on Windows 10+ a balloon is rendered by the same toast system. The decision is the server's model rendered locally (ARCH-72) — channel notify level plus the DND window, both already in `oc_model` — so this adds no server surface. Nothing is raised while the window is in front, for your own messages, or during the first pass after connecting (a backfill is not new mail). Preference: Off / Count / Preview. `MENTIONS` is deliberately treated as silent until REQ-221 gives mentions real semantics — guessing would notify for things that are not mentions. **Verification caveat:** the gating is verified mechanically (level `None` suppresses, `All` raises, DND suppresses, DND off resumes — via a toast counter in the test dump), but **the balloon was never seen on screen**: this desktop does not surface balloons to a screen capture, confirmed with a control test using .NET's own `NotifyIcon`. The tray registration succeeds (`NIM_ADD` returns true). | P1 | M |
| ~~**WIN-19**~~ | ~~**Audit-log paging + filters.**~~ **DONE.** The log scrolls, family filter chips (All / Admin / Account / Security / Moderation) narrow it, and reaching the bottom re-queries with the oldest paged-in timestamp as the cursor. **Caveat:** the filter is client-side over what has been paged in — it narrows what you are looking at, it does not re-query by family. Actor/action filters would need a server-side query parameter. | P1 | S |
| ~~**WIN-20**~~ | ~~**Real profile editor.**~~ **DONE.** One dialog each: display name with a note on where it appears, and current/new/**confirm** password with a mismatch check. The chained prompts had no confirm field at all. | P1 | S |
| ~~**WIN-21**~~ | ~~**Purpose-built dialogs, retiring `text_prompt`.**~~ **DONE.** All seven flows moved to the typed `form_dialog()` and **`text_prompt` is deleted**. New DM also now says "No such user in this workspace" instead of silently doing nothing on an unknown name. | P1 | S |
| ~~**WIN-22**~~ | ~~**Invite dialog with a copyable token.**~~ **DONE.** Invite and webhook tokens now appear in a selectable field, are **put on the clipboard the moment they are shown**, and say plainly that they will not be shown again and what to do if lost. A MessageBox could not be selected from — the one thing that must not be true of a value shown exactly once. Expiry/pending/revoke stay WIN-46. | P1 | S |
| ~~**WIN-23**~~ | ~~**Webhooks overlay depth.**~~ **DONE**, except the date. Scrolling, and an active/disabled chip so an enabled hook is positively marked rather than merely lacking the word "(disabled)". The delete confirmation already existed. **No created-date column is possible:** `WEBHOOK_LIST` carries only id / channel / label / disabled, so that part needs a wire field — folded into WIN-48. | P2 | S |
| ~~**WIN-24**~~ | ~~**Storage overlay refresh + TUI parity.**~~ **DONE.** Grouped into Disk / Policy / Reclaimed like the TUI, free-of-total on one line, pressure and any evictions flagged in red, and a Refresh button — there was previously no way to ask again. | P2 | S |
| ~~**WIN-25**~~ | ~~**Keyboard-shortcut reference (REQ-264).**~~ **DONE.** A sheet generated from a single `KEYMAP` table, on Ctrl+/ and in the workspace menu. | P2 | S |
| ~~**WIN-26**~~ | ~~**Theme / appearance selection (REQ-262).**~~ **DONE.** `theme.h`'s literals became a runtime `oc_theme[]` array behind the same `OC_COL_*` spellings, so every call site is unchanged and a switch takes effect on the next frame. Dark, light, and match-system (reading `AppsUseLightTheme`, defaulting to dark when unreadable). The light palette inverts the *structure*, not just the brightness — the rail stays the darkest surface — and native children (RichEdit, EDIT brushes) are re-skinned on every switch, since they keep their own colours and otherwise stay dark on a light shell. Persisted with the other preferences. | P2 | M |
| ~~**WIN-27**~~ | ~~**Draft persistence (REQ-223).**~~ **DONE, in-memory half.** Up to 24 per-channel drafts survive a channel switch for the life of the process; sending clears the one for that channel, and an edit-in-progress is not treated as a draft. **Cross-restart drafts are not done** and need the server-synced `client_settings` route (ARCH-88 leaves no third option) — a separate decision, not an oversight. | P2 | M |
| ~~**WIN-28**~~ | ~~**Configurable quick reactions (REQ-073).**~~ **DONE.** The six literals became a preference holding **shortcodes**, resolved through the shared catalogue (`oc_emoji_by_name`) so an unknown name drops out rather than writing a broken glyph, with a fallback if the set ends up empty. Edited from Preferences, persisted in the `gui` bucket. Offered in the message menu; inline-on-hover is not done. | P2 | S |
| ~~**WIN-29**~~ | ~~**N-concurrent-workspace model (REQ-012–015).**~~ **DONE.** Up to 8 clients are held and **all are ticked every frame**; only the active one renders. Rather than thread a workspace handle through the ~100 sites using `g_client`/`g_sel`/`g_scroll`, those stay as the *active* workspace's state and a slot array holds the rest — switching saves the active globals into their slot and loads the target's, so the diff is confined to switching and ticking. A red "N elsewhere" badge on the rail avatar, per-row unread in the switcher, notifications that name the originating workspace, and a quit guard that counts unsent messages across all of them. Verified against **two real daemons**: both authed at once, the backgrounded workspace received a new message live (22→23 unread) and raised its notification, and switching back was instant with selection, unread and reactions intact. | P1 | L |

## §2 Ready now, with a small app-core change

The wire already carries these; `client/core` is the only thing in the way.

| # | Item | Core change needed | Pri | Size |
|---|---|---|---|---|
| ~~**WIN-30**~~ | ~~**Create-channel dialog with visibility.**~~ **DONE.** `oc_client_create_channel_ex(c, name, is_public)` added and the dialog offers Public/Private. Verified end-to-end: a private channel lands with `is_public=0` and renders with the lock in the sidebar. | Done | P1 | S |
| ~~**WIN-31**~~ | ~~**Channel member management.**~~ **DONE.** `oc_client_channel_invite` / `oc_client_channel_kick` added, offered as "Add someone…" / "Remove someone…" on the channel menu. Without these a private channel could be created but never populated. | Done | P1 | M |
| ~~**WIN-32**~~ | ~~**Signup / first-owner onboarding (REQ-268).**~~ **DONE.** "Have an invite? Create an account" on sign-in step 2 takes a token, username and password; the connection redeems instead of authenticating, which creates the account and signs in together. Verified end-to-end: a minted invite produced a real member who landed in the workspace. **This also exposed a defect that made invites unusable at all** — see the commit: tokens were random bytes sent to clients verbatim. | Done | P1 | M |
| ~~**WIN-33**~~ | ~~**Mark-all-read / catch-up (REQ-238).**~~ **DONE** client-side: a loop of `oc_client_mark_read` over every channel with unread, reporting how many it caught up. Acks are cumulative per channel so no wire change was needed; REQ-238 may still add a true bulk op. | Done | P2 | S |

## §3 Blocked — needs daemon or protocol work first

Not startable in this client. Each names what must land first.

| # | Item | Blocked on |
|---|---|---|
| **WIN-34** | Channel rename | REQ-036 — no rename op exists |
| **WIN-35** | Channel topic / description | REQ-034 — no topic column |
| **WIN-36** | Channel archive | REQ-035 — no archive flag or read-only enforcement |
| **WIN-37** | Channel details pane (members / pinned / files tabs) | WIN-34–36 + REQ-230 + REQ-143 |
| **WIN-38** | Search paging | Wire gap: `SEARCH_RESULTS` carries `truncated` but **no cursor or offset**, so load-more is impossible today |
| **WIN-39** | Search operators (`from:` / `in:` / `has:` / dates) | REQ-081 — no operator grammar |
| **WIN-40** | Mute channel/DM (suppress + de-emphasize) | REQ-137 — distinct from level=none; needs per-user mute storage |
| **WIN-41** | Starred/favourite conversations + custom sidebar sections | REQ-234 — per-user sidebar state storage. **ARCH-88 settles the open question by elimination**: with no client-local storage it must be server-side, and the `client_settings` bucket already exists for exactly this |
| **WIN-42** | Pin a message | REQ-230 |
| **WIN-43** | Save for later / the **Later** rail stub | REQ-231 |
| **WIN-44** | Copy link / permalink, jump-to-permalink | REQ-232 — needs a permalink form and fetch-around-an-id backfill |
| **WIN-45** | Real `@mentions` (highlight + notify) | REQ-221 — also gates the `MENTIONS` notify level and REQ-135 |
| **WIN-46** | Invite management: links, expiry, pending list, revoke | REQ-026 |
| **WIN-47** | Rich profile fields — avatar, email, timezone, title | REQ-240 |
| **WIN-48** | Webhook reveal / rotate / enable-disable | No such ops; only create/list/delete exist |
| **WIN-49** | Activity feed — the **Activity** and **Alerts** rail stubs | REQ-139 |
| **WIN-50** | Files browser — the **Files** rail stub | REQ-143 |
| **WIN-51** | Forward / quote-share a message | REQ-057 |
| **WIN-52** | Mark a message or conversation unread | REQ-235 |
| **WIN-53** | Custom status (emoji + text + expiry) | REQ-241 / REQ-122 |
| **WIN-54** | Per-channel roster, global notify default, browse-channels directory, group DMs, custom emoji, active-session list | REQ-031 listing op · REQ-134 · REQ-038 · REQ-056 · REQ-072 · REQ-182 has revoke but no list |
| ~~**WIN-55**~~ | ~~Live reconnect countdown in the banner~~ **DONE.** The blocker was a client-core gap, not daemon work: the net thread stated the delay once per backoff as sticky text, so the banner's number never moved and read as a hung client. `oc_model` now carries `reconnect_at_ms` and the banner ticks it down, then shows "Reconnecting…" once the deadline passes. The clock lives in the core (`oc_model_now_ms`) and both the net thread and the frontend read it — two clocks would drift and make the countdown jump. Verified by killing the daemon and watching it count down. |
| ~~**WIN-56**~~ | ~~**Store the session token in Windows Credential Manager**~~ **DONE.** `client/shared/secret_win.c` implements the `oc_secret` seam over `CredWriteW`/`CredReadW`/`CredDeleteW` (one generic credential per workspace, `CRED_PERSIST_LOCAL_MACHINE`), wired into both Windows front-ends via the new shared `oc_secret_open_os()`. **The core now refuses to persist a token to SQLite at all** and erases any left by an older build, so there is no plaintext fallback anywhere. Verified on Windows: the credential appears in `cmdkey /list`, `workspace_state.session_token` is NULL, and silent reconnect reads from the OS store. |
| ~~**WIN-57**~~ | ~~Sign in to *every* remembered workspace at boot~~ **DONE** — unblocked by WIN-29. A launch with no arguments now connects every remembered workspace that has a stored session token, keeping the most-recently-used one active and the rest in the background. Only ones with a token: a workspace without a credential would sit at a failed connection, and boot is not the place to ask. Verified: a no-argument launch brought up both remembered workspaces, one active and one background showing its unread. |
| ~~**WIN-59**~~ | ~~Warn on quit while the in-memory outbox is non-empty~~ **DONE.** `WM_CLOSE` checks `oc_client_outbox_pending()` and confirms before discarding queued sends. |
| ~~**WIN-58**~~ | ~~Move the workspace book out of SQLite~~ **DONE** with ARCH-88 — it lives in the credential store, found by enumeration, so the book needs no storage of its own. |

## §4 Not planned for this client

- **Audio and screenshare** (REQ-150–152, REQ-161) — a separate epic sequenced behind the audio client ([AUDIO.md](./AUDIO.md), [VIDEO.md](./VIDEO.md)), not Win32 depth work.
- **Push device registration** (REQ-132) — `REGISTER_DEVICE_TOKEN` reaches no client, but push targets mobile; a desktop client would use WIN-18 instead.
- **Slash commands** — excluded by design; the GUI is affordance-driven (ARCH-82).
- **GIF/sticker pickers, Canvas, Lists, Clips, Slack Connect, first-party bot/MCP** — REQ-270–275, explicit product exclusions.

---

## Workspace lifecycle (post-WIN-29 refinement)

Sign-out used to drop the **whole app** into the sign-in view while the other
workspaces stayed connected and merely unreachable — the rail is not drawn
there. Three concepts were collapsed into one menu item; they are now distinct:

| | Server effect | Local effect | Cost to return |
|---|---|---|---|
| **Sign out** | revoke this device's session | drop the client, clear the stored token, **keep** the book entry | password |
| **Sign out everywhere** | revoke every session for that user | same locally | password |
| **Remove** | none | delete the credential *and* the book entry (REQ-012) | retype the address |

- Signing out lands in a surviving workspace and stays in the chat UI; the
  sign-in view appears only when nothing is left.
- The switcher marks entries `— signed out`, and clicking one goes **straight to
  the password** using the account stored in the book.
- **Manage workspaces…** is the surface for Remove, which the GUI previously had
  no way to do at all (the TUI did).
- Signing in while a workspace is live renders the card **over the dimmed
  shell** with a Cancel, instead of blanking the app. This is also what stopped
  "Add a workspace…" from signing you out of the one you were in.

## Where this stands

**§1 and §2 are complete** — every item that was buildable in `client/gui/win32/`
or needed only a small `client/core` change is done and was verified running on
Windows, not merely compiled. So are the §3 items whose blocker turned out to be
client-side rather than daemon-side: WIN-55 (a core field), WIN-56/58 (the
credential store), WIN-57 (unblocked by WIN-29), and WIN-59.

**Everything still open is §3, and every one of them is blocked on a server
feature that does not exist.** Not "hard" — absent: there is no rename op
(REQ-036), no topic column (REQ-034), no archive flag (REQ-035), no search
cursor on the wire, no per-user mute storage (REQ-137), no mention semantics
(REQ-221), no pins (REQ-230), no permalinks (REQ-232). Each needs its REQ built
in the daemon first; none is startable in this client. §4 remains deliberately
out of scope.

**So the next move is not in this document.** Picking up WIN-42, WIN-45 or
WIN-49 means implementing REQ-230, REQ-221 or REQ-139 in the daemon; the Win32
work that follows is small by comparison. The exception is WIN-48, which needs
only a `created_at` field on `WEBHOOK_LIST` plus reveal/rotate ops — the
cheapest remaining unblock.

Update [STATUS.md](./STATUS.md)'s parity table when an item changes a ✅/🔨 mark,
and strike the corresponding line in CLIENT_GAP_ANALYSIS.md §4 when one closes.
