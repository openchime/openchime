#!/usr/bin/env bash
# The Workspaces dialog (REQ-012/013): its rows fit, thirty more are listed,
# ordered and filtered, Remove unused takes those not used since a day, Remove
# unreachable names those that do not answer, and Remove all empties it.
#
#   tools/gui/features/workspaces.sh          # run; the client and daemon stop at the end
#   tools/gui/features/workspaces.sh --keep   # ... and leave them running
#
# The dialog laid its rows out at fixed offsets, so a long workspace name cut
# the state beside it ("current" drawn as "cur...") with room to spare. One
# signed-in workspace and one remembered with a name that fills the row; the
# open dialog must report nothing clipped (chromefit's `clipped=`). Then the
# footer's Remove all: Cancel leaves the book as it was, and confirming empties
# it, the workspace on screen included, which signs the client out and leaves
# nothing of the session in the accessibility tree: the sign-in card draws no
# rail, and an owner's stale Admin row once crashed the Windows client there.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin workspaces 9790 /tmp/oc-workspaces "$@"
WAIT_S=$(( ${OC_TEST_WAIT_MS:-10000} / 1000 ))
VIEW_SIGNIN=100
LONG_WS="a-workspace-whose-name-fills-the-whole-row.example.invalid:443"

# The first key=value of that name anywhere in the dump.
kv() { field "$D" "\\b$1=" "$1"; }
key_is() { gui_snap; [ "$(kv "$1")" = "$2" ]; }
wait_key() { waitfor "$WAIT_S" key_is "$1" "$2"; }          # wait_key <key> <want>
# The dialog's chromefit line once it is painted, then its `clipped=`.
not_clipped() { gui_snap; [ "$(field "$D" '^chromefit ' clipped)" = 0 ]; }
item() { gui_snap; grep -q "^a11yitem $1 " "$D"; }
item_prefix() { gui_snap; grep -qF "a11yitem $1" "$D"; }
# On the sign-in view, with nothing of the old session published.
clean() { gui_snap; [ "$(kv view)" = "$VIEW_SIGNIN" ] && ! grep -qE '^a11yitem (rail|shelf|conv|message|admin|composer)[.]?' "$D"; }
signed_out_clean() {
  waitfor "$WAIT_S" clean && return 0
  grep -E '^a11yitem ' "$D" | head -3 >&2; return 1
}
confirm() { gui_snap; grep -q "^confirm open=$1 " "$D"; }
confirm_is() { waitfor "$WAIT_S" confirm "$1"; }            # confirm_is <0|1>: the confirmation's open=
# Press a published button as a person would (lib.sh's press). A button that
# is not there to press stops the script: what follows is about what it does.
must_press() { press "$1" || gui_abort "no '$1' to press"; }   # must_press <aid>

step "fixture: alice, and a workspace with a long name"
gui_launch
"$DRIVE" wsbook "$LONG_WS" dan >/dev/null || gui_abort "the client did not take wsbook"

step "the dialog fits"
"$DRIVE" open 81 >/dev/null || gui_abort "the client did not take open 81"
check "the Workspaces dialog is open" wait_key wsmgr 1
# At least these two. On Windows the list is the development sign-ins, which
# `launch` clears, so nothing of the person's own is in it; on the web it is the
# page's.
n0="$(kv wsbook)"
check "at least two workspaces remembered ($n0)" [ "${n0:-0}" -ge 2 ]
check "nothing in it is clipped" waitfor 5 not_clipped
# By its start: an id is cut at the accessibility tree's limit (OC_ACC_AID_MAX).
check "each row's Remove is published" waitfor 5 item_prefix "wsmgr.remove.${LONG_WS:0:20}"
check "...and Remove all, by its name" waitfor 5 item "modal.button.remove all workspaces"

step "thirty more: listed, ordered, filtered, and the unused removed by date"
# fixture-01..15 last used forty days ago, fixture-16..30 two days ago; the
# cut-off is thirty days ago, so the first fifteen go and the rest stay.
"$DRIVE" key esc >/dev/null
check "the dialog closes" wait_key wsmgr 0
for i in $(seq -w 1 30); do
  age=2; [ "$((10#$i))" -le 15 ] && age=40
  "$DRIVE" wsbook "fixture-$i.example.invalid:443" "user$i" "$age" >/dev/null || gui_abort "the client did not take wsbook $i"
