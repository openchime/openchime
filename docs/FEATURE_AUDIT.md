# Feature audit — Win32 client

Every user-reachable function in the Windows client, one line each, with what it
is supposed to do and whether it has been *seen* to do it.

This exists because the shell has been shipped feature by feature against a suite
that grew alongside it, and a suite that grows alongside the code inherits the
code's blind spots. WIN-111 is the case that forced this: the `chromefit`
invariant compared elements only when their kinds matched, so the composer's text
(`COMPOSER`) was never compared against its own action icons (`BUTTON`) — the one
pair the check existed for. It reported a clean layout across the entire scale
matrix while the typed text was drawn straight through the "+" icon, and that
report was passed on as "verified". The check had the defect exempted from it.

## What counts as tested

A feature is **verified** when it has been driven in a running client and the
resulting state read back. Specifically:

1. **Drive it the way a person does** — a click at the control's published rect,
   or the keystroke, not a direct call to the handler underneath it.
2. **Read the result from the model or the dump**, not from the fact that the
   command was accepted. `ack: ok` means the verb was dispatched.
3. **Show the check can fail.** Before trusting an assertion, break the thing it
   watches and confirm it goes red. An assertion never observed failing is a
   comment.
4. **Where the daemon owns the state, restart the client and look again.** A
   feature the client only remembers locally passes every check in the session
   that set it.

Anything less is recorded as `untested`. Not `probably fine`.

## Status key

| | |
|---|---|
| `—` | untested |
| `ok` | verified, with the date and how |
| `BROKEN` | reproduced defect, with the backlog id |
| `n/a` | not reachable in this build (admin-only, feature-flagged) |

---

## A. Shell and navigation

| # | Feature | Reached by | Status |
|---|---|---|---|
| A1 | Home | rail | — |
| A2 | DMs | rail | — |
| A3 | Activity | rail | — |
| A4 | Files | rail | — |
| A5 | Later | rail | — |
| A6 | Admin (admin only) | rail | — |
| A7 | Threads | Home sidebar shelf | — |
| A8 | Drafts & sent | Home sidebar shelf | — |
| A9 | People / directory | Home sidebar shelf | — |
| A10 | Channel list filter ("Find a conversation") | sidebar box | — |
| A11 | Section collapse / sort / filter | section kebab | — |
| A12 | Members pane show/hide | header chip | — |
| A13 | Window geometry restored across runs | relaunch | — |

## B. Composer

| # | Feature | Reached by | Status |
|---|---|---|---|
| B1 | Type and send | composer, Enter | — |
| B2 | Growth to 4 lines, then scroll | typing | — |
| B3 | Field clear of the action row at every scale | — | fixed WIN-111, pending run |
| B4 | Formatting toolbar (bold/italic/strike/code/quote/lists) | toolbar | — |
| B5 | Rich vs plain text mode | Preferences | — |
| B6 | Attach a file (+) | composer | — |
| B7 | Emoji picker | composer | — |
| B8 | Mention (@) and its autocomplete | composer | — |
| B9 | Send later (chevron: 30 min / 1 hour / tomorrow 9:00) | composer | — |
| B10 | Edit / undo / select-all / clipboard in the field | keys | — |
| B11 | Draft retained when you leave and return | navigation | — |

## C. Message actions (`show_msg_menu`)

| # | Feature | cmd | Status |
|---|---|---|---|
| C1 | More reactions | 7 | — |
| C2 | Edit | 21 | — |
| C3 | Delete | 22 | — |
| C4 | Who reacted | 102 | — |
| C5 | Save for later | 104 | — |
| C6 | Copy link | 105 | — |
| C7 | Forward | 106 | — |
| C8 | Unread from here | 107 | — |
| C9 | Reply in thread | message row | — |
| C10 | Open an attachment (per-attachment items, cmd 30+) | 30+ | — |

## D. Channel actions (`show_channel_menu`)

| # | Feature | cmd | Status |
|---|---|---|---|
| D1 | Join channel | 1 | — |
| D2 | Mark as read | 2 | — |
| D3 | Leave channel | 3 | — |
| D4 | Webhooks | 4 | — |
| D5 | Create webhook | 5 | — |
| D6 | Add someone | 6 | — |
| D7 | Remove someone | 7 | — |
| D8 | Per-channel notification level | menu | — |
| D9 | Archive / unarchive | menu | — |

## E. People

| # | Feature | Reached by | Status |
|---|---|---|---|
| E1 | Message (open a DM) | member menu 1 | — |
| E2 | View profile | member menu 2 | — |
| E3 | Remove from workspace (admin) | member menu 13 | — |
| E4 | Directory search | People pane | — |
| E5 | Presence dot / DND state of others | roster | — |

