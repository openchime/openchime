# OpenChime — Windows GUI Backlog

The execution list for `client/gui/win32/` (ARCH-82). Every item below is a
gap between the shipped Win32 client and the bar set by the TUI, the reference
clients, or a `REQ-NNN`.

**Where this sits.** [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) is the
*analysis* (what is thin, versus Slack and Pumble); [STATUS.md](./STATUS.md)'s
parity table is the *feature reachability* tracker. This document is the *work
list* derived from both — one row per shippable branch.

**Ids.** Items are `WIN-1` … `WIN-84`, numbered once and **stable**: an id is
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
| ~~**WIN-18**~~ | ~~**OS toast notifications (REQ-138).**~~ **DONE.** `Shell_NotifyIconW` with `NIF_INFO` rather than WinRT: the client is pure C (ARCH-82) and `ToastNotificationManager` needs a C++/WinRT projection plus a registered AppUserModelID, while on Windows 10+ a balloon is rendered by the same toast system. The decision is the server's model rendered locally (ARCH-72) — channel notify level plus the DND window, both already in `oc_model` — so this adds no server surface. Nothing is raised while the window is in front, for your own messages, or during the first pass after connecting (a backfill is not new mail). Preference: Off / Count / Preview. `MENTIONS` was deliberately silent until REQ-221 gave mentions real semantics; **since REQ-221 it is evaluated properly** with the shared scanner. **Verification, stated exactly (re-tested 2026-07-28):** the gating is verified mechanically (level `None` suppresses, `All` raises, DND suppresses, DND off resumes — via the toast counter in the test dump). The API path is verified positively too: the dump reports `tray_live=1`, i.e. `Shell_NotifyIconW(NIM_ADD)` was **accepted by the shell**, and `toasts_raised=1` after driving the `toast` hook, i.e. the `NIM_MODIFY`/`NIF_INFO` call executed. What remains unobserved is only the **rendering**: a full-screen capture on this host throws `Win32Exception` and returns a blank bitmap, so the session has no attached display surface to draw a balloon into — which is also why a control test with .NET's own `NotifyIcon` was equally invisible. This is an environment limitation of the dev host, not an unverified code path; seeing it requires running the exe on a Windows desktop with an attached display. | P1 | M |
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
| ~~**WIN-64**~~ | ~~Move the connection indicator to the workspace, where it belongs.~~ **DONE.** A dot beside the workspace name — filled green when live, hollow accent when not, clicking it reconnects. The channel header no longer carries connection text. **Built as three states and cut back to two:** measured against a killed daemon, a "reconnecting" tint blinked to "down" every few seconds, because the model reports a scheduled retry most of the time but nothing during each dial. A 9px dot that blinks reads as broken, and the client retries forever anyway, so the two are the same situation at different instants. The banner (WIN-1) keeps the words and the countdown. Original note: | "connected" currently sits in the *channel* header, which reads as a property of the channel; it is workspace state. Replace it with a small dot beside the workspace name (**Acme HQ**) — filled when connected, hollow when not — and make clicking it retry when disconnected. *Design notes:* it needs a **third state**, because "connecting/backing off" is not the same as "down" — reuse `draw_presence_dot`'s filled/hollow plus an accent ring rather than inventing a widget. It coexists with the WIN-1 banner rather than replacing it: the banner is the loud, explaining affordance with the countdown, the dot is the ambient one, and only the banner should carry the countdown text. Design it as a reusable widget: with N workspaces on the rail (REQ-012–015) each avatar eventually wants the same dot. | P1 | S |
| ~~**WIN-65**~~ | ~~Member panel: role as a second indicator column, not a text label.~~ **DONE.** A crown for owner, a shield for admin, nothing for member — drawn **inline, immediately after the name**, not in a column of its own. The column version was built first and rejected on sight: reserving 18px on every row to serve the rare one is expensive in a 220px pane when almost nobody is an owner or admin. Inline also keeps every name on a single left edge. A long name suppresses the glyph rather than colliding with it. Original note: | Replace the trailing "owner"/"admin" words with a small glyph in a column beside the presence dot. *Design notes:* show it **only** for owner/admin — a marker on every row is noise, and "member" is the default. A bare glyph is not self-describing, so it is an at-a-glance hint whose *answer* is the profile pane (WIN-10), which already names the role; a tooltip would be better still. Worth remembering that role is **tenant-wide**, not per-channel, so in a channel roster it is genuinely secondary information — which is the argument for demoting it from text to an indicator in the first place. | P2 | S |

| ~~**WIN-71**~~ | ~~Files: filters crammed into one chip row, no left column, no name search.~~ **DONE.** Rebuilt on Slack's split — **collection on the left, ownership top-left, type and sort top-right, name search above all of it**. The left column lists "All files" then the channels that have any, with counts; picking one is a real **server-side refetch** (`LIST_FILES` already takes a channel id), not a slice of the page we hold, so the list is exact. Slack's other three collections were not copied: Canvases and Lists do not exist here and its Starred holds messages, so three of four rows would have been stubs. Type and sort became dropdowns — as four permanent chips, type spent the width the filename column wanted to state an axis most people leave on "All". **Two caveats, both stated in the UI:** the channel census comes from the 200-file page, so a channel whose last upload is older will not appear until you visit it (an exact list needs a distinct-channels query — not built); and the list does not scroll yet, so it says how many rows it could not show. | P2 | M |

