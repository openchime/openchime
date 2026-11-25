#!/usr/bin/env bash
# Reduce motion (REQ-269): the Appearance preference, Off, On or Follow system.
#
#   tools/gui/features/motion.sh          # run; the client and daemon stop at the end
#   tools/gui/features/motion.sh --keep   # ... and leave them running
#
# Read from the dump's `motion` line: `pref=` (0 Off, 1 On, 2 Follow system),
# `sys=` (what the client takes the system's setting to be: 1 reduce, 0 not,
# -1 unknown), `reduced=` (the setting in force) and `caretlit=` (whether the
# focused field's caret is lit now). The system's answer is stood in for with
# the test-only `sysmotion 0|1|-1` verb, so both branches of Follow system run
# on any machine. It asserts that:
#
#   - the default is Follow system;
#   - under Follow system, a system that reduces reduces, and one that does
#     not does not; On reduces whatever the system says, Off never does;
#   - reduced, the message box's caret is lit on every one of ten looks over
#     two seconds; not reduced, it is seen both lit and dark in that time;
#   - the Appearance row has the three choices; one clicked is put back by
#     Cancel; Reset returns it to Follow system;
#   - the choice saved is there again after the client starts again.
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin motion 9645 /tmp/oc-motion "Motion Fixture" "$@"

mo() { field "$D" '^motion ' "$1"; }
ed_key() { field "$D" '^ed ' "$1"; }
seen_motion() { say "     motion: $(grep -m1 '^motion ' "$D")"; }
# motion_is <pref> <sys> <reduced>, from a fresh look.
motion_now() { gui_snap; [ "$(mo pref)" = "$1" ] && [ "$(mo sys)" = "$2" ] && [ "$(mo reduced)" = "$3" ]; }
motion_is() { waitfor 5 motion_now "$@" || { seen_motion; return 1; }; }
# The rectangle of Preferences' choice <row> <val>, as the dump places it.
hit() { grep -m1 "^  prefhit row=$1 val=$2 " "$D" | grep -o 'r=[-0-9,]*' | cut -d= -f2; }
# caret_looks: ten looks at the focused message box over two seconds, the
# caret's caretlit= from each, on one line; empty for a look without focus.
caret_looks() {
  local i out=""
  for i in 1 2 3 4 5 6 7 8 9 10; do
    gui_snap
    if [ "$(ed_key focus)" = 1 ]; then out="$out$(mo caretlit)"; else out="$out-"; fi
    sleep 0.2   # paced: the caret's phase over time is what is under test
  done
  printf '%s' "$out"
}

step "the default"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
act channel 1
gui_snap
check "Reduce motion is Follow system" [ "$(mo pref)" = 2 ]

step "Follow system, with the system's answer stood in for"
act sysmotion 1
check "a system that reduces: reduced" motion_is 2 1 1
act sysmotion 0
check "a system that does not: not reduced" motion_is 2 0 0

step "On and Off, whatever the system says"
act motion 1
act sysmotion 0
check "On, with a system that does not reduce: reduced" motion_is 1 0 1
act motion 0
act sysmotion 1
check "Off, with a system that reduces: not reduced" motion_is 0 1 0
act sysmotion -1

step "the caret, reduced"
act motion 1
focused() { gui_snap; [ "$(ed_key focus)" = 1 ] && [ "$(mo reduced)" = 1 ]; }
need "the message box has the keys, and motion is reduced" waitfor 6 focused
looks="$(caret_looks)"
say "     caretlit over two seconds: $looks"
check "the caret is lit on all ten looks" [ "$looks" = 1111111111 ]

step "the caret, not reduced"
act motion 0
unreduced() { gui_snap; [ "$(ed_key focus)" = 1 ] && [ "$(mo reduced)" = 0 ]; }
need "the message box has the keys, and motion is not reduced" waitfor 6 unreduced
looks="$(caret_looks)"
say "     caretlit over two seconds: $looks"
both_phases() { [[ "$looks" == *1* ]] && [[ "$looks" == *0* ]]; }
check "the caret blinks: seen lit and dark" both_phases

step "the Appearance row, Cancel and Reset"
act prefs 0
drawn() { gui_snap; [ "$(grep -c '^  prefhit row=130 ' "$D")" = 3 ]; }
check "Appearance has Reduce motion with three choices" waitfor 5 drawn
"$DRIVE" shot motion_prefs >/dev/null
click_rect "$(hit 130 1)" || gui_abort "the dump gives no rectangle for Reduce motion's On"
on_now() { gui_snap; [ "$(mo pref)" = 1 ]; }
check "clicking On chooses it" waitfor 5 on_now
press modal.button.cancel || gui_abort "Preferences has no Cancel to press"
off_now() { gui_snap; ! grep -q '^modal=prefs ' "$D" && [ "$(mo pref)" = 0 ]; }
check "Cancel puts Off back" waitfor 5 off_now
act prefs 3
reset_drawn() { gui_snap; grep -q '^  prefhit row=9 val=0 ' "$D"; }
waitfor 5 reset_drawn || gui_abort "System drew no Reset"
click_rect "$(hit 9 0)" || gui_abort "the dump gives no rectangle for Reset"
system_now() { gui_snap; [ "$(mo pref)" = 2 ]; }
check "Reset returns it to Follow system" waitfor 5 system_now
press modal.button.save || gui_abort "Preferences has no Save to press"
saved() { gui_snap; ! grep -q '^modal=prefs ' "$D" && [ "$(mo pref)" = 2 ]; }
check "and Save keeps it" waitfor 5 saved

gui_end
