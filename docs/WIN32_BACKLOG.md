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

## WIN-107 — a visual pass over the whole shell

Raised 2026-08-01, from the running client rather than from a list: features have
been landing faster than the surfaces they live on have been re-read, and the
result is a set of small formatting defects that individually look like nothing
and together look like carelessness. WIN-106 fixed three of them one at a time,
which is the wrong shape — three more appeared in the next screenshot.

So this is one deliberate pass, screen by screen, against the running client:
spacing and alignment in the sidebar rows and their badges, the presence and
do-not-disturb marks (small, dark, and hung off the avatar's corner), transcript
gutters and the day divider, the composer's toolbar and its two rows, empty
states, and every pane the rail can reach. Each fix asserted where the assertion
can outlive it — geometry in the dump, not eyeballed once.

**Not to be interleaved with feature work.** The point is to look at the whole
thing at once, which is exactly what shipping a feature at a time cannot do.

## WIN-110 — AutomationIds on every actionable element (REQ-290)

Raised 2026-08-02, out of the testing question rather than the accessibility one.

The client has a UIA provider already (ARCH-99), so the tree exists and a screen
reader can read it. What it does not have is a **stable id per element**, which is
what an automated test needs to find a control without knowing where it was drawn.
The current harness clicks coordinates read out of a debug dump — workable, and
the reason several failures this session needed a human to decide whether they were
real.

Scope: an `AutomationId` on every element a user can act on — rail and shelf rows,
sidebar conversations, tabs, chips, list rows, composer controls, toolbar buttons,
menu items, modal fields and buttons. Composed from identity for anything dynamic
(`sidebar.row.<channel_id>`), never from position. Ids are defined beside the
control and treated as a contract: renaming one is a breaking change.

This is the prerequisite for driving the client the way Windows applications are
supposed to be driven — UIA from a separate test process (FlaUI or equivalent),
locating by id — rather than by synthetic clicks at measured points. It does not
by itself decide where that suite runs; that is the CI-agent question.

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
  198 checks, waiting on state rather than sleeping, refusing to run against the
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

WIN-1 … WIN-109 are done (WIN-92, WIN-94, WIN-108 and WIN-109 landed 2026-08-02) —
accessibility included, built *for* the custom controls rather than by retreating
to native ones (ARCH-99), and rich text with its toolbar as of 2026-07-31. The
items below are each blocked on a daemon requirement or an ARCH decision, or, in
WIN-60's case, on a crash that has not reproduced since it was instrumented.

**WIN-108 (Threads) and WIN-109 (People)** are the Home sidebar's other two shelf
rows, both landed 2026-08-02. Threads answers REQ-062 with ARCH-104: participation
derived from authorship, `thread_follows` holding only overrides, and a per-thread
read cursor, because the channel's advances when you read the channel and says
nothing about a thread inside it. People answers the new REQ-289 — and repaired a
gap in REQ-240, which was marked DONE: title and timezone rode `PROFILE_INFO`
alone, which is sent only to the person who edited them, so no client ever learned
anyone else's and the profile card had to say the fields were not built.

**Huddles, the remaining shelf row, is deliberately absent.** The daemon has calls
end to end, but the client half does not exist — no Opus, no UDP media path, no
device enumeration — and nothing lists calls in progress at any layer. A row would
point at nothing.

**WIN-92 and WIN-94 came off it on 2026-08-02**, daemon first each time. The
pause (REQ-278) is Slack's `snooze`: an absolute instant enforced on read, named
apart from the schedule because cancelling one has never cancelled the other. The
schedule (REQ-136) REPLACED REQ-131's window and flipped its sense — it states
the hours notifications are *allowed* — so the columns were renamed, their values
swapped, and `SET_DND` retired rather than redefined. Keywords (REQ-135) are
written into `mentions` with their own kind, which is what lets the push query,
the activity feed and the reader's highlight all keep working untouched.

Two things moved to `shared/` in the process, both overdue: the keyword matcher
(so a highlight and a notification can never disagree) and the quiet-hours
predicate — the Win32 client had grown *two* divergent copies of the simpler
version, and its own comment said that if the rule ever grew it belonged there.

The harness gained `formnext`: a modal form blocks the command loop, so every
setting reached through one — keywords, priority people, the schedule, the
pause's custom time — was undrivable and therefore untested end to end.

**WIN-97 came off the blocked list on 2026-08-01**, daemon half first: one query
with three predicates over `delivery_cursors`, and three tabs that re-ask the
server rather than filter what the involved-me feed already returned. Building it
found that **the Activity feed had never shown anything at all** — nothing called
`oc_model_activity_begin`, so every row the server sent was dropped by the fold
that gates on an open list. The filters were the reason to look.

**WIN-106** is a batch of three reported from one screenshot on 2026-08-01: the
Drafts pane lit its shelf row while leaving the conversation lit underneath it
(two rows claiming to be where you are — selection now asks whether the main area
is actually showing that conversation); the empty-state button's label sat on its
bottom edge (a hand-picked top inset, now DirectWrite centring in both axes); and
a text caret left blinking in a pane with no field, because `CreateCaret` adopts
the *thread's* caret, so ours kept the visible state a native EDIT had set — the
paint pass now owns its life and kills a caret nothing placed that frame.

**Three of those ids did not come from this list.** WIN-98 (Ctrl+C in the
composer copied nothing, because the transcript's handler claimed the key and
then did nothing with it) surfaced while wiring WIN-96's Ctrl+Shift+C, by trying
it rather than by reading it. WIN-99 (search left open floated its native EDIT
over the "New direct message" picker) and WIN-100 (double-click selected no
word, anywhere — the window class never asked for `CS_DBLCLKS`, so Windows was
not sending the message at all) both arrived from **somebody using the client**.

WIN-100 is the sharpest of the three, because it is a *regression by omission*:
a native EDIT does word selection for free, so the behaviour left with the
control when WIN-80/ARCH-98 replaced it with a self-drawn field, and nothing
noticed for a fortnight. The harness could not have caught it either — until
this fix it had no verb for a gesture at all, only `click`, which is a down/up
pair at one point. `drag`, `dblclick` and `tripleclick` exist now, and the
suite uses all three.

WIN-101 (the composer was WYSIWYG for the styling and not for the markup, which
reads as a leak rather than a feature) came the same way — from using it — and
was ruled on plainly: either off or perfect, and it should match Slack. It is
now perfect in the sense that matters, which is that markup is never on screen:
not while typing, and not as a consequence of an edit somewhere else.

All are the shape this document's closing note already describes: a backlog
records what somebody wrote down, and none of these was on it. An adversarial review on 2026-07-30 found no defect in the
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