| ~~**WIN-73**~~ | ~~**Later** reform~~ **DONE.** A saved message shows a **bookmark in the right margin and a faint inverse band** — `OC_COL_SELECT`, deliberately *not* the accent a mention uses, because a message can be both and identical tints would be unreadable. It costs **no height**: the band reuses the row rect and the glyph sits outside the body's wrap width, so a save landing cannot shift the transcript under the reader. The marker survives a reconnect, which was the whole point of the daemon half — verified by relaunching the client against a live daemon. The view took the **Files shape**: a channel column defaulting to "All channels", newest-saved first with counts, and scrolling through the shared overlay offset. **Caveat:** `LIST_SAVED` takes no channel argument (unlike `LIST_FILES`), so picking a channel filters the page we hold rather than re-asking — the whole list, at the 200 cap, for almost anyone. | — |
| ~~**WIN-74**~~ | ~~DM tabs: drop **About**~~ **DONE.** A DM shows Messages · Files & links · Pins. `tab_applies()` decides per channel kind, and it also guards `select_tab()` and a conversation switch — otherwise moving from a channel to a DM with About open left a pane whose tab did not exist. Pins and Files stay because the daemon genuinely allows both in a DM (membership is the only check). | — |
| ~~**WIN-75**~~ | ~~The palette has no affordance~~ **DONE.** "Jump to…  (Ctrl+K)" in the **New** menu, beside "Search messages…" — the menu that already answers "do something". The shortcut is named in the label so the menu teaches the keystroke instead of replacing it. | — |
| ~~**WIN-76**~~ | ~~Long lists do not scroll~~ **DONE** for Files and Later, both through the offset five other panes already shared (`ovl_use`/`ovl_begin`/`ovl_end`) rather than a sixth private one; the wheel routes to it for both views. The Files list's **"%d more — narrow the filters"** line is deleted: it existed only because the list could not scroll, and a count of unreachable rows is a worse answer than a scrollbar. The 200-row **server** cap notice stays — that one is a real limit. **Still open, split out as WIN-82:** the Files channel census is only as complete as that 200-row page. | — |
| **WIN-77** | Modal frame: ~~`confirm()`~~ **(done)** · pane headers · `form_dialog`'s 16 sites | P1 | L |
| ~~**WIN-78**~~ | ~~Preferences as two panes + appearance depth~~ **DONE** — see §1 | P2 | M |
| ~~**WIN-79**~~ | ~~Replace the native context menus~~ **DONE** (ARCH-98). All four — message, member, channel, image kebab — are the app's own floating menu; `TrackPopupMenu`, `CreatePopupMenu` and `AppendMenuW` appear nowhere in the client. They keep **their own command numbers**, dispatched per kind, because the dropdowns' space was already crowded (a message's "Edit = 21" collides with a notification level). The three **submenus were flattened**: roles and notify levels became ticked sections, which shows the current value that a submenu hid behind a hover; the quick reactions became a new `MK_EMOJIROW` — one row, per-glyph hit-boxes, colour font like the picker. | — |
| **WIN-80** | Custom DirectWrite composer, replacing RichEdit (ARCH-98) | P1 | XL |
| **WIN-81** | The GUI smoke does not run in CI — needs a self-hosted Windows runner | P2 | M |
| ~~**WIN-82**~~ | ~~Files' census only saw the 200-row page~~ **DONE.** `LIST_FILE_CHANNELS` — one `GROUP BY` over attachments with the same membership filter as `LIST_FILES`, over the index migration 0023 already added. The column is now exact, so the "From the 200 most recent files" caveat is deleted rather than reworded. Counts verified against the database directly (17 and 1). | — |
| **WIN-83** | User-defined custom sidebar sections (the other half of REQ-234) | P3 | M |
| **WIN-84** | In the unit-test harness, searches on the read connection see no rows | P2 | M |

### The items added 2026-07-30, in detail

- **WIN-73 — Later.** Two halves. (1) A message you saved shows a **bookmark glyph
  and a slightly inverse row tint** in the transcript, which it cannot do today
  because nothing told the client a message was saved: the list is fetched only when
  the Later view opens. **The plumbing for this has landed** — `oc_replay_msg`
  carries the requesting user's `saved`/`saved_at` (a `LEFT JOIN saved_items` keyed
  to that user, since a save is private where a pin is channel-wide), the net thread
  replays it as `SAVED_UPDATED` beside the existing pin loop, and `oc_msg.saved` is
  applied to the channel list *and* any open thread. No protocol bump was needed:
  the daemon already replays pin state the same way, which also corrects an earlier
  claim of mine that pins were lost on reconnect — they are not
  (`netloop.c` emits synthetic `PIN_UPDATED`). What remains is the drawing.
  (2) The **Later view takes the Files shape** (WIN-71): a left column defaulting to
  "All channels" then the channels holding bookmarks, newest-saved first, with
  counts. One asymmetry to state plainly — `LIST_SAVED` takes **no arguments**,
  unlike `LIST_FILES`, so picking a channel filters the fetched page client-side
  rather than re-asking the server. At the 200-item cap that is the whole list for
  almost everyone; making it exact means adding a channel argument, which is a wire
  change and belongs with WIN-38's bump.

- **WIN-74 — DM tabs.** Pins and Files & links are *correct* in a DM: `process_pin`
  and `LIST_FILES` require only membership, and a DM's two participants are members.
  **About is not.** It renders a TOPIC row with a "Set topic…" button, but
  `process_update_channel` rejects every DM outright (`OC_ERR_INVALID_CHANNEL`,
  commented "a DM has no name to rename, no topic worth setting") — so the button's
  only possible outcome is an error. Only the ADMIN block is gated on
  `kind != DM`; the topic row was missed. Drop the tab: a DM's "about" is the
  peer's profile, which is right-side coded already.

- **WIN-75 — the palette is invisible.** `palette_open()` has exactly two callers:
  Ctrl+K and the test hook. No menu entry, no button, no rail item. That sits badly
  with ARCH-82's affordance-driven rule — the palette is the one surface reachable
  only by a keystroke. Slack's equivalent is also clickable, via its title-bar
  search field.

