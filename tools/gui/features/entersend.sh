#!/usr/bin/env bash
# What Enter does in the composer: the Messages preference "When I press Enter".
#
#   tools/gui/features/entersend.sh          # run; the client and daemon stop at the end
#   tools/gui/features/entersend.sh --keep   # ... and leave them running
#
# In the default mode, "Send the message", Enter sends and Shift+Enter adds a
# line. Set to "Start a new line" (the `enter` verb, which saves it as the
# Preferences row does), Enter adds a line and sends nothing, and Ctrl+Enter
# sends the whole draft, both lines as one message. The client starts again and
# the mode is still there: it is kept in the account's prefs. The dump's
# `compose` line says the mode and the key the shortcuts sheet names for
# sending (`send_key=`), which follows it. `enter send` puts it back. Its own
# daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin entersend 9606 /tmp/oc-entersend "Enter Fixture" "$@"

ed() { field "$D" '^ed ' "$1"; }
cmode() { field "$D" '^compose ' enter; }
skey() { fieldq "$D" '^compose ' send_key; }
msgs() { field "$D" '^  ch 1 ' msgs; }
# The newest message's text, its lines joined by `|`: a body's line break is
# written into the dump as it is, so the `ch` line runs on to the next.
prev2() { grep -m1 -A1 '^  ch 1 ' "$D" | tr '\n' '|' | grep -o 'prev="[^"]*"' | cut -d'"' -f2; }

step "the default: Enter sends, Shift+Enter adds a line"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
act channel 1
gui_snap
check "the mode is send" [ "$(cmode)" = send ]
check "and the sheet names Enter for sending" [ "$(skey)" = Enter ]
act type "default first"
gui_snap
l0="$(ed lines)"; m0="$(msgs)"
act key shift+enter
# The line is counted once something is on it (the browser's layout does not
# count an empty last line), and the box is measured on the next frame.
act typekeys "default second"
grown0() { gui_snap; [ "$(ed lines)" -gt "${l0:-0}" ]; }
check "Shift+Enter adds a line" waitfor 5 grown0
unsent0() { [ "$(ed len)" -gt 0 ] && [ "$(msgs)" = "$m0" ]; }
check "and sends nothing: the draft is still in the box, and no message was posted" unsent0
act type "default sent"
act key enter
sent1() { gui_snap; [ "$(msgs)" -gt "${m0:-0}" ] && [ "$(ed len)" = 0 ]; }
check "Enter sends: one more message, and the box is empty" waitfor 10 sent1

step "Start a new line: Enter adds a line, Ctrl+Enter sends"
act enter newline
gui_snap
check "the mode is newline" [ "$(cmode)" = newline ]
check "and the sheet names Ctrl+Enter for sending" [ "$(skey)" = Ctrl+Enter ]
act type "newline first"
gui_snap
l1="$(ed lines)"; m1="$(msgs)"
act key enter
act typekeys "newline second"
grown1() { gui_snap; [ "$(ed lines)" -gt "${l1:-0}" ]; }
check "Enter adds a line" waitfor 5 grown1
check "and sends nothing: the draft is still in the box" [ "$(ed len)" -gt 0 ]
check "and no message was posted" [ "$(msgs)" = "$m1" ]
act key ctrl+enter
sent2() { gui_snap; [ "$(msgs)" = $((m1 + 1)) ] && [ "$(ed len)" = 0 ]; }
check "Ctrl+Enter sends: one message, and the box is empty" waitfor 10 sent2
both() { gui_snap; [ "$(prev2)" = "newline first|newline second" ]; }
check "and it holds both lines" waitfor 10 both

step "the client starts again"
OC_TEST_NO_BUILD=1 OC_TEST_NO_DAEMON=1 gui_launch
"$DRIVE" size 1100 800 >/dev/null
act channel 1
kept() { gui_snap; [ "$(cmode)" = newline ] && [ "$(skey)" = Ctrl+Enter ]; }
check "the mode is still newline, and the sheet still names Ctrl+Enter" waitfor 10 kept
act key ctrl+slash
"$DRIVE" shot entersend_sheet >/dev/null
act key esc

step "enter send puts it back"
act enter send
gui_snap
check "the mode is send" [ "$(cmode)" = send ]
check "and the sheet names Enter again" [ "$(skey)" = Enter ]
gui_end
