# OpenChime Four-Way Client Gap Analysis — Slack vs Pumble vs OpenChime TUI vs OpenChime Win32 GUI

*Definitive inventory of every user-facing screen/dialog/panel/surface and feature across the clients. Ground truth for TUI/Win32 is the code inventories; **Slack** and **Pumble** are the researched reference surfaces.*

**Re-verification (2026-07-28).** The OpenChime columns were re-certified against the code in full (see the commit trail). The **reference columns were re-checked too**, category by category, against Slack's help centre and API reference and Pumble's help centre, feature list and pricing page:

- **Confirmed unchanged:** Slack's messaging actions (mark unread, save for later, remind me, scheduled send, forward), channel management (topic/description, rename, archive, custom sidebar sections, bookmarks), the tabbed channel layout, custom-status expiry, DND defaults and notification schedules, keyword notifications, the `from:`/`in:`/`has:` search grammar with its four result tabs, SAML SSO with Google Workspace SSO (and that Google Auth is unavailable in Enterprise orgs), and Enterprise audit logs. On Pumble: threads, pins (also 100/channel), guest access with channel scoping and time limits, SSO via SAML2/OAuth2, customizable sections, data retention, and the search filter set.
- **Corrected:** three factual errors. (1) Slack's free tier does not merely cap history at 90 days — it **permanently deletes** anything over a year old. (2) **Slack's SAML starts at Business+, not Pro** — this document had it one tier too low in three places, including inside §7's correction of *Pumble's* marketing, so our correction needed correcting. (3) Pumble's search **sorts** by relevancy/newest/oldest, moving that cell from ❔ to ✅. Pumble's plan gating re-verified exactly against its pricing page: SSO and data retention at Enterprise, guests and user groups at Business (with the "5 free single-channel guests per paid seat" allowance), screen share at Pro.
- **The remaining ❔ cells were re-checked and stay ❔.** That is a finding, not an omission: Pumble's public documentation does not describe them, which is precisely what ❔ means in the legend above. They should not be read as "Pumble lacks this."

Vendor-facing rows are inherently perishable — a competitor can change a plan the day after this is written — so each carries the date it was checked rather than an implied "current".

**Pumble sourcing (researched 2026-07-26).** Pumble is a Slack-shaped SaaS team chat product from COING (the makers of Clockify and Plaky, sold together as the CAKE.com bundle). Its column below is built from Pumble's own pricing page, feature pages, and help centre, cross-checked against independent reviews (Cloudwards) and comparison directories (Capterra, GetApp, TrustRadius). **Vendor-authored comparison content was treated as a claim, not evidence** — where Pumble's marketing and its own help centre disagreed, the help centre won. Two such corrections are recorded in §7.

> **Reconciled 2026-07-30.** Much of what this document calls thin has since
> shipped: group DMs, custom emoji, avatars, user-defined sidebar sections, the
> global notification default, channel visibility, two-pane preferences with a
> colour scheme and text size, and a custom composer. [STATUS.md](./STATUS.md) is
> the authoritative per-requirement view; the rows here are kept for the *analysis*
> — why each gap mattered — rather than as a live tracker. Where the two disagree,
> STATUS.md is right.

## 1. Legend

| Glyph | Meaning |
|-------|---------|
| ✅ | Rich — fully developed, near-parity |
| 🟡 | Adequate — functional, real gaps in depth/polish |
| 🔸 | Thin — exists but read-only, non-navigable, or heavily limited |
| ⚪ | Stub — placeholder / minimal / raw-text stand-in |
| ❌ | Missing — not present at all |
| ⛔ | Out of scope by design — intentionally not on OpenChime's wire |
| ❔ | (Pumble only) not verified in public documentation — absence of evidence, not evidence of absence |

**Plan markers (Pumble column):** ᶠ free · ᴾ Pro · ᴮ Business · ᴱ Enterprise — the cheapest plan on which the feature appears. No marker = all plans.

**Priority key (in gap notes):** P0 = core parity, must-have · P1 = important · P2 = nice-to-have · — = n/a.

---

## 2. Category Tables

