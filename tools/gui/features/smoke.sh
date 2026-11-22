#!/usr/bin/env bash
# Does the Win32 client BOOT and RUN? That is the whole question.
#
#   tools/gui/features/smoke.sh            # run; the client and daemon stop at the end
#   tools/gui/features/smoke.sh --keep     # ... and leave them running
#
# OC_TEST_KEEP_DB=1 keeps the fixture's database from the last run;
# OC_TEST_WAIT_MS (default 6000) is how long a wait tolerates.
#
# THIS IS A SMOKE TEST, RUN BY HAND BEFORE PUSHING A CLIENT CHANGE (the
# pre-push hook does not run it). It asserts that the
# client starts, reaches a daemon, paints its shell, takes the keyboard, sends a
# message and answers a shortcut — and then it stops. It does NOT cover the
# composer's editing rules, the modal frame, the schedule card, the scaling
# matrix, drafts, threads, emoji, avatars or accessibility, and it is not
# supposed to.
#
# The narrowness is the point, not an apology for it. This runs before a client
# change is pushed, so it has to be cheap enough that nobody is tempted to skip it, and
# narrow enough that a failure means "the client is broken" rather than "one of
# two hundred assertions moved". When it fails, stop and look.
#
# KEEP IT THIS SIZE. A suite that takes minutes gets skipped — a feature is verified by driving it, by hand or by a harness of
# its own, not by making the boot check longer.
#
# WHAT IT WILL AND WILL NOT CATCH. It catches a client that will not start, will
# not connect, will not authenticate, paints no shell, cannot be typed into,
# cannot send, or has lost its message-loop shortcuts — the failures that make
# every other test meaningless. It catches nothing subtler, deliberately.
#
# IT IS IN NO SUITE AND NOT IN CI: no GUI test is (docs/details/TESTING.md). Its Win32
# run could not be anyway: the daemon is epoll-based, so Linux-only, and
# GitHub's Windows runners cannot host it. Run it on both platforms with
# tools/gui/run.sh smoke.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"

# --- isolation -----------------------------------------------------
# Its OWN daemon on its OWN port, wiped each run (gui_begin; OC_TEST_KEEP_DB=1
# keeps the database), and it says so rather than adopting whatever is
# listening. gui_drive's staleness check keeps a daemon whose start time
# postdates the binary, which silently adopts an UNRELATED daemon if one holds
# the port — a run once bound to a nine-hour-old daemon holding real workspace
# data and reported confident nonsense. A test that can be wrong about which
# machine it is testing is not a test.
gui_begin smoke 9500 /tmp/oc-smoke "Smoke Fixture" "$@"

# How long a wait tolerates before calling it a failure, in whole seconds for
# waitfor. Only failing runs pay it.
WAIT_MS="${OC_TEST_WAIT_MS:-6000}"
WAIT_S=$(( (WAIT_MS + 999) / 1000 ))

# --- driving -------------------------------------------------------
# EVERY VERB'S EXIT STATUS IS CHECKED, which the regression suite does not do and
# is the single biggest reason a long run there produces failures that are not
# defects: gui_drive exits non-zero when the client never acked, and a discarded
# status turns a dropped command into the NEXT assertion failing for a reason
# that has nothing to do with it. A verb that does not ack is a broken client,
# and this suite stops there and says so: every verb goes through lib.sh's
# act.

# `ack` means the handler ran, not that its effect is painted — most of the
# interesting fields are recorded during WM_PAINT — so nothing here reads a
# dump once and hopes; it waits for the state it is about to assert. Each key
# is read from the one line that owns it: `focus=` and `re=` are on other lines
# too, and the first in the dump is not the composer's.
#   natives re= pal= sbkind= conv= covered=     ed len= focus= text=
#   wnd_active= wnd_focus=
is() {                             # is <line-ERE> <key> <want>, on a fresh dump
  gui_snap && [ "$(field "$D" "$1" "$2")" = "$3" ]
}
has() { gui_snap && grep -E "$1" "$D" >/dev/null; }   # has <regex>, on a fresh dump

expect_eventually() {              # expect_eventually <line-ERE> <key> <want> <label>
  local got
  checks=$((checks + 1))
  if waitfor "$WAIT_S" is "$1" "$2" "$3"; then ok "$4 ($2=$3)"; return 0; fi
  got="$(field "$D" "$1" "$2")"
  fail "$4: expected $2=$3, got ${got:-<missing>} after ${WAIT_MS}ms"
  return 1
}

expect_grep() {                    # expect_grep <regex> <label>
  checks=$((checks + 1))
  if waitfor "$WAIT_S" has "$1"; then ok "$2"; return 0; fi
  fail "$2: no dump matched /$1/ within ${WAIT_MS}ms"
  return 1
}

