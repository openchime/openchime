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

## WIN-107 — a visual pass over the whole shell (CLOSED)

**First pass done 2026-08-02**, screen by screen against the running client, with
Slack as the reference rather than taste. What it found and fixed:

- **The transcript's timestamp was right-aligned to the pane edge.** Slack puts it
  inline beside the author. On a wide window ours left a hand's width of nothing
  between a name and its time, and made the eye cross the whole transcript to
  answer "when". Now inline.
- **The members pane showed with no conversation open** — in the DMs index it
  listed the roster of the channel you had *left*, which reads as a fact about the
  empty pane. Gated on the same predicate as the composer.
- **Sidebar helper text was cut mid-word** ("…once something i") in Files and
  Later: a non-wrapping format in a fixed box. Wraps now.
- **Activity's filter chips took three rows** of a 248px column after WIN-97 added
  three. Tighter padding fits them in two.
- **The People search box had no chrome** — an unframed EDIT on a white pane:
  reported visible by the harness and invisible to a person.
- **A People row with no title sat high**, its name pinned to the top of a 56px
  row with an empty half beneath, which reads as a missing field. Centres now.
- **Drafts rows had no timestamp**, so the list could not be triaged; the row
  helper already had the field.
- **The Threads card** put its reply count and unread pill at the far right and a
  standing "Turn off replies" label in the corner. Slack has the count under the
  message on the left and the action in a ⋯ overflow; ours does now.
- **The Notifications card promised Cancel and could not keep it.** Every control
  in it applies immediately — the level chips always did, and the keyword,
  priority and schedule editors do too — so Cancel could only compensate for the
  two settings it knew about while silently keeping the other three. One **Done**
  button, as Slack has.
- Earlier in the same pass: the presence/DND mark inset into the avatar with a
  2px ring instead of hanging off its corner (it read as an artifact, not a
  status).

**Second pass, same day — the composer.** It had the action buttons BESIDE the
field, so the text was one line tall whatever the box did and a third of a narrow
window went to chrome. Slack's composer is three bands — formatting toolbar, the
text at full width, then the action row with send at its right — and ours is now
the same, growing downward as you type. The New Message pane had its own
arrangement with Send outside the box; it is the same control, so it is drawn the
same way.

**Third pass.** The day divider was a hairline broken around bare text, which
read as a stray word in the transcript; it is a full-width rule with the date in a
pill sitting on it, as Slack's is. Empty states get a large faint icon above the
title — the native stand-in for Slack's illustrations, and the difference between
"nothing here yet" and "this pane failed to load".

**Closed 2026-08-02.** Three passes, each verified against the running client and
the suite. Anything found later is a new id, not a reopening of this one — the
point of the item was the sweep, and the sweep happened.

## WIN-110 — AutomationIds on every actionable element (REQ-290) (CLOSED)

Done 2026-08-02. The provider existed (ARCH-99) and served Name, ControlType and
the text patterns; what it did not serve was an **identity**, so a client could
read the app and not address it.

- `AutomationId` on every element, composed from identity for anything dynamic:
  `conv.<channel_id>`, `message.<id>`, `thread.card.<root>`, `people.row.<uid>`,
  and fixed names for the rest (`rail.home`, `shelf.threads`, `composer.send`,
  `composer.format.bold`, `drafts.tab.sent`, `activity.filter.unreads`,
  `modal.button.save`).
- **InvokePattern** on everything pressable, which is what makes the tree drivable
  rather than merely readable. The token is posted to the UI thread and ends in
  the same call the mouse path makes.
- The tree's navigation was hard-coded to three children (conversations, messages,
  composer), so nothing new was reachable however carefully it was published. It
  walks a generic root-child set now.
- Two defects the external walk caught that the app could not see: three rail rows
  all answered to `rail.more`, and three composer buttons were findable but not
  pressable.

`scripts/uia_probe.ps1 -Assert` now fails on a missing id, a duplicate id or an
uninvokable control, and `scripts/uia_invoke.ps1` presses one by id — used in the
suite to open a pane with no coordinates involved.

**What this unblocks:** a UIA-driven suite (FlaUI or Appium Windows Driver) that
locates by id, on a runner that is not a developer's desktop. That is the CI-agent
question, still open.


## WIN-111 — chrome polish: the layout does not survive scaling

Found 2026-08-02 by an adversarial pass over the whole shell at
**dpi 96/192/240 × zoom −2…+4 × text size Small…Largest**, driven through the new
`dpi`, `zoom` and `textsize` verbs. At 100% everything looks right; the defects
appear as soon as anything scales, which is why they had not been seen.

**Four root causes account for all of it.** Fixing them by cause rather than by
symptom is the point of this item — the list below is what to check afterwards,
not fourteen separate patches.

**A. Fixed-height rows and boxes that do not grow with the text.** Type scales
(ARCH-97 scales fonts, deliberately not block margins), but these heights are
constants, so the glyphs outgrow their container and are clipped or collide:

1. The composer's text overlaps the action row at large scale — the descenders and
   the caret run into the +/emoji/mention icons. `COMPOSER_ACTIONS` and
   `COMPOSER_BTN` are fixed DIP while `ed_line_h()` scales.
2. Thread cards clip their own content: the author line, the preview and the
   "N replies" row overlap inside a fixed `CARDH` of 104.
3. Draft/scheduled/sent rows clip descenders at a fixed `rowh` of 62.
4. Sidebar shelf rows and channel rows clip at `ROW_H` 32.
5. The Notifications overlay's day rows, keyword rows and section titles clip
   vertically.
6. Tab bars (`Messages / Files & links / Pins / About`, and the Drafts tabs) keep a
   fixed 34px band while their labels grow.

**B. Nothing sets a trimming mode, so truncation cuts mid-glyph with no ellipsis.**
DirectWrite draws until it runs out and stops. Everywhere a label can outgrow its
box:

7. Pane headers ("Drafts, scheduled a…") and their subtitles.
8. Channel/DM tab labels ("Files & li").
9. Draft and thread previews.
10. Sidebar helper text was fixed in WIN-107 by wrapping; the rest of these want
    ellipsis rather than wrapping, since they are single-line by design.

**C. Reserved gutters are fixed DIP while what goes in them scales**, so labels
collide with the thing the gutter was reserved for:

11. The shelf row's badge overruns its label — "Drafts, schedule✎ 2" — because the
    label rect reserves a constant 40.
12. The channel header loses its NAME entirely at large scale, rendering as a bare
    "#": the members chip has grown and the title rect is squeezed to nothing.
13. The thread card's channel label does the same.

**D. Modals are a fixed size and neither scroll nor clip their content.**

14. At large scale the Notifications card's rows spill *outside* the card and draw
    over the shell behind it, and the Done button sits in the middle of the
    content it is supposed to be under. `MODAL_LG` is a constant 720×620 DIP.

**How to verify afterwards.** The same matrix, driven the same way — and the
geometry that should hold at every scale belongs in the dump so the suite can
assert it rather than a person re-reading screenshots: no row's content taller
than its row, no text rect narrower than its ellipsis, no element outside the card
that owns it.

**Explicitly not in this item:** anything about what the controls *do*. This is
cleanliness only, per the review's terms.

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

WIN-1 … WIN-110 are done (WIN-92, WIN-94, WIN-107 … WIN-110 landed 2026-08-02) —
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
