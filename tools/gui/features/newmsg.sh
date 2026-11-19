#!/usr/bin/env bash
# The New message pane (REQ-229), driven: does the field that LOOKS focused get
# the keys, does the message go where the pane says, and does what you typed
# survive leaving the pane?
#
#   tools/gui/features/newmsg.sh          # run; the client and daemon stop at the end
#   tools/gui/features/newmsg.sh --keep   # ... and leave them running
#
# It exists because 35 defects were found in this pane by reading it, and the two
# that mattered most were invisible to every other harness: characters typed into
# the To: field were applied to the message body behind it, and Enter in the
# message sent it to the channel that happened to be open before. Both look
# perfectly normal on screen. The dump's `newmsg` line (focus= chips= q= caret=
# sel= matches= body= pending=) is what makes them assertable.
#
# NOT IN CI, for the reason tools/gui/features/smoke.sh gives: the daemon is Linux-only and
# GitHub's Windows runners cannot host it. Run it before pushing a change to the
# pane, the picker or the composer's key routing.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin newmsg 9520 /tmp/oc-newmsg "Newmsg Fixture" "$@"

drive() { "$DRIVE" "$@" >/dev/null 2>&1; }
# A press: down AND up, as a keyboard does. The `key` verb sends the down only,
# deliberately, so a held key can be tested -- and a handler that re-arms on the
# release then ignores every press after the first. Backspace in the To: field is
# one: it takes one recipient per press and re-arms on WM_KEYUP, so a run that
# never released it deleted one chip and silently no-opped every Backspace after.
# Anything pressed more than once in a run goes through this.
tap() { drive key "$1"; drive keyup "$1"; }
# The pane's line (the first `newmsg ` line; a second, `newmsg to=`, holds its
# rects): nm <key>, and nmq <key> for a quoted value, which may hold spaces.
nm()  { field "$D" '^newmsg focus=' "$1"; }
nmq() { fieldq "$D" '^newmsg focus=' "$1"; }
# The first key=value of that name anywhere in the dump.
kv()  { field "$D" "\\b$1=" "$1"; }
# How many recipients the pane holds. The dump joins them with commas, so an
# empty list is 0 and "@bob,@carol" is 2.
nchips() { local c; c="$(nmq chips)"; [ -z "$c" ] && { echo 0; return; }; tr ',' '\n' <<<"$c" | grep -c .; }

gui_launch "$("$DRIVE" devaddr)" "alice:pw"

# --- 1. the keys go where the caret is -----------------------------------------
step "1. the field that looks focused gets the keys"
drive view newmsg || gui_abort "the client did not open New message"
gui_snap
check "opens with the To: field focused (focus=$(nm focus))" [ "$(nm focus)" = "to" ]
drive typekeys "bob"
gui_snap
check "typing lands in the To: query (q='$(nmq q)')" [ "$(nmq q)" = "bob" ]
check "and NOT in the message body ($(nm body) characters there)" [ "$(nm body)" = "0" ]
drive key enter
gui_snap
check "Enter accepts the highlighted match (chips '$(nmq chips)')" [ "$(nmq chips)" = "@bob" ]
check "and clears the query (q='$(nmq q)')" [ "$(nmq q)" = "" ]

# The message box, reached with Tab, takes the characters — and the To: field
# stops taking them. This is the defect a person found by typing.
drive key tab
gui_snap
check "Tab moves to the message (focus=$(nm focus))" [ "$(nm focus)" = "body" ]
drive typekeys "hello there"
gui_snap
check "typing lands in the message ($(nm body) characters, of 11)" [ "$(nm body)" = "11" ]
check "and not in the To: query (q='$(nmq q)')" [ "$(nmq q)" = "" ]
drive key shift+tab
gui_snap
check "Shift+Tab goes back to the recipients, no keyboard trap (focus=$(nm focus))" [ "$(nm focus)" = "to" ]

# The keys that are NOT characters are the ones that used to leak: with the To:
# field focused, Delete deleted from the message, Ctrl+A selected it and Ctrl+V
# pasted into it — none of it visible, because the message was not on screen.
before_body="$(nm body)"
win_clipboard_set_text PASTED
drive key delete
drive key ctrl+v
gui_snap
check "Delete, Ctrl+V and Ctrl+A do not reach the message ($before_body, then $(nm body) characters)" \
  [ "$(nm body)" = "$before_body" ]
check "Ctrl+V pastes into the To: field (q='$(nmq q)')" [ "$(nmq q)" = "PASTED" ]
drive key ctrl+a
drive key delete
gui_snap
check "Ctrl+A then Delete clears the To: field (q='$(nmq q)')" [ "$(nmq q)" = "" ]
drive key tab

# The list FLOATS over the composer, so the composer must not take its clicks:
# it did, and clicking a match put the caret in the message box instead of adding
# the recipient.
step "1b. a match is chosen by clicking it"
drive key shift+tab
drive key ctrl+a
drive key delete
tap backspace              # drop the chip from case 1, so the list has room
drive typekeys "car"
# The rects come from the PAINT, so wait for the list to be drawn rather than
# reading the frame before it.
row0() { grep -m1 '^newmsgrow0' "$D" | cut -d' ' -f2; }
row_drawn() { gui_snap; local r; r="$(row0)"; [ -n "$r" ] && [ "$r" != "0,0,0,0" ]; }
waitfor 10 row_drawn
row="$(row0)"
IFS=, read -r rl rt rr rb <<<"${row:-0,0,0,0}"
rx=$(( (${rl%.*} + ${rr%.*}) / 2 )); ry=$(( (${rt%.*} + ${rb%.*}) / 2 ))
before_chips="$(nmq chips)"
row_there() { [ "$rx" -gt 0 ] && [ "$ry" -gt 0 ]; }
check "the list is on screen with a row to click (rect '$row')" row_there
drive click "$rx $ry"
gui_snap
check "clicking a match adds it (chips '$(nmq chips)', before '$before_chips')" [ "$(nmq chips)" != "$before_chips" ]
check "and the keys stay in the To: field, not the composer (focus=$(nm focus))" [ "$(nm focus)" = "to" ]