done
n30=$((n0 + 30))
check "the book holds all of them ($n30)" wait_key wsbook "$n30"
"$DRIVE" open 81 >/dev/null || gui_abort "the client did not take open 81"
check "the Workspaces dialog is open again" wait_key wsmgr 1
rows_are() { gui_snap; [ "$(field "$D" '^wsmgr rows=' rows)" = "$1" ]; }
wait_rows() { waitfor "$WAIT_S" rows_are "$1"; }             # wait_rows <n>
fixtures() { grep -c '^  wsrow .* ws=fixture-' "$D"; }
first_signed_in() { grep -q '^  wsrow 0 live=1 ' "$D"; }
last_is_old() { local last; last="$(grep '^  wsrow ' "$D" | tail -1)"; grep -q 'ws=fixture-\(0[1-9]\|1[0-5]\)\.' <<<"$last"; }
focus() { gui_snap; grep -q "^wsmgr rows=[0-9]* of=[0-9]* focus=$1 " "$D"; }
focus_is() { waitfor "$WAIT_S" focus "$1"; }                # focus_is <row>, once the key has landed
title_is() { local c; c="$(grep -m1 '^confirm open=' "$D")"; grep -qF "title=\"$1\"" <<<"$c"; }
check "every workspace is listed ($n30)" wait_rows "$n30"
check "...the thirty fixtures among them" [ "$(fixtures)" = 30 ]
check "the signed-in workspace comes first" first_signed_in
check "...and the least recently used last" last_is_old
"$DRIVE" typekeys "fixture-0" >/dev/null
check "typing filters: fixture-0 leaves nine" wait_rows 9
"$DRIVE" key down >/dev/null
check "Down chooses the first row" focus_is 0
"$DRIVE" key esc >/dev/null
check "Esc closes the dialog" wait_key wsmgr 0
"$DRIVE" open 81 >/dev/null || gui_abort "the client did not take open 81"
check "reopened, the filter is empty again" wait_rows "$n30"
must_press "modal.button.remove unused…"
asks() { gui_snap; grep -q '^wsunused open=1 ' "$D"; }
check "Remove unused asks for a day" waitfor "$WAIT_S" asks
"$DRIVE" typekeys "$(date -d '30 days ago' +%F)" >/dev/null
must_press "modal.button.continue"
check "a confirmation asks" confirm_is 1
check "...naming fifteen" title_is "Remove 15 workspaces?"
must_press "modal.button.remove 15"
check "fifteen are removed" wait_key wsbook "$((n30 - 15))"
check "...back in the Workspaces dialog" wait_key wsmgr 1
old_left=0; new_left=0
gui_snap
for i in $(seq -w 1 30); do
  grep -q "^  wsrow .* ws=fixture-$i\." "$D" || continue
  if [ "$((10#$i))" -le 15 ]; then old_left=$((old_left + 1)); else new_left=$((new_left + 1)); fi
done
check "...fixture-01 to 15, the unused ones, gone ($old_left left)" [ "$old_left" = 0 ]
check "...and fixture-16 to 30 kept ($new_left of 15)" [ "$new_left" = 15 ]
n0=$((n30 - 15))

step "Remove unreachable"
# Every one left but alice's is an .invalid address no connection reaches;
# alice's is connected, so it is not asked.
must_press "modal.button.remove unreachable"
check "a confirmation asks, once the check is done" confirm_is 1
check "...naming the $((n0 - 1)) that did not answer" title_is "Remove $((n0 - 1)) workspaces?"
must_press "modal.button.cancel"
check "...and Cancel removes nothing" wait_key wsbook "$n0"

step "Remove all, then Cancel"
must_press "modal.button.remove all workspaces"
check "a confirmation asks" confirm_is 1
must_press "modal.button.cancel"
check "...and Cancel closes it" confirm_is 0
check "...removing nothing" wait_key wsbook "$n0"

step "Remove all, confirmed"
must_press "modal.button.remove all workspaces"
check "a confirmation asks" confirm_is 1
must_press "modal.button.remove all"
check "nothing is remembered" wait_key wsbook 0
check "...and the client is signed out, on the sign-in view, the old session unpublished" signed_out_clean
gui_end
