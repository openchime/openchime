#!/usr/bin/env bash
# The Workspaces dialog (REQ-012/013): its rows fit, and Remove all empties it.
#
#   scripts/gui/workspaces.sh            # run, and leave the client running
#   scripts/gui/workspaces.sh --down     # ... and take it down afterwards
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
. "$HERE/scripts/gui/lib.sh"


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
