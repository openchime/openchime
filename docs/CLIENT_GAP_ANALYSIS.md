# OpenChime Four-Way Client Gap Analysis — Slack vs Pumble vs OpenChime TUI vs OpenChime Win32 GUI

*Definitive inventory of every user-facing screen/dialog/panel/surface and feature across the clients. Ground truth for TUI/Win32 is the code inventories; **Slack** and **Pumble** are the researched reference surfaces.*

**Pumble sourcing (researched 2026-07-26).** Pumble is a Slack-shaped SaaS team chat product from COING (the makers of Clockify and Plaky, sold together as the CAKE.com bundle). Its column below is built from Pumble's own pricing page, feature pages, and help centre, cross-checked against independent reviews (Cloudwards) and comparison directories (Capterra, GetApp, TrustRadius). **Vendor-authored comparison content was treated as a claim, not evidence** — where Pumble's marketing and its own help centre disagreed, the help centre won. Two such corrections are recorded in §7.

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
| Emoji picker (composer) | ✅ full Unicode + search + skin tone | ✅ | 🔸 hardcoded 40 emoji | ❌ none in composer | Win32 has NO composer emoji picker; TUI set is hardcoded. **P1** |
| Custom / workspace emoji | ✅ | ✅ (web/desktop only; perms ᴾ) | ❌ | ❌ | Not surfaced. **P2** |
| Colon emoji autocomplete (`:smile:`) | ✅ | ✅ | 🟡 in autocomplete strip | ❌ | Win32 missing. **P1** |
| @mentions + autocomplete | ✅ | ✅ | 🟡 @user autocomplete | ❌ no @-mention autocomplete | Win32 missing entirely. **P0** |
| @here/@channel/@everyone broadcast | ✅ | ✅ + user groups ᴮ | 🔸 unclear/limited | ❌ | Verify wire support; surface in both. **P1** |
| Channel mentions (#chan autocomplete) | ✅ | ✅ | 🟡 #chan autocomplete | ❌ | Win32 missing. **P1** |
| Slash commands | ✅ | ✅ native commands (`/status`, `/clear-status`, …) | ❌ (dispatcher deleted by design) | ❌ | Both references are slash-driven; OpenChime is affordance-driven — **by design**, but note stale slash-hint text still shown in TUI webhooks overlay (bug). — |
| Message drafts (autosave) | ✅ | ✅ | ❌ | ❌ no draft | Neither persists drafts. **P1** |
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
| Pin message | ✅ | ✅ | ❌ | ❌ no pin | **P1** |
| Save for later / bookmark | ✅ | ✅ Saved Items (sidebar bookmark) | ❌ | ❌ | Later hub out; see Navigation. **P2** |
| Mark unread | ✅ | ✅ | ❌ | ❌ no mark-unread | **P1** |
| Remind me about this | ✅ | ✅ reminders | ❌ | ❌ | **P2** |
| Message actions/shortcuts (app) | ✅ | 🟡 custom apps / MCP server | ⛔ | ⛔ | App shortcuts out of scope. |
| Text/code snippets | ✅ | ❔ | ❌ | ❌ | **P2** |
| Giphy/GIF | ✅ | ✅ GIF support + preview controls | ⛔ | ⛔ | App-provided in Slack — out of scope for us. |
| Link unfurling / rich previews | ✅ | ✅ incl. in the editor | ❌ | ❌ | **P2** |
| Inline image/thumbnail render | ✅ | ✅ (+ media carousel) | ❌ (attachment lines only) | ❌ no inline image render | Win32 is D2D — real win to add. **P1** |
| Voice / video messages | ✅ clips | ✅ | ⛔ | ⛔ | Recording out of scope (AUDIO.md is live calls only). |
| Polls | 🔸 via apps | ✅ native | ❌ | ❌ | **P2** |
| Typing indicators | ✅ | ✅ incl. in threads | 🟡 | ✅/🟡 | Present. — |
| Unread divider ("New" line) / jump-to-unread | ✅ | ✅ | 🟡 (unread markers) | ❌ no new-divider/jump-to-unread | Win32 missing. **P1** |
| Message action menu (host) | ✅ | ✅ customizable quick actions | 🟡 | 🟡 6 hardcoded emoji, no copy-link/pin/forward | Both OpenChime clients functional but shallow. **P1** |
| Mark all read | ✅ Shift+Esc | ✅ | ❌ | ❌ | **P2** |
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
| Create channel | ✅ name+desc+visibility+members | ✅ | 🟡 NAME ONLY prompt | ⚪ STUB name only | Neither OpenChime client sets privacy/topic/members. **P1** |
| Channel topic | ✅ | ✅ | ❌ | ❌ | No topic edit/display. **P1** |
| Channel description/About pane | ✅ | ✅ edit-channel pane | ❌ | ❌ no details pane | **P1** |
| Channel details (Members/Pinned/Files tabs) | ✅ | ✅ | ❌ | ❌ | **P1** |
| Member management (add/remove) | ✅ | ✅ | 🟡 via member menu | 🟡 via member menu | Not channel-scoped roster mgmt. **P1** |
| Per-channel notification prefs | ✅ | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel | ✅ | ✅ | 🔸 (via notify?) | ❌ no mute/dim | **P1** |
| Star/favorite channel | ✅ | ✅ | ❌ | ❌ no starred | **P2** |
| Custom sidebar sections | ✅ | ✅ ᴾ customizable sections | 🟡 fixed groups (Public/DM/Private) | ❌ flat list | User-defined sections absent in both ours; TUI has fixed groups. **P2** |
| Sidebar sort/display options | ✅ | ✅ | 🟡 fold/unfold | ❌ | **P2** |
| Archive channel | ✅ | ✅ (reversible, data preserved) | ❌ | ❌ | **P1** |
| Delete channel | ✅ | ✅ | ❌ | ❌ | **P2** |
| Rename channel | ✅ | 🔸 creator only | ❌ | ❌ | **P1** |
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
| Message yourself / self-DM | ✅ | ✅ | ❌ | ❌ | Note: the **engine** supports self-DM (REQ-055); neither frontend surfaces it. **P2** |
| New DM start | ✅ | ✅ | 🟡 new-DM prompt | 🟡 (via member) | Present. — |
| Members / roster panel | ✅ | ✅ | 🟡 click-to-DM | 🟡 WORKSPACE roster (not per-channel), no search/scroll/count, 256 cap | Ours not channel-scoped; Win32 capped. **P1** |
| View other-user profile | ✅ rich profile pane | ✅ | ❌ (roster overlay unreachable) | ❌ MISSING | Neither of ours can view a peer's profile. **P0** |
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
| Global notify-me level | ✅ | ✅ | 🔸 prefs overlay read-only | ❌ no global settings | **P1** |
| Per-channel/DM overrides | ✅ | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel/DM | ✅ | ✅ | 🔸 | ❌ | **P1** |
| Keyword/highlight-word notifications | ✅ | ✅ | ❌ | ❌ | **P2** |
| Thread-reply notifications toggle | ✅ | ✅ | ❌ | ❌ | **P2** |
| Notification schedule (quiet hours) | ✅ | ✅ custom schedules | 🔸 DND raw text | ⚪ DND raw text | See DND. **P1** |
| DND config surface | ✅ | ✅ | 🔸 | ⚪ | **P1** |
| Activity feed (mentions/reactions/replies) | ✅ | ✅ | ❌ | ❌ no notification inbox/activity | **P1** |
| Activity filters / saved views | ✅ | ❔ | ❌ | ❌ | **P2** |
| All-unreads view | ✅ | ✅ | ❌ | ❌ | **P2** |
| Desktop/OS toast + preview toggle | ✅ | ✅ | ❌ | ❌ no OS toast | Win32 native toast is high-value. **P1** |
| Notification sounds & badges | ✅ | ✅ | ❌ | ❌ | **P2** |
| Email notifications | ✅ | ✅ (email reminder after 24 h inactivity) | ⛔/❌ | ⛔/❌ | Server-side; not a client surface. — |
| Mobile push | ✅ | ✅ | 🔗 daemon emitter built (ARCH-85); no mobile client | 🔗 same | Push is a **federated-only** function for us (REQ-133). — |
| VIP / priority people | ✅ | ❔ | ❌ | ❌ | **P2** |
| Review list_notify_prefs screen | ✅ | ✅ | 🔸 read-only overlay | ❌ | **P1** |

### 2.7 Search

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Search entry | ✅ top bar | ✅ top bar | 🟡 search prompt | 🔸 modal prompt only (no in-overlay box) | Ours both prompt-driven. **P1** |
| Results view | ✅ | ✅ navigable | 🔸 read-only, NON-navigable, shows userNNN not names | 🔸 read-only, no scroll | Ours un-navigable; Win32 click jumps to CHANNEL not the matched message. **P0** |
| Search modifiers (from:/in:/has:…) | ✅ full grammar | 🟡 `from:` + checkbox filters | ❌ | ❌ | **P2** |
| Search filters panel | ✅ | ✅ date, channel, has-file/reaction/link, exclude webhooks | ❌ | ❌ | **P2** |
| Result tabs (Messages/Files/Channels/People) | ✅ | ✅ (+ Apps) | ❌ | ❌ | **P2** |
| Sort (relevance/newest) | ✅ | ❔ | ❌ | ❌ | **P2** |
| Term highlight in results | ✅ | ✅ | ❌ | ❌ | **P1** |
| Paging / load-more | ✅ | ✅ | ❌ | ❌ (128 cap, ignores truncation) | **P1** |
| Full history searchable on the free tier | ❌ 90-day cap | ✅ unlimited | ✅ (no cap, ever) | ✅ | **Pumble matches our "no history cap" wedge** — it is no longer a Slack-only differentiator. — |
| Quick switcher search (people/channels) | ✅ | ✅ | ✅ Ctrl+K palette | ❌ | See Navigation. **P1** |
| AI/enterprise search | ✅ | 🟡 AI Assistant add-on | ⛔ | ⛔ | Out of scope. |

### 2.8 Files / Attachments

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| File upload | ✅ drag-drop/paste/1GB | ✅ | 🔸 raw path text, no browser | 🟡 + button + drag-drop, no preview/progress | TUI needs a file picker. **P1** |
| Download file | ✅ | ✅ | 🔸 auto first attachment, no chooser | 🟡 native Save dialog | TUI has no chooser/save-path. **P1** |
| Inline image previews/thumbnails | ✅ | ✅ | ❌ (attach lines only) | ❌ no inline render | **P1** |
| Media carousel / gallery | ✅ | ✅ (iOS; files, links, docs) | ❌ | ❌ | **P2** |
| Inline video/audio playback | ✅ | ✅ | ⛔ | ⛔ | Media playback out of scope. |
| Inline PDF/doc preview | ✅ | ✅ clickable previews | ❌ | ❌ open-in-place missing | **P2** |
| Code/text snippets | ✅ | ❔ | ❌ | ❌ | **P2** |
| Files browser / channel Files tab | ✅ | ✅ (Files tab in search) | ❌ | ❌ | **P2** |
| File comments/sharing/permissions | ✅ | ❔ | ❌ | ❌ | **P2** |
| External file services (Drive/Dropbox) | ✅ | ✅ Google Drive | ⛔ | ⛔ | Out of scope. |
| Upload progress / preview | ✅ | ✅ | ❌ | ❌ | **P2** |
| Storage model | per-workspace by plan | 10 GB workspaceᶠ → 10/20/100 GB **per seat** ᴾᴮᴱ | operator's disk (ARCH-77 tiers) | same | We have no seat-metered storage — the ceiling is the box. — |
| Attachment lines in transcript | ✅ | ✅ | ✅ incl reclaimed | ✅ attachment lines | Present. — |

### 2.9 Account & Profile

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Login dialog | ✅ | ✅ | ✅ fields+remember-me+validation+retry | ✅ themed native, prefilled; NO error display on fail, no remember-me surfaced | Win32 login shows no error on bad login. **P0** |
| Signup / first-owner UI | ✅ | ✅ self-serve | ❌ | ❌ | Neither client has signup/first-owner flow. **P1** |
| Profile viewer (self) | ✅ editor | ✅ editor | 🟡 READ-ONLY viewer | ⚪ name+password only, chained prompts | Neither of ours is a real editor. **P1** |
| Change display name | ✅ | ✅ | 🟡 | 🟡 (chained prompt) | Present. — |
| Change password | ✅ | ✅ | 🟡 2 prompts | 🟡 (no confirm field) | Win32 lacks confirm field. **P1** |
| Avatar upload | ✅ | ✅ | ❌ | ❌ | **P2** |
| Email edit | ✅ | ✅ | ❌ | ❌ | **P1** |
| Timezone | ✅ | ✅ | ❌ | ❌ | **P2** |
| Custom status set | ✅ | ✅ | ❌ | ❌ | See Presence. **P1** |
| Job title / pronouns / custom fields | ✅ | 🟡 profile fields | ❌ | ❌ | **P2** |
| Connecting/reconnect screen | ✅ | ✅ | ⚪ stub | 🟡 reconnect ok, 🔸 no toast/banner/countdown | **P1** |
| Preferences dialog (hub) | ✅ | ✅ | ❌ (file-only config) | ❌ no settings screen | No in-app settings editor in either of ours. **P0** |
| Themes / appearance | ✅ | ✅ | ❌ no in-app toggle | ❌ | **P2** |
| 12/24h & panel toggles in-app | ✅ | ✅ | ❌ (file-only) | ❌ | **P2** |

### 2.10 Admin

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Invite people | ✅ email/link/roles/CSV | ✅ email + link | 🔸 hardcoded MEMBER only, token in hard-to-see banner | ⚪ member/admin only, token via MessageBox, no expiry/target/list/revoke | Both of ours very thin; no invite management. **P1** |
| Invite link (shareable) | ✅ | ✅ | ❌ | ❌ | **P2** |
| Guest accounts (single/multi-channel) | ✅ | ✅ ᴮ (5 single-channel guests free per paid seat) | ❌ | ❌ | **P2** |
| Role assignment (owner/admin/member) | ✅ | ✅ ᴮ permissions & roles | 🟡 role mgmt | 🟡 | Present. — |
| Deactivate / remove user | ✅ | ✅ | 🔸 remove, NO confirm | ❌ no deactivate in member menu | Win32 missing; TUI unsafe. **P1** |
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
| SSO — **SAML 2.0** | ✅ ᴾ (Pro) | ✅ **ᴱ Enterprise only** | ⛔ **not supported at all** | ⛔ | **No SAML anywhere in the product** — not built, not designed (ARCH-55 has no SAML path; recorded as REQ-027). Disqualifying in SAML-mandatory RFPs. See §8. |
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
| Header bar | ✅ | ✅ | 🟡 | 🟡 channel name + typing line; no member count/header actions (the channel-column header has settings/compose buttons) | **P2** |
| Sidebar channel/DM list | ✅ | ✅ | ✅ grouped, unreads, badges, lock/@ markers | 🟡 flat single "CHANNELS" group + a filter box; hides unnamed, 512-row cap, no scroll, no DM section/grouping/collapse/starred/muted | Win32 gained the filter (nav epic) but is still flat and unscrollable. **P0** |
| Custom sidebar sections | ✅ | ✅ ᴾ | 🟡 fixed groups | ❌ | **P2** |
| Unreads-only sidebar mode | ✅ | ✅ | ❌ | ❌ | **P2** |
| Quick switcher / command palette | ✅ Cmd+K | ✅ quick search | ✅ Ctrl+K ~19 fuzzy actions | ❌ rail menus + a name filter; NO Ctrl+K/keyboard palette | Win32 actions are mouse-driven off the rail; no keyboard-driven action surface. **P1** |
| Global search bar | ✅ | ✅ | 🟡 prompt | 🔸 modal | See Search. **P1** |
| Channel browser / add channels | ✅ | ✅ | ❌ | ❌ | **P1** |
| New message / compose (Cmd+N) | ✅ | ✅ | 🟡 new-DM/new-channel prompts | 🟡 | Present. — |
| Profile/account menu | ✅ | ✅ | 🟡 via launcher | 🟡 profile menu off the rail avatar | Present. — |
| Preferences hub | ✅ | ✅ | ❌ | ❌ | **P0** (settings screen) |
| Themes/appearance | ✅ | ✅ | ❌ | ❌ | **P2** |
| Keyboard shortcuts reference | ✅ | ✅ | ✅ help overlay RICH | ❌ | Win32 has no shortcut help. **P2** |
| History nav (back/forward) | ✅ | ❔ | ❌ | ❌ | **P2** |
| Move between unreads (keyboard) | ✅ | ❔ | 🟡 | ❌ | **P2** |
| Right-hand panel system | ✅ | ✅ | 🟡 overlays | 🟡 overlays | Ours overlay-based, mostly read-only. **P1** |
| Star/favorite conversations | ✅ | ✅ | ❌ | ❌ | **P2** |
| Autocomplete strip (@/#/:emoji) | ✅ | ✅ | 🟡 | ❌ | Win32 missing. **P1** |
| Keybinding hint bar | ✅ | ❌ | 🟡 | ❌ | **P2** |
| Error / toast surface (send fail, rate-limit, bad login) | ✅ | ✅ | 🔸 partial | 🔸 only `last_error` in the empty transcript; no toast/banner, login shows no auth error | Win32 has no transient toast/banner and the login dialog surfaces no error. **P0** |
| Terminal / TUI client | ❌ | ❌ | ✅ **the only one of the four** | — | Genuine category differentiator. — |
| Slackbot conversation | ✅ | ✅ Pumblebot | ⛔ | ⛔ | Out of scope. |

### 2.14 Session / Workspace

| Feature / Surface | Slack | Pumble | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|---|
| Multi-workspace switcher rail | ✅ | ✅ | ✅ remembered+open, unread, per-ws state | 🟡 rail switcher built (single-client stop/reconnect; no N-concurrent, no background unread) | Win32 has the switcher UI; the remaining delta is the TUI's N-concurrent-client model. **P1** |
| Add workspace | ✅ | ✅ | 🟡 | 🟡 "Add a workspace…" in the switcher | **P1** |
| Workspace switch by number | ✅ Cmd+1..9 | ❔ | 🟡 | ❌ | **P2** |
| Reconnect / auto-reconnect | ✅ | ✅ | ⚪ stub | 🟡 works, no countdown/banner | **P1** |
| Connection status indicator | ✅ | ✅ | 🟡 status line | 🔸 no toast/banner/countdown | **P1** |
| Logout | ✅ | ✅ | 🟡 no log-out-everywhere | ✅ Sign out / Sign out everywhere | Present; TUI lacks revoke-all. — |
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

1. **Search results (both clients)** — *Today:* read-only, non-navigable list; TUI shows raw `userNNN` instead of names; Win32 click jumps to the CHANNEL not the matched message, no highlight, no scroll, 128-result cap ignoring truncation. *Done:* navigable results (arrow/click to the exact message), resolved display names, term highlighting, in-overlay query box + refine, paging/load-more, filter tabs. **P0**

2. **Win32 error/failure surface (near-absent)** — *Today:* no transient toast/banner for send failure, rate-limit, or storage pressure, and the login dialog shows nothing on auth failure; the only failure surface is `last_error` text rendered in an empty transcript (winmain.c). *Done:* inline login error text + a global toast/banner channel for transient failures with retry affordance. **P0**

3. **Settings / Preferences (missing in both)** — *Today:* config is file-only; no in-app `set_setting` surface, no theme/12-24h/panel toggles. *Done:* a Preferences hub (notifications, appearance, time format, sidebar behavior) writing through the existing config. **P0**

4. **DND / notification schedule (both)** — *Today:* raw `HH:MM`/`HH:MM-HH:MM` text parse, no pickers, no schedule display, no "others see DND." *Done:* time pickers, recurring-schedule editor, current-state display, and a DND indicator to peers. **P1**

5. **Audit log (both)** — *Today:* read-only, no scroll/paging/filter/search/export; TUI ~22-row cap, Win32 offset fixed at 0. *Done:* paged/scrollable view with actor/action/date filters and export. **P1**

6. **Webhooks overlays (both)** — *Today:* read-only lists; TUI still shows a STALE slash-command hint and CANNOT delete (core supports delete); Win32 delete is via MessageBox with no enable/disable/rotate/reveal/date and no scroll. *Done:* full CRUD (create/reveal/rotate/enable-disable/delete), metadata columns, scroll. TUI delete is a quick core-backed win. **P1**

7. **Invite dialog (both)** — *Today:* TUI hardcodes MEMBER role with token in a hard-to-see banner; Win32 is member/admin only with token via MessageBox; neither has expiry/target/pending-list/revoke. *Done:* role + expiry + target selection, a pending-invites list with revoke, and a copyable token/link surface. **P1**

8. **Thread overlays (both)** — *Today:* TUI is a read-only dump with no per-reply actions; Win32 has a working reply composer but replies are read-only (no react/edit/delete) and there's no scrollbar. *Done:* full per-reply actions inside the thread + scrollbar. **P1**

9. **Profile / account editor (both)** — *Today:* TUI is a read-only viewer; Win32 chains one-line prompts for name+password only (no confirm field). Neither edits avatar/email/timezone/status. *Done:* a real profile editor form with validation and the missing fields. **P1**

10. **New-channel dialog (both)** — *Today:* name-only (TUI prompt / Win32 stub); no privacy, topic, or member step. *Done:* full create flow (visibility toggle, topic/description, add-members). **P1**

11. **Win32 sidebar** — *Today:* flat list, hides unnamed channels, 512 cap, no DM section, no sections/collapsing/search/starred/muted. *Done:* grouped Public/DM/Private sections (TUI-parity), search box, mute/star affordances, load-more. **P0**

12. **Storage report (Win32)** — *Today:* read-only KV dump, no refresh/actions/history (TUI's is RICH by comparison). *Done:* match TUI richness + refresh. **P2**

13. **Members/roster panel (both)** — *Today:* workspace-wide roster (not per-channel), no search/scroll/count; Win32 256-cap. *Done:* per-channel roster, search, count, scroll, profile-on-click. **P1**

14. **Win32 "six-forms-in-one-prompt" anti-pattern** — *Today:* six distinct flows collapse into one generic single-line native prompt. *Done:* purpose-built dialogs per flow. **P1**

15. **Roster overlay (TUI)** — *Today:* THIN and effectively UNREACHABLE (no input path). *Done:* wire an entry point or remove. **P2**

---

## 4. Missing Entirely (per client)

### Win32 GUI — missing
- **N-concurrent-workspace model** — the rail switcher UI exists, but Win32 stop/reconnects a single client; no holding N clients at once with background unread ("N elsewhere"), as the TUI does. **P1**
- **Settings/Preferences screen** — the workspace menu has a **Preferences** item, but it opens a "coming soon" `MessageBox`; no in-app config exists. **P0**
- **@-mention / #channel / :emoji autocomplete** in composer. **P0**
- **Composer emoji picker** (none; only 6 hardcoded reaction emoji). **P1**
- **Error/toast surface** for any failure (login, send, rate-limit). **P0**
- **Command palette / Ctrl+K** (rail menus only; no keyboard-driven action surface). **P1**
- **DM section/list** in sidebar; **sidebar sections/collapsing/starred/muted** and sidebar **scrolling** past the visible rows. The rail's **DMs** view exists but renders the same flat channel sidebar as Home — it has no DM-specific behaviour. **P0**
- **View other-user profile.** **P0**
- **Notification inbox / Activity feed; OS toast; global notify prefs; `list_notify_prefs` review** — the rail's **Activity** and **Alerts** entries are "coming soon" stubs. **P1**
- **Channel rename / topic / archive / delete / details pane.** **P1**
- **Jump-to-unread / new-message divider / mark-unread.** **P1**
- **Copy-link / permalink, pin, forward** on messages. **P1**
- **Inline image/thumbnail rendering; open-in-place.** **P1**
- **Signup / first-owner UI; remember-me surfaced; login error display.** **P0**
- **Avatar / rich profile editor; custom status; email/timezone.** **P1**
- **Active-session list** (revoke-all *is* built — "Sign out everywhere"; listing sessions is not). **P2**
- **Notification-prefs review screen** (`list_notify_prefs` never called). **P1**
- **Keyboard shortcut reference/help overlay.** **P2**
- **Search paging, filters, term highlight, in-overlay input, jump-to-message.** **P0**
- **Draft persistence; scheduled send; drafts hub.** **P2**
- **Reconnect countdown/banner.** **P1**
- **Stub rail views** — **Activity** (REQ-139), **Files** (REQ-143), **Later** (REQ-231) and **Notifications** are reachable placeholders rendering "coming soon", not built surfaces. **P1**

### TUI — missing
- **Delete webhook** (core supports it; not surfaced). **P1**
- **Log-out-everywhere / revoke sessions** (core supports `OC_LOGOUT_ALL`; Win32 surfaces it, the TUI does not). **P2**
- **In-app settings / config editor** (file-only; no theme/12-24h/panel toggles). **P0**
- **Invite-as-admin / invite roles + expiry / pending-invite list.** **P1**
- **Destructive confirmations** (delete message, remove user have none). **P1**
- **Multi-line composer.** **P1**
- **Composer emoji beyond hardcoded 40; custom emoji.** **P2**
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
> gap below, numbered `WIN-1`…`WIN-54`, split by whether it is buildable today or
> blocked on daemon work. This section stays the *rationale* for the ordering.

> **The nav epic changed where several of these land.** The left-nav rail
> (STATUS.md §"The shell") already ships the *destinations* for items 5, 7 and 8
> — Preferences, Later, Activity/Alerts and Files are all rail entries rendering
> "coming soon". So those items are now "fill in a stub view", not "invent a
> surface", which lowers their cost. Item 3's sidebar filter box also exists;
> what remains there is grouping, a DM section, scrolling, and mute/star.

1. **Global error/toast + login error display.** Failures are currently silent — highest trust/usability risk, small surface. **P0**
2. **Search that works** — navigable results, jump-to-matched-message (not channel), name resolution, in-overlay query + paging + highlight. **P0**
3. **Sidebar overhaul** — DM section + Public/Private grouping (TUI parity), collapsing, scrolling, mute/star, load-more past 512; the filter box already exists, and the rail's **DMs** view needs real DM-only behaviour. **P0**
4. **Composer autocomplete + emoji picker** — @mention/#channel/:emoji popover and a real emoji picker (drop the 6 hardcoded). Core chat affordance. **P0**
5. **Settings / Preferences screen** — replace the workspace menu's "coming soon" Preferences box; write through existing config (time format, notifications, appearance). **P0**
6. **View other-user profile** — clickable avatar/name → profile pane. **P0**
7. **Message actions depth** — copy-link/permalink, pin, forward, mark-unread; jump-to-unread + new-message divider. **P1**
8. **Notifications: OS toast + Activity/notification inbox (fills the rail's Activity + Alerts stubs) + a `list_notify_prefs` review screen + editable global prefs; DND with pickers.** **P1**
9. **Channel management** — rename/topic/archive/details pane + a real create-channel dialog (privacy/topic/members). **P1**
10. **Command palette (Ctrl+K)** + inline image/thumbnail rendering (D2D makes this a strong differentiator); the **Files** and **Later** rail stubs are the natural follow-on. **P1**

*Follow-ups just behind the top 10: full webhook CRUD, paged audit log, richer invite dialog, per-reply thread actions, real profile editor, the N-concurrent-workspace model (the rail switcher UI is already built; the delta is holding N clients at once), reconnect banner/countdown, and replacing the six-forms-in-one-prompt pattern with purpose-built dialogs.*
---

## 6. Where OpenChime Exceeds / Differs Favorably — now tested against *both* references

This analysis is reference-centric (what Slack and Pumble have that we lack). For balance, the places OpenChime is **ahead of or deliberately different from** them. **Adding Pumble materially weakens several claims that held against Slack alone** — those are marked ⚠️ and are the honest headline of this revision.

| Area | OpenChime | Slack | Pumble | Verdict |
|---|---|---|---|---|
| **Read receipts (seen-by)** | ✅ per-message "✓ Seen by …" from per-member read cursors (REQ-090, `readers[]`) | ❌ none (deliberately) | ❌ none — *"seeing who viewed the message is not supported"* (Pumble FAQ) | **Holds against both.** Still the single cleanest capability differentiator. |
| **Self-hosting / data ownership** | ✅ run your own daemon; data in your SQLite/blob store | ❌ SaaS-only | ❌ SaaS-only, US-hosted | **Holds, and widens** — Cloudwards scored Pumble's privacy 60% and flagged US data-disclosure exposure. |
| **Data residency / jurisdiction** | ✅ your box, your country, air-gappable | 🟡 paid residency options | ❌ none | **Holds against both.** |
| **Deployment models** | ✅ stand-alone / federated / hosted (ARCH-76) | ❌ single cloud | ❌ single cloud | **Holds against both.** |
| **Terminal client** | ✅ full-featured TUI (every engine feature reachable) | ❌ | ❌ | **Holds against both** — no mainstream competitor ships one. |
| **Native, lightweight clients** | ✅ pure-C native per platform — TUI + Win32 GUI (~3 MB, Direct2D), no Electron | ❌ Electron desktop (heavy) | ❌ web-tech desktop app (Win/mac/Linux `.deb`/`.rpm`) | **Holds on footprint** — but see the caveat below: Pumble *ships* Linux/macOS/mobile clients today and we do not. |
| **Full history on the free tier** | ✅ self-hosted = unlimited retention (you set policy) | ❌ free tier caps at 90 days | ✅ **unlimited history on the free plan** | ⚠️ **No longer a differentiator.** This was our stated wedge against Slack (ARCH-15 calls FTS5 "a competitive wedge against Slack's history caps"). Pumble gives it away free. Against Pumble the wedge must be restated as *ownership*, not *retention*. |
| **Pricing model** | ✅ self-hosted free; hosted flat plan | ❌ per-user seat pricing | 🟡 free tier w/ unlimited users; cheap per-seat above it | ⚠️ **Weakened but survives.** Detailed price analysis is out of scope for this repo and now lives in the control-plane repo (`openchime-saas`, CP-4); the durable argument against both is *ownership*, not headline price. |
| **Affordance-only interaction** | ✅ menus/dialogs/drag-drop, no slash-command sprawl (design choice) | mixed (heavy slash surface) | mixed (native `/`-commands: `/status`, `/clear-status`, …) | Unchanged — a deliberate simplicity choice, not a capability claim. |
| **Audit log** | ✅ built, four families, per-family flood cap (ARCH-79) | ✅ paid tiers | ❔ not publicly documented | Likely holds vs Pumble, but ❔ — do not assert publicly without a harder source. |

> **Caveats, stated plainly.** (1) Federation and the hosted flat plan are architectural/product commitments (ARCH-76 / control-plane M0–M7) at varying maturity; read-receipts, self-hosting, native clients, and retention control are shipped today. (2) **Client-surface breadth is where we lose to both**: Pumble ships web + Windows + macOS + Linux + iOS + Android today, plus calls, and we ship a Linux/Windows TUI and an incomplete Windows GUI. On the §2 tables Pumble is at or near Slack parity on nearly every row where our two clients are ❌. Cheapness and ownership do not compensate for a missing client.

---

## 7. Pumble — profile, plan gating, and two vendor-claim corrections

**What it is.** A Slack-shaped SaaS team chat product from **COING** (Clockify, Plaky), positioned explicitly as the cheap Slack alternative. Deliberately narrower than Slack: no Canvas, no Lists, no Enterprise-Grid equivalent, no cross-org federation, and ~10 named integrations against Slack's 2,600+. Within *core chat*, it is close to Slack parity and ahead of both OpenChime clients on almost every row in §2.

**Plan gating is the story.** Pumble's cheapest tier is not a comparable product — what each vendor gates behind which tier is the real difference (specific per-seat pricing is out of scope for this repo; see the control-plane repo):

| Capability | Cheapest Pumble tier | Cheapest Slack tier |
|---|---|---|
| Unlimited message history + unlimited users | **Free** | Pro (lowest paid) |
| Group video meetings, screen share | Pro | Free (limited) / Pro |
| Guests, roles & permissions, unlimited integrations | Business | Pro |
| **SSO (SAML2/OAuth2)** | **Enterprise (top tier)** | **Pro (lowest paid)** |
| **Data-retention policy** | **Enterprise (top tier)** | Business+ |

So the like-for-like comparison depends entirely on whether you need SSO — see §8.

**Two corrections to Pumble's own comparison marketing** (its blog vs its help centre / Slack's site):

1. Its comparison page shows **SSO as a flat ✓ for Pumble**, implying parity. Its pricing page confirms SSO is **Enterprise-only** — its top tier. Slack includes SAML SSO from **Pro**, its *lowest paid* tier.
2. It claims **voice/video messages as a Pumble-only feature (✗ for Slack)**. Slack has shipped Clips for years. Treat that page as marketing, not evidence.

**Also worth recording:** Pumble has **no read receipts** (confirmed in its own FAQ — *"seeing who viewed the message is not supported"*), which preserves REQ-090's seen-by as a genuine differentiator against both references; and Pumble **matches our unlimited-history wedge on its free tier**, which retires that argument (see §6).

---

## 8. SSO — what OpenChime supports and does not

*Pricing/seat-cost analysis has been removed from this document — pricing is out of scope for this repo (REQUIREMENTS.md preamble) and lives in the control-plane repo (`openchime-saas`, CP-4). What remains here is the one **capability** row that decides the Slack-vs-Pumble comparison for an enterprise buyer, because our position on it (REQ-027) has to be stated exactly rather than implied by a ✅.*

SSO is the capability that most often decides an enterprise deal, so our own position on it has to be exact:

| | Slack | Pumble | **OpenChime** |
|---|---|---|---|
| SAML 2.0 | ✅ from its lowest paid tier | ✅ top tier only | ❌ **not supported — not built, not designed, not on any roadmap** |
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