# --- 1. it starts, and it reaches a daemon ---------------------------------
step "it starts"
gui_launch
connected() { is '^authed=' authed 1 && [ "$(field "$D" '^authed=' connected)" = 1 ]; }
check "the client launches, connects and authenticates" waitfor 20 connected
connected || gui_abort "NOT CONNECTED: the client signed in and is not connected to :$OC_TEST_PORT"

# Assert the fixture before asserting anything about the product: reaching the
# wrong daemon does not look like a broken harness, it looks like broken features.
fixture="$(fieldq "$D" '^workspace ' name)"
check "and it is the smoke fixture on :$OC_TEST_PORT, not somebody's workspace (\"${fixture:-<none>}\")" \
  [ "$fixture" = "$OC_TEST_WS" ]
[ "$fixture" = "$OC_TEST_WS" ] ||
  gui_abort "WRONG DAEMON: reached \"${fixture:-<none>}\", expected \"$OC_TEST_WS\": every assertion below would be about the wrong data"

# --- 2. it paints a shell you can use --------------------------------------
# The three predicates in app.c, in the one view that has to work: a channel
# list on the left, a conversation in the middle, nothing covering the window.
# The full seven-view matrix is the regression suite's; this asks only whether
# the shell came up at all.
step "it paints"
act view home
act channel general
expect_eventually '^natives ' sbkind  1 "the channel list is the second column"
expect_eventually '^natives ' conv    1 "the middle column is a conversation"
expect_eventually '^natives ' covered 0 "nothing is covering the window"
expect_eventually '^natives ' re      1 "the composer has a box to draw into"

# --- 3. it takes the keyboard ----------------------------------------------
# A window shown by a process that does not own the foreground is not activated,
# and the composer's auto-focus is gated on having focus — so a client that fails
# this looks fine in a screenshot and ignores every key. Both halves are checked
# because they are different facts: the WINDOW can hold the keyboard while the
# composer has not claimed it.
step "it takes the keyboard"
keyboard() { is '^wnd_' wnd_focus 1 && [ "$(field "$D" '^ed ' focus)" = 1 ]; }
checks=$((checks + 1))
if waitfor 8 keyboard; then ok "the window is focused and the composer holds the caret"
else fail "the composer never took the keyboard: $(grep -E '^wnd_|^ed ' "$D" | tr '\n' ' ')"; fi

# --- 4. it sends a message -------------------------------------------------
# The end-to-end path in one assertion: real WM_CHARs into the self-drawn field,
# Enter, and the message coming back from the daemon into the transcript. If this
# works, the wire, the composer, the send path and the fan-out are all alive.
#
# The text is UNIQUE PER RUN, so the assertions below cannot be satisfied by a
# message an earlier run left behind. The default wipe would give that too, but
# only as a side effect — and OC_TEST_KEEP_DB=1 removes it, at which point a
# fixed string would make this check pass without anything being sent. A test
# that can pass for the wrong reason is worse than one that is absent.
step "it sends"
MSG="smoke $$ $(date +%s)"
act key ctrl+a
act key backspace
act chars "$MSG"
expect_grep "^ed .*text=\"$MSG\"" "typing reaches the composer"
act key enter
expect_eventually '^ed ' len 0 "Enter sends and clears the field"
# Two separate facts, because they fail separately: the daemon accepted it and
# said so (the channel's preview is server state), and the transcript actually
# painted a row for it. Do not fold them into one alternation: the `msgrow` line
# carries geometry and no body, so a pattern that would accept either really only
# ever tests the preview, while reading as though it tested both.
expect_grep "prev=\"$MSG\"" "the daemon stored it and sent it back"
expect_grep '^msgrows n=[1-9]' "and the transcript painted a row for it"

# --- 5. its shortcuts still work -------------------------------------------
# Asserted as a round trip, not a state: "Esc closed the palette" checked against
# pal=0 passes when the palette never opened. And asserted with the caret in the
# composer, which is the case that used to be dead — the chords are dispatched
# from the message loop precisely so a focused field cannot eat them.
step "its shortcuts work"
act key ctrl+k
if expect_eventually '^natives ' pal 1 "Ctrl+K opens the command palette from the composer"; then
  act key esc
  expect_eventually '^natives ' pal 0 "and Esc closes it"
else
  act key esc
fi

# --- 6. it is still alive --------------------------------------------------
# Everything above could pass against a client that crashed on the last keystroke,
# because a stale dump file reads exactly like a fresh one. So the last thing this
# does is make the client answer.
step "it survives"
check "the client is still answering at the end of the run" gui_snap
say "(a boot check, not a feature suite: see the header)"
gui_end
