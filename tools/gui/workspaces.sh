#!/usr/bin/env bash
# The Workspaces dialog (REQ-012/013): its rows fit, thirty more are listed,
# ordered and filtered, Remove unused takes those not used since a day, Remove
# unreachable names those that do not answer, and Remove all empties it.
#
#   tools/gui/workspaces.sh            # run, and leave the client running
#   tools/gui/workspaces.sh --down     # ... and take it down afterwards
#
# The dialog laid its rows out at fixed offsets, so a long workspace name cut
# the state beside it ("current" drawn as "cur...") with room to spare. One
# signed-in workspace and one remembered with a name that fills the row; the
# open dialog must report nothing clipped (chromefit's `clipped=`). Then the
# footer's Remove all: Cancel leaves the book as it was, and confirming empties
# it, the workspace on screen included, which signs the client out and leaves
# nothing of the session in the accessibility tree: the sign-in card draws no
# rail, and an owner's stale Admin row once crashed the Windows client there.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"


export OC_TEST_PORT="${OC_TEST_PORT:-9720}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-workspaces}"
WAIT_MS="${OC_TEST_WAIT_MS:-10000}"
VIEW_SIGNIN=100
LONG_WS="a-workspace-whose-name-fills-the-whole-row.example.invalid:443"

# One fresh dump: the ack line, then the dump.
snap() { "$DRIVE" dump workspaces 2>/dev/null | tail -n +2; }
key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }
wait_key() {                       # wait_key <key> <want> [ms]
  local key="$1" want="$2" ms="${3:-$WAIT_MS}" t=0
  while :; do
    [ "$(key_of "$(snap)" "$key")" = "$want" ] && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
# The dialog's chromefit line once it is painted, then its `clipped=`.
clipped() { snap | grep -m1 '^chromefit ' | grep -o '\bclipped=[0-9]*' | cut -d= -f2; }
has_item() { local S; S="$(snap)"; grep -q "^a11yitem $1 " <<<"$S"; }
has_item_prefix() { local S; S="$(snap)"; grep -qF "a11yitem $1" <<<"$S"; }
# On the sign-in view, with nothing of the old session published. Polled by
# the clock, not a sleep: each dump is a round trip to the client's tick.
signed_out_clean() {
  local S end=$(( ${EPOCHREALTIME/./} / 1000 + WAIT_MS ))
  while :; do
    S="$(snap)"
    [ "$(key_of "$S" view)" = "$VIEW_SIGNIN" ] && ! grep -qE '^a11yitem (rail|shelf|conv|message|admin|composer)[.]?' <<<"$S" && return 0
    [ "$(( ${EPOCHREALTIME/./} / 1000 ))" -ge "$end" ] && { grep -E '^a11yitem ' <<<"$S" | head -3 >&2; return 1; }
  done
}
confirm_is() {                     # confirm_is <0|1>: the confirmation's open= (its own line)
  local t=0
  while :; do
    local S; S="$(snap)"; grep -q "^confirm open=$1 " <<<"$S" && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
# Press a published button as a person would: a click at its centre, from the
# rectangle the dump publishes for it (lib.sh's sign_in_card does the same).
press() {                          # press <aid>, once it is published
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

say "== fixture: alice, and a workspace with a long name"
"$DRIVE" kill >/dev/null 2>&1
kill_dev_daemon || { say "the old daemon did not stop"; exit 1; }
rm -rf "$OC_TEST_DIR"
"$DRIVE" launch >/dev/null || { say "the client did not launch"; exit 1; }
check "alice is signed in" wait_key authed 1
"$DRIVE" wsbook "$LONG_WS" dan >/dev/null

say "== the dialog fits"
"$DRIVE" open 81 >/dev/null
check "the Workspaces dialog is open" wait_key wsmgr 1
# At least these two. On Windows the list is the development sign-ins, which
# `launch` clears, so nothing of the person's own is in it; on the web it is the
# page's.
n0="$(key_of "$(snap)" wsbook)"
check "at least two workspaces remembered ($n0)" [ "${n0:-0}" -ge 2 ]
check "nothing in it is clipped" [ "$(clipped)" = 0 ]
# By its start: an id is cut at the accessibility tree's limit (OC_ACC_AID_MAX).
check "each row's Remove is published" has_item_prefix "wsmgr.remove.${LONG_WS:0:20}"
check "...and Remove all, by its name" has_item "modal.button.remove all workspaces"

say "== thirty more: listed, ordered, filtered, and the unused removed by date"
# fixture-01..15 last used forty days ago, fixture-16..30 two days ago; the
# cut-off is thirty days ago, so the first fifteen go and the rest stay.
"$DRIVE" key esc >/dev/null
check "the dialog closes" wait_key wsmgr 0
for i in $(seq -w 1 30); do
  age=2; [ "$((10#$i))" -le 15 ] && age=40
  "$DRIVE" wsbook "fixture-$i.example.invalid:443" "user$i" "$age" >/dev/null
done
n30=$((n0 + 30))
check "the book holds all of them ($n30)" wait_key wsbook "$n30"
"$DRIVE" open 81 >/dev/null
check "the Workspaces dialog is open again" wait_key wsmgr 1
# Each reads one whole dump first: under pipefail, grep -q or -m1 stopping
# early would fail the pipe it reads from even on a match.
wsrows() { local S; S="$(snap)"; grep -m1 '^wsmgr rows=' <<<"$S" | grep -o '\brows=[0-9]*' | cut -d= -f2; }
wait_rows() {                      # wait_rows <n>
  local t=0
  while :; do
    [ "$(wsrows)" = "$1" ] && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
fixtures() { local S; S="$(snap)"; grep -c '^  wsrow .* ws=fixture-' <<<"$S"; }
first_signed_in() { local S; S="$(snap)"; grep -q '^  wsrow 0 live=1 ' <<<"$S"; }
last_is_old() { local S; S="$(snap)"; grep '^  wsrow ' <<<"$S" | tail -1 | grep -q 'ws=fixture-\(0[1-9]\|1[0-5]\)\.'; }
focus_is() {                       # focus_is <row>, once the key has landed
  local S t=0
  while :; do
    S="$(snap)"; grep -q "^wsmgr rows=[0-9]* of=[0-9]* focus=$1 " <<<"$S" && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
title_is() { local S; S="$(snap)"; grep -m1 '^confirm open=' <<<"$S" | grep -qF "title=\"$1\""; }
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
"$DRIVE" open 81 >/dev/null
check "reopened, the filter is empty again" wait_rows "$n30"
press "modal.button.remove unused…"
asks_day() {
  local t=0
  while :; do
    local S; S="$(snap)"; grep -q '^wsunused open=1 ' <<<"$S" && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
check "Remove unused asks for a day" asks_day
"$DRIVE" typekeys "$(date -d '30 days ago' +%F)" >/dev/null
press "modal.button.continue"
check "a confirmation asks" confirm_is 1
check "...naming fifteen" title_is "Remove 15 workspaces?"
press "modal.button.remove 15"
check "fifteen are removed" wait_key wsbook "$((n30 - 15))"
check "...back in the Workspaces dialog" wait_key wsmgr 1
old_left=0; new_left=0
S="$(snap)"
for i in $(seq -w 1 30); do
  grep -q "^  wsrow .* ws=fixture-$i\." <<<"$S" || continue
  if [ "$((10#$i))" -le 15 ]; then old_left=$((old_left + 1)); else new_left=$((new_left + 1)); fi
done
check "...fixture-01 to 15, the unused ones, gone ($old_left left)" [ "$old_left" = 0 ]
check "...and fixture-16 to 30 kept ($new_left of 15)" [ "$new_left" = 15 ]
n0=$((n30 - 15))

say "== Remove unreachable"
# Every one left but alice's is an .invalid address no connection reaches;
# alice's is connected, so it is not asked.
press "modal.button.remove unreachable"
check "a confirmation asks, once the check is done" confirm_is 1
check "...naming the $((n0 - 1)) that did not answer" title_is "Remove $((n0 - 1)) workspaces?"
press "modal.button.cancel"
check "...and Cancel removes nothing" wait_key wsbook "$n0"

say "== Remove all, then Cancel"
press "modal.button.remove all workspaces"
check "a confirmation asks" confirm_is 1
press "modal.button.cancel"
check "...and Cancel closes it" confirm_is 0
check "...removing nothing" wait_key wsbook "$n0"

say "== Remove all, confirmed"
press "modal.button.remove all workspaces"
check "a confirmation asks" confirm_is 1
press "modal.button.remove all"
check "nothing is remembered" wait_key wsbook 0
check "...and the client is signed out, on the sign-in view, the old session unpublished" signed_out_clean

[ "${1:-}" = --down ] && { "$DRIVE" kill >/dev/null 2>&1; kill_dev_daemon; }
echo "gui_workspaces: $((checks - fails))/$checks passed ($((SECONDS - START))s)"
[ "$fails" = 0 ]
