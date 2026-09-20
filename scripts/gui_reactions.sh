#!/usr/bin/env bash
# Reacting from the who-reacted pane in the Win32 client (REQ-070/071).
#
#   scripts/gui_reactions.sh          # run, and leave the pair running
#   scripts/gui_reactions.sh --down   # ... and take it down afterwards
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
# Two clients over gui_pair.sh: bob reacts first, so alice's pane has somebody
# else's reaction to join.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
START=$SECONDS
fails=0
checks=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9580}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-reactions}"
CH=1

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

D="/tmp/oc-reactions-dump.txt"
snap() { "$PAIR" a dump reactions > "$D" 2>&1; }
# Field `k` of the pane's first chip, or of its header line.
chip()  { snap; grep -m1 '^  rxnchip 0 ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
pane()  { snap; grep -m1 '^reactors ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# Click the centre of the pane's first chip.
click_chip() {
  local r l t rr b
  r="$(snap; grep -m1 '^  rxnchip 0 ' "$D" | grep -o 'r=[0-9,]*' | cut -d= -f2)"
  IFS=, read -r l t rr b <<< "$r"
  [ -n "${b:-}" ] || return 1
  "$PAIR" a click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== fixture: alice and bob, one message, bob reacts"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
# Wide enough for the context pane the who-reacted list lives in.
"$PAIR" a size 1200 820 >/dev/null
"$PAIR" a channel $CH >/dev/null
"$PAIR" b channel $CH >/dev/null
"$PAIR" a send "react to this" >/dev/null
sleep 1
THUMB="$(printf '\xF0\x9F\x91\x8D')"
"$PAIR" b react "0 $THUMB" >/dev/null                 # bob: thumbs up on the newest
got_one() { [ "$(pane people)" = 1 ]; }

say "== the pane carries the message's chips"
"$PAIR" a reactors 0 >/dev/null
open_now() { [ "$(pane open)" = 1 ]; }
check "the who-reacted pane is open" waitfor 10 open_now
check "...listing the one person who reacted" waitfor 10 got_one
one_chip() { [ "$(pane chips)" = 1 ] && [ "$(chip count)" = 1 ] && [ "$(chip mine)" = 0 ]; }
check "one chip, count 1, not yours" waitfor 10 one_chip
check "the chip is published to assistive technology" bash -c "grep -q '^a11yitem reactors.chip.0 ' $D"
"$PAIR" a shot reactions_pane >/dev/null

say "== pressing it adds your reaction"
click_chip
mine_now() { [ "$(chip mine)" = 1 ] && [ "$(chip count)" = 2 ]; }
check "the chip is yours, and counts two" waitfor 10 mine_now
# The message itself must agree: the transcript's own chip is the same reaction.
on_message() { snap; grep -qE '^ *msgrow .*' "$D" && [ "$(pane people)" = 2 ]; }
check "...and the message agrees" waitfor 10 on_message

say "== pressing it again takes it back"
# It must have been yours to give back, or this check would pass on a chip that
# was never pressed at all.
was_mine="$(chip mine)"
click_chip
back() { [ "$was_mine" = 1 ] && [ "$(chip mine)" = 0 ] && [ "$(chip count)" = 1 ]; }
check "the chip was yours, and now is not, and counts one" waitfor 10 back
rm -f "$D"

say "== the hovered message carries the quick reactions"
hrx()  { snap; grep -m1 '^hoverreact ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
# The first cell's rect and whether it is already yours, from the dump.
cell0_rect() { snap; grep -m1 '^hoverreact ' "$D" | tr ' ' '\n' | sed -n 4p | cut -d@ -f2; }
cell0_mine() { snap; grep -m1 '^hoverreact ' "$D" | tr ' ' '\n' | sed -n 4p | cut -d: -f2 | cut -d@ -f1; }
click_cell0() {
  local l t r b; IFS=, read -r l t r b <<< "$(cell0_rect)"
  [ -n "${b:-}" ] || return 1
  "$PAIR" a click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}
"$PAIR" a send "hover over me" >/dev/null
sleep 1
snap
BY="$(grep -m1 '^  msgrow ' "$D" | grep -o 'body=[0-9,]*' | cut -d, -f2)"
"$PAIR" a move 600 "$BY" >/dev/null
six() { [ "$(hrx n)" = 6 ]; }
check "six quick reactions on the row under the pointer" waitfor 5 six
"$PAIR" a shot reactions_hoverstrip >/dev/null
check "each is published to assistive technology" bash -c "grep -q '^a11yitem msg.react.0 ' $D"
not_mine_yet() { [ "$(cell0_mine)" = 0 ]; }
check "...and the first is not yours yet" not_mine_yet
click_cell0
"$PAIR" a move 600 "$BY" >/dev/null
now_mine() { [ "$(cell0_mine)" = 1 ]; }
check "pressing the first one reacts" waitfor 10 now_mine
click_cell0
"$PAIR" a move 600 "$BY" >/dev/null
check "pressing it again takes it back" waitfor 10 not_mine_yet
"$PAIR" a move 200 300 >/dev/null
gone() { [ "$(hrx n)" = 0 ]; }
check "off the row, the strip goes" waitfor 5 gone

say ""
say "gui_reactions: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
