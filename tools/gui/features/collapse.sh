#!/usr/bin/env bash
# Long messages (CLIENT.md "Long messages"): collapsed by the reader, and
# auto-collapsed past 25 lines.
#
#   tools/gui/features/collapse.sh          # run; the client and daemon stop at the end
#   tools/gui/features/collapse.sh --keep   # ... and leave them running
#
# A 40-line message is drawn cut to its first lines with "Show more", which
# opens it. A short message collapsed from its menu (Collapse) shows its first
# line and "Show message", and Show message opens it again; the daemon's
# answer is what folds and unfolds it. Its own daemon on its own port, wiped
# each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin collapse 9700 /tmp/oc-collapse "Collapse Fixture" "$@"

cut_of() { field "$D" "^  cutmsg mid=$1 " "$2"; }
kb() { gui_snap; field "$D" '^kbfocus ' "$1"; }

step "a 40-line message is cut to its first lines"
gui_launch
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
long=""; for i in $(seq 1 40); do long="${long}log line $i\\n"; done
"$DRIVE" sendraw "${long}end of log" >/dev/null
auto() { gui_snap; grep -q '^  cutmsg .* kind=2 ' "$D"; }
check "it is drawn cut, with Show more" waitfor 10 auto
LONG="$(grep -m1 '^  cutmsg .* kind=2 ' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
check "Show more is a button for assistive technology" grep -q "^a11yitem message\.$LONG\.expand " "$D"
"$DRIVE" shot collapse_long >/dev/null
click_rect "$(cut_of "$LONG" r)"
opened() { gui_snap; [ -z "$(cut_of "$LONG" kind)" ]; }
check "Show more opens it whole" waitfor 5 opened

step "a cut inside a spoiler leaves it covered"
sb=""; for i in $(seq 1 3); do sb="${sb}before $i\\n"; done
sb="${sb}\`\`\`spoiler Ending\\n"
for i in $(seq 1 30); do sb="${sb}hidden $i\\n"; done
"$DRIVE" sendraw "${sb}\`\`\`\\nafter" >/dev/null
spl_cut() { gui_snap; MID="$(grep '^  cutmsg .* kind=2 ' "$D" | tail -1 | grep -o 'mid=[0-9]*' | cut -d= -f2)";
            [ "${MID:-$LONG}" != "$LONG" ] && grep -q "^  spoiler mid=$MID k=0 shown=0 lines=[0-9]* " "$D"; }
check "the message is cut, and the block above the cut is drawn covered" waitfor 10 spl_cut
SP="$(grep -m1 "^  spoiler mid=$MID k=0 " "$D" | grep -o 'lines=[0-9]*' | cut -d= -f2)"
check "every hidden line above the cut is under the cover ($SP)" [ "${SP:-0}" -ge 6 ]
"$DRIVE" shot collapse_spoiler >/dev/null
click_rect "$(cut_of "$MID" r)"
spl_open() { gui_snap; [ -z "$(cut_of "$MID" kind)" ] && grep -q "^  spoiler mid=$MID k=0 shown=0 lines=3[0-9] " "$D"; }
check "opened whole, all its lines are still covered" waitfor 5 spl_open

step "a cut inside an inline spoiler closes it"
words=""; for i in $(seq 1 480); do words="${words}hide$i "; done
"$DRIVE" sendraw "before\\n>!${words}INLINELAST!<\\nafter" >/dev/null
inl_cut() { gui_snap; IN="$(grep '^  cutmsg .* kind=2 ' "$D" | tail -1 | grep -o 'mid=[0-9]*' | cut -d= -f2)";
            [ "${IN:-$MID}" != "$MID" ] && grep -q "^  spoiler mid=$IN k=0 shown=0 lines=[0-9]* " "$D"; }
check "the message is cut, and the part above the cut is drawn covered" waitfor 10 inl_cut
INL="$(grep -m1 "^  spoiler mid=$IN k=0 " "$D" | grep -o 'lines=[0-9]*' | cut -d= -f2)"
inl_lines() { [ "${INL:-0}" -ge 8 ] && [ "${INL:-99}" -le 10 ]; }
check "under a cover on every line above the cut ($INL)" inl_lines
"$DRIVE" shot collapse_inline >/dev/null
click_rect "$(cut_of "$IN" r)"
inl_open() { gui_snap; [ -z "$(cut_of "$IN" kind)" ] && [ "$(grep -m1 "^  spoiler mid=$IN k=0 " "$D" | grep -o 'lines=[0-9]*' | cut -d= -f2)" -gt 10 ] &&
             grep -q "^  spoiler mid=$IN k=0 shown=0 " "$D"; }
check "opened whole, the rest is still covered" waitfor 5 inl_open

step "a short message collapsed from its menu"
send_stored "first line of a note"
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
folded() { gui_snap; grep -q '^  cutmsg .* kind=1 ' "$D"; }
check "the daemon's answer folds it: first line and Show message" waitfor 10 folded
SHORT="$(grep -m1 '^  cutmsg .* kind=1 ' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
"$DRIVE" shot collapse_folded >/dev/null
click_rect "$(cut_of "$SHORT" r)"
unfolded() { gui_snap; [ -z "$(cut_of "$SHORT" kind)" ]; }
check "Show message opens it again" waitfor 10 unfolded
gui_end
