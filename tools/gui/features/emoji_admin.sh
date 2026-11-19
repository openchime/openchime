#!/usr/bin/env bash
# Admin > Custom emoji (REQ-072): the workspace's custom emoji, listed and
# deleted.
#
#   tools/gui/features/emoji_admin.sh          # run; the client and daemon stop at the end
#   tools/gui/features/emoji_admin.sh --keep   # ... and leave them running
#
# The owner adds three emoji. The emoji picker's Manage link opens the pane,
# which lists all three by name, each with a Delete (they added them all). The
# search box narrows the list. Delete asks first, and on Enter the emoji leaves
# the list and the workspace's catalogue.
#
# Then the same again by keys alone, each through the path a real key takes (the
# `key` and `typekeys` verbs): Ctrl+K opens the pane from another Admin tab; Tab
# and F6 reach the search box; the arrows move through the rows; Delete on the
# selected row asks about that row, Esc declines and Enter deletes it. A member
# reaches the pane the same way, and Delete does nothing on a row that is not
# theirs. Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin emoji_admin 9525 /tmp/oc-emoji-admin "Emoji Fixture" "$@"

admin() { field "$D" '^emojiadmin ' "$1"; }
rows() { grep '^  emojirow ' "$D" | awk '{print $2}' | tr '\n' ' '; }
del_of() { grep "^  emojirow $1 " "$D" | grep -o 'del=[0-9,.-]*' | cut -d= -f2; }

step "the owner adds three custom emoji"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
mkdir -p "$LIN_DIR"
make_png "$LIN_DIR/em-party.png" 32x32 f59e0b
make_png "$LIN_DIR/em-parrot.png" 32x32 22c55e
make_png "$LIN_DIR/em-ship.png" 32x32 3b82f6
count_is() { gui_snap; grep -q "^cemoji=$1\$" "$D"; }
n=0
for e in party parrot ship; do
  "$DRIVE" emoji_add "$e $(cpath em-$e.png)" >/dev/null
  n=$((n + 1)); waitfor 30 count_is "$n" || gui_abort "the fixture's emoji :$e: was not added"
done
check "the catalogue has three" count_is 3

step "the picker's Manage link opens the pane"
"$DRIVE" emoji 0 >/dev/null
manage() { gui_snap; [ -n "$(admin manage)" ] && [ "$(admin manage)" != "0,0,0,0" ]; }
check "the picker shows Manage over the workspace's emoji" waitfor 5 manage
check "Manage is published to assistive technology" grep -q '^a11yitem picker.manage ' "$D"
click_rect "$(admin manage)"
listed() { gui_snap; [ "$(admin shown)" = 1 ] && [ "$(rows)" = "parrot party ship " ]; }
check "the pane lists all three, by name" waitfor 5 listed
three_dels() { [ "$(grep -c '^  emojirow .* del=[1-9]' "$D")" = 3 ]; }
check "each has a Delete for their creator" three_dels
check "the Delete buttons are published" grep -q '^a11yitem admin.emoji.party.delete ' "$D"
"$DRIVE" shot emoji_admin_list >/dev/null

step "search narrows it"
"$DRIVE" emoji_find par >/dev/null
narrowed() { gui_snap; [ "$(rows)" = "parrot party " ] && [ "$(admin dels)" = 2 ]; }
check "'par' leaves parrot and party" waitfor 5 narrowed
"$DRIVE" emoji_find - >/dev/null
all_back() { gui_snap; [ "$(rows)" = "parrot party ship " ]; }
check "clearing it brings ship back" waitfor 5 all_back

step "Delete asks, then deletes"
click_rect "$(del_of ship)"
asked() { gui_snap; grep -q '^confirm open=1 act=19 ' "$D"; }
check "Delete asks first" waitfor 5 asked
"$DRIVE" shot emoji_admin_confirm >/dev/null
"$DRIVE" key enter >/dev/null
gone() { gui_snap; [ "$(rows)" = "parrot party " ] && grep -q '^cemoji=2$' "$D"; }
check "ship leaves the pane and the catalogue" waitfor 10 gone

step "by keys: the palette opens the pane, from another Admin tab"
key() { "$DRIVE" key "$1" >/dev/null || gui_abort "the client did not answer key $1"; }
typed() { "$DRIVE" typekeys "$1" >/dev/null || gui_abort "the client did not answer typekeys $1"; }
palette() { key ctrl+k; typed "$1"; key enter; }
is() { gui_snap; [ "$(admin "$1")" = "$2" ]; }           # is <field> <value>, this moment
# A key is handled before its ack, so `is` reads what it set; what a frame draws
# and what the daemon answers are waited for.
palette "manage groups"
check "'manage groups' leaves the pane (it is on Groups)" waitfor 5 is shown 0
palette "custom emoji"
check "'custom emoji' opens the pane" waitfor 5 is shown 1
check "...with no row selected" is sel -
check "...and the box not focused" is searching 0

step "by keys: the search box"
key tab
check "Tab focuses it" is searching 1
typed part
narrow_party() { gui_snap; [ "$(rows)" = "party " ] && [ "$(admin q)" = part ]; }
check "typing narrows the list to party" waitfor 5 narrow_party
for _ in 1 2 3 4; do key back; done
both() { gui_snap; [ "$(rows)" = "parrot party " ] && [ "$(admin q)" = "" ]; }
check "Backspace brings parrot back" waitfor 5 both
key esc
check "Esc gives the keyboard back to the pane" is searching 0
key f6
check "F6 focuses it too" is searching 1
key f6
check "...and F6 leaves it" is searching 0

step "by keys: the rows, and Delete"
key down
check "Down selects the first row" is sel parrot
key down
check "Down moves to the next" is sel party
key down
check "...and stops at the last" is sel party
key up
check "Up moves back" is sel parrot
key end
check "End goes to the last" is sel party
typed x
check "a letter is not taken while the keyboard is on the rows" is q ""
key tab
check "Tab goes back to the box" is searching 1
key down
on_first_row() { is searching 0 && [ "$(admin sel)" = parrot ]; }
check "Down from the box goes to the first row" on_first_row
key end
key del
asked_party() { gui_snap; grep -q '^confirm open=1 act=19 .* emoji="party" ' "$D"; }
check "Delete asks about the selected row" waitfor 5 asked_party
"$DRIVE" shot emoji_admin_keys_confirm >/dev/null
key esc
closed() { gui_snap; grep -q '^confirm open=0 ' "$D"; }
check "Esc declines" waitfor 5 closed
still_there() { is sel party && [ "$(rows)" = "parrot party " ]; }
check "...and the row is still there, still selected" waitfor 5 still_there
key del
check "Delete asks again" waitfor 5 asked_party
key enter
party_gone() { gui_snap; [ "$(rows)" = "parrot " ] && grep -q '^cemoji=1$' "$D"; }
check "Enter deletes it, from the pane and the catalogue" waitfor 10 party_gone
sel_parrot() { is sel parrot; }
check "the selection falls on the row left" waitfor 5 sel_parrot
"$DRIVE" shot emoji_admin_keys >/dev/null

step "by keys: a member, on a row that is not theirs"
gui_launch "$("$DRIVE" devaddr)" bob:pw
listed_for_bob() { gui_snap; grep -q '^cemoji=1$' "$D"; }
check "bob has the catalogue" waitfor 20 listed_for_bob
palette "custom emoji"
check "the palette opens the pane for a member" waitfor 5 is shown 1
check "...with no Delete on alice's emoji" waitfor 5 is dels 0
key down
check "Down selects the row" is sel parrot
key del
check "Delete does not ask" closed
rm -f "$LIN_DIR"/em-*.png
gui_end