# --- 2. the To: field is a text field ------------------------------------------
step "2. the To: field edits like a text field"
drive key shift+tab
drive typekeys "carol"
drive key left
drive key left
gui_snap
check "Left moves the caret inside the query (caret $(nm caret) after two Lefts in 'carol')" [ "$(nm caret)" = "3" ]
# "carol" with the caret between r and o: Backspace takes the r, not the l.
tap backspace
gui_snap
check "Backspace deletes at the caret, not at the end (q='$(nmq q)')" [ "$(nmq q)" = "caol" ]
drive key esc
gui_snap
check "Escape clears the query (q='$(nmq q)')" [ "$(nmq q)" = "" ]

# --- 3. the message goes where the pane says -----------------------------------
step "3. Enter sends to the recipients, not to the last channel"
# A message in #general first, so g_sel names a channel a wrong send would reach.
gen() { grep -m1 'ch [0-9]* "general"' "$D"; }
dm()  { grep -m1 'ch [0-9]* DM' "$D"; }
drive view home
drive channel general || gui_abort "the client did not open general"
drive type "in the channel"
drive key enter
in_general() { gui_snap; local g; g="$(gen)"; [[ "$g" == *'prev="in the channel"'* ]]; }
waitfor 10 in_general || gui_abort "#general's own message did not come back from the daemon"
drive view newmsg
drive typekeys "bob"
drive key enter
drive key tab
drive typekeys "for bob only"
drive key enter
# Until the DM holds it, or ten seconds: then #general is read.
to_bob() { gui_snap; local m; m="$(dm)"; [[ "$m" == *'for bob only'* ]]; }
waitfor 10 to_bob
gmsgs="$(gen | grep -o 'msgs=[0-9]*' | cut -d= -f2)"
check "#general still holds only its own message ($gmsgs)" [ "$gmsgs" = "1" ]
not_in_general() { local g; g="$(gen)"; [[ "$g" != *'for bob only'* ]]; }
check "and its newest message is not the addressed one" not_in_general
check "the DM received it (dm row: $(dm))" to_bob

# --- 4. leaving and coming back keeps both halves ------------------------------
step "4. the draft keeps the words AND the people"
drive view newmsg
drive key ctrl+a
drive key delete
drive typekeys "bob"
drive key enter
drive key tab
drive typekeys "half written"
# The debounce writes it: the dump's cpreview draft= is 0 once nothing is
# left to write.
written() { gui_snap; [ "$(field "$D" '^cpreview ' draft)" = 0 ]; }
waitfor 10 written || gui_abort "the pane's draft was never written"
drive view home
drive view newmsg
gui_snap
check "the words come back ($(nm body) characters, of 12)" [ "$(nm body)" = "12" ]
check "the people come back ('$(nmq chips)')" [ "$(nmq chips)" = "@bob" ]

# --- 5. nothing is written into another conversation's draft -------------------
# `drafthere` is whether the SELECTED conversation has a draft. The pane's text
# used to be filed under whatever was open before, on every way out of it.
step "5. the pane's text is nobody else's draft"
drive view home
drive channel general
gui_snap
check "#general has no draft of the new message" [ "$(kv drafthere)" = "0" ]
check "exactly one draft exists: the pane's own ($(kv draftn))" [ "$(kv draftn)" = "1" ]

# --- 6. Backspace takes one recipient per press ---------------------------------
# The rule the client enforces on the key's release, which is why a press in this
# script is a press and not a hold. Two recipients, so taking too many shows.
step "6. Backspace takes one recipient per press"
drive view newmsg
drive typekeys "carol"
drive key enter              # the top match, beside the restored @bob
gui_snap
check "two recipients: '$(nmq chips)'" [ "$(nchips)" = "2" ]
tap backspace
gui_snap
check "one press takes one: '$(nmq chips)'" [ "$(nchips)" = "1" ]
# Two again, and now the key is HELD -- pressed twice without a release. The
# second recipient must survive, which is what the latch is for: a held key used
# to walk backwards through the whole list and take the people behind the one
# aimed at.
drive typekeys "carol"
drive key enter
gui_snap
check "two again, to hold the key against: '$(nmq chips)'" [ "$(nchips)" = "2" ]
drive key backspace
drive key backspace
gui_snap
check "a held Backspace takes only the one it started on: '$(nmq chips)'" [ "$(nchips)" = "1" ]
drive keyup backspace
tap backspace              # released and re-armed: the last one goes

# --- 7. sending needs a recipient, and says so ---------------------------------
step "7. a refusal says why"
# The recipients are gone BEFORE Enter is pressed, asserted here: this check used
# to run with the restored recipient still attached, so the message was sent and
# whether the check passed depended on which dump line was read first.
gui_snap
check "nobody is addressed ('$(nmq chips)'), so the refusal can be tested" [ "$(nchips)" = "0" ]
drive key tab
drive typekeys "nobody is addressed"
gui_snap
before_body="$(nm body)"
drive key enter
gui_snap
check "the message is still in the box ($before_body, then $(nm body) characters)" [ "$(nm body)" = "$before_body" ]
check "and the composer says why" grep -q 'Who is this for?' "$D"
gui_end