- **WIN-76 — lists that stop.** The Files view and Later both draw rows until they
  run out of pane and then simply stop; Files at least *says* how many it could not
  show. Neither scrolls. The audit and webhook lists have not been checked.

- ~~**WIN-77 — the rest of the modal work.**~~ **DONE.** Five dialog idioms are one.
  `pane_header()` gives the six middle-column panes their own header with a real ✕ —
  the "Esc to close" caption was a keyboard fact standing in for a control, telling
  you a keystroke and then making you find it, while the target every other window in
  the OS puts there did not exist. Esc still works and is in the shortcut sheet, where
  a keyboard fact belongs. `confirm()` replaced the `MessageBoxW` calls; **one
  survives on purpose**, in `WM_CLOSE`, because quit must be answered before the
  window goes away and our own frame needs a message loop that is about to end. And
  `form_dialog`'s **16 call sites** now draw on the modal frame: our chrome, the
  platform's `EDIT`s. The GDI popup's window class, `prompt_proc` and its button
  drawing are deleted.
  **Verified**, and two of the three bugs were found by asserting rather than looking:
  Esc/Enter were dead in the form (a single-line EDIT eats both, and a key sent to the
  focused child never reaches the message loop — `form_edit_proc` answers them now),
  and the hint line was centred because `g_micro` is centre-aligned by construction.
  The smoke is 71 checks, up from 59, including that Cancel does not commit.

- **WIN-54b — the global notify default (REQ-134) is DONE.** Migration **0028** adds
  `users.notify_default`; `SET_NOTIFY_DEFAULT` (0x0077) sets it and the daemon answers
  with a full `NOTIFY_PREFS` snapshot, which now carries the default — a client that
  had to guess what "no per-channel row" means is a client that will eventually
  disagree with the server about whether your phone should ring. It sits at the top of
  the Notifications sheet, outside the scroller, above the channels that override it,
  and it obeys the modal frame's snapshot/restore like every other control there.
  **Two server-side bugs fell out of wiring it:** `oc_push_collect` hardcoded the
  fallback level to ALL, so "only mention me" would have applied to nothing but the
  channels you had touched individually; and it never consulted `np.muted`, so a muted
  channel at level ALL still pushed. Both now covered by `test_push.c`.

- **WIN-54a — the channel directory (REQ-038) is DONE.** No daemon work was needed,
  which is worth recording: `process_list_channels` has always returned every PUBLIC
  channel plus a `joined` flag, and the sidebar even lists the unjoined ones. What was
  missing was a place to see them together. "Browse channels…" in the New menu opens a
  `MODAL_LG` listing every public channel with its topic, **unjoined first** — what
  you can act on is why you opened it — with a primary **Join** or a plain **Open**.
  Joining does NOT close the sheet (a directory is somewhere you browse); Open does.
  **Not built:** member counts, because the channel list carries none — a count needs
  LIST_MEMBERS per channel — and a name filter, since the palette already searches
  conversations by name.

- **WIN-80 — the composer becomes ours.** Approved 2026-07-30 and recorded as
  **ARCH-98**, which amends ARCH-82's LOCKED choice of RichEdit. RichEdit was chosen
  for good reasons — editing, IME, selection, clipboard and undo for free — and all
  of that has to be **written by hand** now. What it buys: colour emoji in the field
  (RichEdit draws through GDI, so today the same character looks different in the
  composer than in the picker or on a reaction chip), WYSIWYG for REQ-220, and a
  surface we can make accessible for REQ-269. **The stated risk is IME**: it breaks
  silently for CJK input on a machine where the author will not notice, and it is the
  one part of this that cannot be self-verified. Lands behind the GUI smoke.

- **WIN-81 — the smoke is not in CI.** `scripts/gui_smoke.sh` asserts 59 chrome
  invariants and has caught a real regression, but it runs by hand: the daemon is
  epoll-based so it is Linux-only, and GitHub's Windows runners cannot host it (no
  Linux containers). Needs a self-hosted Windows box with the daemon reachable.
  Until then the pre-push gate is a human remembering, which is the same class of
  problem as everything else in this file.

- **WIN-84 — the read connection sees nothing, in the test harness only.** While
  building WIN-39's daemon test: six messages committed, then a search returns **0
  rows** — and stays empty across 40 retries over a second. A **filters-only** search
  (which never touches FTS) is empty too, so it is not an FTS problem: the read-only
  connection (ARCH-66) is not seeing the writer's committed rows at all. The identical
  generated SQL, captured from the running daemon and executed against the same
  database by hand, returns all six.
  **It does not reproduce against the live daemon** — the GUI's search returns 50
  results with `from:` applied correctly — so this is specific to the in-process test
  harness, where writer and reader share a process and a freshly created DB. Prime
  suspects: a leaked prepared statement pinning a read snapshot, or the reader opening
  before WAL is established. Recorded because a search that silently returns nothing
  is the worst failure mode a search can have, and because `test_search_filters_and_paging`
  had to be written around it — it asserts that filters build, execute and never widen
  a result, not absolute counts.

