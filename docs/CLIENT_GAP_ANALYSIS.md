# OpenChime Three-Way Client Gap Analysis — Slack vs OpenChime TUI vs OpenChime Win32 GUI

*Definitive inventory of every user-facing screen/dialog/panel/surface and feature across the three clients. Ground truth for TUI/Win32 is the code inventories; Slack is the researched reference surface.*

## 1. Legend

| Glyph | Meaning |
|-------|---------|
| ✅ | Rich — fully developed, near-parity |
| 🟡 | Adequate — functional, real gaps in depth/polish |
| 🔸 | Thin — exists but read-only, non-navigable, or heavily limited |
| ⚪ | Stub — placeholder / minimal / raw-text stand-in |
| ❌ | Missing — not present at all |
| ⛔ | Out of scope by design — intentionally not on OpenChime's wire |

**Priority key (in gap notes):** P0 = core parity, must-have · P1 = important · P2 = nice-to-have · — = n/a.

---

## 2. Category Tables

### 2.1 Messaging

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Message composer (basic send) | ✅ | 🟡 single-line only | ✅ RichEdit, Enter-send + button | TUI needs multi-line composer. **P1** |
| Rich-text formatting toolbar (bold/italic/strike/code) | ✅ | ❌ | ❌ | No WYSIWYG or markup toolbar in either client. **P2** |
| Markdown/mrkdwn shortcuts | ✅ | ❌ | ❌ | No inline `*bold*`/`_italic_` auto-convert. **P2** |
| Ordered/unordered lists, blockquote, code block buttons | ✅ | ❌ | ❌ | — **P2** |
| Hyperlink insertion dialog | ✅ | ❌ | ❌ | **P2** |
| Block Kit rich layout | ✅ | ⛔ | ⛔ | App-authored blocks — out of scope (no app platform). |
| Emoji picker (composer) | ✅ full Unicode + search + skin tone | 🔸 hardcoded 40 emoji | ❌ none in composer | Win32 has NO composer emoji picker; TUI set is hardcoded. **P1** |
| Custom / workspace emoji | ✅ | ❌ | ❌ | Not surfaced. **P2** |
| Colon emoji autocomplete (`:smile:`) | ✅ | 🟡 in autocomplete strip | ❌ | Win32 missing. **P1** |
| @mentions + autocomplete | ✅ | 🟡 @user autocomplete | ❌ no @-mention autocomplete | Win32 missing entirely. **P0** |
| @here/@channel/@everyone broadcast | ✅ | 🔸 unclear/limited | ❌ | Verify wire support; surface in both. **P1** |
| Channel mentions (#chan autocomplete) | ✅ | 🟡 #chan autocomplete | ❌ | Win32 missing. **P1** |
| Slash commands | ✅ | ❌ (dispatcher deleted by design) | ❌ | OpenChime is affordance-driven, not slash-driven — **by design**, but note stale slash-hint text still shown in TUI webhooks overlay (bug). — |
| Message drafts (autosave) | ✅ | ❌ | ❌ no draft | Neither persists drafts. **P1** |
| Drafts & sent hub | ✅ | ❌ | ❌ | **P2** |
| Scheduled send | ✅ | ❌ | ❌ | **P2** |
| Edit message | ✅ | 🟡 edit flow | 🟡 (edited) shown; edit via context | TUI/Win32 both edit; thread replies NOT editable in Win32. **P1** |
| Delete message | ✅ | 🔸 NO confirmation | 🟡 via context menu | Add destructive confirm in TUI. **P1** |
| Reactions (emoji) | ✅ any emoji | 🟡 (hardcoded set) | 🟡 6 hardcoded emoji | Both limited to hardcoded reaction sets. **P1** |
| Who-reacted list | ✅ | 🔸 read-only | 🔸 read-only overlay | Read-only both. **P2** |
| Quick/one-click reactions | ✅ top-3 inline | ❌ | ❌ | **P2** |
| Copy link / permalink | ✅ | ❌ | ❌ no copy-link | **P1** |
| Copy text | ✅ | 🔸 (via selection?) | ✅ text-select + Ctrl+C | TUI has click-drag select+copy (recent). Win32 has it. **P2** |
| Forward / share message | ✅ | ❌ | ❌ no forward | **P2** |
| Pin message | ✅ | ❌ | ❌ no pin | **P1** |
| Save for later / bookmark | ✅ | ❌ | ❌ | Later hub out; see Navigation. **P2** |
| Mark unread | ✅ | ❌ | ❌ no mark-unread | **P1** |
| Remind me about this | ✅ | ❌ | ❌ | **P2** |
| Message actions/shortcuts (app) | ✅ | ⛔ | ⛔ | App shortcuts out of scope. |
| Text/code snippets | ✅ | ❌ | ❌ | **P2** |
| Giphy/GIF | ✅ | ⛔ | ⛔ | App-provided — out of scope. |
| Link unfurling / rich previews | ✅ | ❌ | ❌ | **P2** |
| Inline image/thumbnail render | ✅ | ❌ (attachment lines only) | ❌ no inline image render | Win32 is D2D — real win to add. **P1** |
| Typing indicators | ✅ | 🟡 | ✅/🟡 | Present. — |
| Unread divider ("New" line) / jump-to-unread | ✅ | 🟡 (unread markers) | ❌ no new-divider/jump-to-unread | Win32 missing. **P1** |
| Message action menu (host) | ✅ | 🟡 | 🟡 6 hardcoded emoji, no copy-link/pin/forward | Both functional but shallow. **P1** |
| Mark all read | ✅ Shift+Esc | ❌ | ❌ | **P2** |
| Transcript rendering | ✅ | ✅ wrapped, colors, times, (edited), tombstones, reactions, attach lines, N replies, seen-by | ✅ grouped, avatars, dividers, seen-by, scrollbar; 600-msg cap | Both RICH. Win32 caps at 600 msgs w/ no load-more. **P1** (paging) |

### 2.2 Threads

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Threaded replies | ✅ | 🔸 read-only dump, no per-reply actions | 🟡 reply composer works, replies read-only | Neither allows react/edit/delete inside thread. **P1** |
| Also-send-to-channel | ✅ | ❌ | ❌ | **P2** |
| Threads view (all threads) | ✅ | ❌ | ❌ | Aggregated followed-threads view absent. **P2** |
| Follow/unfollow thread | ✅ | ❌ | ❌ | **P2** |
| Participant avatars + reply count | ✅ | 🟡 N replies shown | 🟡 shown | Present inline. — |
| Thread overlay scrollbar | ✅ | 🔸 none | 🔸 no scrollbar | Both un-scrollable. **P1** |

### 2.3 Channels

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Public channels | ✅ | ✅ grouped Public section | 🟡 flat list | Win32 has no sectioning. **P1** |
| Private channels | ✅ | ✅ Private group + lock marker | 🟡 flat, lock unclear | **P1** |
| Create channel | ✅ name+desc+visibility+members | 🟡 NAME ONLY prompt | ⚪ STUB name only | Neither sets privacy/topic/members. **P1** |
| Channel topic | ✅ | ❌ | ❌ | No topic edit/display. **P1** |
| Channel description/About pane | ✅ | ❌ | ❌ no details pane | **P1** |
| Channel details (Members/Pinned/Files tabs) | ✅ | ❌ | ❌ | **P1** |
| Member management (add/remove) | ✅ | 🟡 via member menu | 🟡 via member menu | Not channel-scoped roster mgmt. **P1** |
| Per-channel notification prefs | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel | ✅ | 🔸 (via notify?) | ❌ no mute/dim | **P1** |
| Star/favorite channel | ✅ | ❌ | ❌ no starred | **P2** |
| Custom sidebar sections | ✅ | 🟡 fixed groups (Public/DM/Private) | ❌ flat list | User-defined sections absent both; TUI has fixed groups. **P2** |
| Sidebar sort/display options | ✅ | 🟡 fold/unfold | ❌ | **P2** |
| Archive channel | ✅ | ❌ | ❌ | **P1** |
| Delete channel | ✅ | ❌ | ❌ | **P2** |
| Rename channel | ✅ | ❌ | ❌ | **P1** |
| Posting/management permissions | ✅ | ❌ | ❌ | **P2** |
| Channel bookmarks | ✅ | ❌ | ❌ | **P2** |
| Channel canvas | ✅ | ⛔ | ⛔ | Canvas out of scope. |
| Leave channel | ✅ | 🟡 in channel action menu | 🟡 (verify) | TUI has Leave; Win32 context menu lacks it? **P1** |
| Join / open channel | ✅ | 🟡 Join/Open menu | 🟡 | Present. — |
| Browse channels directory | ✅ searchable/filter/sort | ❌ | ❌ no sidebar search | No channel browser/search. **P1** |
| Default channels | ✅ | ❌ (admin) | ❌ | Admin — **P2** |
| Slack Connect (shared/external) | ✅ | ⛔ | ⛔ | Out of scope. |
| Channel context/action menu | ✅ | 🟡 Join/Open/Notify/Webhooks/Leave | 🟡 no rename/topic/archive/delete/details | Win32 menu very shallow. **P1** |

### 2.4 People & DMs

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Direct messages (1:1) | ✅ | ✅ DM group in sidebar | 🟡 no dedicated DM list/section | Win32 lacks a DM section. **P0** |
| Group DMs (multi-person) | ✅ up to 8 | ❌ | ❌ | Verify wire support. **P2** |
| Message yourself / self-DM | ✅ | ❌ | ❌ | **P2** |
| New DM start | ✅ | 🟡 new-DM prompt | 🟡 (via member) | Present. — |
| Members / roster panel | ✅ | 🟡 click-to-DM | 🟡 WORKSPACE roster (not per-channel), no search/scroll/count, 256 cap | Both not channel-scoped; Win32 capped. **P1** |
| View other-user profile | ✅ rich profile pane | ❌ (roster overlay unreachable) | ❌ MISSING | Neither can view a peer's profile. **P0** |
| Member action menu (message/role/remove) | ✅ | 🟡 | 🟡 no profile/deactivate | Present, shallow. **P1** |
| Add people to DM / convert to channel | ✅ | ❌ | ❌ | **P2** |
| Close/leave DM from sidebar | ✅ | ❌ | ❌ | **P2** |
| DM peer notify/block | ✅ | ❌ | ❌ | **P2** |
| DM sort/filter | ✅ | 🟡 (grouped) | ❌ | **P2** |
| Presence dot in DM/roster | ✅ | 🟡 | 🟡 | Present. — |
| Slack Connect DMs (external) | ✅ | ⛔ | ⛔ | Out of scope. |

### 2.5 Presence & Status

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Active/Away presence dot | ✅ | 🟡 | 🟡 | Present. — |
| Manually set away/active | ✅ | 🟡 away/online only | 🔸 online/away only | Present but binary. **P1** |
| Custom status (emoji + text) | ✅ | ❌ | ❌ no custom status | **P1** |
| Status expiry / clear-after | ✅ | ❌ | ❌ | **P2** |
| Pause notifications w/ status | ✅ | ❌ | ❌ | **P2** |
| Out-of-Office status | ✅ | ❌ | ❌ | **P2** |
| DND / pause notifications | ✅ presets | 🔸 raw HH:MM text parse | ⚪ raw HH:MM-HH:MM prompt | Both raw-text; no pickers/schedule/display. **P1** |
| DND indicator to others | ✅ | ❌ | ❌ | **P2** |

### 2.6 Notifications

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Global notify-me level | ✅ | 🔸 prefs overlay read-only | ❌ no global settings | **P1** |
| Per-channel/DM overrides | ✅ | 🟡 notify level | 🟡 per-channel level | Present. — |
| Mute channel/DM | ✅ | 🔸 | ❌ | **P1** |
| Keyword/highlight-word notifications | ✅ | ❌ | ❌ | **P2** |
| Thread-reply notifications toggle | ✅ | ❌ | ❌ | **P2** |
| Notification schedule (quiet hours) | ✅ | 🔸 DND raw text | ⚪ DND raw text | See DND. **P1** |
| DND config surface | ✅ | 🔸 | ⚪ | **P1** |
| Activity feed (mentions/reactions/replies) | ✅ | ❌ | ❌ no notification inbox/activity | **P1** |
| Activity filters / saved views | ✅ | ❌ | ❌ | **P2** |
| All-unreads view | ✅ | ❌ | ❌ | **P2** |
| Desktop/OS toast + preview toggle | ✅ | ❌ | ❌ no OS toast | Win32 native toast is high-value. **P1** |
| Notification sounds & badges | ✅ | ❌ | ❌ | **P2** |
| Email notifications | ✅ | ⛔/❌ | ⛔/❌ | Likely server-side; not a client surface. — |
| VIP / priority people | ✅ | ❌ | ❌ | **P2** |
| Review list_notify_prefs screen | ✅ | 🔸 read-only overlay | ❌ | **P1** |

### 2.7 Search

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Search entry | ✅ top bar | 🟡 search prompt | 🔸 modal prompt only (no in-overlay box) | Both prompt-driven. **P1** |
| Results view | ✅ | 🔸 read-only, NON-navigable, shows userNNN not names | 🔸 read-only, no scroll | Both un-navigable; Win32 click jumps to CHANNEL not the matched message. **P0** |
| Search modifiers (from:/in:/has:…) | ✅ | ❌ | ❌ | **P2** |
| Search filters panel | ✅ | ❌ | ❌ | **P2** |
| Result tabs (Messages/Files/Channels/People) | ✅ | ❌ | ❌ | **P2** |
| Sort (relevance/newest) | ✅ | ❌ | ❌ | **P2** |
| Term highlight in results | ✅ | ❌ | ❌ | **P1** |
| Paging / load-more | ✅ | ❌ | ❌ (128 cap, ignores truncation) | **P1** |
| AI/enterprise search | ✅ | ⛔ | ⛔ | Out of scope. |

### 2.8 Files / Attachments

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| File upload | ✅ drag-drop/paste/1GB | 🔸 raw path text, no browser | 🟡 + button + drag-drop, no preview/progress | TUI needs a file picker. **P1** |
| Download file | ✅ | 🔸 auto first attachment, no chooser | 🟡 native Save dialog | TUI has no chooser/save-path. **P1** |
| Inline image previews/thumbnails | ✅ | ❌ (attach lines only) | ❌ no inline render | **P1** |
| Inline video/audio playback | ✅ | ⛔ | ⛔ | Media playback out of scope. |
| Inline PDF/doc preview | ✅ | ❌ | ❌ open-in-place missing | **P2** |
| Code/text snippets | ✅ | ❌ | ❌ | **P2** |
| Files browser / channel Files tab | ✅ | ❌ | ❌ | **P2** |
| File comments/sharing/permissions | ✅ | ❌ | ❌ | **P2** |
| External file services (Drive/Dropbox) | ✅ | ⛔ | ⛔ | Out of scope. |
| Upload progress / preview | ✅ | ❌ | ❌ | **P2** |
| Attachment lines in transcript | ✅ | ✅ incl reclaimed | ✅ attachment lines | Present. — |

### 2.9 Account & Profile

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Login dialog | ✅ | ✅ fields+remember-me+validation+retry | ✅ themed native, prefilled; NO error display on fail, no remember-me surfaced | Win32 login shows no error on bad login. **P0** |
| Signup / first-owner UI | ✅ | ❌ | ❌ | Neither client has signup/first-owner flow. **P1** |
| Profile viewer (self) | ✅ editor | 🟡 READ-ONLY viewer | ⚪ name+password only, chained prompts | Neither is a real editor. **P1** |
| Change display name | ✅ | 🟡 | 🟡 (chained prompt) | Present. — |
| Change password | ✅ | 🟡 2 prompts | 🟡 (no confirm field) | Win32 lacks confirm field. **P1** |
| Avatar upload | ✅ | ❌ | ❌ | **P2** |
| Email edit | ✅ | ❌ | ❌ | **P1** |
| Timezone | ✅ | ❌ | ❌ | **P2** |
| Custom status set | ✅ | ❌ | ❌ | See Presence. **P1** |
| Job title / pronouns / custom fields | ✅ | ❌ | ❌ | **P2** |
| Connecting/reconnect screen | ✅ | ⚪ stub | 🟡 reconnect ok, 🔸 no toast/banner/countdown | **P1** |
| Preferences dialog (hub) | ✅ | ❌ (file-only config) | ❌ no settings screen | No in-app settings editor either client. **P0** |
| Themes / appearance | ✅ | ❌ no in-app toggle | ❌ | **P2** |
| 12/24h & panel toggles in-app | ✅ | ❌ (file-only) | ❌ | **P2** |

### 2.10 Admin

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Invite people | ✅ email/link/roles/CSV | 🔸 hardcoded MEMBER only, token in hard-to-see banner | ⚪ member/admin only, token via MessageBox, no expiry/target/list/revoke | Both very thin; no invite management. **P1** |
| Invite link (shareable) | ✅ | ❌ | ❌ | **P2** |
| Guest accounts (single/multi-channel) | ✅ | ❌ | ❌ | **P2** |
| Role assignment (owner/admin/member) | ✅ | 🟡 role mgmt | 🟡 | Present. — |
| Deactivate / remove user | ✅ | 🔸 remove, NO confirm | ❌ no deactivate in member menu | Win32 missing; TUI unsafe. **P1** |
| Manage members table | ✅ sortable/bulk | 🟡 roster | 🟡 roster pane | No admin members table. **P2** |
| Manage channels (admin) | ✅ | ❌ | ❌ | **P2** |
| Custom emoji administration | ✅ | ❌ | ❌ | **P2** |
| Storage report | ✅ analytics | ✅ RICH read-only modal | 🔸 read-only KV dump, no actions/refresh | TUI richer; Win32 thin. **P2** |
| Audit log | ✅ searchable/filter | 🟡 read-only, NO scroll/paging/filter, ~22-row cap | 🔸 read-only, offset fixed 0, no paging/filter/search/export/scroll | Both un-paged. **P1** |
| Webhooks — list/view | ✅ (app config) | 🔸 read-only, STALE slash-hint text | 🔸 list+delete via MessageBox, no enable/disable/rotate/reveal/date | **P1** |
| Webhooks — create | ✅ | 🟡 | 🟡 one-field, token via MessageBox | Present. — |
| Webhooks — delete | ✅ | ❌ MISSING (core supports it) | 🟡 delete via MessageBox | TUI can't delete despite core support. **P1** |
| Workspace settings (name/URL/icon/defaults) | ✅ | ❌ | ❌ | **P2** |
| Import/Export data | ✅ | ❌ | ❌ | **P2** |
| Analytics dashboard / export | ✅ | 🔸 storage only | 🔸 storage only | **P2** |

### 2.11 Enterprise / Security / Compliance

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| 2FA | ✅ | ❌ | ❌ | Likely server/roadmap. **P2** |
| SSO (SAML / OIDC / Google) | ✅ | ❌ (OIDC noted as remaining in memory) | ❌ | OIDC browser flow is planned. **P1** |
| SCIM / JIT provisioning | ✅ | ⛔ | ⛔ | Out of scope for client. |
| Session mgmt / forced sign-out | ✅ | 🟡 logout | 🟡 logout, no log-out-everywhere | No revoke-all-sessions. **P2** |
| IP allowlist / domain restrict | ✅ | ⛔ | ⛔ | Server-side, out of client scope. |
| Encryption / EKM | ✅ | ⛔ | ⛔ | Out of scope. |
| DLP (native/3rd-party) | ✅ | ⛔ | ⛔ | Out of scope. |
| Access logs | ✅ | ❌ | ❌ | **P2** |
| Audit logs (dashboard/API) | ✅ | 🔸 (see Admin) | 🔸 | **P1** |
| Retention policies | ✅ | ❌ | ❌ | Server-side; no client surface. **P2** |
| Data export / eDiscovery / legal holds | ✅ | ⛔ | ⛔ | Out of scope. |
| Enterprise Grid multi-workspace org | ✅ | ⛔ | ⛔ | Out of scope (federation is a separate model). |
| Information barriers / domain claiming | ✅ | ⛔ | ⛔ | Out of scope. |
| Data residency / HIPAA / FedRAMP | ✅ | ⛔ | ⛔ | Out of scope. |

### 2.12 Integrations / Automation

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Incoming webhooks | ✅ | 🟡 create; 🔸 list; ❌ delete | 🟡 create+delete; 🔸 list | OpenChime's one real integration surface. **P1** |
| Outgoing webhooks | ✅ | ❌ | ❌ | Verify wire support. **P2** |
| App directory / marketplace | ✅ | ⛔ | ⛔ | No third-party app platform — out of scope. |
| Bots / bot users | ✅ | ⛔ | ⛔ | Out of scope. |
| Slash commands (app) | ✅ | ⛔ | ⛔ | Out of scope (and OpenChime is affordance-driven). |
| Web API / Events / Socket Mode | ✅ | ⛔ | ⛔ | Out of scope for client. |
| Workflow Builder + triggers/steps/forms | ✅ | ⛔ | ⛔ | Out of scope. |
| Connectors / custom functions | ✅ | ⛔ | ⛔ | Out of scope. |
| Scheduled messages / reminders | ✅ | ❌ | ❌ | **P2** |
| Email-to-channel / RSS / Salesforce | ✅ | ⛔ | ⛔ | Out of scope. |
| Lists / Canvas automations | ✅ | ⛔ | ⛔ | Out of scope. |

### 2.13 Navigation / UI-Shell

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Header bar | ✅ | 🟡 | 🟡 no member count/header actions | **P2** |
| Sidebar channel/DM list | ✅ | ✅ grouped, unreads, badges, lock/@ markers | 🟡 flat, hides unnamed, 512 cap, no DM/sections/search/starred/muted | Win32 sidebar much weaker. **P0** |
| Custom sidebar sections | ✅ | 🟡 fixed groups | ❌ | **P2** |
| Unreads-only sidebar mode | ✅ | ❌ | ❌ | **P2** |
| Quick switcher / command palette | ✅ Cmd+K | ✅ Ctrl+K ~19 fuzzy actions | ❌ app menu only, NO Ctrl+K/keyboard palette | Win32 lacks a command palette. **P1** |
| Global search bar | ✅ | 🟡 prompt | 🔸 modal | See Search. **P1** |
| Channel browser / add channels | ✅ | ❌ | ❌ | **P1** |
| New message / compose (Cmd+N) | ✅ | 🟡 new-DM/new-channel prompts | 🟡 | Present. — |
| Profile/account menu | ✅ | 🟡 via launcher | 🟡 app menu off avatar | Present. — |
| Preferences hub | ✅ | ❌ | ❌ | **P0** (settings screen) |
| Themes/appearance | ✅ | ❌ | ❌ | **P2** |
| Keyboard shortcuts reference | ✅ | ✅ help overlay RICH | ❌ | Win32 has no shortcut help. **P2** |
| History nav (back/forward) | ✅ | ❌ | ❌ | **P2** |
| Move between unreads (keyboard) | ✅ | 🟡 | ❌ | **P2** |
| Right-hand panel system | ✅ | 🟡 overlays | 🟡 overlays | Overlay-based, mostly read-only. **P1** |
| Star/favorite conversations | ✅ | ❌ | ❌ | **P2** |
| Autocomplete strip (@/#/:emoji) | ✅ | 🟡 | ❌ | Win32 missing. **P1** |
| Keybinding hint bar | ✅ | 🟡 | ❌ | **P2** |
| Error / toast surface (send fail, rate-limit, bad login) | ✅ | 🔸 partial | ❌ NO error/toast surface anywhere | Win32 silently swallows failures. **P0** |
| Slackbot conversation | ✅ | ⛔ | ⛔ | Out of scope. |

### 2.14 Session / Workspace

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Multi-workspace switcher rail | ✅ | ✅ remembered+open, unread, per-ws state | ❌ single workspace avatar only — MISSING | The last big TUI-vs-Win32 delta. **P1** |
| Add workspace | ✅ | 🟡 | ❌ | **P1** |
| Workspace switch by number | ✅ Cmd+1..9 | 🟡 | ❌ | **P2** |
| Reconnect / auto-reconnect | ✅ | ⚪ stub | 🟡 works, no countdown/banner | **P1** |
| Connection status indicator | ✅ | 🟡 status line | 🔸 no toast/banner/countdown | **P1** |
| Logout | ✅ | 🟡 | 🟡 no log-out-everywhere | Present. — |
| Quit | ✅ | 🟡 | 🟡 | Present. — |

### 2.15 Calls / Huddles / Canvas / Lists / Clips (Out-of-scope by design)

| Feature / Surface | Slack | TUI | Win32 | Gap notes & priority |
|---|---|---|---|---|
| Huddles (audio/video/screen) | ✅ | ⛔ | ⛔ | Not on OpenChime's wire — by design. |
| Slack Calls (1:1/group) | ✅ | ⛔ | ⛔ | Out of scope. |
| Third-party call apps | ✅ | ⛔ | ⛔ | Out of scope. |
| Clips (recording + transcription) | ✅ | ⛔ | ⛔ | Out of scope. |
| Canvas documents (+ media/comments/templates) | ✅ | ⛔ | ⛔ | Out of scope. |
| Lists (tables/board/automations) | ✅ | ⛔ | ⛔ | Out of scope. |

---

## 3. Underdeveloped Screens & Dialogs (ranked)

Surfaces that *exist* in TUI and/or Win32 but are thin/stub/read-only. Ranked by user impact.

1. **Search results (both clients)** — *Today:* read-only, non-navigable list; TUI shows raw `userNNN` instead of names; Win32 click jumps to the CHANNEL not the matched message, no highlight, no scroll, 128-result cap ignoring truncation. *Done:* navigable results (arrow/click to the exact message), resolved display names, term highlighting, in-overlay query box + refine, paging/load-more, filter tabs. **P0**

2. **Win32 error/failure surface (missing entirely)** — *Today:* no toast/banner for bad login, send failure, rate-limit, or storage pressure; login dialog shows nothing on auth failure. *Done:* inline login error text + a global toast/banner channel for transient failures with retry affordance. **P0**

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
- **Multi-workspace switcher** — single global client, no rail. **P1**
- **Settings/Preferences screen** — no in-app config at all. **P0**
- **@-mention / #channel / :emoji autocomplete** in composer. **P0**
- **Composer emoji picker** (none; only 6 hardcoded reaction emoji). **P1**
- **Error/toast surface** for any failure (login, send, rate-limit). **P0**
- **Command palette / Ctrl+K** (app menu only). **P1**
- **DM section/list** in sidebar; **sidebar sections/collapsing/search/starred/muted**. **P0**
- **View other-user profile.** **P0**
- **Notification inbox / Activity feed; OS toast; global notify prefs; list_notify_prefs review.** **P1**
- **Channel rename / topic / archive / delete / details pane.** **P1**
- **Jump-to-unread / new-message divider / mark-unread.** **P1**
- **Copy-link / permalink, pin, forward** on messages. **P1**
- **Inline image/thumbnail rendering; open-in-place.** **P1**
- **Signup / first-owner UI; remember-me surfaced; login error display.** **P0**
- **Avatar / rich profile editor; custom status; email/timezone.** **P1**
- **Log-out-everywhere / revoke sessions.** **P2**
- **Keyboard shortcut reference/help overlay.** **P2**
- **Search paging, filters, term highlight, in-overlay input, jump-to-message.** **P0**
- **Draft persistence; scheduled send; drafts hub.** **P2**
- **Reconnect countdown/banner.** **P1**

### TUI — missing
- **Delete webhook** (core supports it; not surfaced). **P1**
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

1. **Global error/toast + login error display.** Failures are currently silent — highest trust/usability risk, small surface. **P0**
2. **Search that works** — navigable results, jump-to-matched-message (not channel), name resolution, in-overlay query + paging + highlight. **P0**
3. **Sidebar overhaul** — DM section + Public/Private grouping (TUI parity), collapsing, search, mute/star, load-more past 512. **P0**
4. **Composer autocomplete + emoji picker** — @mention/#channel/:emoji popover and a real emoji picker (drop the 6 hardcoded). Core chat affordance. **P0**
5. **Settings / Preferences screen** — writing through existing config; time format, notifications, appearance. **P0**
6. **View other-user profile** — clickable avatar/name → profile pane. **P0**
7. **Message actions depth** — copy-link/permalink, pin, forward, mark-unread; jump-to-unread + new-message divider. **P1**
8. **Notifications: OS toast + Activity/notification inbox + editable global prefs; DND with pickers.** **P1**
9. **Channel management** — rename/topic/archive/details pane + a real create-channel dialog (privacy/topic/members). **P1**
10. **Command palette (Ctrl+K)** + inline image/thumbnail rendering (D2D makes this a strong differentiator). **P1**

*Follow-ups just behind the top 10: full webhook CRUD, paged audit log, richer invite dialog, per-reply thread actions, real profile editor, multi-workspace switcher (last remaining TUI-vs-Win32 delta), reconnect banner/countdown, and replacing the six-forms-in-one-prompt pattern with purpose-built dialogs.*