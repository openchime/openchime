#!/usr/bin/env bash
# Completing a mention, a channel and a pick from the list in the composer, in
# the Win32 client or the browser's (OC_TEST_PLATFORM=web) (REQ-221).
#
#   tools/gui/features/mentions.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/mentions.sh --keep   # ... and leave them running
#
# alice types the start of a name and takes the suggestion. It asserts that
#
#   - Tab, Enter and a click each put the whole name and a space in the box,
#     never the name less its last character;
#   - after a pick made with the mouse the draft is written, as after Tab or
#     Enter (the client also saves what the box holds on its own, so this shows
#     the result is kept rather than which path marked it);
#   - Down moves the choice to the next row and Enter takes that one; Up from
#     the first row wraps to the last;
#   - an emoji outside the Basic Multilingual Plane (a surrogate pair in the
#     box's UTF-16) completes whole;
#   - a channel completes whole too;
#   - a message sent with a completed mention reaches bob as a mention.
#
# Two clients over tools/gui/harness/win32/pair.sh, its own daemon on its own port.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin mentions 9730 /tmp/oc-mentions "$@"
CH=1

# The composer's text as the dump gives it, and the popover's rows.
text_is() { gui_snap a; grep -qxF "composer tables=0 text=\"$1\"" "$DA"; }
ac_n()    { field "$DA" '^ac ' n; }
ac_sel()  { field "$DA" '^ac ' sel; }
ac_repl() { grep -m1 "^  acrow $1 " "$DA" | sed 's/.* repl="\([^"]*\)".*/\1/'; }
ac_open() { gui_snap a; [ "${1:-1}" -le "$(ac_n)" ] 2>/dev/null; }
# Empties the box, and waits for the dump to say so: what is typed next starts
# from nothing.
clear_box() {
  "$PAIR" a key ctrl+a >/dev/null && "$PAIR" a key delete >/dev/null ||
    gui_abort "alice's client did not take the keys that empty the box"
  waitfor 5 text_is '' || gui_abort "the box did not empty: $(grep -m1 '^composer ' "$DA")"
}
click_row() {  # click the popover's row $1
  click_rect a "$(grep -m1 "^  acrow $1 " "$DA" | grep -o 'r=[0-9,.-]*' | cut -d= -f2)"
}

step "fixture: alice and bob in #general"
gui_pair_up
"$PAIR" a size 1200 820 >/dev/null || gui_abort "alice's window did not take its size"
"$PAIR" a channel $CH >/dev/null || gui_abort "alice did not open #general"
"$PAIR" b channel $CH >/dev/null || gui_abort "bob did not open #general"

step "Tab, Enter and a click each take the whole name"
"$PAIR" a typekeys "@bo" >/dev/null
offers_bob() { gui_snap a; [ "$(ac_repl 0)" = '@bob' ]; }
check "typing @bo offers bob" waitfor 5 offers_bob
"$PAIR" a key tab >/dev/null
check "Tab leaves '@bob ' in the box" waitfor 5 text_is '@bob '
clear_box
"$PAIR" a typekeys "@bo" >/dev/null
check "typing @bo again opens the list" waitfor 5 ac_open
"$PAIR" a key enter >/dev/null
check "Enter leaves '@bob ', and sends nothing" waitfor 5 text_is '@bob '
clear_box
# The emptied box's draft goes first (its write is debounced), so the draft
# after the click is the click's result.
undrafted() { gui_snap a; grep -q 'drafthere=0' "$DA"; }
check "the emptied box leaves no draft" waitfor 10 undrafted
"$PAIR" a type "@bo" >/dev/null
check "@bo set in the box opens the list" waitfor 5 ac_open
check "a click on the row can be made" click_row 0
check "a click leaves '@bob '" waitfor 5 text_is '@bob '
drafted() { gui_snap a; grep -q 'drafthere=1' "$DA"; }
check "...and the draft is written" waitfor 10 drafted
clear_box

step "Down chooses the next row"
"$PAIR" a typekeys "@" >/dev/null
check "@ alone offers more than one" waitfor 5 ac_open 2
second="$(ac_repl 1)"
"$PAIR" a key down >/dev/null
sel_one() { gui_snap a; [ "$(ac_sel)" = 1 ]; }
check "Down moves the choice to the second row" waitfor 5 sel_one
"$PAIR" a key enter >/dev/null
check "Enter takes the second: '$second '" waitfor 5 text_is "$second "
clear_box
"$PAIR" a typekeys "@" >/dev/null
waitfor 5 ac_open 2 || gui_abort "@ alone did not open the list a second time"
n_rows="$(ac_n)"
"$PAIR" a key up >/dev/null
wrapped() { gui_snap a; [ "$(ac_sel)" = $((n_rows - 1)) ]; }
check "Up from the first row wraps to the last" waitfor 5 wrapped
"$PAIR" a key esc >/dev/null
clear_box

step "an emoji outside the Basic Multilingual Plane completes whole"
"$PAIR" a typekeys ":grin" >/dev/null
check ":grin opens the list" waitfor 5 ac_open
emo="$(ac_repl 0)"
# Four bytes in UTF-8 is a surrogate pair in the composer's UTF-16.
wide() { [ "$(printf '%s' "$emo" | od -An -tx1 | tr -d ' \n' | cut -c1-2)" = f0 ]; }
check "the first suggestion for :grin is a surrogate pair ($emo)" wide
"$PAIR" a key tab >/dev/null
check "Tab leaves the whole emoji and a space" waitfor 5 text_is "$emo "
clear_box

step "a channel completes whole"
"$PAIR" a typekeys "#gen" >/dev/null
check "#gen opens the list" waitfor 5 ac_open
"$PAIR" a key tab >/dev/null
check "Tab leaves '#general '" waitfor 5 text_is '#general '
clear_box

step "the mention reaches bob"
"$PAIR" a typekeys "@bo" >/dev/null
waitfor 5 ac_open || gui_abort "@bo did not open the list for the message to bob"
"$PAIR" a key tab >/dev/null
waitfor 5 text_is '@bob ' || gui_abort "the mention to bob did not complete"
"$PAIR" a typekeys "are you there" >/dev/null
"$PAIR" a key enter >/dev/null
check "alice's message went as written" waitfor 10 newest_is a 1 "@bob are you there"
"$PAIR" b view activity >/dev/null || gui_abort "bob's client did not open Activity"
in_activity() { gui_snap b; grep -Eq '^acttab=[0-9]+ actwire=[0-9]+ actrows=[1-9]' "$DB"; }
check "bob's Activity has it" waitfor 15 in_activity
"$PAIR" b shot mentions_activity >/dev/null
gui_end
