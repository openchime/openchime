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
| **WIN-14** | **New-message divider + jump-to-unread (REQ-236).** *Today:* neither. `oc_channel.read_marker` already exists, and REQ-236 is explicitly client-derived — no server change needed. *Done:* a "New" separator at the read marker and a jump affordance on entering a channel with unread. | P1 | M |
| **WIN-15** | **Per-reply actions inside threads + thread scrollbar.** *Today:* the reply composer works but replies are read-only and the overlay cannot scroll. Replies are ordinary `oc_msg` with ids, so react/edit/delete already apply. *Done:* the full message action menu inside a thread, and a scrollbar. | P1 | M |
| **WIN-16** | **Transcript paging past the 600-message cap.** *Today:* `enum { CAP = 600 }` with no load-more; older history is unreachable in a busy channel. *Done:* scroll-to-top loads the previous page via backfill. | P1 | M |
| **WIN-17** | **Inline image thumbnails (REQ-142).** *Today:* attachments render as text lines only. The download path exists and WIC is already linked. *Done:* inline thumbnails for common image types, click to expand; typed placeholder otherwise. | P1 | M |
| **WIN-18** | **OS toast notifications (REQ-138).** *Today:* no OS-level notification at all. This is per-client rendering of the server's existing notify decision (ARCH-72), not a new server surface. *Done:* native toasts honouring the channel level and DND, with a content-preview toggle. | P1 | M |
| ~~**WIN-19**~~ | ~~**Audit-log paging + filters.**~~ **DONE.** The log scrolls, family filter chips (All / Admin / Account / Security / Moderation) narrow it, and reaching the bottom re-queries with the oldest paged-in timestamp as the cursor. **Caveat:** the filter is client-side over what has been paged in — it narrows what you are looking at, it does not re-query by family. Actor/action filters would need a server-side query parameter. | P1 | S |
| ~~**WIN-20**~~ | ~~**Real profile editor.**~~ **DONE.** One dialog each: display name with a note on where it appears, and current/new/**confirm** password with a mismatch check. The chained prompts had no confirm field at all. | P1 | S |
| ~~**WIN-21**~~ | ~~**Purpose-built dialogs, retiring `text_prompt`.**~~ **DONE.** All seven flows moved to the typed `form_dialog()` and **`text_prompt` is deleted**. New DM also now says "No such user in this workspace" instead of silently doing nothing on an unknown name. | P1 | S |
| ~~**WIN-22**~~ | ~~**Invite dialog with a copyable token.**~~ **DONE.** Invite and webhook tokens now appear in a selectable field, are **put on the clipboard the moment they are shown**, and say plainly that they will not be shown again and what to do if lost. A MessageBox could not be selected from — the one thing that must not be true of a value shown exactly once. Expiry/pending/revoke stay WIN-46. | P1 | S |
| ~~**WIN-23**~~ | ~~**Webhooks overlay depth.**~~ **DONE**, except the date. Scrolling, and an active/disabled chip so an enabled hook is positively marked rather than merely lacking the word "(disabled)". The delete confirmation already existed. **No created-date column is possible:** `WEBHOOK_LIST` carries only id / channel / label / disabled, so that part needs a wire field — folded into WIN-48. | P2 | S |
| ~~**WIN-24**~~ | ~~**Storage overlay refresh + TUI parity.**~~ **DONE.** Grouped into Disk / Policy / Reclaimed like the TUI, free-of-total on one line, pressure and any evictions flagged in red, and a Refresh button — there was previously no way to ask again. | P2 | S |
| ~~**WIN-25**~~ | ~~**Keyboard-shortcut reference (REQ-264).**~~ **DONE.** A sheet generated from a single `KEYMAP` table, on Ctrl+/ and in the workspace menu. | P2 | S |
| ~~**WIN-26**~~ | ~~**Theme / appearance selection (REQ-262).**~~ **DONE.** `theme.h`'s literals became a runtime `oc_theme[]` array behind the same `OC_COL_*` spellings, so every call site is unchanged and a switch takes effect on the next frame. Dark, light, and match-system (reading `AppsUseLightTheme`, defaulting to dark when unreadable). The light palette inverts the *structure*, not just the brightness — the rail stays the darkest surface — and native children (RichEdit, EDIT brushes) are re-skinned on every switch, since they keep their own colours and otherwise stay dark on a light shell. Persisted with the other preferences. | P2 | M |
| **WIN-27** | **Draft persistence (REQ-223).** *Today:* switching channels discards a half-typed message. **ARCH-88 narrows the options**: a client writes no files, so a draft is either in-memory only (survives a channel switch, not a restart) or server-synced via the `client_settings` bucket. In-memory is the cheap half and needs no ARCH decision; cross-restart drafts now *require* the synced route. | P2 | M |
| ~~**WIN-28**~~ | ~~**Configurable quick reactions (REQ-073).**~~ **DONE.** The six literals became a preference holding **shortcodes**, resolved through the shared catalogue (`oc_emoji_by_name`) so an unknown name drops out rather than writing a broken glyph, with a fallback if the set ends up empty. Edited from Preferences, persisted in the `gui` bucket. Offered in the message menu; inline-on-hover is not done. | P2 | S |
| **WIN-29** | **N-concurrent-workspace model (REQ-012–015).** *Today:* the rail switcher stop/reconnects a **single** `oc_client`, so a background workspace receives nothing and accrues no unread. *Done:* hold N clients, tick all, render one — the TUI's model — with per-row unread and an "N elsewhere" badge. The largest single item here. | P1 | L |

## §2 Ready now, with a small app-core change

The wire already carries these; `client/core` is the only thing in the way.

| # | Item | Core change needed | Pri | Size |
|---|---|---|---|---|
| ~~**WIN-30**~~ | ~~**Create-channel dialog with visibility.**~~ **DONE.** `oc_client_create_channel_ex(c, name, is_public)` added and the dialog offers Public/Private. Verified end-to-end: a private channel lands with `is_public=0` and renders with the lock in the sidebar. | Done | P1 | S |
| **WIN-31** | **Channel member management.** `INVITE_TO_CHANNEL` / `REMOVE_FROM_CHANNEL` exist on the wire but in no client. | Add both intents | P1 | M |
| **WIN-32** | **Signup / first-owner onboarding (REQ-268).** `REDEEM_INVITE` exists on the wire but in no client, so bringing up a tenant still needs a command line. | Add the redeem intent | P1 | M |
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
| **WIN-55** | Live reconnect countdown in the banner | Core gap: the net thread pushes `reconnecting in Ns…` once per backoff as sticky text; a ticking countdown needs a retry-deadline field on `oc_model` |
| ~~**WIN-56**~~ | ~~**Store the session token in Windows Credential Manager**~~ **DONE.** `client/shared/secret_win.c` implements the `oc_secret` seam over `CredWriteW`/`CredReadW`/`CredDeleteW` (one generic credential per workspace, `CRED_PERSIST_LOCAL_MACHINE`), wired into both Windows front-ends via the new shared `oc_secret_open_os()`. **The core now refuses to persist a token to SQLite at all** and erases any left by an older build, so there is no plaintext fallback anywhere. Verified on Windows: the credential appears in `cmdkey /list`, `workspace_state.session_token` is NULL, and silent reconnect reads from the OS store. |
| **WIN-57** | Sign in to *every* remembered workspace at boot | WIN-29 — Win32 holds one `oc_client`, so boot connects only the most-recently-used workspace that has a token, not all of them |
| ~~**WIN-59**~~ | ~~Warn on quit while the in-memory outbox is non-empty~~ **DONE.** `WM_CLOSE` checks `oc_client_outbox_pending()` and confirms before discarding queued sends. |
| ~~**WIN-58**~~ | ~~Move the workspace book out of SQLite~~ **DONE** with ARCH-88 — it lives in the credential store, found by enumeration, so the book needs no storage of its own. |

## §4 Not planned for this client

- **Audio and screenshare** (REQ-150–152, REQ-161) — a separate epic sequenced behind the audio client ([AUDIO.md](./AUDIO.md), [VIDEO.md](./VIDEO.md)), not Win32 depth work.
- **Push device registration** (REQ-132) — `REGISTER_DEVICE_TOKEN` reaches no client, but push targets mobile; a desktop client would use WIN-18 instead.
- **Slash commands** — excluded by design; the GUI is affordance-driven (ARCH-82).
- **GIF/sticker pickers, Canvas, Lists, Clips, Slack Connect, first-party bot/MCP** — REQ-270–275, explicit product exclusions.

---

## Suggested order

The first six are [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §5's top
items, re-expressed as ids and re-checked against the tree:

~~WIN-1~~, ~~WIN-2~~ (done — failures are no longer silent anywhere) **→ WIN-3** (search stops lying about where a hit is) → **WIN-5 + WIN-6**
(the sidebar becomes navigable) → **WIN-7** (the core chat affordance) →
**WIN-9** (settings, replacing the "coming soon" box) → **WIN-10**.

After that the cheap wins cluster — WIN-12, WIN-13, WIN-19, WIN-20, WIN-22 are
all S and close visible 🔨/🔸 marks — with **WIN-29** as the one large piece worth
scheduling deliberately rather than squeezing in.

Update [STATUS.md](./STATUS.md)'s parity table when an item changes a ✅/🔨 mark,
and strike the corresponding line in CLIENT_GAP_ANALYSIS.md §4 when one closes.
