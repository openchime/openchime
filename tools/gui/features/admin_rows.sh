#!/usr/bin/env bash
# The audit log's rows (REQ-251): each has a menu, by right-click and by the
# keyboard, that goes where the entry happened and filters by what it names.
#
#   tools/gui/features/admin_rows.sh          # run; the client and daemon stop at the end
#   tools/gui/features/admin_rows.sh --keep   # ... and leave them running
#
# The owner makes a channel and sets its topic, which the audit log records
# with the channel. Right-clicking that entry offers Open member, Go to channel
# and the two filters, and not Go to message (it names no message); Go to
# channel opens the channel. Then by keys alone: Down chooses a row, Shift+F10
# opens its menu, and Filter by this actor narrows the log to the owner. Its
# own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin admin_rows 9565 /tmp/oc-admin-rows "Audit Fixture" "$@"

MENU_AUDIT=14                       # the menu kind the dump's `menu=` names

has() { gui_snap && grep "$1" "$D" >/dev/null; }
lacks() { gui_snap && ! grep "$1" "$D" >/dev/null; }
# The topic's entry: its row, and the centre of its rectangle in DIPs.
topic_row() { grep -m1 '^  auditrow .* action=channel.topic ' "$D"; }

step "a channel whose topic the owner sets"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "audited" >/dev/null || gui_abort "the client did not answer mkchan"
waitfor 10 has '^  ch [0-9]* "audited"' || gui_abort "the channel was not made"
CID="$(grep -m1 '^  ch [0-9]* "audited"' "$D" | awk '{print $2}')"
"$DRIVE" channel audited >/dev/null || gui_abort "the client did not open the channel"
"$DRIVE" chup 0 "Set for the audit log" >/dev/null || gui_abort "the client did not answer chup"

step "the audit log, by right-click"
"$DRIVE" menu 61 >/dev/null || gui_abort "the client did not open the audit log"
check "the topic's entry is listed, naming its channel" waitfor 10 has "^  auditrow .* channel=$CID target=[0-9]* action=channel.topic "
ACTOR="$(topic_row | grep -o '\bactor=[0-9]*' | cut -d= -f2)"
IFS=, read -r l t r b <<<"$(topic_row | grep -o 'rect=[0-9,.-]*' | cut -d= -f2)"
[ -n "${b:-}" ] || gui_abort "the topic's entry has no rectangle to right-click"
"$DRIVE" rclick "$(( (l + r) / 2 ))" "$(( (t + b) / 2 ))" >/dev/null
check "a right-click opens the entry's menu" waitfor 10 has "^menu=$MENU_AUDIT "
check "...offering Open member" grep -q '^a11yitem menu.1 ' "$D"
check "...Go to channel" grep -q '^a11yitem menu.2 ' "$D"
check "...and the two filters" grep -q '^a11yitem menu.5 ' "$D"
check "...but not Go to message: it names no message" lacks '^a11yitem menu.3 '
press menu.2
in_chan() { gui_snap && [ "$(field "$D" '^hist ' cid)" = "$CID" ]; }
check "Go to channel opens the channel" waitfor 10 in_chan

step "by the keyboard"
"$DRIVE" menu 61 >/dev/null || gui_abort "the client did not open the audit log"
check "the audit log is open again" waitfor 10 has '^audit open=1 '
"$DRIVE" key down >/dev/null
focus0() { gui_snap && [ "$(field "$D" '^audit open=' focus)" = 0 ]; }
check "Down chooses the first row" waitfor 10 focus0
"$DRIVE" key shift+f10 >/dev/null
check "Shift+F10 opens its menu" waitfor 10 has "^menu=$MENU_AUDIT "
press menu.4
by_actor() { gui_snap && [ -n "$ACTOR" ] && [ "$(field "$D" '^audit open=' actor)" = "$ACTOR" ]; }
check "Filter by this actor narrows the log to the owner" waitfor 10 by_actor
gui_end
