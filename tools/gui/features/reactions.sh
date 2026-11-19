#!/usr/bin/env bash
# Reacting from the who-reacted pane, in the Win32 client or the browser's
# (OC_TEST_PLATFORM=web) (REQ-070/071).
#
#   tools/gui/features/reactions.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/reactions.sh --keep   # ... and leave them running
#
# The pane lists the people who reacted. It now carries the message's chips too,
# one per distinct emoji, so a reaction can be added or taken back from the pane
# rather than only from the message. It asserts that
#
#   - the pane shows a chip per emoji, with its count, and marks the ones that
#     are yours;
#   - pressing a chip that is not yours adds your reaction: the count rises, the
#     chip is marked yours, and the message itself agrees;
#   - pressing it again takes it back;
#   - each chip is published to assistive technology, saying which it would do;
#   - the message under the pointer carries a strip of the quick reactions, one
#     press each, which appears only while the pointer is on it.
#
# Two clients over tools/gui/harness/win32/pair.sh: bob reacts first, so alice's pane has somebody
# else's reaction to join.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin reactions 9580 /tmp/oc-reactions "$@"
CH=1

# Field `k` of the pane's first chip, or of its header line, in a fresh look
# at alice's client.
chip()  { gui_snap a; field "$DA" '^  rxnchip 0 ' "$1"; }
pane()  { gui_snap a; field "$DA" '^reactors ' "$1"; }
# Click the centre of the pane's first chip.
click_chip() { gui_snap a; click_rect a "$(field "$DA" '^  rxnchip 0 ' r)"; }

step "fixture: alice and bob, one message, bob reacts"
gui_pair_up
# Wide enough for the context pane the who-reacted list lives in.
"$PAIR" a size 1200 820 >/dev/null || gui_abort "alice's window did not take its size"
# At 96 dpi, so the window is that wide in layout too: a scaled Windows display
# (144 dpi makes it 800) leaves no room for the pane, which then draws nothing.
if [ "$PLAT" = win32 ]; then "$PAIR" a dpi 96 >/dev/null || gui_abort "alice's client did not take 96 dpi"; fi
"$PAIR" a channel $CH >/dev/null || gui_abort "alice did not open #general"
"$PAIR" b channel $CH >/dev/null || gui_abort "bob did not open #general"
"$PAIR" a send "react to this" >/dev/null || gui_abort "alice's client did not take the message"
# bob reacts to his newest message: it must be this one.
waitfor 10 newest_is b $CH "react to this" || gui_abort "alice's message did not reach bob"
THUMB="$(printf '\xF0\x9F\x91\x8D')"
"$PAIR" b react "0 $THUMB" >/dev/null || gui_abort "bob's client did not take the reaction"  # thumbs up on the newest
got_one() { [ "$(pane people)" = 1 ]; }

step "the pane carries the message's chips"
"$PAIR" a reactors 0 >/dev/null
open_now() { [ "$(pane open)" = 1 ]; }
check "the who-reacted pane is open" waitfor 10 open_now
check "...listing the one person who reacted" waitfor 10 got_one
one_chip() { [ "$(pane chips)" = 1 ] && [ "$(chip count)" = 1 ] && [ "$(chip mine)" = 0 ]; }
check "one chip, count 1, not yours" waitfor 10 one_chip
check "the chip is published to assistive technology" grep -q '^a11yitem reactors.chip.0 ' "$DA"
"$PAIR" a shot reactions_pane >/dev/null

step "pressing it adds your reaction"
click_chip
mine_now() { [ "$(chip mine)" = 1 ] && [ "$(chip count)" = 2 ]; }
check "the chip is yours, and counts two" waitfor 10 mine_now
# The chip is drawn from the message's own reactions; the people are the
# daemon's list, asked again on each reaction to the message.
both_listed() { [ "$(pane people)" = 2 ]; }
check "...and the list of who reacted has both" waitfor 10 both_listed

step "pressing it again takes it back"
# It must have been yours to give back, or this check would pass on a chip that
# was never pressed at all.
was_mine="$(chip mine)"
click_chip
back() { [ "$was_mine" = 1 ] && [ "$(chip mine)" = 0 ] && [ "$(chip count)" = 1 ]; }
check "the chip was yours, and now is not, and counts one" waitfor 10 back

step "the hovered message carries the quick reactions"
hrx()  { gui_snap a; field "$DA" '^hoverreact ' "$1"; }
# The first cell's rect and whether it is already yours, from the dump.
cell0_rect() { gui_snap a; grep -m1 '^hoverreact ' "$DA" | tr ' ' '\n' | sed -n 4p | cut -d@ -f2; }
cell0_mine() { gui_snap a; grep -m1 '^hoverreact ' "$DA" | tr ' ' '\n' | sed -n 4p | cut -d: -f2 | cut -d@ -f1; }
click_cell0() { click_rect a "$(cell0_rect)"; }
"$PAIR" a send "hover over me" >/dev/null || gui_abort "alice's client did not take the second message"
# Back from the daemon, and drawn: a row for it.
hover_drawn() { newest_is a $CH "hover over me" && grep -q '^  msgrow ' "$DA"; }
waitfor 10 hover_drawn || gui_abort "alice's second message did not come back from the daemon"
BY="$(grep -m1 '^  msgrow ' "$DA" | grep -o 'body=[0-9,]*' | cut -d, -f2)"
"$PAIR" a move 600 "$BY" >/dev/null
six() { [ "$(hrx n)" = 6 ]; }
# A fresh fixture: neither client has ever saved preferences, so this is the
# default set the client builds at start, not one a saved setting supplied.
check "six quick reactions on the row under the pointer, with no preferences ever saved" waitfor 5 six
"$PAIR" a shot reactions_hoverstrip >/dev/null
check "each is published to assistive technology" grep -q '^a11yitem msg.react.0 ' "$DA"
not_mine_yet() { [ "$(cell0_mine)" = 0 ]; }
check "...and the first is not yours yet" not_mine_yet
check "the first one can be pressed" click_cell0
"$PAIR" a move 600 "$BY" >/dev/null
now_mine() { [ "$(cell0_mine)" = 1 ]; }
check "pressing the first one reacts" waitfor 10 now_mine
check "...and pressed again" click_cell0
"$PAIR" a move 600 "$BY" >/dev/null
check "pressing it again takes it back" waitfor 10 not_mine_yet
"$PAIR" a move 200 300 >/dev/null
gone() { [ "$(hrx n)" = 0 ]; }
check "off the row, the strip goes" waitfor 5 gone
gui_end