- **WIN-79 — two menu systems, one product.** Left-click dropdowns (workspace,
  profile, New, switcher, sidebar section) are drawn by the app. Right-click context
  menus are **native GDI popups**: four `TrackPopupMenu` sites — the message kebab,
  the member menu, the image-thumbnail kebab and the channel menu — built from seven
  `CreatePopupMenu` calls and 39 `AppendMenuW` items. The custom system's own header
  comment says "One reusable floating menu replaces the old `TrackPopupMenu`", so
  the replacement was started and abandoned half-way.

  It is not only that they look foreign (OS-themed, so light-on-dark in dark mode,
  and outside ARCH-97's type tokens). `TPM_RETURNCMD` **runs its own modal message
  loop**, with three consequences: our tick and repaints stop while a menu is up;
  the message-loop shortcuts from `SHORTCUTS[]` cannot fire; and **the harness cannot
  drive them at all** — which the code already concedes, since the `pin` test verb
  exists only because "the kebab's Pin item goes through a modal `TrackPopupMenu`
  the harness cannot navigate". Every context-menu action is therefore unverifiable
  except by hand.

  The one piece of real work hiding here: three of them have **submenus**
  (reactions, roles, notification levels) and the custom system has no submenu kind —
  only ITEM / SECTION / SEP. So this either adds one, or flattens them the way Slack
  does (a submenu becomes an item that opens a panel or dialog). Flattening is
  probably right for notification levels and roles; the quick-reaction row wants to
  stay inline.

- ~~**WIN-78 — Preferences.**~~ **DONE.** Two panes — **Appearance / Messages /
  Notifications / Advanced** — because fourteen rows are not one subject: how the app
  looks, how messages read, when it interrupts you and the machine-level escape
  hatches are four questions, and a flat list makes you re-read all of them to find
  one. New in Appearance: **accent colour** (four choices, each a dark/light PAIR —
  the bright blue that pops on navy is unreadable on white, which is why the two
  palettes differ in their accent at all, and picking a colour must not undo that),
  **text size**, and **message density** (which scales the block MARGINS only —
  shrinking the text to fit more messages is a text-size preference wearing the wrong
  name). Advanced carries **zoom**, the DPI override and Reset. Entry from the **You**
  menu and **Ctrl+,** as well as the workspace menu.
  ARCH-97's three multipliers stay three: text size follows the account, zoom is this
  window only and is deliberately NOT persisted, DPI belongs to the display.
  **Two bugs found by asserting rather than looking:** `key ctrl+=` was a silent
  no-op, because the verb's key parser fell through to `atoi("=")` = VK 0 and still
  acked "ok" — a harness lying in the one direction that matters; and a preference
  row's hint was clipped mid-word ("use zoom f") because the label ran to a fixed
  `right - 210` while the chips are as wide as their labels. The chips are measured
  first now and the text stops where they start.


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
| ~~**WIN-34**~~ | ~~Channel rename~~ **DONE.** REQ-036/ARCH-93: owner/admin, from the **About** tab. The id is untouched, so membership and history follow the rename; a collision reports `CHANNEL_EXISTS`. | — |
| ~~**WIN-35**~~ | ~~Channel topic / description~~ **DONE.** REQ-034/ARCH-93: any member sets it from the **About** tab; it renders on the channel header's second line, yielding to a typing indicator. | — |
| ~~**WIN-36**~~ | ~~Channel archive~~ **DONE.** REQ-035/ARCH-93: owner/admin, confirmed, reversible. Header badge, read-only composer that says why, and the daemon refuses every write path regardless. | — |
| ~~**WIN-37**~~ | ~~Channel details pane (members / pinned / files tabs)~~ **DONE.** A Slack-shaped channel tab strip under the header — **Messages · Files & links · Pins** — replacing the ad-hoc "Pinned"/"Members" header buttons, plus a compact member-count chip. The members pane now lists the **channel's** roster (REQ-031's new `LIST_MEMBERS`), not the tenant's. Completed by WIN-34–36, which added the fourth tab, **About**. | — |
| ~~**WIN-38**~~ | ~~Search paging~~ **DONE.** `SEARCH` gained `before_id` — a **keyset** cursor, not an offset, so a message posted while you page cannot make a row repeat or vanish. "Load more results" appears only when the server says there is more, sits after the last row, and appends. Its height counts toward the scroll total, because the first version drew it permanently just past the fold: reachable only in theory. Verified live: 50 results → 60 after one page. | — |
| ~~**WIN-39**~~ | ~~Search operators~~ **DONE.** `from:` / `in:` / `has:file|link|image` / `after:` / `before:`, parsed in **`shared/searchq.c`** — the `shared/mention.c` precedent, so the client's "Filters:" line and the daemon's WHERE clause cannot disagree. The wire carries **predicates, not a grammar**: these are column filters, and MATCHing `from:alice` as text would find messages that merely mention it. Dates resolve to epoch-ms **in the client**, because only it knows the timezone. Filters alone are a valid search (no words needed); an unknown `has:` value stays as search text rather than being silently dropped. | — |
| ~~**WIN-40**~~ | ~~Mute channel/DM~~ **DONE.** Migration 0026 adds `notification_prefs.muted` — a column, not a fourth level, because they answer different questions: `level` decides whether the daemon NOTIFIES you, mute also de-emphasises the row and drops its badge. The row dims and the count stops shouting; the count still exists server-side. Setting a level no longer wipes the mute: the upsert touches one column instead of `INSERT OR REPLACE`, which would have silently un-muted a conversation as a side effect. | — |
| ~~**WIN-41**~~ | ~~Starred conversations~~ **DONE** (the favourites half of REQ-234). A third **Starred** section, first on screen, holding channels and DMs alike — and each appears **once**: a starred conversation is lifted OUT of its normal section, as Slack does. Star/unstar from the channel menu; the set persists in the `client_settings` bucket appended to the existing sidebar string, so a bucket written by an older client still parses and simply has no stars. Collapsed automatically in the DMs view, where a starred #channel would contradict the point of that view. **User-defined custom sections are split out as WIN-83** rather than claimed here. | — |
| ~~**WIN-42**~~ | ~~Pin a message~~ **DONE.** REQ-230/ARCH-90 built in the daemon (migration 0022) and surfaced here: "Pin to channel" / "Unpin from channel" in the message kebab, a "Pinned by …" marker above the message inline, and a **Pinned** button in the channel header opening the list — each row jumps to the message in context, or unpins it. | — |
| ~~**WIN-43**~~ | ~~Save for later / the **Later** rail stub~~ **DONE.** REQ-231/ARCH-95: "Save for later" in the message menu, a **Later** view listing what you saved with Remove, and a click jumping to the message in its channel. Private — keyed (user, message), the mirror of a pin. | — |
| ~~**WIN-44**~~ | ~~Copy link / jump-to-permalink~~ **DONE.** The inbound half was all that was missing — `HISTORY_AROUND` and Copy link already existed. A permalink pasted into the **command palette** navigates: switch channel, arm the jump, fetch around the id if it is outside the loaded window (ARCH-96). Deliberately **not** the composer, where pasting must keep inserting the text, because sharing a link is the common case. Every failure says why — not signed in, another workspace, a channel you cannot see — since a link that silently does nothing is indistinguishable from a broken app. **Found while building it:** the outbound format omitted the port (`g_host` holds the host alone), so a link from a workspace on 8443 pointed at 443 and could not be followed. Having only one half of a feature is what hid that. | — |
| ~~**WIN-45**~~ | ~~Real `@mentions` (highlight + notify)~~ **DONE.** REQ-221/ARCH-89 built in the daemon (migration 0021, `shared/mention.c`) and surfaced here: mention spans are accent-coloured and semi-bold in `body_layout`, a message naming you tints its row and gets an accent bar, and the in-app `MENTIONS` notify level is now evaluated with the same scanner instead of being silently skipped. | — |
| ~~**WIN-46**~~ | ~~Invite management: pending list, expiry, revoke~~ **DONE.** `LIST_INVITES`/`REVOKE_INVITE` over columns migration 0002 already had, surfaced as **Admin → Invites**: role, an expiry **countdown** ("expires in 6d" — an epoch stamp does not answer the question), who minted it, and Revoke behind a themed confirmation. The token is absent by necessity and the pane says so: only its SHA-256 is stored, so a lost invite is revoked and re-minted, never re-shown. | — |
| **WIN-47** | Rich profile fields — ~~title, timezone~~ **(done)** · avatar image | REQ-240 — migration 0027 adds `title`, `timezone` and `avatar_attachment_id`; title and timezone are editable and carried in `PROFILE_INFO`. The **avatar image is not built**: the column and the design are there (an attachment id, so the existing store handles upload, dedup and reclamation) but the upload/render path is real work and is left honestly open |
| ~~**WIN-48**~~ | ~~Webhook rotate / enable-disable~~ **DONE.** Two opcodes (`SET_WEBHOOK_STATE`, `ROTATE_WEBHOOK`) over the `disabled` column that migration 0016 already had, and per-row **Disable/Enable · Rotate · Delete** buttons in the webhook list. Disable answers with the channel's list so the view cannot drift; rotate answers with the same shown-once frame create uses, and is **confirmed** because the old token dies immediately. Authorised by CHANNEL membership, not tenant role — a webhook belongs to a channel. **"Reveal" is struck from the item as impossible**: only the token's SHA-256 is stored, so the list says so in words rather than offering a button that cannot work. | — |
| ~~**WIN-49**~~ | ~~Activity feed — the **Activity** rail stub~~ **DONE.** REQ-139/ARCH-95: mentions, reactions to your messages and replies under your threads, newest first, with a marker on what arrived since you last looked. A union of three queries — no maintained list to drift. **Structured list-and-detail** (ARCH-94): the feed is the *second column* and the conversation stays in the middle, so clicking an item shows the thread from that point instead of replacing the transcript with a page you then have to leave. Filter by All / Mentions / Reactions / Threads. | — |
| ~~**WIN-50**~~ | ~~Files browser — the **Files** rail stub~~ **DONE.** The workspace-wide view over `LIST_FILES` with `channel_id 0`, which the daemon already answered — so this was a fetch and a header, not a protocol change. Each row names the channel it came from (the point of a cross-channel list) and clicking one switches channel and jumps to the message. Type filter (All / Images / Documents / Other) on both this and the channel tab, and the 200-row cap is stated rather than silently truncating. | — |
| ~~**WIN-51**~~ | ~~Forward / quote-share~~ **DONE.** "Forward…" in the message menu; the **palette picks the destination**, since it already lists every conversation with a filter. While a forward is pending it shows conversations ONLY and labels itself "Forward to…" — the action rows were listed first, so "Create a channel" sat under the selection and choosing it could only cancel. It sends a **quote, not a copy of the file**: `link_attachments` will only link an attachment whose `message_id IS NULL` and whose uploader and channel match the sender, so re-linking is impossible by design — which is also why forwarding cannot leak a private channel's file. **No IDOR here**, contrary to the plan's worry. The quote names any attachment and appends a permalink. | — |
| ~~**WIN-52**~~ | ~~Mark unread~~ **DONE.** "Unread from here" in the message menu — named for what it does, since the cursor moves to just BEFORE that message. It needs its own op (`SET_READ_CURSOR`) because `CLIENT_ACK` upserts `MAX(...)` so a replayed ack can never rewind a cursor; that is a correctness property, so marking unread is an explicit act with an explicit op rather than a relaxation of the ack path. The client then **re-asks the server** for the counts instead of recomputing the badge locally — one round trip for a number that cannot disagree with the daemon's. Ids are not dense, so it walks to the actual previous message rather than `mid - 1`. | — |
| ~~**WIN-53**~~ | ~~Custom status (emoji + text + expiry)~~ **DONE.** Migration 0027 on `users`; set from the profile menu with an expiry choice (30m / 1h / 4h / today), and shown dimmed after the name in the members pane. **The daemon enforces the expiry**, because a client that is not running cannot clear its own status — an expired one simply reads as absent, applied on read so no reader needs a clock. The client computes an ABSOLUTE expiry ("today" is local midnight, a different instant per user) so the daemon never has to know what "4 hours" meant. | — |
| **WIN-54** | ~~Per-channel roster~~ **(WIN-37)**, ~~browse-channels directory~~ **(done)**, ~~active-session list~~ **(done — REQ-182: `LIST_SESSIONS`, Active sessions in the profile menu, current device marked, last-seen + expiry; read-only because revoking ONE session has no op and "Sign out everywhere" already exists)**, ~~global notify default~~ **(done — REQ-134, WIN-54b: migration 0028 + `SET_NOTIFY_DEFAULT`, and it fixed two push bugs on the way)**, group DMs, custom emoji | REQ-031 listing op · REQ-134 · REQ-038 · REQ-056 · REQ-072 · REQ-182 has revoke but no list |
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

