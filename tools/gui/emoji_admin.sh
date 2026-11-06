#!/usr/bin/env bash
# Admin > Custom emoji (REQ-072): the workspace's custom emoji, listed and
# deleted.
#
#   tools/gui/emoji_admin.sh          # run, and leave the client running
#   tools/gui/emoji_admin.sh --kill   # ... and shut it down afterwards
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
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it (tools/gui/uploads.sh).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }

export OC_TEST_PORT="${OC_TEST_PORT:-9660}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-emoji-admin}"
export OC_TEST_WS="${OC_TEST_WS:-Emoji Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="$OC_TEST_DIR-dump.txt"
snap() { "$DRIVE" dump emoji_admin 2>/dev/null | tail -n +2 > "$D"; }
admin() { grep -m1 '^emojiadmin ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
rows() { grep '^  emojirow ' "$D" | awk '{print $2}' | tr '\n' ' '; }
del_of() { grep "^  emojirow $1 " "$D" | grep -o 'del=[0-9,.-]*' | cut -d= -f2; }
# Polled on the outcome: each dump is a round trip to the client's tick.
waitfor() {  # waitfor <secs> <command...>
  local end=$((SECONDS + $1)); shift
  while :; do
    "$@" && return 0
    [ "$SECONDS" -ge "$end" ] && return 1
  done
}
click_rect() {  # click the centre of an l,t,r,b rect; refuses an empty one
  local l t r b; IFS=, read -r l t r b <<< "$1"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt "${l%.*}" ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}
make_png() {  # a 32x32 PNG in one colour
  python3 - "$1" "$2" <<'PY'
import struct, sys, zlib
w = h = 32
px = bytes.fromhex(sys.argv[2])
rows = b''.join(b'\0' + px * w for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
}

say "== the owner adds three custom emoji"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
signed_in() { snap; grep -q 'authed=1' "$D"; }
waitfor 20 signed_in
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
mkdir -p "$LIN_DIR"
make_png "$LIN_DIR/em-party.png" f59e0b
make_png "$LIN_DIR/em-parrot.png" 22c55e
make_png "$LIN_DIR/em-ship.png" 3b82f6
count_is() { snap; grep -q "^cemoji=$1\$" "$D"; }
n=0
for e in party parrot ship; do
  "$DRIVE" emoji_add "$e $(cpath em-$e.png)" >/dev/null
  n=$((n + 1)); waitfor 30 count_is "$n"
done
check "the catalogue has three" count_is 3

say "== the picker's Manage link opens the pane"
"$DRIVE" emoji 0 >/dev/null
manage() { snap; [ -n "$(admin manage)" ] && [ "$(admin manage)" != "0,0,0,0" ]; }
check "the picker shows Manage over the workspace's emoji" waitfor 5 manage
check "Manage is published to assistive technology" grep -q '^a11yitem picker.manage ' "$D"
click_rect "$(admin manage)"
listed() { snap; [ "$(admin shown)" = 1 ] && [ "$(rows)" = "parrot party ship " ]; }
check "the pane lists all three, by name" waitfor 5 listed
check "each has a Delete for their creator" bash -c "[ \"\$(grep -c '^  emojirow .* del=[1-9]' $D)\" = 3 ]"
check "the Delete buttons are published" grep -q '^a11yitem admin.emoji.party.delete ' "$D"
"$DRIVE" shot emoji_admin_list >/dev/null

say "== search narrows it"
"$DRIVE" emoji_find par >/dev/null
narrowed() { snap; [ "$(rows)" = "parrot party " ] && [ "$(admin dels)" = 2 ]; }
check "'par' leaves parrot and party" waitfor 5 narrowed
"$DRIVE" emoji_find - >/dev/null
all_back() { snap; [ "$(rows)" = "parrot party ship " ]; }
check "clearing it brings ship back" waitfor 5 all_back

say "== Delete asks, then deletes"
click_rect "$(del_of ship)"
asked() { snap; grep -q '^confirm open=1 act=19 ' "$D"; }
check "Delete asks first" waitfor 5 asked
"$DRIVE" shot emoji_admin_confirm >/dev/null
"$DRIVE" key enter >/dev/null
gone() { snap; [ "$(rows)" = "parrot party " ] && grep -q '^cemoji=2$' "$D"; }
check "ship leaves the pane and the catalogue" waitfor 10 gone

say "== by keys: the palette opens the pane, from another Admin tab"
key() { "$DRIVE" key "$1" >/dev/null || fail "the client did not answer key $1"; }
typed() { "$DRIVE" typekeys "$1" >/dev/null || fail "the client did not answer typekeys $1"; }
palette() { key ctrl+k; typed "$1"; key enter; }
is() { snap; [ "$(admin "$1")" = "$2" ]; }           # is <field> <value>, this moment
# A key is handled before its ack, so `is` reads what it set; what a frame draws
# and what the daemon answers are waited for.
palette "manage groups"
check "'manage groups' leaves the pane (it is on Groups)" waitfor 5 is shown 0
palette "custom emoji"
check "'custom emoji' opens the pane" waitfor 5 is shown 1
check "...with no row selected" is sel -
check "...and the box not focused" is searching 0

say "== by keys: the search box"
key tab
check "Tab focuses it" is searching 1
typed part
narrow_party() { snap; [ "$(rows)" = "party " ] && [ "$(admin q)" = part ]; }
check "typing narrows the list to party" waitfor 5 narrow_party
for _ in 1 2 3 4; do key back; done
both() { snap; [ "$(rows)" = "parrot party " ] && [ "$(admin q)" = "" ]; }
check "Backspace brings parrot back" waitfor 5 both
key esc
check "Esc gives the keyboard back to the pane" is searching 0
key f6
check "F6 focuses it too" is searching 1
key f6
check "...and F6 leaves it" is searching 0

say "== by keys: the rows, and Delete"
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
asked_party() { snap; grep -q '^confirm open=1 act=19 .* emoji="party"$' "$D"; }
check "Delete asks about the selected row" waitfor 5 asked_party
"$DRIVE" shot emoji_admin_keys_confirm >/dev/null
key esc
closed() { snap; grep -q '^confirm open=0 ' "$D"; }
check "Esc declines" waitfor 5 closed
still_there() { is sel party && [ "$(rows)" = "parrot party " ]; }
check "...and the row is still there, still selected" waitfor 5 still_there
key del
check "Delete asks again" waitfor 5 asked_party
key enter
party_gone() { snap; [ "$(rows)" = "parrot " ] && grep -q '^cemoji=1$' "$D"; }
check "Enter deletes it, from the pane and the catalogue" waitfor 10 party_gone
sel_parrot() { is sel parrot; }
check "the selection falls on the row left" waitfor 5 sel_parrot
"$DRIVE" shot emoji_admin_keys >/dev/null

say "== by keys: a member, on a row that is not theirs"
"$DRIVE" launch "$("$DRIVE" devaddr)" bob:pw >/dev/null 2>&1 || fail "bob's client did not launch"
listed_for_bob() { snap; grep -q 'authed=1' "$D" && grep -q '^cemoji=1$' "$D"; }
check "bob is signed in, and has the catalogue" waitfor 20 listed_for_bob
palette "custom emoji"
check "the palette opens the pane for a member" waitfor 5 is shown 1
check "...with no Delete on alice's emoji" waitfor 5 is dels 0
key down
check "Down selects the row" is sel parrot
key del
check "Delete does not ask" closed
rm -f "$D" "$LIN_DIR"/em-*.png

say ""
say "gui_emoji_admin: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
