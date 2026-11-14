#!/usr/bin/env bash
# The audit log's rows (REQ-251): each has a menu, by right-click and by the
# keyboard, that goes where the entry happened and filters by what it names.
#
#   tools/gui/admin_rows.sh          # run, and leave the client running
#   tools/gui/admin_rows.sh --kill   # ... and shut it down afterwards
#
# The owner makes a channel and sets its topic, which the audit log records
# with the channel. Right-clicking that entry offers Open member, Go to channel
# and the two filters, and not Go to message (it names no message); Go to
# channel opens the channel. Then by keys alone: Down chooses a row, Shift+F10
# opens its menu, and Filter by this actor narrows the log to the owner. Its
# own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9730}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-admin-rows}"
export OC_TEST_WS="${OC_TEST_WS:-Audit Fixture}"
WAIT_MS="${OC_TEST_WAIT_MS:-10000}"
MENU_AUDIT=14                       # the menu kind the dump's `menu=` names

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

# Each check reads one whole dump: under pipefail, grep -q stopping early would
# fail the pipe it reads from even on a match.
snap() { "$DRIVE" dump admin_rows 2>/dev/null | tail -n +2; }
waitfor() {                         # waitfor <command...>, polled up to WAIT_MS
  local t=0
  while :; do
    "$@" && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
has() { local S; S="$(snap)"; grep -q "$1" <<<"$S"; }
lacks() { local S; S="$(snap)"; ! grep -q "$1" <<<"$S"; }
field() {                           # field <line-prefix> <key>: a key's value on the first such line
  local S; S="$(snap)"; grep -m1 "^$1" <<<"$S" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'
}
# The topic's entry: its row, and the centre of its rectangle in DIPs.
topic_row() { local S; S="$(snap)"; grep -m1 '^  auditrow .* action=channel.topic ' <<<"$S"; }
# A published control pressed as a person would: a click at its centre.
press() {                           # press <aid>
  local S line="" dpi l t r b t0=0
  while :; do
    S="$(snap)"
    line="$(grep -m1 "^a11yitem $1 " <<<"$S")" && break
    [ "$t0" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t0=$((t0 + 100))
  done
  dpi="$(sed -n 's/.* dpi=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
  read -r l t r b <<<"$(awk '{print $(NF-3), $(NF-2), $(NF-1), $NF}' <<<"$line")"
  [ "${dpi:-0}" -gt 0 ] || return 1
  "$DRIVE" click "$(( (l + r) * 48 / dpi ))" "$(( (t + b) * 48 / dpi ))" >/dev/null
}

say "== a channel whose topic the owner sets"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
check "the owner is signed in" waitfor has 'authed=1'
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "audited" >/dev/null
chan_made() { local S; S="$(snap)"; grep -q '^  ch [0-9]* "audited"' <<<"$S"; }
check "the channel exists" waitfor chan_made
CID="$(snap | grep -m1 '^  ch [0-9]* "audited"' | awk '{print $2}')"
"$DRIVE" channel audited >/dev/null
"$DRIVE" chup 0 "Set for the audit log" >/dev/null

say "== the audit log, by right-click"
"$DRIVE" menu 61 >/dev/null
check "the topic's entry is listed, naming its channel" waitfor has "^  auditrow .* channel=$CID target=[0-9]* action=channel.topic "
ACTOR="$(topic_row | grep -o '\bactor=[0-9]*' | cut -d= -f2)"
IFS=, read -r l t r b <<<"$(topic_row | grep -o 'rect=[0-9,.-]*' | cut -d= -f2)"
"$DRIVE" rclick "$(( (l + r) / 2 ))" "$(( (t + b) / 2 ))" >/dev/null
check "a right-click opens the entry's menu" waitfor has "^menu=$MENU_AUDIT "
check "...offering Open member" has '^a11yitem menu.1 '
check "...Go to channel" has '^a11yitem menu.2 '
check "...and the two filters" has '^a11yitem menu.5 '
check "...but not Go to message: it names no message" lacks '^a11yitem menu.3 '
press menu.2
in_chan() { [ "$(field 'hist ' cid)" = "$CID" ]; }
check "Go to channel opens the channel" waitfor in_chan

say "== by the keyboard"
"$DRIVE" menu 61 >/dev/null
check "the audit log is open again" waitfor has '^audit open=1 '
"$DRIVE" key down >/dev/null
focus0() { [ "$(field 'audit open=' focus)" = 0 ]; }
check "Down chooses the first row" waitfor focus0
"$DRIVE" key shift+f10 >/dev/null
check "Shift+F10 opens its menu" waitfor has "^menu=$MENU_AUDIT "
press menu.4
by_actor() { [ "$(field 'audit open=' actor)" = "$ACTOR" ]; }
check "Filter by this actor narrows the log to the owner" waitfor by_actor

say ""
say "gui_admin_rows: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
