# OpenChime — Windows GUI Backlog

The execution list for `client/gui/win32/` (ARCH-82). Every item below is a
gap between the shipped Win32 client and the bar set by the TUI, the reference
clients, or a `REQ-NNN`.

**Where this sits.** [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) is the
*analysis* (what is thin, versus Slack and Pumble); [STATUS.md](./STATUS.md)'s
parity table is the *feature reachability* tracker. This document is the *work
list* derived from both — one row per shippable branch.

**Ids.** Items are `WIN-1` … `WIN-58`, numbered once and **stable**: an id is
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
| **WIN-3** | **Search results that navigate (REQ-080).** *Today:* clicking a result switches to the *channel*, not the message; authors render as ids; no term highlight. `oc_search_result` already carries `message_id` + `channel_id`. *Done:* scroll-to-and-flash the matched message, resolved display names, highlighted terms. | P0 | M |
| **WIN-4** | **Search overlay input + truncation notice.** *Today:* a modal one-line prompt, then a read-only list capped at 128 that silently ignores the `truncated` flag. *Done:* an in-overlay query box that refines without reopening, scrolling, and an honest "more results exist" line. (True paging is WIN-38.) | P1 | M |
| **WIN-5** | **Sidebar DM section + Public/Private grouping (REQ-267).** *Today:* one flat "CHANNELS" list. The model already carries `kind` and `is_public`, so this is rendering only. *Done:* collapsible Public / Private / DM sections at TUI parity, with the private lock marker. | P0 | M |
| **WIN-6** | **Sidebar scrolling.** *Today:* the list stops drawing at the pane bottom (`if (y > h) break`) with a 512-row hit-box cap and no scrollbar — channels past the fold are unreachable. *Done:* a scrollable, virtualized list with no fixed cap. | P0 | M |
| **WIN-7** | **Composer autocomplete for `@user` / `#channel` / `:emoji:` (REQ-265).** *Today:* none; the TUI has all three. Roster and channel data are already in the model. *Done:* an inline popover that filters as you type, Tab/Enter to accept. (Real mention *semantics* are WIN-45.) | P0 | M |
| **WIN-8** | **Composer emoji picker (REQ-265).** *Today:* no picker anywhere; reactions are 6 hardcoded emoji in a context submenu. DirectWrite already renders color emoji. *Done:* a searchable full-Unicode picker, reachable from the composer and the reaction menu. | P1 | M |
| **WIN-9** | **Settings / Preferences hub (REQ-261).** *Today:* the workspace menu's "Preferences" opens a *"coming soon"* `MessageBox`. `oc_client_set_setting` exists in the core with **no caller in any frontend**. *Done:* a real preferences dialog writing through the `gui` settings bucket — time format, panel defaults, notification behaviour, appearance. | P0 | M |
| **WIN-10** | **View another user's profile (REQ-266).** *Today:* impossible from anywhere; clicking a member opens a DM. *Done:* a profile pane from any avatar or name, showing what the roster knows (name, role, presence). Richer fields follow REQ-240 (WIN-47). | P0 | S |
| **WIN-11** | **Command palette / Ctrl+K (REQ-260).** *Today:* every action is mouse-driven off the rail; the TUI has ~19 fuzzy actions. *Done:* a keyboard-driven palette over the same action catalog, plus channel/DM quick-switch. | P1 | M |
| **WIN-12** | **Notification-prefs review screen.** *Today:* `oc_client_list_notify_prefs` is never called, so the server-synced prefs have no review surface — this is the open 🔨 in STATUS.md's parity table. *Done:* a prefs overlay listing every channel's level, editable in place. | P1 | S |
| **WIN-13** | **DND time pickers.** *Today:* a raw `HH:MM-HH:MM` text prompt parsed with `sscanf`. *Done:* real time pickers, the current window displayed, and a clear "off" state. | P1 | S |
| **WIN-14** | **New-message divider + jump-to-unread (REQ-236).** *Today:* neither. `oc_channel.read_marker` already exists, and REQ-236 is explicitly client-derived — no server change needed. *Done:* a "New" separator at the read marker and a jump affordance on entering a channel with unread. | P1 | M |
| **WIN-15** | **Per-reply actions inside threads + thread scrollbar.** *Today:* the reply composer works but replies are read-only and the overlay cannot scroll. Replies are ordinary `oc_msg` with ids, so react/edit/delete already apply. *Done:* the full message action menu inside a thread, and a scrollbar. | P1 | M |
| **WIN-16** | **Transcript paging past the 600-message cap.** *Today:* `enum { CAP = 600 }` with no load-more; older history is unreachable in a busy channel. *Done:* scroll-to-top loads the previous page via backfill. | P1 | M |
| **WIN-17** | **Inline image thumbnails (REQ-142).** *Today:* attachments render as text lines only. The download path exists and WIC is already linked. *Done:* inline thumbnails for common image types, click to expand; typed placeholder otherwise. | P1 | M |
| **WIN-18** | **OS toast notifications (REQ-138).** *Today:* no OS-level notification at all. This is per-client rendering of the server's existing notify decision (ARCH-72), not a new server surface. *Done:* native toasts honouring the channel level and DND, with a content-preview toggle. | P1 | M |
| **WIN-19** | **Audit-log paging + filters.** *Today:* `oc_client_audit_query(c, 0)` is called once — "before = now" — with no way to reach older entries, though the frame is timestamp-cursor paged (`before_ms` + `limit`). *Done:* scroll-to-load-older, plus actor/action/family filters. | P1 | S |
| **WIN-20** | **Real profile editor.** *Today:* display name and password are two chained one-line prompts, and the password change has **no confirm field**. *Done:* one dialog with validation and a confirmation field. | P1 | S |
| **WIN-21** | **Purpose-built dialogs, retiring `text_prompt`.** *Today:* six distinct flows (new channel, new DM, search, DND, display name, password) collapse into one generic single-line prompt. *Done:* each flow gets a dialog fitting its actual shape. Overlaps WIN-13/20/30 — do those first, then delete the helper. | P1 | M |
| **WIN-22** | **Invite dialog with a copyable token.** *Today:* the one-time token appears in a `MessageBox` — unselectable, easy to lose. *Done:* a dialog with a copy button and an explicit "shown once" warning. (Expiry/pending/revoke are WIN-46.) | P1 | S |
| **WIN-23** | **Webhooks overlay depth.** *Today:* list + click-to-delete, no scrolling and no metadata columns. *Done:* scrolling, created-date and disabled state, and a delete confirmation. (Reveal/rotate/enable-disable are WIN-48.) | P2 | S |
| **WIN-24** | **Storage overlay refresh + TUI parity.** *Today:* a read-only key/value dump with no refresh; the TUI's version is materially richer and flags pressure in red. *Done:* match the TUI, add refresh. | P2 | S |
| **WIN-25** | **Keyboard-shortcut reference (REQ-264).** *Today:* none; the TUI has a `?` overlay. *Done:* a discoverable shortcut list, generated from one binding table so it cannot drift. | P2 | S |
| **WIN-26** | **Theme / appearance selection (REQ-262).** *Today:* one hardcoded dark palette in `theme.h`; no in-app control. *Done:* light / dark / follow-system, applied live, persisted via WIN-9. | P2 | M |
| **WIN-27** | **Draft persistence (REQ-223).** *Today:* switching channels discards a half-typed message. *Done:* per-channel drafts kept across switches and restarts (client-local; cross-device sync needs REQ-223's ARCH decision). | P2 | M |
| **WIN-28** | **Configurable quick reactions (REQ-073).** *Today:* 6 hardcoded emoji. The `gui` settings bucket can hold a per-user set without any daemon change. *Done:* a user-chosen quick set, offered inline on hover. | P2 | S |
| **WIN-29** | **N-concurrent-workspace model (REQ-012–015).** *Today:* the rail switcher stop/reconnects a **single** `oc_client`, so a background workspace receives nothing and accrues no unread. *Done:* hold N clients, tick all, render one — the TUI's model — with per-row unread and an "N elsewhere" badge. The largest single item here. | P1 | L |

## §2 Ready now, with a small app-core change

The wire already carries these; `client/core` is the only thing in the way.

| # | Item | Core change needed | Pri | Size |
|---|---|---|---|---|
| **WIN-30** | **Create-channel dialog with visibility.** Today the dialog is name-only, but `oc_create_channel` already has an `is_public` field — `oc_client_create_channel(c, name)` simply drops it. | Add the flag to the facade | P1 | S |
| **WIN-31** | **Channel member management.** `INVITE_TO_CHANNEL` / `REMOVE_FROM_CHANNEL` exist on the wire but in no client. | Add both intents | P1 | M |
| **WIN-32** | **Signup / first-owner onboarding (REQ-268).** `REDEEM_INVITE` exists on the wire but in no client, so bringing up a tenant still needs a command line. | Add the redeem intent | P1 | M |
| **WIN-33** | **Mark-all-read / catch-up (REQ-238).** Achievable client-side as a per-channel cursor advance over the existing `CLIENT_ACK`, pending REQ-238's decision on a true bulk op. | Batch helper (optional) | P2 | S |

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
| **WIN-41** | Starred/favourite conversations + custom sidebar sections | REQ-234 — per-user sidebar state storage |
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
| **WIN-58** | Move the **workspace book** (address, username, label, last-used) out of SQLite into the credential store | Nothing blocks it — the token moved in WIN-56, but the non-secret workspace list still lives in `workspace_book`. Keeping the {address, username, token} triple in one store removes the two-store desync on "forget workspace"; Credential Manager enumeration (`CredEnumerateW`) and libsecret attribute search both support it. **P2** |

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