## Beyond the numbered list

Work done after §1/§2 closed, from reviewing the client against Slack rather
than against the backlog:

- **DPI awareness** — the process declared none and the render target was pinned
  to 96, so it shipped blurry on any scaled display. Per-monitor-v2, with the
  two boundaries that do not scale themselves (native children, mouse messages)
  converted by hand.
- **Application icon** — generated from the app's own mark plus the vendored
  Lucide bell; there was no icon at all.
- **Composer** — grows to four lines (Shift+Enter used to scroll out of sight),
  a placeholder naming the target conversation, a paper-plane send, and an `@`
  button so the mention trigger is an affordance and not just a keystroke.
- **Emoji consistency** — colour is content, monochrome is chrome. Message
  bodies never set `ENABLE_COLOR_FONT` while the picker did, so the same
  character looked different in two places. **Known limit:** the composer's own
  field renders emoji monochrome because RichEdit draws through GDI, which
  cannot do colour fonts; fixing it means replacing the input with a custom
  DirectWrite editor.
- **Presence dots** — two avatar tints were byte-identical to the online and
  away colours, so the dot vanished on those avatars. Palette split from the
  semantic colours, and dots ringed in their surface.
- **Image affordances** to Slack's pattern: the filename above the thumbnail,
  and a hover toolbar on the image with **save** and a **kebab** (view full
  size / save as / copy filename). The menu is only what works — Slack's
  copy-link, save-for-later and share map to REQ-232, REQ-231 and REQ-057,
  none of which exist, and four greyed-out entries is worse than three real
  ones.