### 2.1 Messaging

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Message composer (basic send) | ✅ | ✅ | 🟡 single-line only | ✅ RichEdit, Enter-send + button | TUI needs multi-line composer. **P1** |
| Rich-text formatting toolbar (bold/italic/strike/code) | ✅ | ✅ | ❌ | ❌ | No WYSIWYG or markup toolbar in either client. **P2** |
| Markdown/mrkdwn shortcuts | ✅ | ❔ | ❌ | ❌ | No inline `*bold*`/`_italic_` auto-convert. **P2** |
| Ordered/unordered lists, blockquote, code block buttons | ✅ | ✅ | ❌ | ❌ | — **P2** |
| Hyperlink insertion dialog | ✅ | ❔ | ❌ | ❌ | **P2** |
| Block Kit rich layout | ✅ | 🔸 app SDK, no Block-Kit equivalent | ⛔ | ⛔ | App-authored blocks — out of scope (no app platform). |
| Emoji picker (composer) | ✅ full Unicode + search + skin tone | ✅ | 🟡 shared catalogue + search | 🟡 shared catalogue + search | Both now use the app-core catalogue (`oc_emoji_all`, ~179 entries, categories + search). Short of full Unicode and no skin tone. **P2** |
| Custom / workspace emoji | ✅ | ✅ (web/desktop only; perms ᴾ) | ❌ | ❌ | Not surfaced. **P2** |
| Colon emoji autocomplete (`:smile:`) | ✅ | ✅ | 🟡 in autocomplete strip | 🟡 in autocomplete panel | Both drive `oc_complete` (`OC_AC_EMOJI`). — |
| @mentions + autocomplete | ✅ | ✅ | 🟡 @user autocomplete, no highlight/notify | ✅ autocomplete + `@` button, accent-highlighted spans, row tint, notify | **Real mentions now exist on the engine** (REQ-221/ARCH-89): the daemon resolves names against the channel roster and gates the MENTIONS notify level. Win32 surfaces all of it; the TUI still only autocompletes the text. **P1 (TUI)** |
| @here/@channel/@everyone broadcast | ✅ | ✅ + user groups ᴮ | 🔸 text only | ✅ recognised + highlighted | Built (REQ-221): all three are reserved audiences stored as broadcast rows. Known limit — `@here` is treated like `@channel` for **push**, because presence is invisible to the push worker (ARCH-66). No user groups. **P1 (TUI)** |
| Channel mentions (#chan autocomplete) | ✅ | ✅ | 🟡 #chan autocomplete | 🟡 #chan autocomplete | Both drive `oc_complete` (`OC_AC_CHANNEL`). A `#channel` reference is **not** a link in either — it completes text only. **P2** |
| Slash commands | ✅ | ✅ native commands (`/status`, `/clear-status`, …) | ❌ (dispatcher deleted by design) | ❌ | Both references are slash-driven; OpenChime is affordance-driven — **by design**, but note stale slash-hint text still shown in TUI webhooks overlay (bug). — |
| Message drafts (autosave) | ✅ | ✅ | ❌ | 🔸 per-channel, in memory only | Win32 saves/restores a draft when you switch channels (`draft_save`/`draft_restore`, 32 slots); nothing survives a restart, and a stateless client (ARCH-88) cannot persist one locally — a real drafts feature needs server storage. **P1** |
| Drafts & sent hub | ✅ | ✅ "Drafts & scheduled" sidebar | ❌ | ❌ | **P2** |
| Scheduled send | ✅ | ✅ incl. in threads + **recurring messages** | ❌ | ❌ | **P2** |
| Edit message | ✅ | ✅ | 🟡 edit flow | 🟡 (edited) shown; edit via context | TUI/Win32 both edit; thread replies NOT editable in Win32. **P1** |
| Delete message | ✅ | ✅ | 🔸 NO confirmation | 🟡 via context menu | Add destructive confirm in TUI. **P1** |
| Reactions (emoji) | ✅ any emoji | ✅ any emoji + customizable quick reactions | 🟡 (hardcoded set) | 🟡 6 hardcoded emoji | Both OpenChime clients limited to hardcoded reaction sets. **P1** |
| Who-reacted list | ✅ | ✅ | 🔸 read-only | 🔸 read-only overlay | Read-only both. **P2** |
| Quick/one-click reactions | ✅ top-3 inline | ✅ customizable | ❌ | ❌ | **P2** |
| Copy link / permalink | ✅ | ❔ | ❌ | ❌ no copy-link | **P1** |
| Copy text | ✅ | ✅ | 🔸 terminal-native only | ✅ in-app text-select + Ctrl+C | TUI has **no** in-app copy — relies on the terminal emulator's mouse selection (no clipboard code). Win32 has real in-app selection+copy (ARCH-82). **P2** |
| Forward / share message | ✅ | ✅ quote message | ❌ | ❌ no forward | **P2** |
| Pin message | ✅ 100/channel | ✅ 100/channel | ❌ | ✅ pin/unpin + inline "Pinned by" marker + Pins tab, 100/channel | Built (REQ-230/ARCH-90): channel-scoped, any member may pin or unpin, 100/channel, survives reconnect. **P1 (TUI)** |
| Save for later / bookmark | ✅ | ✅ Saved Items (sidebar bookmark) | ❌ | ✅ "Save for later" + the **Later** view | Built (REQ-231/ARCH-95): private, keyed (user, message) — the mirror of a pin. **P2 (TUI)** |
| Mark unread | ✅ | ✅ | ❌ | ❌ no mark-unread | **P1** |
| Remind me about this | ✅ | ✅ reminders | ❌ | ❌ | **P2** |
| Message actions/shortcuts (app) | ✅ | 🟡 custom apps / MCP server | ⛔ | ⛔ | App shortcuts out of scope. |
| Text/code snippets | ✅ | ❔ | ❌ | ❌ | **P2** |
| Giphy/GIF | ✅ | ✅ GIF support + preview controls | ⛔ | ⛔ | App-provided in Slack — out of scope for us. |
| Link unfurling / rich previews | ✅ | ✅ incl. in the editor | ❌ | ❌ | **P2** |
| Inline image/thumbnail render | ✅ | ✅ (+ media carousel) | ❌ (attachment lines only, TUI exempt by ARCH-75) | ✅ inline thumbnails + click-to-expand lightbox + hover save/kebab | Win32 built (WIC decode, 160px box, space reserved before decode so the transcript does not jump). **— (TUI exempt)** |
| Voice / video messages | ✅ clips | ✅ | ⛔ | ⛔ | Recording out of scope (AUDIO.md is live calls only). |
| Polls | 🔸 via apps | ✅ native | ❌ | ❌ | **P2** |
| Typing indicators | ✅ | ✅ incl. in threads | 🟡 | ✅/🟡 | Present. — |
| Unread divider ("New" line) / jump-to-unread | ✅ | ✅ | 🟡 (unread markers) | ✅ "New" divider + "N new ↑" header jump | Win32 built (WIN-14); the marker is snapshotted on entry so it survives reading the channel. TUI has markers but no divider or jump. **P2 (TUI)** |
| Message action menu (host) | ✅ | ✅ customizable quick actions | 🟡 | 🟡 6 quick emoji + More…, react/thread/copy/edit/delete/**pin**/who-reacted/download | Pin landed with REQ-230; still no copy-link (needs permalinks REQ-232) or forward. **P2** |
| Mark all read | ✅ Shift+Esc | ✅ | ❌ | 🟡 "Mark all as read" in the workspace menu + command palette | No keyboard shortcut for it in Win32. **P2** |
| Catch-up / digest | ✅ | ✅ "Catch Up" (mobile) | ❌ | ❌ | **P2** |
| Transcript rendering | ✅ | ✅ | ✅ wrapped, colors, times, (edited), tombstones, reactions, attach lines, N replies, seen-by | ✅ grouped, avatars, dividers, seen-by, scrollbar; 600-msg cap | Both RICH. Win32 caps at 600 msgs w/ no load-more. **P1** (paging) |

### 2.2 Threads

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Threaded replies | ✅ | ✅ full actions in-thread | 🔸 read-only dump, no per-reply actions | 🟡 reply composer works, replies read-only | Neither OpenChime client allows react/edit/delete inside thread. **P1** |
| Also-send-to-channel | ✅ | ❔ | ❌ | ❌ | **P2** |
| Threads view (all threads) | ✅ | ✅ Threads sidebar | ❌ | ❌ | Aggregated followed-threads view absent. **P2** |
| Follow/unfollow thread | ✅ | ❔ | ❌ | ❌ | **P2** |
| Participant avatars + reply count | ✅ | ✅ | 🟡 N replies shown | 🟡 shown | Present inline. — |
| Typing indicator inside a thread | ✅ | ✅ | ❌ | ❌ | **P2** |
| Scheduled send inside a thread | ✅ | ✅ | ❌ | ❌ | **P2** |
| Thread overlay scrollbar | ✅ | ✅ | 🔸 none | 🔸 no scrollbar | Both OpenChime clients un-scrollable. **P1** |

### 2.3 Channels

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Public channels | ✅ | ✅ | ✅ grouped Public section | 🟡 flat list | Win32 has no sectioning. **P1** |
| Private channels | ✅ | ✅ | ✅ Private group + lock marker | 🟡 flat, lock unclear | **P1** |
| Create channel | ✅ name+desc+visibility+members | ✅ | 🟡 NAME ONLY prompt | 🟡 name + public/private | Win32 sets visibility (`oc_client_create_channel_ex`); neither sets a topic (REQ-034 unbuilt) or an initial member list. **P1** |
| Channel topic | ✅ | ✅ | ❌ | ✅ header line + About-tab editor | Built (REQ-034/ARCH-93); any member may set it. **P1 (TUI)** |
| Channel description/About pane | ✅ | ✅ edit-channel pane | ❌ | 🟡 **About** tab: name, topic, member count, created date, admin actions | No separate long-form description — REQ-034 is one topic line. **P2** |
| Channel details (Members/Pinned/Files tabs) | ✅ | ✅ | ❌ | ✅ tab strip: Messages · Files & links · Pins, + members pane | Built (WIN-37). No settings/About tab — that needs channel topic/rename/archive (REQ-034/035/036), all unbuilt. **P1 (TUI)** |
| Member management (add/remove) | ✅ | ✅ | 🟡 via member menu | 🟡 via member menu, over the channel's own roster | The roster is now channel-scoped (REQ-031 `LIST_MEMBERS`); add/remove is still a menu action rather than a management surface. **P2** |
| Per-channel notification prefs | ✅ | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel | ✅ | ✅ | 🔸 (via notify?) | ❌ no mute/dim | **P1** |
| Star/favorite channel | ✅ | ✅ | ❌ | ❌ no starred | **P2** |
| Custom sidebar sections | ✅ | ✅ ᴾ customizable sections | 🟡 fixed groups (Public/DM/Private) | 🟡 fixed groups (Channels / Direct messages), collapsible, per-section filter/sort menu | **User-defined** sections absent in both; both group by kind. **P2** |
| Sidebar sort/display options | ✅ | ✅ | 🟡 fold/unfold | ❌ | **P2** |
| Archive channel | ✅ | ✅ (reversible, data preserved) | ❌ | ✅ reversible, confirmed, read-only enforced server-side | Built (REQ-035/ARCH-93). **P1 (TUI)** |
| Delete channel | ✅ | ✅ | ❌ | ⛔ | **Deliberate:** we offer archive (reversible) instead — deletion is not offered for channels holding history (REQ-035). — |
| Rename channel | ✅ | 🔸 creator only | ❌ | ✅ owner/admin, id-stable | Built (REQ-036/ARCH-93). **P1 (TUI)** |
| Posting/management permissions | ✅ | ✅ ᴮ | ❌ | ❌ | **P2** |
| Channel bookmarks | ✅ | ❔ | ❌ | ❌ | **P2** |
| Channel canvas | ✅ | ❌ | ⛔ | ⛔ | Canvas out of scope for us; Pumble has no equivalent either. |
| Leave channel | ✅ | ✅ | 🟡 in channel action menu | 🟡 sidebar right-click → Leave channel | Verified present in both (winmain.c channel menu). — |
| Join / open channel | ✅ | ✅ | 🟡 Join/Open menu | 🟡 | Present. — |
| Browse channels directory | ✅ searchable/filter/sort | ✅ (Channels tab in search) | ❌ | 🔸 sidebar filter only | Win32 has a "Find a conversation" box that substring-filters *joined* channels; neither client can browse/join undiscovered ones. **P1** |
| Default channels | ✅ | ❔ | ❌ (admin) | ❌ | Admin — **P2** |
| Slack Connect (shared/external) | ✅ | 🔸 guest access ᴮ only | ⛔ | ⛔ | Out of scope; Pumble has no cross-org federation either. |
| Channel context/action menu | ✅ | ✅ | 🟡 Join/Open/Notify/Webhooks/Leave | 🟡 no rename/topic/archive/delete/details | Win32 menu very shallow. **P1** |

### 2.4 People & DMs

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Direct messages (1:1) | ✅ | ✅ | ✅ DM group in sidebar | 🟡 no dedicated DM list/section | Win32 lacks a DM section. **P0** |
| Group DMs (multi-person) | ✅ up to 8 | ✅ | ❌ | ❌ | Verify wire support. **P2** |
| Message yourself / self-DM | ✅ | ✅ | ❌ | ✅ a row in the DMs index | REQ-055 was built on the engine and unreachable in every client until the DMs index gave it a row. **P2 (TUI)** |
| New DM start | ✅ | ✅ | 🟡 new-DM prompt | ✅ the DMs index lists everyone, including people you have never messaged | Slack's shape: the DM destination doubles as the start-a-conversation surface. — |
| Members / roster panel | ✅ | ✅ | 🟡 click-to-DM, **workspace** roster | 🟡 **channel** roster + header count chip + presence dot and an inline role glyph (crown/shield); no search/scroll | Win32 fixed (REQ-031/ARCH-91): it listed the *tenant* roster beside a channel name, which was wrong for any workspace with more people than one channel. TUI still shows the workspace roster. Server cap 500. **P1 (TUI)** |
| View other-user profile | ✅ rich profile pane | ✅ | ❌ (its `/profile` modal is your OWN identity) | 🟡 profile card in the right-hand **context pane** (ARCH-94): avatar, name, live presence, role, Message | Win32 built (WIN-10). Thin next to Slack's — no title, timezone, email or custom fields (REQ-240/241, unbuilt). TUI still cannot view a peer. **P1 (TUI)** |
| Member action menu (message/role/remove) | ✅ | ✅ | 🟡 | 🟡 no profile/deactivate | Present, shallow. **P1** |
| Add people to DM / convert to channel | ✅ | ❔ | ❌ | ❌ | **P2** |
| Close/leave DM from sidebar | ✅ | ✅ quick-close conversation | ❌ | ❌ | **P2** |
| DM peer notify/block | ✅ | ✅ mute | ❌ | ❌ | **P2** |
| DM sort/filter | ✅ | ✅ | 🟡 (grouped) | ❌ | **P2** |
| Import DMs / migrate | ✅ | ✅ native Slack import + DM CSV import | ❌ | ❌ | Migration path is a real Pumble go-to-market lever. **P2** |
| Presence dot in DM/roster | ✅ | ✅ | 🟡 | 🟡 | Present. — |
| Slack Connect DMs (external) | ✅ | ❌ | ⛔ | ⛔ | Out of scope; Pumble has no equivalent. |

### 2.5 Presence & Status

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Active/Away presence dot | ✅ | ✅ (auto-away after 30 min idle) | 🟡 | 🟡 | Present. — |
| Manually set away/active | ✅ | ✅ | 🟡 away/online only | 🔸 online/away only | Present but binary. **P1** |
| Custom status (emoji + text) | ✅ | ✅ (`/status`; emoji required) | ❌ | ❌ no custom status | **P1** |
| Status expiry / clear-after | ✅ | ✅ (period argument) | ❌ | ❌ | **P2** |
| Pause notifications w/ status | ✅ | ✅ DND | ❌ | ❌ | **P2** |
| Out-of-Office status | ✅ | 🟡 via custom status | ❌ | ❌ | **P2** |
| DND / pause notifications | ✅ presets | ✅ mode + schedule | 🔸 raw HH:MM text parse | ⚪ raw HH:MM-HH:MM prompt | Ours both raw-text; no pickers/schedule/display. **P1** |
| DND indicator to others | ✅ | ✅ (shown in the presence dot) | ❌ | ❌ | **P2** |

### 2.6 Notifications

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Global notify-me level | ✅ | ✅ | ✅ | ❌ | **DONE** (REQ-134): `users.notify_default` + `SET_NOTIFY_DEFAULT`, at the top of the Notifications sheet with the per-channel rows below it as overrides. TUI does not surface it. |
| Per-channel/DM overrides | ✅ | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel/DM | ✅ | ✅ | 🔸 | ❌ | **P1** |
| Keyword/highlight-word notifications | ✅ | ✅ | ❌ | ❌ | **P2** |
| Thread-reply notifications toggle | ✅ | ✅ | ❌ | ❌ | **P2** |
| Notification schedule (quiet hours) | ✅ | ✅ custom schedules | 🔸 DND raw text | 🔸 one daily DND window only | Richer schedules are REQ-136, unbuilt on the engine. **P1** |
| DND config surface | ✅ | ✅ | 🔸 | 🟡 dialog + state shown in the Notifications pane | **P1 (TUI)** |
| Activity feed (mentions/reactions/replies) | ✅ | ✅ | ❌ | ✅ Activity view: all three kinds, newest first, new-since marker, click to jump | Built (REQ-139/ARCH-95) as a union of three queries — no maintained list. **P1 (TUI)** |
| Activity filters / saved views | ✅ | ❔ | ❌ | 🟡 All / Mentions / Reactions / Threads | No saved views, and no DMs facet — that is a property of the channel, not of the activity. **P2** |
| All-unreads view | ✅ | ✅ | ❌ | ❌ | **P2** |
| Desktop/OS toast + preview toggle | ✅ | ✅ | ❌ | ✅ tray balloon (`Shell_NotifyIconW` + `NIF_INFO`) + in-app toasts, honouring the notify level | Win32 built (WIN-18) — `ToastNotification` needs WinRT/C++ and the client is pure C (ARCH-82). The API path is verified (`tray_live=1` — the shell accepted the icon; `toasts_raised` increments on the call); only the **rendering** is unobserved, because this dev host has no attached display surface — a full-screen capture throws and returns blank, which is why a control test with .NET's own NotifyIcon was equally invisible. No preview on/off toggle. **P2** |
| Notification sounds & badges | ✅ | ✅ | ❌ | ❌ | **P2** |
| Email notifications | ✅ | ✅ (email reminder after 24 h inactivity) | ⛔/❌ | ⛔/❌ | Server-side; not a client surface. — |
| Mobile push | ✅ | ✅ | 🔗 daemon emitter built (ARCH-85); no mobile client | 🔗 same | Push is a **federated-only** function for us (REQ-133). — |
| VIP / priority people | ✅ | ❔ | ❌ | ❌ | **P2** |
| Review list_notify_prefs screen | ✅ | ✅ | 🔸 read-only overlay | 🟡 Notifications pane: per-channel level, editable, + DND state | Win32 built. **P1 (TUI)** |

### 2.7 Search

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Search entry | ✅ top bar | ✅ top bar | 🟡 search prompt | 🟡 search overlay with a native query box (Ctrl+F / menu) | Neither has Slack's always-present top-bar search field. **P2** |
| Results view | ✅ | ✅ navigable | 🔸 read-only, NON-navigable, shows userNNN not names | 🔸 read-only, no scroll | Ours un-navigable; Win32 click jumps to CHANNEL not the matched message. **P0** |
| Search modifiers (from:/in:/has:…) | ✅ full grammar | 🟡 `from:` + checkbox filters | ❌ | ❌ | **P2** |
| Search filters panel | ✅ | ✅ date, channel, has-file/reaction/link, exclude webhooks | ❌ | ❌ | **P2** |
| Result tabs (Messages/Files/Channels/People) | ✅ | ✅ (+ Apps) | ❌ | ❌ | **P2** |
| Sort (relevance/newest) | ✅ | ✅ relevancy / newest / oldest | ❌ | ❌ | Confirmed on Pumble's side 2026-07-28 (was ❔). **P2** |
| Term highlight in results | ✅ | ✅ | ❌ | ❌ | **P1** |
| Paging / load-more | ✅ | ✅ | ❌ | ❌ (128 cap, ignores truncation) | **P1** |
| Full history searchable on the free tier | ❌ 90 days visible, **and anything over a year is permanently deleted** | ✅ unlimited | ✅ (no cap, ever) | ✅ | **Pumble matches our "no history cap" wedge** — it is no longer a Slack-only differentiator. Slack's free tier is harsher than "a cap" though: it *deletes*. — |
| Quick switcher search (people/channels) | ✅ | ✅ | ✅ Ctrl+K palette | ✅ Ctrl+K command palette + a "Find a conversation" sidebar filter | Both built. **—** |
| AI/enterprise search | ✅ | 🟡 AI Assistant add-on | ⛔ | ⛔ | Out of scope. |

### 2.8 Files / Attachments

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| File upload | ✅ drag-drop/paste/1GB | ✅ | 🔸 raw path text, no browser | 🟡 + button + drag-drop, no preview/progress | TUI needs a file picker. **P1** |
| Download file | ✅ | ✅ | 🔸 auto first attachment, no chooser | 🟡 native Save dialog | TUI has no chooser/save-path. **P1** |
| Inline image previews/thumbnails | ✅ | ✅ | ❌ (attach lines only; TUI exempt, ARCH-75) | ✅ thumbnails + lightbox (see §2.3) | Win32 built. **— (TUI exempt)** |
| Media carousel / gallery | ✅ | ✅ (iOS; files, links, docs) | ❌ | ❌ | **P2** |
| Inline video/audio playback | ✅ | ✅ | ⛔ | ⛔ | Media playback out of scope. |
| Inline PDF/doc preview | ✅ | ✅ clickable previews | ❌ | ❌ open-in-place missing | **P2** |
| Code/text snippets | ✅ | ❔ | ❌ | ❌ | **P2** |
| Files browser / channel Files tab | ✅ | ✅ (Files tab in search) | ❌ | ✅ channel **Files & links** tab **and** a workspace-wide **Files** view, both with a type filter | Built (REQ-143/ARCH-91). Newest-first, 200 cap, reclaimed files listed and flagged. No type filter UI yet (the `mime` is on the wire). **P1 (TUI)** |
| File comments/sharing/permissions | ✅ | ❔ | ❌ | ❌ | **P2** |
| External file services (Drive/Dropbox) | ✅ | ✅ Google Drive | ⛔ | ⛔ | Out of scope. |
| Upload progress / preview | ✅ | ✅ | ❌ | ❌ | **P2** |
| Storage model | per-workspace by plan | 10 GB workspaceᶠ → 10/20/100 GB **per seat** ᴾᴮᴱ | operator's disk (ARCH-77 tiers) | same | We have no seat-metered storage — the ceiling is the box. — |
| Attachment lines in transcript | ✅ | ✅ | ✅ incl reclaimed | ✅ attachment lines | Present. — |

### 2.9 Account & Profile

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Login dialog | ✅ | ✅ | ✅ fields+remember-me+validation+retry | ✅ two-step in-window view (workspace→credentials), inline errors, retry, remember-me | Win32 rebuilt Slack-shaped (WIN-2); hosted `.openchime.io` default + Advanced for self-hosted. — |
| Signup / first-owner UI | ✅ | ✅ self-serve | ❌ | 🟡 "Have an invite? Create an account" — token + password redeem, signs in on success | Win32 built (WIN-32). Invite-gated by design (ARCH-59): there is no open self-serve signup, and the first owner is bootstrapped by the daemon. **P1 (TUI)** |
| Profile viewer (self) | ✅ editor | ✅ editor | 🟡 READ-ONLY viewer | 🟡 display-name and password dialogs (one each) | Win32 edits the two fields that exist; there are no others to edit yet (avatar/email/timezone/title are REQ-240/241, unbuilt). **P2** |
| Change display name | ✅ | ✅ | 🟡 | 🟡 (chained prompt) | Present. — |
| Change password | ✅ | ✅ | 🟡 2 prompts | 🟡 (no confirm field) | Win32 lacks confirm field. **P1** |
| Avatar upload | ✅ | ✅ | ❌ | ❌ | **P2** |
| Email edit | ✅ | ✅ | ❌ | ❌ | **P1** |
| Timezone | ✅ | ✅ | ❌ | ❌ | **P2** |
| Custom status set | ✅ | ✅ | ❌ | ❌ | See Presence. **P1** |
| Job title / pronouns / custom fields | ✅ | 🟡 profile fields | ❌ | ❌ | **P2** |
| Connecting/reconnect screen | ✅ | ✅ | ⚪ stub | ✅ banner with the specific reason, a **live** retry countdown and "Retry now" | Win32 built (WIN-1, countdown fixed in WIN-55 — the deadline is carried in the model so the number actually moves). **P1 (TUI)** |
| Preferences dialog (hub) | ✅ | ✅ | 🟡 config file + some in-app toggles | ✅ Preferences pane: appearance, time format, day dividers, members pane | Win32 built (WIN-9). **P1 (TUI)** |
| Themes / appearance | ✅ | ✅ | ❌ no in-app toggle | ✅ dark / light / match-system, switchable in Preferences | Win32 built (WIN-26, `theme.c` — the light palette inverts *structure*, not just brightness). **P2 (TUI)** |
| 12/24h & panel toggles in-app | ✅ | ✅ | ❌ (file-only) | ✅ in Preferences | Win32 built. **P2 (TUI)** |

### 2.10 Admin

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Invite people | ✅ email/link/roles/CSV | ✅ email + link | 🔸 hardcoded MEMBER only, token in hard-to-see banner | ⚪ member/admin only, token via MessageBox, no expiry/target/list/revoke | Both of ours very thin; no invite management. **P1** |
| Invite link (shareable) | ✅ | ✅ | ❌ | ❌ | **P2** |
| Guest accounts (single/multi-channel) | ✅ | ✅ ᴮ (5 single-channel guests free per paid seat) | ❌ | ❌ | **P2** |
| Role assignment (owner/admin/member) | ✅ | ✅ ᴮ permissions & roles | 🟡 role mgmt | 🟡 | Present. — |
| Deactivate / remove user | ✅ | ✅ | 🔸 remove, NO confirm | 🟡 "Remove from workspace" in the member menu | Removal disables rather than deletes (REQ-025), so authored messages keep a valid author; there is no separate reactivate surface. TUI still lacks a confirm. **P1 (TUI)** |
| Manage members table | ✅ sortable/bulk | ✅ | 🟡 roster | 🟡 roster pane | No admin members table in ours. **P2** |
| Manage channels (admin) | ✅ | ✅ | ❌ | ❌ | **P2** |
| Custom emoji administration | ✅ | ✅ (perm control ᴾ) | ❌ | ❌ | **P2** |
| Storage report | ✅ analytics | ❔ | ✅ RICH read-only modal | 🔸 read-only KV dump, no actions/refresh | TUI richer; Win32 thin. **P2** |
| Audit log | ✅ searchable/filter | ❔ not documented publicly | 🟡 read-only, NO scroll/paging/filter, ~22-row cap | 🔸 read-only, offset fixed 0, no paging/filter/search/export/scroll | Ours both un-paged — but note we **have** one and Pumble does not advertise one. **P1** |
| Webhooks — list/view | ✅ (app config) | ✅ | 🔸 read-only, STALE slash-hint text | 🔸 list+delete via MessageBox, no enable/disable/rotate/reveal/date | **P1** |
| Webhooks — create | ✅ | ✅ incoming webhooks (form-encoded or JSON) | 🟡 | 🟡 one-field, token via MessageBox | Present. — |
| Webhooks — delete | ✅ | ✅ | ❌ MISSING (core supports it) | 🟡 delete via MessageBox | TUI can't delete despite core support. **P1** |
| Workspace settings (name/URL/icon/defaults) | ✅ | ✅ | ❌ | ❌ | **P2** |
| Import/Export data | ✅ | 🟡 Slack import; export public channels ᶠ | ❌ | ❌ | **P2** |
| Analytics dashboard / export | ✅ | ❔ | 🔸 storage only | 🔸 storage only | **P2** |

### 2.11 Enterprise / Security / Compliance

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| 2FA / MFA | ✅ | ✅ | ❌ | ❌ | Likely server/roadmap. **P2** |
| SSO — **SAML 2.0** | ✅ **Business+ and Enterprise** | ✅ **ᴱ Enterprise only** | ⛔ **not supported at all** | ⛔ | **No SAML anywhere in the product** — not built, not designed (ARCH-55 has no SAML path; recorded as REQ-027). Disqualifying in SAML-mandatory RFPs. See §8. |
| SSO — **OIDC / social login** | ✅ ᴾ | ✅ OAuth2 ᴱ | ❌ client half unbuilt (Google-only upstream) | ❌ | Daemon verification built; browser flow + PKCE + courier ⛔ (REQ-020). **P1** — see §8. |
| SCIM / JIT provisioning | ✅ | ❔ | ⛔ | ⛔ | Out of scope for client (ours is REQ-253, federated). |
| Session mgmt / forced sign-out | ✅ | ❔ | 🟡 logout only | 🟡 logout **+ "Sign out everywhere"** | Win32 surfaces revoke-all (`OC_LOGOUT_ALL`); the TUI does not. Neither lists active sessions. **P2** |
| IP allowlist / domain restrict | ✅ | ❔ | ⛔ | ⛔ | Server-side, out of client scope. |
| Encryption at rest / in transit | ✅ + EKM | ✅ (256-bit TLS; no EKM) | ⛔ | ⛔ | Out of scope. |
| DLP (native/3rd-party) | ✅ | ❌ | ⛔ | ⛔ | Out of scope. |
| Access logs | ✅ | ❔ | ❌ | ❌ | **P2** |
| Audit logs (dashboard/API) | ✅ | ❔ | 🔸 (see Admin) | 🔸 | **P1** |
| Retention policies | ✅ | ✅ **ᴱ Enterprise only** | ❌ | ❌ | Server-side for us (REQ-250, unbuilt); no client surface. **P2** |
| Data export / eDiscovery / legal holds | ✅ | 🔸 public-channel export only | ⛔ | ⛔ | Out of scope. |
| Compliance certifications | SOC 2 | ISO/IEC 27001:2022 + SOC 2 | n/a (self-hosted — the operator's own posture) | n/a | Self-hosting moves certification to the operator. — |
| Data residency choice | ✅ | ❌ (US-hosted; flagged by Cloudwards as a privacy concern) | ✅ **your box, your jurisdiction** | ✅ | A genuine self-hosting advantage over *both* references. — |
| Enterprise Grid multi-workspace org | ✅ | ❌ | ⛔ | ⛔ | Out of scope (federation is a separate model). |
| Information barriers / domain claiming | ✅ | ❌ | ⛔ | ⛔ | Out of scope. |
| HIPAA / FedRAMP | ✅ | ❌ | ⛔ | ⛔ | Out of scope. |

### 2.12 Integrations / Automation

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Incoming webhooks | ✅ | ✅ | 🟡 create; 🔸 list; ❌ delete | 🟡 create+delete; 🔸 list | OpenChime's one real integration surface. **P1** |
| Outgoing webhooks | ✅ | ✅ (via API/Zapier/Pipedream) | ❌ | ❌ | Verify wire support (REQ-173, unbuilt). **P2** |
| App directory / marketplace | ✅ 2,600+ | 🟡 small catalog (3ᶠ / 10ᴾ / unlimitedᴮ apps) | ⛔ | ⛔ | We have a **federated** directory (REQ-175, control-plane M7 built) — client surface not built. |
| Bots / bot users | ✅ | ✅ Pumblebot + custom apps | ⛔ | ⛔ | Out of scope. |
| Slash commands (app) | ✅ | ✅ | ⛔ | ⛔ | Out of scope (and OpenChime is affordance-driven). |
| Web API / SDK | ✅ | ✅ JS SDK + API | ⛔ | ⛔ | Out of scope for client. |
| MCP server | ✅ | ✅ (added 2026-06) | ⛔ | ⛔ | Out of scope; worth noting as a 2026 table-stakes shift. |
| Named integrations | 2,600+ | Google Drive/Gmail/Calendar, GitHub, Jira, Zendesk, Zoom, Calendly, Zapier, Clockify, Plaky | ⛔ | ⛔ | Pumble's integration count is **~2 orders of magnitude** below Slack's — its real weakness. |
| Workflow Builder + triggers/steps/forms | ✅ | 🟡 automation notifications (Plaky/Clockify) | ⛔ | ⛔ | Out of scope (REQ-174, unbuilt). |
| Connectors / custom functions | ✅ | ✅ custom app creation | ⛔ | ⛔ | Out of scope. |
| Scheduled messages / reminders | ✅ | ✅ + recurring | ❌ | ❌ | **P2** |
| Email-to-channel / RSS / Salesforce | ✅ | ❔ | ⛔ | ⛔ | Out of scope. |
| Lists / Canvas automations | ✅ | ❌ (Plaky is the separate product) | ⛔ | ⛔ | Out of scope. |

### 2.13 Navigation / UI-Shell

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Header bar | ✅ | ✅ | 🟡 | 🟡 channel name + topic/typing line + member-count chip, over a **channel tab strip** (Messages · Files & links · Pins · About); connection state moved to a dot on the workspace name where it belongs | Rebuilt to Slack's two-row shape (WIN-37). Still no star/favourite (REQ-234), call or search affordance in the channel header. **P2** |
| Sidebar channel/DM list | ✅ | ✅ | ✅ grouped, unreads, badges, lock/@ markers | 🟡 flat single "CHANNELS" group + a filter box; hides unnamed, 512-row cap, no scroll, no DM section/grouping/collapse/starred/muted | Win32 gained the filter (nav epic) but is still flat and unscrollable. **P0** |
| Custom sidebar sections | ✅ | ✅ ᴾ | 🟡 fixed groups | 🟡 fixed groups, collapsible | See §2.6. **P2** |
| Unreads-only sidebar mode | ✅ | ✅ | ❌ | ❌ | **P2** |
| Quick switcher / command palette | ✅ Cmd+K | ✅ quick search | ✅ Ctrl+K ~19 fuzzy actions | ✅ Ctrl+K palette + rail menus + a "Find a conversation" filter | Both built. **—** |
| Global search bar | ✅ | ✅ | 🟡 prompt | 🟡 overlay + native query box | See §2.7. **P2** |
| Channel browser / add channels | ✅ | ✅ | ❌ | ❌ | **P1** |
| New message / compose (Cmd+N) | ✅ | ✅ | 🟡 new-DM/new-channel prompts | 🟡 | Present. — |
| Profile/account menu | ✅ | ✅ | 🟡 via launcher | 🟡 profile menu off the rail avatar | Present. — |
| Preferences hub | ✅ | ✅ | 🟡 config file | ✅ Preferences pane | See §2.10. **P1 (TUI)** |
| Themes/appearance | ✅ | ✅ | ❌ | ✅ three modes | See §2.10. **P2 (TUI)** |
| Keyboard shortcuts reference | ✅ | ✅ | ✅ help overlay RICH | ✅ "Keyboard shortcuts" pane | Both built. **—** |
| History nav (back/forward) | ✅ | ❔ | ❌ | ❌ | **P2** |
| Move between unreads (keyboard) | ✅ | ❔ | 🟡 | ❌ | **P2** |
| Right-hand panel system | ✅ | ✅ | 🟡 overlays | 🟡 overlays | Ours overlay-based, mostly read-only. **P1** |
| Star/favorite conversations | ✅ | ✅ | ❌ | ❌ | **P2** |
| Autocomplete strip (@/#/:emoji) | ✅ | ✅ | 🟡 | 🟡 panel over the composer | Both drive the shared `oc_complete`. **—** |
| Keybinding hint bar | ✅ | ❌ | 🟡 | ❌ | **P2** |
| Error / toast surface (send fail, rate-limit, bad login) | ✅ | ✅ | 🔸 partial | ✅ toast stack + connection banner + inline sign-in errors | Win32 complete (WIN-1 + WIN-2's rebuilt sign-in, which reports a bad password inline and clears the field instead of dropping you into an empty window). **P1 (TUI)** |
| Terminal / TUI client | ❌ | ❌ | ✅ **the only one of the four** | — | Genuine category differentiator. — |
| Slackbot conversation | ✅ | ✅ Pumblebot | ⛔ | ⛔ | Out of scope. |

### 2.14 Session / Workspace

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Multi-workspace switcher rail | ✅ | ✅ | ✅ remembered+open, unread, per-ws state | 🟡 rail switcher built (single-client stop/reconnect; no N-concurrent, no background unread) | Win32 has the switcher UI; the remaining delta is the TUI's N-concurrent-client model. **P1** |
| Add workspace | ✅ | ✅ | 🟡 | 🟡 "Add a workspace…" in the switcher | **P1** |
| Workspace switch by number | ✅ Cmd+1..9 | ❔ | 🟡 | ❌ | **P2** |
| Reconnect / auto-reconnect | ✅ | ✅ | ⚪ stub | ✅ auto-reconnect + banner + "Retry now" + live countdown | Win32 done (WIN-1/WIN-55). **P1 (TUI)** |
| Connection status indicator | ✅ | ✅ | 🟡 status line | 🟡 header state + connection banner (WIN-1) | **P1** |
| Logout | ✅ | ✅ | 🟡 no log-out-everywhere; quits | ✅ Sign out / Sign out everywhere, returns to sign-in | Win32 returns to the sign-in view rather than quitting (WIN-2); TUI lacks revoke-all. — |
| Quit | ✅ | ✅ | 🟡 | 🟡 | Present. — |

### 2.15 Calls / Huddles / Canvas / Lists / Clips

*Mostly out-of-scope by design for the OpenChime **clients** — but note the audio row is now only half true: the daemon's call signaling + UDP relay sidecar are built (REQ-150–152, ARCH-73); it is the **client** half (Opus, UDP, AEC — AUDIO.md) that does not exist.*

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Huddles / ad-hoc audio | ✅ | ✅ | ⛔ client | ⛔ client | **Server built** (ARCH-73); client Opus/UDP is Phase-2 (AUDIO.md). |
| 1:1 voice + video calls | ✅ | ✅ ᶠ (free tier) | ⛔ client | ⛔ client | Pumble gives 1:1 calls away free; group calls are paid. |
| Group video meetings | ✅ 50 | ✅ 50 ᴾ / 100 ᴮᴱ | ⛔ | ⛔ | — |
| Screen share | ✅ | ✅ (annotations added 2026-06) | ⛔ **permanently** (ARCH-75, no graphics) | ⛔ designed, not started | **Now in scope** as REQ-161 ([VIDEO.md](./VIDEO.md), ARCH-86/87) — VP9/libvpx baseline over the existing opaque relay. Gated behind the audio client. Buys parity, not differentiation. **P2** |
| Screen-share annotations | ✅ | ✅ | ⛔ | ⛔ | Out of scope (VIDEO.md §2). |
| Meeting recordings | ✅ | ✅ ᴮ | ⛔ | ⛔ | Out of scope. |
| Noise reduction | ✅ | ✅ | ⛔ | ⛔ | AEC is designed (AUDIO.md §6) but unbuilt. |
| Third-party call apps | ✅ | ✅ Zoom | ⛔ | ⛔ | Out of scope. |
| Clips (recording + transcription) | ✅ | 🟡 voice/video messages | ⛔ | ⛔ | Out of scope. |
| Canvas documents (+ media/comments/templates) | ✅ | ❌ | ⛔ | ⛔ | Out of scope; Pumble has none either. |
| Lists (tables/board/automations) | ✅ | ❌ (sold separately as Plaky) | ⛔ | ⛔ | Out of scope. |

---

## 3. Underdeveloped Screens & Dialogs (ranked)

Surfaces that *exist* in TUI and/or Win32 but are thin/stub/read-only. Ranked by user impact.

1. **Search results** — *Win32: mostly done* — a result click jumps to the exact message and flashes it (WIN-3), with an in-overlay query box and resolved display names. *Remaining:* term highlighting, and **paging is blocked on the wire** — `SEARCH_RESULTS` carries `truncated` but no cursor (WIN-38), so the 128-result cap cannot be paged past. The **TUI** list is still read-only, non-navigable and shows raw `userNNN`. **P0 (TUI) / P1 (paging)**

2. **Win32 error/failure surface** — **closed (WIN-1 + WIN-2).** A toast stack carries in-session failures (verified against a live `send rate exceeded`), a connection banner carries the reason plus Retry now, and the rebuilt sign-in view reports DNS and auth failures inline and retryably. *Remaining:* the banner's countdown is static text rather than ticking (WIN-55). —

3. **Settings / Preferences** — *Win32: done* (WIN-9/WIN-26 — appearance incl. dark/light/system, time format, day dividers, members pane, desktop notifications, quick reactions). *Remaining:* the **TUI** is still config-file-only with no in-app toggles. **P1 (TUI)**

4. **DND / notification schedule (both)** — *Today:* Win32 has a real dialog (an on/off check plus From/To fields, validated, midnight-crossing noted) but still typed `HH:MM` rather than a time picker; the TUI parses raw text. Neither shows a schedule, and DND is invisible to others. Richer schedules are REQ-136, unbuilt. *Done:* pickers, weekday schedules, and a DND indicator in presence. **P1**

5. **Audit log (both)** — *Today:* read-only, no scroll/paging/filter/search/export; TUI ~22-row cap, Win32 offset fixed at 0. *Done:* paged/scrollable view with actor/action/date filters and export. **P1**

6. **Webhooks overlays (both)** — *Today:* read-only lists; TUI still shows a STALE slash-command hint and CANNOT delete (core supports delete); Win32 delete is via MessageBox with no enable/disable/rotate/reveal/date and no scroll. *Done:* full CRUD (create/reveal/rotate/enable-disable/delete), metadata columns, scroll. TUI delete is a quick core-backed win. **P1**

7. **Invite dialog (both)** — *Today:* TUI hardcodes MEMBER role with token in a hard-to-see banner; Win32 is member/admin only with token via MessageBox; neither has expiry/target/pending-list/revoke. *Done:* role + expiry + target selection, a pending-invites list with revoke, and a copyable token/link surface. **P1**

8. **Thread overlays (both)** — *Today:* TUI is a read-only dump with no per-reply actions; Win32 has a working reply composer but replies are read-only (no react/edit/delete) and there's no scrollbar. *Done:* full per-reply actions inside the thread + scrollbar. **P1**

9. **Profile / account editor (both)** — *Today:* the TUI is a read-only viewer; Win32 has a purpose-built dialog each for display name and password (with a confirm field, WIN-20). Neither edits avatar/email/timezone/status — **because those fields do not exist yet** (REQ-240/241, unbuilt). *Done:* the fields first, then an editor. **P1**

10. **New-channel dialog (both)** — *Today:* name-only (TUI prompt / Win32 stub); no privacy, topic, or member step. *Done:* full create flow (visibility toggle, topic/description, add-members). **P1**

11. **Win32 sidebar** — *Today:* flat list, hides unnamed channels, 512 cap, no DM section, no sections/collapsing/search/starred/muted. *Done:* grouped Public/DM/Private sections (TUI-parity), search box, mute/star affordances, load-more. **P0**

12. **Storage report (Win32)** — *Today:* read-only KV dump, no refresh/actions/history (TUI's is RICH by comparison). *Done:* match TUI richness + refresh. **P2**

13. **Members/roster panel** — *Win32: done* — per-channel roster with a header count chip and profile-on-click (REQ-031/ARCH-91). *Remaining:* search and scrolling in the pane; and the **TUI** still shows the workspace roster. **P1 (TUI)**

14. **Win32 "six-forms-in-one-prompt" anti-pattern** — **closed (WIN-21).** Every flow that used the generic single-line prompt now has a typed multi-field modal (`form_dialog`), and `text_prompt` is deleted. **—**

15. **Roster overlay (TUI)** — *Today:* THIN and effectively UNREACHABLE (no input path). *Done:* wire an entry point or remove. **P2**

---

## 4. Missing Entirely (per client)

### Win32 GUI — missing

*Rewritten 2026-07-28 against the code; entries closed since the original audit are listed at the end rather than deleted, so the record stays honest.*

- **N-concurrent-workspace model** — the rail switcher UI exists, but Win32 stop/reconnects a **single** client; no holding N clients at once with background unread. **P1**
- **Mark-unread** (REQ-235); **mute channel/DM** (REQ-137); **star/favourite** (REQ-234). **P1**
- **Copy-link / permalink and forward** on messages — permalinks are REQ-232, unbuilt. **P1**
- **Search paging, filters, term highlight** — paging is blocked on the wire: `SEARCH_RESULTS` carries `truncated` but no cursor (WIN-38). In-overlay input and jump-to-message are built. **P1**
- **Rich text / formatting toolbar** (REQ-220) — blocked on an ARCH decision about the markup dialect. **P2**
- **Avatar upload, email, timezone, job title** in the profile editor (REQ-240/241). **P1**
- **Custom status** (emoji + text, REQ-122/240) — presence is online/away only. **P1**
- **Active-session list** — revoke-all *is* built ("Sign out everywhere"); enumerating sessions is not (REQ-182 has no list op). **P2**
- ~~**Global (workspace-default) notify level** (REQ-134)~~ — **DONE**, engine and Win32. Still open: **keyword / priority-people alerts** (REQ-135) and a **notification schedule** richer than one daily DND window (REQ-136), both unbuilt on the engine. **P1**
- **DND configuration surface** — the value is displayed, but it is set through a raw `HH:MM-HH:MM` prompt with no picker. **P1**
- **Audit-log paging/filtering** — read-only, offset fixed at 0. **P2**
- **Webhook enable/disable/rotate/reveal**, and dates in the list. **P2**
- **Invite depth** — member/admin only, token shown via `MessageBox`, no expiry/target/pending list. **P1**
- **Storage report depth** — read-only KV dump, no refresh or actions (the TUI's is richer). **P2**
- **Draft persistence across restarts** — drafts are per-channel and in memory only; a stateless client (ARCH-88) cannot persist them locally, so this needs server storage. **P2**
- **Group DMs** (REQ-056), **self-DM surface** (REQ-055 exists on the engine, unsurfaced), **browse-channels directory** (REQ-038). **P2**
- **Accessibility / keyboard-only operation** (REQ-269) — recorded, unimplemented, needs an ARCH decision. **P1**

**Closed since the original audit** (kept for the record): **activity feed** (REQ-139) and **saved items** (REQ-231), which were the last two rail stubs · **channel topic, rename and archive** with the About tab (REQ-034/035/036, WIN-34–36) · error/toast surface and the connection banner with a live countdown (WIN-1/WIN-55) · sign-in rebuilt in-window with inline errors, remember-me and retry (WIN-2) · Preferences hub with theme / 12-24h / panel toggles (WIN-9/WIN-26) · view another user's profile (WIN-10) · composer emoji picker over the shared ~179-emoji catalogue (WIN-8) · `@`/`#`/`:` autocomplete (`oc_complete`) · Ctrl+K command palette · sidebar sections, collapsing, scrolling and a "Find a conversation" filter · DM section and rail DMs view · OS tray balloon (WIN-18, delivery still visually unconfirmed) · notification-prefs review/edit pane · jump-to-unread and the "New" divider (WIN-14) · inline image thumbnails + lightbox · signup / invite redeem (WIN-32) · keyboard-shortcut reference · display-name and password dialogs (WIN-20) · channel member management (WIN-31) · **@mentions** (REQ-221) · **pins** (REQ-230) · **channel Files tab and per-channel roster** (REQ-143/031, WIN-37).

### TUI — missing

*The TUI reached every engine feature on the wire until 2026-07-28; it is now **four behind**, listed first.*

- **@mentions** (REQ-221) — it autocompletes `@name` but does not highlight a mention or act on the MENTIONS notify level. **P1**
- **Pins** (REQ-230) — no pin/unpin action and no pins list. **P1**
- **Channel Files listing** (REQ-143) — `LIST_FILES` is on the wire and unused. **P1**
- **Per-channel member roster** (REQ-031 `LIST_MEMBERS`) — its roster is still workspace-wide. **P1**
- **Delete webhook** (core supports it; not surfaced). **P1**
- **Log-out-everywhere / revoke sessions** (core supports `OC_LOGOUT_ALL`; Win32 surfaces it, the TUI does not). **P2**
- **In-app settings / config editor** (file-only; no theme/12-24h/panel toggles). **P0**
- **Invite-as-admin / invite roles + expiry / pending-invite list.** **P1**
- **Destructive confirmations** (delete message, remove user have none). **P1**
- **Multi-line composer.** **P1**
- **Custom / workspace emoji.** (The picker itself now searches the shared ~179-emoji catalogue via `oc_emoji_search`.) **P2**
- **View other-user profile** (roster overlay unreachable). **P0**
- **Channel topic / description / rename / privacy edit / archive / details.** **P1**
- **Channel browser / sidebar search.** **P1**
- **Custom status; status expiry; timezone; email/avatar edit.** **P1**
- **Activity feed / all-unreads / global notify prefs (editable).** **P1**
- **File picker for upload; download chooser/save-path.** **P1**
- **Inline image rendering.** **P2**
- **Copy-link, pin, forward, mark-unread, save-for-later, remind.** **P1**
- **Per-reply actions inside threads; thread scroll.** **P1**
- **Search: navigation, name resolution, highlight, paging.** **P0**
- **Signup / first-owner UI.** **P1**
- **Reconnect (proper, not stub); connecting screen depth.** **P1**
- **Group DMs / self-DM.** **P2**
- **Drafts, scheduled send.** **P2**

---

## 5. Recommended Build Order for Win32 (current focus) — top 10 by impact

> **The execution list lives in [WIN32_BACKLOG.md](./WIN32_BACKLOG.md)** — every
> gap below, numbered `WIN-1`…`WIN-70`, split by whether it is buildable today or
> blocked on daemon work. This section stays the *rationale* for the ordering.

**Rewritten 2026-07-28.** The original top 10 is essentially spent: items 1, 4, 5,
6 and 10 are done outright, 2 is done except paging (blocked on the wire), 3 is
done except mute/star, and 7 is half done (pin ✅, permalink/mark-unread ❌).
What follows replaces it.

**The pattern that now sets the order:** the remaining Win32 gaps are mostly
*daemon* features, and the client half is small. REQ-221 (@mentions), REQ-230
(pins) and REQ-143/031 (files + channel roster) each took a day of engine work and
a fraction of that in the GUI. Anything below that says "needs REQ-x" should be
read as "build the engine feature; the Win32 work is the easy part."

1. ~~**Channel management — rename, topic, archive**~~ **Done** (REQ-034/035/036,
   ARCH-93) — and it added the **About** tab, completing the channel surface.
2. **Activity feed / notification inbox** (REQ-139). Fills the rail's Activity
   stub, and mentions finally make it meaningful — a mention you can only see by
   already looking at the channel is half a feature. Migration 0021 indexes
   mentions by user, so the mentions half is a query, not new state. **P1**
3. **Permalinks + jump-to-message-in-context** (REQ-232). Unblocks copy-link, and
   fixes a live limitation: clicking a pin only jumps if that message happens to be
   in loaded history. Needs a fetch-around-an-id backfill mode. **P1**
4. **Search paging, filters and term highlight.** Blocked on the wire —
   `SEARCH_RESULTS` carries `truncated` but no cursor (WIN-38), so this starts with
   a protocol change. **P1**
5. **Mark-unread, mute, star** (REQ-235/137/234). Three small engine features that
   together make the sidebar behave the way people expect. **P1**
6. **Profile depth + custom status** (REQ-240/241/122). Avatar, email, timezone,
   title; presence is still online/away only. **P1**
7. **Notification depth** — ~~workspace-default level (REQ-134, done)~~, keyword and
   priority-people alerts (REQ-135), a real schedule (REQ-136), and a DND picker
   instead of a raw `HH:MM-HH:MM` prompt. REQ-135 is cheap now: it is the same
   match→notify path @mentions built. **P1**
8. **Rich text** (REQ-220). Biggest single feature left, and still needs an ARCH
   decision on the markup dialect before any code. It also gates the composer
   toolbar and snippets (REQ-226). **P2**
9. **Accessibility / keyboard-only operation** (REQ-269). Recorded, unimplemented,
   needs an ARCH decision — and gets more expensive the longer the UI grows. **P1**
10. **N-concurrent workspaces.** The rail switcher exists but stop/reconnects a
    single client, so background unread across workspaces does not work. **P1**

*Two cheap wins outside the ten:* the rail's **Files** stub can be filled almost
for free — `LIST_FILES` already accepts `channel_id 0` for "every channel I can
read" — and **saved items** (REQ-231) fills the **Later** stub with a per-user
table that is deliberately the mirror image of pins (ARCH-90).

*Follow-ups just behind: full webhook CRUD, paged audit log, richer invite dialog,
per-reply thread actions, group DMs, browse-channels directory.*

---

## 6. Where OpenChime Exceeds / Differs Favorably — now tested against *both* references

This analysis is reference-centric (what Slack and Pumble have that we lack). For balance, the places OpenChime is **ahead of or deliberately different from** them. **Adding Pumble materially weakens several claims that held against Slack alone** — those are marked ⚠️ and are the honest headline of this revision.

| Area | OpenChime | Slack | Pumble | Verdict |
|---|---|---|---|---|
| **Read receipts (seen-by)** | ✅ per-message "✓ Seen by …" from per-member read cursors (REQ-090, `readers[]`) | ❌ none (deliberately) | ❌ none — *"seeing who viewed the message is not supported"* (Pumble FAQ) | **Holds against both.** Still the single cleanest capability differentiator. |
| **Self-hosting / data ownership** | ✅ run your own daemon; data in your SQLite/blob store | ❌ SaaS-only | ❌ SaaS-only, US-hosted | **Holds, and widens** — Cloudwards scored Pumble's privacy 60% and flagged US data-disclosure exposure. |
| **Data residency / jurisdiction** | ✅ your box, your country, air-gappable | 🟡 paid residency options | ❌ none | **Holds against both.** |
| **Deployment models** | ✅ stand-alone / federated / hosted (ARCH-76) | ❌ single cloud | ❌ single cloud | **Holds against both.** |
| **Terminal client** | ✅ full-featured TUI | ❌ | ❌ | **Holds against both** — no mainstream competitor ships one. *Caveat as of 2026-07-28:* it no longer reaches **every** engine feature — @mentions, pins, the channel files listing and the per-channel roster are on the wire and unsurfaced there. |
| **Native, lightweight clients** | ✅ pure-C native per platform — TUI + Win32 GUI (~3 MB, Direct2D), no Electron | ❌ Electron desktop (heavy) | ❌ web-tech desktop app (Win/mac/Linux `.deb`/`.rpm`) | **Holds on footprint** — but see the caveat below: Pumble *ships* Linux/macOS/mobile clients today and we do not. |
| **Full history on the free tier** | ✅ self-hosted = unlimited retention (you set policy) | ❌ free tier shows 90 days and permanently deletes past one year | ✅ **unlimited history on the free plan** | ⚠️ **No longer a differentiator.** This was our stated wedge against Slack (ARCH-15 calls FTS5 "a competitive wedge against Slack's history caps"). Pumble gives it away free. Against Pumble the wedge must be restated as *ownership*, not *retention*. |
| **Pricing model** | ✅ self-hosted free; hosted flat plan | ❌ per-user seat pricing | 🟡 free tier w/ unlimited users; cheap per-seat above it | ⚠️ **Weakened but survives.** Detailed price analysis is out of scope for this repo and now lives in the control-plane repo (`openchime-saas`, CP-4); the durable argument against both is *ownership*, not headline price. |
| **Affordance-only interaction** | ✅ menus/dialogs/drag-drop, no slash-command sprawl (design choice) | mixed (heavy slash surface) | mixed (native `/`-commands: `/status`, `/clear-status`, …) | Unchanged — a deliberate simplicity choice, not a capability claim. |
| **Audit log** | ✅ built, four families, per-family flood cap (ARCH-79) | ✅ paid tiers | ❔ not publicly documented | Likely holds vs Pumble, but ❔ — do not assert publicly without a harder source. |

> **Caveats, stated plainly.** (1) Federation and the hosted flat plan are real code, not just commitments: the control plane's **M0–M7 are built and green** (39 no-DB + 45 Postgres-integration tests, re-run 2026-07-28), with SCIM (M8) the one milestone unbuilt. Read-receipts, self-hosting, native clients and retention control are shipped today. (2) **Client-surface breadth is where we lose to both**: Pumble ships web + Windows + macOS + Linux + iOS + Android today, plus calls, and we ship a Linux/Windows TUI and a Windows GUI — no macOS, no Linux GUI, no web, no mobile. On the §2 tables Pumble is at or near Slack parity on nearly every row where our two clients are ❌. Cheapness and ownership do not compensate for a missing client.

---

## 7. Pumble — profile, plan gating, and two vendor-claim corrections

**What it is.** A Slack-shaped SaaS team chat product from **COING** (Clockify, Plaky), positioned explicitly as the cheap Slack alternative. Deliberately narrower than Slack: no Canvas, no Lists, no Enterprise-Grid equivalent, no cross-org federation, and ~10 named integrations against Slack's 2,600+. Within *core chat*, it is close to Slack parity and ahead of both OpenChime clients on almost every row in §2.

**Plan gating is the story.** Pumble's cheapest tier is not a comparable product — what each vendor gates behind which tier is the real difference (specific per-seat pricing is out of scope for this repo; see the control-plane repo):

| Capability | Cheapest Pumble tier | Cheapest Slack tier |
|---|---|---|
| Unlimited message history + unlimited users | **Free** | Pro (lowest paid) |
| Group video meetings, screen share | Pro | Free (limited) / Pro |
| Guests, roles & permissions, unlimited integrations | Business | Pro |
| **SSO (SAML2/OAuth2)** | **Enterprise (top tier)** | **Business+** (its middle paid tier) |
| **Data-retention policy** | **Enterprise (top tier)** | Business+ |

So the like-for-like comparison depends entirely on whether you need SSO — see §8.

**Two corrections to Pumble's own comparison marketing** (its blog vs its help centre / Slack's site):

1. Its comparison page shows **SSO as a flat ✓ for Pumble**, implying parity. Its pricing page confirms SSO is **Enterprise-only** — its top tier, where Slack's SAML starts at **Business+**, its middle paid tier. *(Corrected 2026-07-28: this line previously said Slack included SAML from Pro, its lowest paid tier. Slack's own help page says Business+ and Enterprise. The point survives — Pumble gates SSO one rung higher on its ladder — but it is narrower than first written, and our own correction of a vendor's marketing had to be corrected in turn.)*
2. It claims **voice/video messages as a Pumble-only feature (✗ for Slack)**. Slack has shipped Clips for years. Treat that page as marketing, not evidence.

**Also worth recording:** Pumble has **no read receipts** (confirmed in its own FAQ — *"seeing who viewed the message is not supported"*), which preserves REQ-090's seen-by as a genuine differentiator against both references; and Pumble **matches our unlimited-history wedge on its free tier**, which retires that argument (see §6).

---

## 8. SSO — what OpenChime supports and does not

*Pricing/seat-cost analysis has been removed from this document — pricing is out of scope for this repo (REQUIREMENTS.md preamble) and lives in the control-plane repo (`openchime-saas`, CP-4). What remains here is the one **capability** row that decides the Slack-vs-Pumble comparison for an enterprise buyer, because our position on it (REQ-027) has to be stated exactly rather than implied by a ✅.*

SSO is the capability that most often decides an enterprise deal, so our own position on it has to be exact:

| | Slack | Pumble | **OpenChime** |
|---|---|---|---|
| SAML 2.0 | ✅ from Business+ (its middle paid tier) | ✅ top tier only | ❌ **not supported — not built, not designed, not on any roadmap** |
| OIDC / social login | ✅ | ✅ OAuth2 (top tier) | 🟡 **designed + daemon-side built**, brokered by the central relay (ARCH-56/57) |
| Bring-your-own IdP direct to the server | ✅ | ✅ | ⛔ **excluded by design** (ARCH-55) — OIDC always routes through central |
| Providers reachable today | Any SAML/OIDC IdP | Any SAML2/OAuth2 IdP | **Google only** (Entra/Apple deferred on the `IUpstreamIdp` seam) |
| End-to-end login working today | ✅ | ✅ | ❌ **client half unbuilt** — browser flow + PKCE + loopback redirect are ⛔ (STATUS.md REQ-020/027, CLIENT.md §6, AUTH.md §7) |

**Consequences to be honest about:**

- **An RFP that says "SAML 2.0 required" disqualifies us outright.** SAML remains the enterprise default for Okta/Entra/Ping estates; ARCH-55's "no direct-to-IdP mode" was chosen to keep the C daemon lean (no JWKS fetching, no multi-provider handling) and that tradeoff is sound — but its cost is exactly this row. Now recorded in the spec as REQ-027.
- **Our OIDC is not equivalent to their SSO.** ARCH-55 routes every OIDC login through the project's central relay. For a self-hoster that means social login costs a runtime dependency on us at login time (AUTH.md §3.4) and gives the project visibility into *who signs into which workspace* (§3.4's stated privacy tradeoff). A stand-alone deployment declining that is on **local accounts only** — no SSO of any kind.
- **Nobody can complete an OIDC login today**, in any deployment model, because the client courier path does not exist. The daemon verifies (`daemon/jwt.c`, tested) and the control plane mints (M5, Google), but nothing carries the token between them. `scripts/demo-oidc.sh` proves the mint↔verify contract with a dev endpoint, deliberately bypassing the browser flow.
- **Where this actually leaves us competitively:** against a buyer who needs SSO, we are *absent*. The credible pitch at the 50–100-seat target is self-hosting, data residency, and read receipts — not identity.

> Recorded here as a gap, not a proposal. If SAML is ever wanted, the ARCH-55-consistent shape is almost certainly **central terminating SAML and continuing to re-issue the same ES256 JWT** (the daemon's verification contract, and its leanness, would not change) — that is a control-plane decision (`openchime-saas`, a CP-N item), not a daemon one. The nearer-term and much cheaper item is finishing the **client OIDC courier path** (REQ-020), which turns a designed capability into a real one.

---

## 9. Sources

Researched 2026-07-26. Vendor-authored comparison content was treated as claim, not evidence (see §7).

**Primary (vendor):** [Pumble pricing](https://pumble.com/pricing) · [Slack pricing](https://slack.com/pricing) · [Pumble help: search](https://pumble.com/help/using-pumble/search-navigation/how-to-search/) · [unread/mark-unread](https://pumble.com/help/using-pumble/messages/unread-messages/) · [scheduled messages](https://pumble.com/help/using-pumble/messages/schedule-messages/) · [recurring messages](https://pumble.com/help/using-pumble/messages/set-recurring-messages/) · [status & availability](https://pumble.com/help/profile/profile-settings/change-your-status-and-availability/) · [custom emoji](https://pumble.com/help/workspace-administration/workspace-settings/add-custom-emojis-to-your-workspace/) · [edit/archive channel](https://pumble.com/help/using-pumble/channels/edit-channel/) · [incoming webhooks](https://pumble.com/help/integrations/add-pumble-apps/incoming-webhooks-for-pumble/) · [changelog](https://pumble.com/help/whats-new/) · [FAQ: who viewed a message](https://pumble.com/help/faq/is-it-possible-to-see-who-viewed-a-message/) · [desktop apps incl. Linux](https://pumble.com/help/getting-started/pumble-apps/pumble-for-desktop/)

**Vendor comparison (claims, cross-checked):** [Pumble vs Slack](https://pumble.com/blog/pumble-vs-slack/)

**Independent / third-party:** [Cloudwards Pumble review](https://www.cloudwards.net/pumble-review/) · [Capterra](https://www.capterra.com/p/218055/Pumble/pricing/) · [GetApp Slack vs Pumble](https://www.getapp.com/collaboration-software/a/slack/compare/pumble/) · [TrustRadius Pumble](https://www.trustradius.com/products/pumble/pricing)
