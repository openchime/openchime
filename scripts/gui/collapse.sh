#!/usr/bin/env bash
# Long messages (CLIENT.md "Long messages"): collapsed by the reader, and
# auto-collapsed past 25 lines.
#
#   scripts/gui/collapse.sh          # run, and leave the client running
#   scripts/gui/collapse.sh --kill   # ... and shut it down afterwards
#
# A 40-line message is drawn cut to its first lines with "Show more", which
# opens it. A short message collapsed from its menu (Collapse) shows its first
# line and "Show message", and Show message opens it again; the daemon's
# answer is what folds and unfolds it. Its own daemon on its own port, wiped
# each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9700}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-collapse}"
export OC_TEST_WS="${OC_TEST_WS:-Collapse Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-collapse-dump.txt"
snap() { "$DRIVE" dump collapse 2>/dev/null | tail -n +2 > "$D"; }
cut_of() { grep "^  cutmsg mid=$1 " "$D" | head -1 | grep -o "\b$2=[^ ]*" | cut -d= -f2; }
kb() { snap; grep -m1 '^kbfocus ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
click_rect() {  # click the centre of an l,t,r,b rect; refuses an empty one
  local l t r b; IFS=, read -r l t r b <<< "$1"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt "${l%.*}" ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== a 40-line message is cut to its first lines"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null
long=""; for i in $(seq 1 40); do long="${long}log line $i\\n"; done
"$DRIVE" sendraw "${long}end of log" >/dev/null
auto() { snap; grep -q '^  cutmsg .* kind=2 ' "$D"; }
check "it is drawn cut, with Show more" waitfor 10 auto
LONG="$(grep -m1 '^  cutmsg .* kind=2 ' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
check "Show more is a button for assistive technology" grep -q "^a11yitem message\.$LONG\.expand " "$D"
"$DRIVE" shot collapse_long >/dev/null
click_rect "$(cut_of "$LONG" r)"
opened() { snap; [ -z "$(cut_of "$LONG" kind)" ]; }
check "Show more opens it whole" waitfor 5 opened

say "== a cut inside a spoiler leaves it covered"
sb=""; for i in $(seq 1 3); do sb="${sb}before $i\\n"; done
sb="${sb}\`\`\`spoiler Ending\\n"
for i in $(seq 1 30); do sb="${sb}hidden $i\\n"; done
"$DRIVE" sendraw "${sb}\`\`\`\\nafter" >/dev/null
spl_cut() { snap; MID="$(grep '^  cutmsg .* kind=2 ' "$D" | tail -1 | grep -o 'mid=[0-9]*' | cut -d= -f2)";
            [ "${MID:-$LONG}" != "$LONG" ] && grep -q "^  spoiler mid=$MID k=0 shown=0 lines=[0-9]* " "$D"; }
check "the message is cut, and the block above the cut is drawn covered" waitfor 10 spl_cut
SP="$(grep -m1 "^  spoiler mid=$MID k=0 " "$D" | grep -o 'lines=[0-9]*' | cut -d= -f2)"
check "every hidden line above the cut is under the cover ($SP)" [ "${SP:-0}" -ge 6 ]
"$DRIVE" shot collapse_spoiler >/dev/null
click_rect "$(cut_of "$MID" r)"
spl_open() { snap; [ -z "$(cut_of "$MID" kind)" ] && grep -q "^  spoiler mid=$MID k=0 shown=0 lines=3[0-9] " "$D"; }
check "opened whole, all its lines are still covered" waitfor 5 spl_open

say "== a short message collapsed from its menu"
"$DRIVE" send "first line of a note" >/dev/null
sleep 1
"$DRIVE" move 20 20 >/dev/null
"$DRIVE" key ctrl+down >/dev/null
"$DRIVE" key shift+f10 >/dev/null
found=0
for _ in $(seq 1 20); do
  [ "$(kb menuhovercmd)" = 112 ] && { found=1; break; }
  "$DRIVE" key down >/dev/null
done
check "its menu offers Collapse to the keyboard" [ "$found" = 1 ]
"$DRIVE" key enter >/dev/null
folded() { snap; grep -q '^  cutmsg .* kind=1 ' "$D"; }
check "the daemon's answer folds it: first line and Show message" waitfor 10 folded
SHORT="$(grep -m1 '^  cutmsg .* kind=1 ' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
"$DRIVE" shot collapse_folded >/dev/null
click_rect "$(cut_of "$SHORT" r)"
unfolded() { snap; [ -z "$(cut_of "$SHORT" kind)" ]; }
check "Show message opens it again" waitfor 10 unfolded
rm -f "$D"

say ""
say "gui_collapse: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
