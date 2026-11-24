#!/usr/bin/env bash
# Scrolling the members pane in the Win32 client (REQ-031).
#
#   tools/gui/features/members.sh          # run; the client and daemon stop at the end
#   tools/gui/features/members.sh --keep   # ... and leave them running
#
# The pane lists a channel's members and stopped drawing at the bottom edge, so
# in a channel with more members than fit the window the rest could not be seen,
# opened or messaged from the roster. It asserts that
#
#   - a roster larger than the pane draws only what fits, and says so;
#   - the wheel moves it, and the last member -- unreachable before -- can be
#     reached and is a row that answers a click;
#   - it stops at the end rather than scrolling into nothing, and the top is the
#     top;
#   - an owner's menu on a member is wide enough for its longest item;
#   - switching channel puts the pane back at its own top;
#   - each row reports the shape its presence dot is drawn as (REQ-122):
#     alice, signed in, is online, and a member who has never signed in is
#     offline.
#
# And, last, that a reset's dialog shows the whole message to send, its code
# included, without scrolling.
#
# Its own daemon on its own port, bootstrapped with sixty people: enough that the
# pane holds well under half of them at any window size this drives.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin members 9690 /tmp/oc-members "Members Fixture" "$@"
# alice plus fifty-nine others, all of whom land in #general on registration.
users="alice:pw:owner"
for i in $(seq -w 1 59); do users="$users,member$i:pw:member"; done
export OC_TEST_USERS="$users"

mem()  { gui_snap; field "$D" '^members ' "$1"; }
rows() { grep -c '^memrow ' "$D"; }
# Turn the wheel over the pane: its middle, which is where a hand would be. The
# argument is DETENTS, one notch each, which the verb takes as raw wheel delta --
# 120 to a notch, as Windows sends it.
wheel() {  # wheel <notches, negative = down the list>
  local W=1100
  "$DRIVE" wheel "$(( W - 150 )) 400 $(( $1 * 120 ))" >/dev/null
}

step "sixty people, a pane that holds twenty"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
all_here() { [ "$(mem n)" = 60 ]; }
check "the channel has sixty members" waitfor 20 all_here
# The rows are recorded as they are painted, which can be a frame after the
# list itself arrives: wait for them, then judge how many.
fits() { gui_snap; drawn="$(rows)"; [ "$drawn" -gt 5 ] && [ "$drawn" -lt 60 ]; }
drawn=0
check "only what fits is drawn (more than 5, fewer than 60)" waitfor 10 fits
say "  ($drawn of 60 drawn)"
more_below() { [ "$(mem max)" != 0 ]; }
check "...and the pane says the rest is below it" more_below
at_top() { [ "$(mem scroll)" = 0 ]; }
check "it starts at the top" at_top
"$DRIVE" shot members_top >/dev/null

step "a row's presence is a shape, not only a colour"
# The dump's mark= is the shape the row's dot is drawn as, from the one place
# that decides it. alice is the one signed in; member01 never has been.
mark_of() { grep "^memrow .*name=\"$1\"" "$D" | head -1 | grep -o 'mark=[a-z]*' | cut -d= -f2; }
alice_on()  { gui_snap; [ "$(mark_of alice)" = online ]; }
check "alice, signed in, is marked online" waitfor 10 alice_on
never_off() { gui_snap; [ "$(mark_of member01)" = offline ]; }
check "a member who has never signed in is marked offline" waitfor 10 never_off

step "the wheel reaches every member"
# Walk the whole list, collecting the members DRAWN at each position: the point
# of the pane is that all sixty can be seen, and counting the distinct people
# seen on the way down is that question asked directly. Reading only the last
# row would not ask it -- without the offset applied the pane draws the same
# first screen for ever, and its last row is a real member either way.
SEEN="$OC_TEST_DIR/seen.txt"
TOPU="$OC_TEST_DIR/top.txt"
: > "$SEEN"
gui_snap; grep '^memrow ' "$D" | grep -o 'uid=[0-9]*' | cut -d= -f2 | tee "$TOPU" >> "$SEEN"
for _ in $(seq 1 40); do
  wheel -1
  gui_snap; grep '^memrow ' "$D" | grep -o 'uid=[0-9]*' | cut -d= -f2 >> "$SEEN"
