# OpenChime — Windows GUI Backlog

The live work list for `client/gui/win32/` (ARCH-82). One row per shippable
branch.

**Ids** are `WIN-N`, assigned once and **stable** — never renumbered, never
reused — so a commit or branch can cite one. `WIN-1` … `WIN-84` are all closed;
their detail lives in git history rather than here (this document was an 88-item
archive until 2026-07-30 and is now the open list only, per its own rule that a
backlog is for work that remains).

**Where this sits.** [STATUS.md](./STATUS.md) is the authoritative
per-requirement view of what is built. [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md)
is the *analysis* — why a gap mattered, measured against Slack and Pumble. This
document is only the work list derived from them.

**Pri:** P0 core parity · P1 important · P2 nice-to-have.
**Size:** S ≈ a sitting · M ≈ a day · L ≈ multi-day / architectural.

---

## Open

| # | Item | Pri | Size |
|---|---|---|---|
| **WIN-96** | **Formatting toolbar and its keyboard shortcuts (REQ-220).** The authoring *affordance* for the markup WIN-90 now parses and renders: a toolbar over the composer (bold, italic, strikethrough, code, quote, lists) plus **Ctrl+B / Ctrl+I** and friends wrapping the selection in the delimiters. Startable since 2026-07-31 — it was split out deliberately because WIN-90 is what makes formatting *exist*, and a second way to author it was worth nothing until the first worked. **Constrained by ARCH-100:** anything the toolbar produces must be expressible in text ([MARKDOWN.md](./MARKDOWN.md) §5), or the two authoring paths diverge and a message becomes uneditable in one of them. The parser is the check on that — a toolbar insertion that `oc_rt_scan` does not read back the same way is a bug in the toolbar. | P2 | M |

New work belongs here as new ids (never reused); the items below are waiting on
a requirement or a decision.

## Open, but not startable here

Each needs its `REQ` built first; the Win32 half is small by comparison — the
pattern REQ-221 and REQ-230 both followed.

| # | Item | Blocked on |
|---|---|---|
| **WIN-97** | **Activity filters: Unreads, DMs, Channels (REQ-139).** The feed answers "what involved me" — mentions, reactions, thread replies — and not "what have I not read". Slack's Activity has both, and the three missing filters are the second half. One query with three predicates rather than three features: *messages past my read cursor, in conversations I belong to*, filtered by kind (DMs) or by notification level (Channels), or unfiltered (Unreads). The cursor already exists — `delivery_cursors`, REQ-090. Needs a daemon half (the query and its wire op) before the Win32 tabs. Slack's saved custom views are deliberately not in scope. | P1 | M |
| **WIN-91** | **Drafts across a restart.** Drafts are per-channel and in memory (`g_drafts`, 24 slots), so they die with the process. **Decided (ARCH-101): a server-side `drafts` table keyed `(user_id, channel_id, thread_root)` with its own ops** — not the `client_settings` bucket, which is partitioned per frontend and would leave a GUI draft invisible in the TUI. Client work: save on switch/blur/quit and only when changed, restore on entering a conversation, never overwrite a composer being typed in, and mark the sidebar row — **matching Slack's surface**. Daemon work: the table, `SET_DRAFT`/`LIST_DRAFTS`, fan-out to the user's other connections, and the delete cascades. | — (decision made; needs the daemon half built) |
| **WIN-92** | **"Pause notifications until…" (REQ-278).** The client can set a *recurring daily* window (REQ-131) and nothing else, so the most-reached-for form — "until 17:00", or for 30 minutes — is not expressible: a minutes-of-day pair is periodic, so "until 5pm today" would silence 5pm every day. **Spec settled against Slack's `dnd` API:** presets are *durations from now* (30m / 1h / 2h / until tomorrow / custom), `dnd_until_ms` on `users` with 0 meaning ended, a pause only ever adds silence, and ending a pause is distinct from ending the current scheduled period. Also needs DND carried to **other users** as a second, independent axis beside presence — the fact only, never the end time (REQ-122). VIPs pierce a pause when REQ-135 lands; senders never do (a deliberate divergence from Slack). | REQ-278 daemon half + REQ-122 presence bit |
| **WIN-94** | **Notification schedule and keyword alerts (REQ-135/136).** Two halves of the same subsystem, both specified 2026-07-31. **Schedule:** *Every day / Weekdays / Custom* with an independent start and end per weekday, stored against the user's local calendar day — and it **replaces** REQ-131's single daily window rather than joining it, because Slack has one recurring mechanism and two would be able to disagree. **Keywords:** part of the *mentions* level rather than their own switch, matched case-insensitively and exactly, phrases allowed, surfacing in the activity feed as mentions — and firing **in threads**, where Slack's do not. **Priority people** pierce a level and a pause but never a mute. | REQ-135 / REQ-136 daemon halves |