- **Window placement** remembered across runs, **click-to-expand** for inline
  images, and **keyboard conversation movement** (Alt+Up/Down, Alt+Shift for
  unread only, F6 between composer and filter).
- **Accessibility is REQ-269**, recorded and deliberately unimplemented: the
  client answers no `WM_GETOBJECT`, so a screen reader sees a blank window.

## Known defects, unfixed

Recorded because a defect nobody wrote down is a defect nobody fixes — and a
defect **fixed** but never written down loses its lesson and comes back. The 2026-07-30
batch below was found and fixed inside one session; each is struck rather than
omitted, because the pattern across them is worth more than any single fix: **four of
the six asked the wrong question of the data.**

- ~~**2026-07-30 batch — six defects, mostly one mistake wearing different clothes.**~~
  **ALL FIXED.** No ids: each was found and closed the same day, and they are here for
  the pattern.
  - *Global shortcuts were dead while typing* (115fbde). Ctrl+K, Ctrl+F, Ctrl+/,
    Alt+arrows and F6 were handled in the main window's `WM_KEYDOWN`, which almost
    never has focus — the composer does, and a native child eats what it does not
    recognise. The shortcut sheet advertised them anyway, its comment claiming it
    "cannot drift from what the key handlers actually do". Now one `SHORTCUTS[]`
    table drives both the sheet and a message-loop dispatcher.
  - *Esc could not dismiss a menu, flyout or lightbox while typing* (55edb90) — same
    cause, found while fixing the above.
  - *The New menu floated mid-sidebar* (55edb90). It recomputed rail geometry from
    the window height and was wrong by a whole item; it now anchors to the rect the
    painter recorded.
  - *Uneven gaps inside a message group* (0c815a1), 38px then 32px. `MSG_BOT` asked
    "am I grouped?" when the space between two messages belongs to the **boundary** —
    the first message of a group is never grouped, so it always paid the wide margin.
  - *An attachment-only message reserved a line for a body it does not have*
    (60b7ff2) — `body_text()` returns a space so DirectWrite has something to
    measure, and both the height and the draw advanced by it.
  - *Three modal-frame bugs* (25817eb): the command palette survived a modal opening
    and **silently ate every click** (its box is hidden when covered, so it was
    invisible); an in-card click that matched no control fell through to the shell
    behind; and the first click after opening was swallowed because the repaint used
    `GetActiveWindow()`, NULL whenever the window is not foreground.

  **The pattern:** a hit-box that stored one axis, a margin that asked one message
  about a pair, a reservation for text that does not exist, a menu that re-derived
  geometry the painter already had. Ask the right thing, of the thing that knows.