## F. You (`open_profile_menu`)

| # | Feature | cmd | Status |
|---|---|---|---|
| F1 | Set status Online / Away | 10 / 11 | — |
| F2 | Set / change / clear a custom status | 51 / 52 | — |
| F3 | Change display name | 30 | — |
| F4 | Change password | 31 | — |
| F5 | Edit profile | 53 | — |
| F6 | Change / remove photo | 55 / 56 | — |
| F7 | Active sessions | 54 | — |
| F8 | Notification schedule | 50 | — |
| F9 | Pause notifications (30 m / 1 h / 2 h / tomorrow / custom) | 58-64 | — |
| F10 | Resume notifications | 57 | — |

## G. Workspace (`open_ws_menu`)

| # | Feature | cmd | Status |
|---|---|---|---|
| G1 | Invite as member / admin | 40 / 41 | — |
| G2 | Preferences | 70 | — |
| G3 | Notifications (keywords, priority people, schedule) | 71 | — |
| G4 | Mark all as read | 73 | — |
| G5 | Keyboard shortcuts | 72 | — |
| G6 | Storage usage (admin) | 60 | — |
| G7 | Audit log (admin) | 61 | — |
| G8 | Reconnect now | 2 | — |
| G9 | Sign out | 3 | — |
| G10 | Sign out everywhere | 5 | — |

## H. Create (`open_new_menu`)

| # | Feature | cmd | Status |
|---|---|---|---|
| H1 | New channel | 1 | — |
| H2 | New direct message | 6 | — |
| H3 | New group message | 83 | — |
| H4 | New sidebar section | 82 | — |
| H5 | Browse channels | 9 | — |
| H6 | Search messages | 4 | — |
| H7 | Jump to (Ctrl+K) | 8 | — |
| H8 | Upload a file | 7 | — |
| H9 | Add custom emoji | 84 | — |

## I. Workspaces (`open_switcher`)

| # | Feature | cmd | Status |
|---|---|---|---|
| I1 | Add a workspace | 80 | — |
| I2 | Manage workspaces | 81 | — |
| I3 | Switch between workspaces | switcher | — |

## J. Keyboard

| # | Feature | Key | Status |
|---|---|---|---|
| J1 | Command palette | Ctrl+K | — |
| J2 | Search | Ctrl+F | — |
| J3 | Keyboard shortcuts card | Ctrl+/ | — |
| J4 | Preferences | Ctrl+, | — |
| J5 | Zoom in / out / reset | Ctrl+= / - / 0 | — |
| J6 | Previous / next conversation | Alt+↑ / ↓ | — |
| J7 | Previous / next **unread** | Alt+Shift+↑ / ↓ | — |
| J8 | Move focus between panes | F6 | — |
| J9 | Close the top surface | Esc | — |

## K. Connection and identity

| # | Feature | Reached by | Status |
|---|---|---|---|
| K1 | Sign in (password) | login dialog | — |
| K2 | Explicit host and port | login dialog | — |
| K3 | Auto-reconnect after the daemon drops | kill the daemon | — |
| K4 | Offline outbox drains on reconnect | send while down | — |
| K5 | Cached history on a cold start | relaunch offline | — |
| K6 | Credentials remembered across runs | relaunch | — |

## L. Cross-cutting

| # | Feature | Status |
|---|---|---|
| L1 | Chrome fits at every DPI × zoom × text size | fixed WIN-111, pending run |
| L2 | Every control has a stable AutomationId and is invokable | — |
| L3 | Nothing paints outside the window or its modal card | — |
| L4 | Unread counts agree with the server after a restart | — |
| L5 | Preferences survive a restart (they are server-side) | — |

---

## Method

One area per session, top to bottom. Each session:

1. Drive every row in the area against a running client.
2. Record `ok` with the date and the evidence, or `BROKEN` with a reproduction.
3. File each `BROKEN` as a backlog item before fixing anything — the list is the
   deliverable, not a to-do to be quietly worked around.
4. Do not fix while auditing. A fix mid-audit changes the thing being measured,
   and the remaining rows get tested against a build nobody has characterised.

The list above was extracted from the code — `RAIL_ITEMS`, the `mi_item()` calls
in each menu, `accel_run`'s cases — rather than from memory or the backlog, so it
covers what the app actually offers rather than what was meant to be built. It is
regenerable; if a menu gains an item, this table is stale until it is re-extracted.