done
distinct="$(sort -u "$SEEN" | grep -c .)"
all_seen() { [ "$distinct" = 60 ]; }
check "all sixty were drawn on the way down ($distinct seen)" all_seen
at_end() { [ "$(mem scroll)" = "$(mem max)" ] && [ "$(mem max)" != 0 ]; }
check "the wheel scrolls to the end" waitfor 5 at_end
# The last row is now the roster's LAST member -- the one the pane could never
# reach -- not merely whichever row happened to be at the bottom of the first
# screen.
gui_snap
last_uid="$(grep '^memrow ' "$D" | tail -1 | grep -o 'uid=[0-9]*' | cut -d= -f2)"
# Somebody who was NOT on the first screen: compared against the list seen while
# scrolling, this would hold whether or not the pane ever moved.
is_new() { [ -n "$last_uid" ] && ! grep -qx "$last_uid" "$TOPU"; }
check "the bottom row is someone the first screen never showed ($last_uid)" is_new
"$DRIVE" shot members_bottom >/dev/null
# And it answers a click: the profile card opens on that person.
r="$(grep '^memrow ' "$D" | tail -1 | grep -o 'r=[0-9,]*' | cut -d= -f2)"
IFS=, read -r l t rr b <<< "$r"
click_rect "$r"
opened() { gui_snap; [ "$(grep -m1 -o 'profileuid=[0-9]*' "$D" | cut -d= -f2)" = "$last_uid" ]; }
check "clicking it opens that person" waitfor 5 opened
"$DRIVE" shot members_profile >/dev/null
"$DRIVE" key esc >/dev/null

step "it stops at both ends"
for _ in $(seq 1 20); do wheel -1; done
stops() { [ "$(mem scroll)" = "$(mem max)" ]; }
check "past the end it stays at the end" waitfor 5 stops
for _ in $(seq 1 80); do wheel 1; done
check "and back at the top it stays there" waitfor 5 at_top

step "the member menu fits its longest item"
# An owner's menu on a member carries both resets, and the longer one --
# "Reset password and two-step sign-in..." -- was cut at the menu's fixed width.
gui_snap
r="$(grep '^memrow ' "$D" | sed -n 3p | grep -o 'r=[0-9,]*' | cut -d= -f2)"
IFS=, read -r l t rr b <<< "$r"
"$DRIVE" rclick $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
has_resets() { gui_snap; grep -q '^  menurow cmd=16 ' "$D"; }
check "the menu offers the reset with two-step sign-in" waitfor 5 has_resets
"$DRIVE" shot members_menu >/dev/null
none_cut() { ! grep '^  menurow .* cut=1$' "$D" >/dev/null; }
check "no item is cut ($(grep -c '^  menurow .* cut=1$' "$D") cut)" none_cut
"$DRIVE" key esc >/dev/null

step "another channel starts at its own top"
"$DRIVE" mkchan quiet >/dev/null
two() { gui_snap && grep '^channels=2' "$D" >/dev/null; }
waitfor 10 two || gui_abort "the second channel was not made"
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open #general"
for _ in $(seq 1 10); do wheel -1; done
scrolled() { [ "$(mem scroll)" != 0 ]; }
check "scrolled down in #general" waitfor 5 scrolled
"$DRIVE" channel 2 >/dev/null
check "#quiet's roster starts at the top" waitfor 10 at_top

step "a reset shows its code without scrolling"
# The owner resets a member: the dialog holds the message to send them, and
# the reset code is in it -- all of it shown, none of it below the field's
# fold. The dump's formlines are the EDIT's own count against what its box
# shows.
"$DRIVE" channel 1 >/dev/null
for _ in $(seq 1 80); do wheel 1; done
gui_snap
r="$(grep '^memrow ' "$D" | sed -n 3p | grep -o 'r=[0-9,]*' | cut -d= -f2)"
IFS=, read -r l t rr b <<< "$r"
"$DRIVE" rclick $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
reset_row() { gui_snap; grep -q '^  menurow cmd=15 ' "$D"; }
check "the member menu offers a reset" waitfor 5 reset_row
click_rect "$(grep '^  menurow cmd=15 ' "$D" | grep -o 'r=[0-9,]*' | cut -d= -f2)"
dialog_up() { gui_snap; grep -q '^  formlines 0 ' "$D"; }
check "the reset dialog opens" waitfor 10 dialog_up
"$DRIVE" shotfull members_reset >/dev/null
n_lines=""; n_shown=""; n_first=""
all_shown() {
  gui_snap
  n_lines="$(field "$D" '^  formlines 0 ' lines)"; n_shown="$(field "$D" '^  formlines 0 ' shown)"
  n_first="$(field "$D" '^  formlines 0 ' first)"
  [ -n "$n_lines" ] && [ "$n_lines" -gt 5 ] && [ "$n_lines" -le "$n_shown" ] && [ "$n_first" = 0 ]
}
waitfor 3 all_shown
check "all of the message is shown ($n_lines lines, $n_shown shown, from line $n_first)" all_shown
"$DRIVE" key esc >/dev/null
gui_end