- ~~**WIN-72 — hovering one column highlighted whatever shared its Y.**~~ **FIXED
  2026-07-29.** Reported from a screenshot: the pointer on an Activity row lit up
  the transcript message on the same line. `oc_msgrow` stored `top`/`bot` and no
  x at all, so every test against it matched the full width of the window —
  hover, right-click (a message menu from the members pane) and the start of a
  text selection. **This is WIN-66 exactly, in a second place:** a hit-box that
  records one axis will be asked about both. Rows now carry `left`/`right` from
  the pane that drew them, `msgrow_at()` takes x, and the y-only variant is kept
  only for drag-extend, where leaving the pane should still extend the selection.
  Thread replies got the same treatment. Verified from the dump, which now
  reports the row band and the live hover id — the screenshot that found it
  could not have told me whether the fix worked, because hover does not survive
  the round trip to a render.

- ~~**WIN-70 — native children leaked into views they do not belong to.**~~
  **FIXED 2026-07-29 — audited globally, not patched again.** After the third
  occurrence the whole family was reviewed: six native children (composer, find
  box, search box, emoji-picker box, palette box, sign-in fields), each deciding
  its own visibility at its own site, each asking a slightly different question.
  The audit found a **second** live instance nobody had reported — the composer
  stayed usable in the DMs index, where there is no conversation to type into —
  and one latent one, the search box being a middle-column overlay that a modal
  would have punched through. All six are now decided in `layout_natives()` from
  the same state the painter uses, against three shared predicates:
  `sidebar_kind()` (what is in the second column), `main_is_conversation()` (is
  the middle column something you can type into) and `window_is_covered()`
  (does something own the whole window). A fourth fix came with it: hiding the
  composer's native child had left its box and buttons still *painted*, so the
  drawing now asks the same question the control does. Original report: The "Find a
  conversation" EDIT is a native child, so it composites **above** the Direct2D
  output: any view where it is not hidden shows a bare rectangle over whatever is
  really drawn there. Its visibility asked `view_has_sidebar()` — "does this view
  have a second column" — when the question it must answer is "is the **channel
  list** what is in that column". Those were the same thing until that column
  started hosting other lists, and the difference cost the same bug three times:
  the workspace menu, then the DMs list, then Activity. Each was patched with
  another `&& g_view != …`. It now asks `sidebar_kind()`, which names the
  column's tenant, and the painter switches on the same function — so a new
  tenant must declare itself in one place rather than silently inheriting the
  previous one's chrome.

- ~~**WIN-61 — a deleted message keeps its reaction chips and its attachment.**~~
  **FIXED 2026-07-29** on both sides: the client tombstones the message in the
  model (clearing reactions, attachments and any pin, in the thread list as well
  as the channel list), and the daemon detaches the attachments — `message_id`
  NULL, which is the *orphan* state its storage-maintenance sweep already
  collects, so the blob is reclaimed by a path already written and tested.
  Regression-tested on both sides. Original report kept below.

  ~~Original report:~~
  Observed 2026-07-28: a tombstone rendered "(message deleted)" with 👍1 😮1 still
  under it and `notes.txt` still attached. Three distinct faults behind one
  screenshot:
  1. **Client (this repo, `client/core/model.c`).** `OC_EV_DELETE` sets
     `deleted = 1` and nothing else, so the model keeps `reactions` and `attach`
     for a message whose body is gone. The daemon *did* delete the reaction rows —
     the client is showing state the server no longer has, which is worse than a
     cosmetic bug because a reload would disagree with the live view.
  2. **Daemon (REQ-052).** `process_delete` drops `pins` and `reactions` but
     **not `attachments`** — so the row and its blob survive a tombstone. That is
     a storage leak as well as a UI one, and it is the same class of miss the pin
     and reaction cleanups already fix beside it.
  3. **A tombstone should also lose its thread replies' claim on it** — worth
     checking while in there; not yet verified either way.
- ~~**WIN-63 — reaction chips sit above the attachments, not below them.**~~
  **FIXED 2026-07-29** — chips are drawn last, under everything the message
  carries. Only `draw_message` needed changing: `msg_height` counts the meta
  lines, and reordering them does not change the total, which is why the
  backlog's original note about "one reordering applied twice" was wrong.

  ~~Original report:~~
  `draw_message` lays a block out as body → chips → attachments/thumbnails →
  thread line, so on a message with an image the reactions are stranded in the
  middle. They belong at the **bottom of the block**, under everything the
  message carries, which is where every reference client puts them and where the
  eye expects the "footer" of a message. Both `msg_height` and `draw_message`
  order the meta lines the same way, so the fix is one reordering applied twice —
  and they must stay in step or the transcript's hit-boxes drift from what is
  drawn.
- ~~**WIN-69 — the DMs rail item was Home with a section folded.**~~ **FIXED
  2026-07-29.** `VIEW_DMS` differed from Home by one line (`collapsed[CHANNELS]`),
  which is a bookmark, not a destination. Slack's earns its place by being a
  different *kind* of list — person-centric, every row a human, including people
  you have never messaged, so it doubles as the start-a-conversation surface.
  Ours now does the same over the roster we already hold: conversations first,
  everyone else below, picking anyone opens or creates the DM and lands you in
  it. It also gives **self-DM** (REQ-055) its first surface in any client.
- ~~**WIN-68 — the Admin rail item was a signpost to somewhere else.**~~
  **FIXED 2026-07-29.** It rendered "Storage & audit — open from the workspace
  menu": a destination whose entire content was directions. Both reports were
  already built, so Admin is now a real view with **Storage** and **Audit log**
  tabs, refetched on entry because a stale point-in-time report is worse than a
  moment's wait. The reports gained an `embedded` mode so they do not draw a
  second header offering "Esc to close" for something Esc does not close.
  Webhooks moved to the channel's **About** tab at the same time — channel-scoped
  admin belongs with the channel, not behind a right-click.