## Closed without a fix, and why

- **WIN-60 — the unreproduced crash while typing.** Closed 2026-07-31. Three
  crashes were seen on 2026-07-29, all while typing, none reproduced. The
  crash filter (`crash_filter`, breadcrumbs + minidump) went in at **21:49 that
  evening** — *after* the last occurrence — and **the composer was rewritten the
  next morning** (WIN-80/ARCH-98, 09:54 on 2026-07-30), replacing the native
  RichEdit child with the self-drawn editor. So every occurrence happened in code
  that no longer exists, and the instrumentation has never once seen the
  replacement fail.
  
  Evidence for closing rather than merely parking it: the crash filter is
  **verified working** — a deliberate fault (`crashtest`) produces the report and
  a 7.7 MB minidump — so zero crash files is a real result and not a silent
  handler. Since the rewrite the composer has taken thousands of harness-driven
  keystrokes without a fault: a targeted stress run (40 rounds of typing,
  non-ASCII where the UTF-16↔UTF-8 offset mapping runs, the ED_MAX boundary,
  undo/redo churn over the 16-deep snapshot stack, completion on all three
  triggers, draft save/restore across switches, resize mid-layout) plus repeated
  full smoke runs.
  
  A read of the current composer for the usual suspects also found it sound:
  `ed_insert_n` caps rather than overflows, the undo snapshots are bounded, and
  both IME composition branches are length-guarded against `g_ed_comp`.
  
  **If it recurs it is new information and a new id** — the report will now carry
  the faulting address, the access kind and the last breadcrumbs, which is
  exactly what the original three lacked.

## Environment, not defects

- **WIN-18's tray balloon rendering is unobserved.** The API path is verified
  positively (`tray_live=1` — the shell accepted the icon; `toasts_raised`
  increments on the call) and the gating is verified mechanically. Only the
  *rendering* is unseen, because this dev host has no attached display surface:
  a full-screen capture throws and returns blank, which is why a control test
  with .NET's own `NotifyIcon` was equally invisible. Seeing it needs a Windows
  desktop with a display.
- **The smoke does not run in CI** (WIN-81, closed as far as code can take it).
  The daemon is epoll-based and Linux-only, and GitHub's Windows runners cannot
  host it. The job exists and skips until a self-hosted Windows+WSL runner does.
  Until then the smoke is a local gate. It is a trustworthy one since WIN-87/88:
  130 checks, waiting on state rather than sleeping, refusing to run against the
  wrong daemon, and including a real UIA client walking the accessibility tree
  from outside the process.

## Not planned for this client

- **Audio and screenshare** (REQ-150–152, REQ-161) — a separate epic sequenced
  behind the audio client ([AUDIO.md](./AUDIO.md), [VIDEO.md](./VIDEO.md)), not
  Win32 depth work.
- **Push device registration** (REQ-132) — `REGISTER_DEVICE_TOKEN` reaches no
  client, but push targets mobile; a desktop client uses WIN-18 instead.
- **Slash commands** — excluded by design; the GUI is affordance-driven (ARCH-82).
- **GIF/sticker pickers, Canvas, Lists, Clips, Slack Connect, first-party
  bot/MCP** — REQ-270–275, explicit product exclusions.

---

## What "closed" does and does not mean

WIN-1 … WIN-90 plus WIN-93 and WIN-95 are done — accessibility included, built
*for* the custom controls rather than by retreating to native ones (ARCH-99),
and rich text as of 2026-07-31. **WIN-90 closing is what opened WIN-96**, which
is the one startable item above; the rest are each blocked on a daemon
requirement or an ARCH decision, or, in WIN-60's case, on a crash that has not
reproduced since it was instrumented. An adversarial review on 2026-07-30 found no defect in the
previously-closed list, several claims verified by round-trip against a live
daemon (pin → Pins tab; save → Later; Activity, Files, Later and Admin all
rendering real content).

**Two things worth remembering from how the recent items were found.** Neither
WIN-85 nor WIN-86 came from an assertion — both were spotted by *looking* at the
running client. And the two most serious defects of the batch were not on any
list at all: a composer that reached negative width and disappeared, and a
transfer-slot desync that made every upload fail, both surfaced only once the
smoke was made trustworthy enough to believe. A backlog records what somebody
wrote down; it is not a survey of what is wrong.

Update [STATUS.md](./STATUS.md)'s parity table when an item changes a mark.
