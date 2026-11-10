#!/usr/bin/env bash
# Completing a mention, a channel and a pick from the list in the composer, in
# the Win32 client or the browser's (OC_TEST_PLATFORM=web) (REQ-221).
#
#   tools/gui/mentions.sh          # run, and leave the pair running
#   tools/gui/mentions.sh --down   # ... and take it down afterwards
#
# alice types the start of a name and takes the suggestion. It asserts that
#
#   - Tab, Enter and a click each put the whole name and a space in the box,
#     never the name less its last character;
#   - after a pick made with the mouse the draft is written, as after Tab or
#     Enter (the client also saves what the box holds on its own, so this shows
#     the result is kept rather than which path marked it);
#   - Down moves the choice to the next row and Enter takes that one;
#   - a channel completes whole too;
#   - a message sent with a completed mention reaches bob as a mention.
#
# Two clients over tools/gui/pair.sh, its own daemon on its own port.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9730}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-mentions}"
CH=1

DA="/tmp/oc-mentions-a.txt"; DB="/tmp/oc-mentions-b.txt"
snap_a() { "$PAIR" a dump mentions_a > "$DA" 2>&1; }
snap_b() { "$PAIR" b dump mentions_b > "$DB" 2>&1; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# The composer's text as the dump gives it, and the popover's rows.
text_is() { snap_a; grep -qxF "composer tables=0 text=\"$1\"" "$DA"; }
ac_n()    { grep -m1 '^ac ' "$DA" | grep -o '\bn=[0-9]*' | cut -d= -f2; }
ac_sel()  { grep -m1 '^ac ' "$DA" | grep -o '\bsel=[0-9]*' | cut -d= -f2; }
ac_repl() { grep -m1 "^  acrow $1 " "$DA" | sed 's/.* repl="\([^"]*\)".*/\1/'; }
ac_open() { snap_a; [ "${1:-1}" -le "$(ac_n)" ] 2>/dev/null; }
clear_box() { "$PAIR" a key ctrl+a >/dev/null; "$PAIR" a key delete >/dev/null; }
click_row() {  # click the popover's row $1
  local r l t rr b
  r="$(grep -m1 "^  acrow $1 " "$DA" | grep -o 'r=[0-9,.-]*' | cut -d= -f2)"
  IFS=, read -r l t rr b <<< "$r"
  [ -n "${b:-}" ] && [ "${rr%.*}" -gt 0 ] 2>/dev/null || return 1
  "$PAIR" a click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== fixture: alice and bob in #general"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_TEST_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
"$PAIR" a size 1200 820 >/dev/null
"$PAIR" a channel $CH >/dev/null
"$PAIR" b channel $CH >/dev/null

say "== Tab, Enter and a click each take the whole name"
"$PAIR" a typekeys "@bo" >/dev/null
offers_bob() { snap_a; [ "$(ac_repl 0)" = '@bob' ]; }
check "typing @bo offers bob" waitfor 5 offers_bob
"$PAIR" a key tab >/dev/null
check "Tab leaves '@bob ' in the box" waitfor 5 text_is '@bob '
clear_box
"$PAIR" a typekeys "@bo" >/dev/null
waitfor 5 ac_open >/dev/null
"$PAIR" a key enter >/dev/null
check "Enter leaves '@bob ', and sends nothing" waitfor 5 text_is '@bob '
clear_box
# The emptied box's draft goes first (its write is debounced), so the draft
# after the click is the click's result.
undrafted() { snap_a; grep -q 'drafthere=0' "$DA"; }
check "the emptied box leaves no draft" waitfor 10 undrafted
"$PAIR" a type "@bo" >/dev/null
waitfor 5 ac_open >/dev/null
check "a click on the row can be made" click_row 0
check "a click leaves '@bob '" waitfor 5 text_is '@bob '
drafted() { snap_a; grep -q 'drafthere=1' "$DA"; }
check "...and the draft is written" waitfor 10 drafted
clear_box

say "== Down chooses the next row"
"$PAIR" a typekeys "@" >/dev/null
check "@ alone offers more than one" waitfor 5 ac_open 2
second="$(ac_repl 1)"
"$PAIR" a key down >/dev/null
sel_one() { snap_a; [ "$(ac_sel)" = 1 ]; }
check "Down moves the choice to the second row" waitfor 5 sel_one
"$PAIR" a key enter >/dev/null
check "Enter takes the second: '$second '" waitfor 5 text_is "$second "
clear_box

say "== a channel completes whole"
"$PAIR" a typekeys "#gen" >/dev/null
waitfor 5 ac_open >/dev/null
"$PAIR" a key tab >/dev/null
check "Tab leaves '#general '" waitfor 5 text_is '#general '
clear_box

say "== the mention reaches bob"
"$PAIR" a typekeys "@bo" >/dev/null
waitfor 5 ac_open >/dev/null
"$PAIR" a key tab >/dev/null
"$PAIR" a typekeys "are you there" >/dev/null
"$PAIR" a key enter >/dev/null
sent() { snap_a; grep -q '^  ch 1 .*prev="@bob are you there"' "$DA"; }
check "alice's message went as written" waitfor 10 sent
"$PAIR" b view activity >/dev/null
in_activity() { snap_b; grep -Eq '^acttab=[0-9]+ actwire=[0-9]+ actrows=[1-9]' "$DB"; }
check "bob's Activity has it" waitfor 15 in_activity
"$PAIR" b shot mentions_activity >/dev/null

say ""
say "gui_mentions: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