- ~~**WIN-67 — surfaces opened in whichever pane happened to be handy.**~~
  **FIXED 2026-07-29 (ARCH-94).** A person's profile replaced the *conversation*,
  which cost you your scroll position and closed any open thread, and your own
  preferences opened in the same slot as messages. Now: middle = the
  conversation and everything channel-scoped, right = the **context pane**
  (members · a person's card · who reacted, with one level of back), and your own
  account (preferences, shortcuts, workspaces, notification settings) is a
  **modal** over a dimmed shell. The pane widened 220 → 300 because a profile did
  not fit; the profile card was relaid out vertically for it.
- ~~**WIN-66 — the profile pane opened by itself.**~~ **FIXED 2026-07-29.** The
  members-pane hit test compared **y only** — `g_memrows` stored `top`/`bot` and
  no x — so a click anywhere across the window at a member row's height opened
  that person's profile over the transcript. With three members that made the top
  ~100px of the *whole* window a profile trap, which is why it seemed to happen
  at random. The right-click handler had the identical defect and opened the
  member menu the same way; the compiler found it when the struct changed. Rows
  now store the full rect. The rail and sidebar were never affected — both guard
  on x at the call site, so this was an inconsistency, not a pattern.
- ~~**WIN-62 — reaction chips are not clickable.**~~ **FIXED 2026-07-29.**
  Clicking a chip +1s it, clicking one that is already yours undoes it —
  direction from `reaction_is_mine`, the same rule the message menu uses, so the
  two entry points cannot disagree. Verified live in both directions.

  ~~Original report:~~ The chips render but have no
  hit-boxes, so the only way to react is the message menu. Clicking a chip should
  **+1 it**, and clicking one you are already part of should **undo** your
  reaction. The app-core already supports both (`oc_client_react` with
  add/remove, and `reaction_is_mine` picks the direction — the menu uses exactly
  that); this is a hit-testing gap in the GUI, not a protocol one.

- **WIN-60 — unreproduced crash while typing.** *Now diagnosable, 2026-07-29.*
  It stayed unreproduced because a crash left **nothing behind** — no dump, no log,
  no record of what the app was doing — and it happened twice more today while the
  client was being driven, with "it exited" as the only evidence. That is an
  anecdote, not a bug report.
  The client now installs an unhandled-exception filter (`crash_filter`) that
  writes `crash-<pid>.txt`: exception code, faulting address, module base and the
  **RVA** between them, read/write and the target address for an access violation,
  view/DPI/authed state, and the last 64 **breadcrumbs** with ms-before-crash
  (hook commands, channel switches, clicks, sends, autocomplete accepts, uploads).
  A minidump is written beside it when dbghelp is present — and the report *says*
  whether one exists, because a zero-length `.dmp` looks like evidence and is not
  (the first version produced exactly that). Reports land in the test dir while the
  harness is driving, `%LOCALAPPDATA%\OpenChime` otherwise.
  `scripts/crash_resolve.sh <report>` turns the RVA into a file and line;
  `WIN_CFLAGS` gained `-g` for it. Verified with a deliberate fault behind the test
  hook (`crashtest`), resolving to the exact line — an untested crash handler is
  the one piece of code certain to run for the first time at the worst moment.
  **Still unfixed**, and there is one more sighting: on 2026-07-30 the client
  exited during harness driving and wrote **no report at all**, which means either a
  clean exit path triggered by accident or a fault that bypasses
  `SetUnhandledExceptionFilter` (a stack overflow or a fail-fast, both of which do).
  Worth knowing before trusting the instrumentation. Original report: The client was seen to exit once
  during composer input. It has not reproduced since, there is no dump, and the
  build is warning-free, so there is nothing yet to point at. Left open
  deliberately rather than closed as "could not reproduce": the composer is a
  RichEdit child with hand-written subclassing, autocomplete and draft
  save/restore all touching the same buffer, which is the most likely place for a
  latent lifetime bug. **Next step when it recurs:** run under a debugger with
  `gflags` page-heap on the exe, and capture the crash dump before restarting.
- **WIN-18's balloon rendering is unobserved on this host** — not a defect, an
  environment limit. The API path is verified positively (`tray_live=1`,
  `toasts_raised` increments); the dev host has no attached display surface, so
  nothing that draws to the shell can be seen or captured here. See WIN-18.

## Where this stands

**§1 and §2 are complete** — every item that was buildable in `client/gui/win32/`
or needed only a small `client/core` change is done and was verified running on
Windows, not merely compiled. So are the §3 items whose blocker turned out to be
client-side rather than daemon-side: WIN-55 (a core field), WIN-56/58 (the
credential store), WIN-57 (unblocked by WIN-29), and WIN-59.

**Everything still open is §3, and every one of them is blocked on a server
feature that does not exist.** Not "hard" — absent: there is no rename op
(REQ-036), no topic column (REQ-034), no archive flag (REQ-035), no search
cursor on the wire, no per-user mute storage (REQ-137), no permalinks
(REQ-232). Each needs its REQ built in the daemon first; none is
startable in this client. §4 remains deliberately out of scope.

**WIN-42 and WIN-45 are the proof of that pattern, both now closed:** REQ-221 was built in the
daemon first (migration 0021 plus a scanner in `shared/` that both sides link),
and the Win32 half — highlight, row tint, notify level — was a fraction of the
work; REQ-230 went the same way a day later. **So the next move is still not in
this document.** Picking up WIN-49 means implementing REQ-139 in the daemon; the
Win32 work that follows is small by comparison. The exception is WIN-48, which needs
only a `created_at` field on `WEBHOOK_LIST` plus reveal/rotate ops — the
cheapest remaining unblock.

Update [STATUS.md](./STATUS.md)'s parity table when an item changes a ✅/🔨 mark,
and strike the corresponding line in CLIENT_GAP_ANALYSIS.md §4 when one closes.
