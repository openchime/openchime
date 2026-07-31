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

*Nothing startable in `client/gui/win32/` is open.* New work belongs here as new
ids (never reused); the items below are waiting on a requirement or a decision.

## Open, but not startable here

Each needs its `REQ` built first; the Win32 half is small by comparison — the
pattern REQ-221 and REQ-230 both followed.

| # | Item | Blocked on |
|---|---|---|
| **WIN-90** | **Rich text / formatting.** No markup parsing or formatting toolbar yet, but it is now **startable**: the dialect is settled (ARCH-100, [MARKDOWN.md](./MARKDOWN.md)) and the rendering side was unblocked by the custom composer (WIN-80/ARCH-98) — the field is ours to lay out. Work is a parser in `client/core/richtext.[ch]` returning spans, DirectWrite ranges in `body_layout` beside the existing mention highlighting, and a composer that shows formatting as you type. Also gates snippets (REQ-226). | — (dialect settled; this is now ordinary client work) |
| **WIN-91** | **Drafts across a restart.** Drafts are per-channel and in memory (`g_drafts`, 24 slots), so they die with the process. A stateless client (ARCH-88) cannot persist one locally, so this needs server-side storage — the `client_settings` route or a real op. | An ARCH decision on where a draft lives |
| **WIN-92** | **"Pause notifications until…" — the transient DND that does not exist.** The client can set a *recurring daily* window (REQ-131) and nothing else, so the single most-reached-for form — "do not disturb until 17:00", or for 30 minutes — is not expressible: a minutes-of-day pair is periodic, so "until 5pm today" would silence 5pm every day. Needs `dnd_until_ms` (an absolute instant), the presets + custom end time, a resume-now affordance, and the DND state carried to *other* users so a sender sees it before writing (REQ-122 — `oc_user` has no DND field today). The storage pattern is already proven in-tree by custom-status expiry: an absolute stamp the daemon enforces **on read**, so no client needs a clock and nothing has to sweep. | **REQ-278** (new) + REQ-122 |
| **WIN-94** | **Recurring schedule and keyword alerts.** The DND window is typed `HH:MM` with no time picker and no per-weekday schedule (Slack: Every day / Weekdays / Custom, with independent start and end per day). Keyword and priority-people alerts are the same family and cheap now — the match→notify path @mentions built (ARCH-89) — and priority people is also REQ-278's VIP escape hatch. | REQ-135 / REQ-136 |

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

**Everything startable in `client/gui/win32/` is closed** (2026-07-31). WIN-1 …
WIN-89 plus WIN-93 and WIN-95 are done — accessibility included, built *for* the
custom controls rather than by retreating to native ones (ARCH-99). The items
above are each blocked on a daemon requirement or an ARCH decision, or, in
WIN-60's case, on a crash that has not reproduced since it was instrumented. An adversarial review on 2026-07-30 found no defect in the
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
