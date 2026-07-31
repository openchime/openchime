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
| **WIN-60** | **Unreproduced crash while typing.** Seen three times, never reproduced, no dump at the time. Since 2026-07-29 the client installs an unhandled-exception filter (`crash_filter`) writing `crash-<pid>.txt` with the exception code, faulting address and module base, resolvable via `scripts/crash_resolve.sh`. Nothing has been caught since. Open because unreproduced is not fixed. | P1 | — |

## Open, but not startable here

Each needs its `REQ` built first; the Win32 half is small by comparison — the
pattern REQ-221 and REQ-230 both followed.

| # | Item | Blocked on |
|---|---|---|
| **WIN-89** | **Accessibility / keyboard-only operation.** The client answers no `WM_GETOBJECT` at all (verified: zero occurrences), so a screen reader sees one blank window. A self-drawn UI gets nothing for free, and this gets more expensive the longer the UI grows. | REQ-269 — needs an ARCH decision on the UI-Automation surface |
| **WIN-90** | **Rich text / formatting.** No markup parsing or formatting toolbar. Unblocked on the *rendering* side by the custom composer (WIN-80/ARCH-98) — the field is ours to lay out — but still needs the dialect settled. Also gates snippets (REQ-226). | REQ-220 — needs an ARCH decision on the markup dialect |
| **WIN-91** | **Drafts across a restart.** Drafts are per-channel and in memory (`g_drafts`, 24 slots), so they die with the process. A stateless client (ARCH-88) cannot persist one locally, so this needs server-side storage — the `client_settings` route or a real op. | An ARCH decision on where a draft lives |
| **WIN-92** | **"Pause notifications until…" — the transient DND that does not exist.** The client can set a *recurring daily* window (REQ-131) and nothing else, so the single most-reached-for form — "do not disturb until 17:00", or for 30 minutes — is not expressible: a minutes-of-day pair is periodic, so "until 5pm today" would silence 5pm every day. Needs `dnd_until_ms` (an absolute instant), the presets + custom end time, a resume-now affordance, and the DND state carried to *other* users so a sender sees it before writing (REQ-122 — `oc_user` has no DND field today). The storage pattern is already proven in-tree by custom-status expiry: an absolute stamp the daemon enforces **on read**, so no client needs a clock and nothing has to sweep. | **REQ-278** (new) + REQ-122 |
| **WIN-94** | **Recurring schedule and keyword alerts.** The DND window is typed `HH:MM` with no time picker and no per-weekday schedule (Slack: Every day / Weekdays / Custom, with independent start and end per day). Keyword and priority-people alerts are the same family and cheap now — the match→notify path @mentions built (ARCH-89) — and priority people is also REQ-278's VIP escape hatch. | REQ-135 / REQ-136 |

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
  Until then the smoke is a local gate — and see WIN-87 on how far it can be
  trusted as one.

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

WIN-1 … WIN-84 are closed, and an adversarial review on 2026-07-30 —
116 smoke invariants plus hand-driven probing of the surfaces the smoke does not
reach — found no defect in any of them. Every feature the parity table claims is
reachable was confirmed reachable, several by round-trip against a live daemon
(pin → Pins tab; save → Later; the Activity, Files, Later and Admin views all
render real content).

That is a statement about the *known* list. The gaps found by using the client
are not on any list until somebody writes them down, and WIN-85 and WIN-86 above
are exactly that: both were found by looking at the running client during this
review, not by any assertion. Update [STATUS.md](./STATUS.md)'s parity table
when an item changes a mark.
