#!/usr/bin/env bash
# Scrolling the members pane in the Win32 client (REQ-031).
#
#   scripts/gui_members.sh          # run, and leave the client running
#   scripts/gui_members.sh --kill   # ... and shut it down afterwards
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
#   - switching channel puts the pane back at its own top.
#
# Its own daemon on its own port, bootstrapped with sixty people: enough that the
# pane holds well under half of them at any window size this drives.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9600}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-members}"
export OC_DEV_WS="${OC_DEV_WS:-Members Fixture}"
# alice plus fifty-nine others, all of whom land in #general on registration.
users="alice:pw:owner"
for i in $(seq -w 1 59); do users="$users,member$i:pw:member"; done
export OC_DEV_USERS="$users"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"

D="/tmp/oc-members-dump.txt"
snap() { "$DRIVE" dump members >/dev/null 2>&1; cp "$LIN_DIR/members.txt" "$D" 2>/dev/null; }
mem()  { snap; grep -m1 '^members ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
rows() { grep -c '^memrow ' "$D"; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# Turn the wheel over the pane: its middle, which is where a hand would be. The
# argument is DETENTS, one notch each, which the verb takes as raw wheel delta --
# 120 to a notch, as Windows sends it.
wheel() {  # wheel <notches, negative = down the list>
  local W=1100
  "$DRIVE" wheel "$(( W - 150 )) 400 $(( $1 * 120 ))" >/dev/null
}

say "== sixty people, a pane that holds twenty"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null
all_here() { [ "$(mem n)" = 60 ]; }
check "the channel has sixty members" waitfor 20 all_here
snap
drawn="$(rows)"
fits() { [ "$drawn" -gt 5 ] && [ "$drawn" -lt 60 ]; }
check "only what fits is drawn ($drawn of 60)" fits
more_below() { [ "$(mem max)" != 0 ]; }
check "...and the pane says the rest is below it" more_below
at_top() { [ "$(mem scroll)" = 0 ]; }
check "it starts at the top" at_top
"$DRIVE" shot members_top >/dev/null

say "== the wheel reaches every member"
# Walk the whole list, collecting the members DRAWN at each position: the point
# of the pane is that all sixty can be seen, and counting the distinct people
# seen on the way down is that question asked directly. Reading only the last
# row would not ask it -- without the offset applied the pane draws the same
# first screen for ever, and its last row is a real member either way.
SEEN="/tmp/oc-members-seen.txt"
TOPU="/tmp/oc-members-top.txt"
: > "$SEEN"
snap; grep '^memrow ' "$D" | grep -o 'uid=[0-9]*' | cut -d= -f2 | tee "$TOPU" >> "$SEEN"
for _ in $(seq 1 40); do
  wheel -1
  snap; grep '^memrow ' "$D" | grep -o 'uid=[0-9]*' | cut -d= -f2 >> "$SEEN"
done
distinct="$(sort -u "$SEEN" | grep -c .)"
all_seen() { [ "$distinct" = 60 ]; }
check "all sixty were drawn on the way down ($distinct seen)" all_seen
at_end() { [ "$(mem scroll)" = "$(mem max)" ] && [ "$(mem max)" != 0 ]; }
check "the wheel scrolls to the end" waitfor 5 at_end
# The last row is now the roster's LAST member -- the one the pane could never
# reach -- not merely whichever row happened to be at the bottom of the first
# screen.
snap
last_uid="$(grep '^memrow ' "$D" | tail -1 | grep -o 'uid=[0-9]*' | cut -d= -f2)"
# Somebody who was NOT on the first screen: compared against the list seen while
# scrolling, this would hold whether or not the pane ever moved.
is_new() { [ -n "$last_uid" ] && ! grep -qx "$last_uid" "$TOPU"; }
check "the bottom row is someone the first screen never showed ($last_uid)" is_new
"$DRIVE" shot members_bottom >/dev/null
# And it answers a click: the profile card opens on that person.
r="$(grep '^memrow ' "$D" | tail -1 | grep -o 'r=[0-9,]*' | cut -d= -f2)"
IFS=, read -r l t rr b <<< "$r"
"$DRIVE" click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
opened() { snap; [ "$(grep -m1 -o 'profileuid=[0-9]*' "$D" | cut -d= -f2)" = "$last_uid" ]; }
check "clicking it opens that person" waitfor 5 opened
"$DRIVE" shot members_profile >/dev/null
"$DRIVE" key esc >/dev/null
rm -f "$SEEN" "$TOPU"

say "== it stops at both ends"
for _ in $(seq 1 20); do wheel -1; done
stops() { [ "$(mem scroll)" = "$(mem max)" ]; }
check "past the end it stays at the end" waitfor 5 stops
for _ in $(seq 1 80); do wheel 1; done
check "and back at the top it stays there" waitfor 5 at_top

say "== another channel starts at its own top"
"$DRIVE" mkchan quiet >/dev/null
sleep 1
"$DRIVE" channel 1 >/dev/null
for _ in $(seq 1 10); do wheel -1; done
scrolled() { [ "$(mem scroll)" != 0 ]; }
check "scrolled down in #general" waitfor 5 scrolled
"$DRIVE" channel 2 >/dev/null
check "#quiet's roster starts at the top" waitfor 10 at_top
rm -f "$D"

say ""
say "gui_members: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
