#!/usr/bin/env bash
# Spoilers (MARKDOWN.md §1d): text hidden until a reader chooses to see it.
#
#   tools/gui/spoiler.sh          # run, and leave the client running
#   tools/gui/spoiler.sh --kill   # ... and shut it down afterwards
#
# A message with an inline spoiler and a labelled block is sent. Both are drawn
# covered, each a button for assistive technology, and the sidebar's preview
# says "[spoiler]" rather than what it hides. A click reveals the first, and a
# second click covers it again; the labelled block reveals the same way. From
# the keyboard, the message's menu (Ctrl+Down, Shift+F10) shows them all on
# Enter. A spoiler at the start of a line and one in a table's cell are drawn
# covered, revealed and covered again the same way, the cell's row reading
# "[spoiler]" meanwhile; a quote stays a quote, an unclosed `>!` is one, and
# `||text||` is no spoiler in a line or in a row. A cell past a table's columns,
# a table past those drawn as grids and a /me action hide theirs too, and the
# composer's Quote button keeps a spoiler whole. Its own daemon on its own port,
# wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9690}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-spoiler}"
export OC_TEST_WS="${OC_TEST_WS:-Spoiler Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="$OC_TEST_DIR-dump.txt"
snap() { "$DRIVE" dump spoiler 2>/dev/null | tail -n +2 > "$D"; }
spl() { grep "^  spoiler .* k=$1 " "$D" | head -1 | grep -o "\b$2=[^ ]*" | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
click_rect() {  # click the centre of an l,t,r,b rect; refuses an empty one
  local l t r b; IFS=, read -r l t r b <<< "$1"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt "${l%.*}" ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== a message with two spoilers"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" sendraw 'The killer is >!the butler!< of course.\n```spoiler Ending\nShe lives.\n```' >/dev/null
covered() { snap; [ "$(grep -m1 '^spoilers ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)" = 2 ] &&
            [ "$(spl 0 shown)" = 0 ] && [ "$(spl 1 shown)" = 0 ]; }
check "both are drawn covered" waitfor 10 covered
check "each is a button for assistive technology" \
  bash -c "grep -q '^a11yitem message\.[0-9]*\.spoiler\.0 ' '$D' && grep -q '^a11yitem message\.[0-9]*\.spoiler\.1 ' '$D'"
check "the preview names it and gives nothing away" grep -q '^  ch 1 .*prev="The killer is \[spoiler\] of course' "$D"
check "and nothing anywhere says what it hides" bash -c "! grep -q 'prev=\"[^\"]*butler' '$D'"
"$DRIVE" shot spoiler_covered >/dev/null

say "== revealed, and covered again"
click_rect "$(spl 0 r)"
shown0() { snap; [ "$(spl 0 shown)" = 1 ] && [ "$(spl 1 shown)" = 0 ]; }
check "a click reveals the first, and only it" waitfor 5 shown0
"$DRIVE" shot spoiler_shown >/dev/null
click_rect "$(spl 0 r)"
hidden0() { snap; [ "$(spl 0 shown)" = 0 ]; }
check "a second click covers it again" waitfor 5 hidden0
click_rect "$(spl 1 r)"
shown1() { snap; [ "$(spl 1 shown)" = 1 ]; }
check "the labelled block reveals the same way" waitfor 5 shown1

say "== from the keyboard"
kb() { snap; grep -m1 '^kbfocus ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
"$DRIVE" move 20 20 >/dev/null
"$DRIVE" key ctrl+down >/dev/null           # onto the newest message: this one
"$DRIVE" key shift+f10 >/dev/null           # its actions
found=0
for _ in $(seq 1 20); do
  [ "$(kb menuhovercmd)" = 110 ] && { found=1; break; }
  "$DRIVE" key down >/dev/null
done
check "the message's menu offers its spoilers to the keyboard" [ "$found" = 1 ]
"$DRIVE" key enter >/dev/null
# One was shown (the block): Enter takes them all to the other state of the
# first, which was covered -- so both show.
both() { snap; [ "$(spl 0 shown)" = 1 ] && [ "$(spl 1 shown)" = 1 ]; }
check "Enter on it shows every spoiler in the message" waitfor 5 both
say "== however many, and however long"
many=""; for i in $(seq 1 10); do many="${many}>!secret$i!< "; done
"$DRIVE" sendraw "$many" >/dev/null
tenth() { snap; MID="$(grep '^  spoiler mid=[0-9]* k=9 ' "$D" | head -1 | grep -o 'mid=[0-9]*' | cut -d= -f2)"; [ -n "$MID" ]; }
check "the tenth spoiler in a message is drawn" waitfor 10 tenth
check "and all ten are covered" [ "$(grep -c "^  spoiler mid=$MID k=[0-9]* shown=0 " "$D")" = 10 ]
blk='```spoiler Long\nline 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n| a | b |\n|---|---|\n| 1 | 2 |\n```'
"$DRIVE" sendraw "$blk" >/dev/null
eight() { snap; grep -q "^  spoiler mid=[0-9]* k=0 shown=0 lines=1[01] " "$D"; }
check "every line of a long block has its cover" waitfor 10 eight
BLK="$(grep -m1 "^  spoiler mid=[0-9]* k=0 shown=0 lines=1[01] " "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
check "and a table inside it is not drawn as a grid" bash -c "! grep -q '^  table msg=$BLK ' '$D'"
"$DRIVE" shot spoiler_many >/dev/null

# Each of these is its own message, the newest on the screen, so its spoilers
# are found by its id. `$1` is a word only that message holds in the dump.
newest_mid() {  # the id of the newest message drawn with at least one spoiler
  snap; grep '^  spoiler mid=' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2 | sort -n | tail -1
}
mspl() { grep "^  spoiler mid=$1 k=$2 " "$D" | head -1 | grep -o "\b$3=[^ ]*" | cut -d= -f2; }
send_new() {  # send_new <body>: sends it, waits for a spoiler in a newer message; sets NEW
  local before after t=0; before="$(newest_mid)"
  "$DRIVE" sendraw "$1" >/dev/null
  while [ "$t" -lt 40 ]; do
    after="$(newest_mid)"
    if [ -n "$after" ] && [ "$after" != "$before" ]; then NEW="$after"; return 0; fi
    sleep 0.25; t=$((t + 1))
  done
  NEW=""; return 1
}
cycle() {  # cycle <what>: covered, revealed by a click, and covered again
  local what="$1" r
  is() { snap; [ "$(mspl "$NEW" 0 shown)" = "$1" ]; }
  check "$what: drawn covered" is 0
  check "$what: a button for assistive technology" grep -q "^a11yitem message\.$NEW\.spoiler\.0 " "$D"
  r="$(mspl "$NEW" 0 r)"
  click_rect "$r"
  check "$what: a click reveals it" waitfor 5 is 1
  click_rect "$(mspl "$NEW" 0 r)"
  check "$what: a second click covers it" waitfor 5 is 0
}

say "== at the start of a line"
send_new '>!LINESTART!< and then some'
check "a line beginning with one sends a spoiler" [ -n "$NEW" ]
START_MID="$NEW"
cycle "at a line's start"
check "and the preview gives nothing away" bash -c "grep -q '^  ch 1 .*prev=\"\[spoiler\] and then some' '$D' && ! grep -q 'prev=\"[^\"]*LINESTART' '$D'"
"$DRIVE" shot spoiler_line_start >/dev/null

say "== in a table's cell"
send_new '| Who | Note | Old |\n|---|---|---|\n| >!CELLHIDDEN!< | plain | z |\n| ||TABLELEAK|| |'
check "a table with one in a cell sends a spoiler" [ -n "$NEW" ]
cycle "in a cell"
snap
check "the table is a grid" grep -q "^  table msg=$NEW i=0 rows=2 cols=3 " "$D"
check "the cell's row reads [spoiler] while it is covered" \
  grep -qF "tcell msg=$NEW i=0 row=1 col=0 text=\"[spoiler]\"" "$D"
check "and no cell reads what it hides" bash -c "! grep -q 'tcell .*CELLHIDDEN' '$D'"
check "the cell's plain neighbour is unaffected" grep -qF "tcell msg=$NEW i=0 row=1 col=1 text=\"plain\"" "$D"
check "the sidebar's preview is the table's line" bash -c "grep -q '^  ch 1 .*prev=\"Table: 2 rows' '$D'"
"$DRIVE" shot spoiler_cell_covered >/dev/null
click_rect "$(mspl "$NEW" 0 r)"
revealed() { snap; [ "$(mspl "$NEW" 0 shown)" = 1 ] && grep -qF "tcell msg=$NEW i=0 row=1 col=0 text=\"CELLHIDDEN\"" "$D"; }
check "revealed, the cell reads what it hid" waitfor 5 revealed
"$DRIVE" shot spoiler_cell_shown >/dev/null
click_rect "$(mspl "$NEW" 0 r)"
is0() { snap; [ "$(mspl "$NEW" 0 shown)" = 0 ] && ! grep -q 'tcell .*CELLHIDDEN' "$D"; }
check "covered again, no cell reads it" waitfor 5 is0
# The old form in a row is cells: two empty and one of ordinary text, which is
# why it could never hide anything, and why this form replaced it.
check "the old form in a row is cells: empty" grep -qF "tcell msg=$NEW i=0 row=2 col=0 text=\"\"" "$D"
check "and ordinary text" grep -qF "tcell msg=$NEW i=0 row=2 col=2 text=\"TABLELEAK\"" "$D"

say "== by the keyboard, in a cell"
menu_on_new() {  # focus the newest message, open its menu, stop on "Show spoilers"
  local n
  "$DRIVE" move 20 20 >/dev/null
  for n in $(seq 1 20); do [ "$(kb mid)" = "$NEW" ] && break; "$DRIVE" key ctrl+down >/dev/null; done
  "$DRIVE" key shift+f10 >/dev/null
  for n in $(seq 1 20); do
    [ "$(kb menuhovercmd)" = 110 ] && return 0
    "$DRIVE" key down >/dev/null
  done
  return 1
}
check "the message's menu offers it" menu_on_new
"$DRIVE" key enter >/dev/null
cellkb() { snap; [ "$(mspl "$NEW" 0 shown)" = 1 ]; }
check "Enter on it shows the cell's spoiler" waitfor 5 cellkb
check "and the menu again" menu_on_new
"$DRIVE" key enter >/dev/null
cellkb0() { snap; [ "$(mspl "$NEW" 0 shown)" = 0 ]; }
check "covers it again" waitfor 5 cellkb0

say "== what is not one, and what holds one"
# The body as the dialect read it; the newest message of those drawn by the word.
marks() {  # marks <mid> <quotes> <spoilers>
  snap; grep -q "^  marks msg=$1 quotes=$2 spoilers=$3\$" "$D"
}
sent_mid() {  # the newest message the dump lists
  snap; grep '^  marks msg=' "$D" | grep -o 'msg=[0-9]*' | cut -d= -f2 | sort -n | tail -1
}
send_plain() {  # send_plain <body> <preview starts>: sends it, waits for the preview; sets LAST
  "$DRIVE" sendraw "$1" >/dev/null
  shown_prev() { snap; grep -qF "prev=\"$PREV" "$D"; }
  PREV="$2"
  waitfor 10 shown_prev || return 1
  LAST="$(sent_mid)"
}
check "a block quote is sent" send_plain '> PLAINQUOTE stays a quote' '> PLAINQUOTE stays a quote'
check "it stays a quote, with no spoiler" marks "$LAST" 1 0
"$DRIVE" shot spoiler_quote >/dev/null
check "an unclosed one at a line's start is sent" send_plain '>!UNCLOSED stays a quote' '>!UNCLOSED stays a quote'
check "it stays a quote too, and hides nothing" marks "$LAST" 1 0
check "the old form in a line is sent" send_plain '||OLDFORM|| is ordinary text' '||OLDFORM|| is ordinary text'
check "it is ordinary text, and hides nothing" marks "$LAST" 0 0
check "a quote holding one is sent" send_plain '> >!INQUOTE!< said it' '> [spoiler] said it'
check "it is a spoiler inside a quote" marks "$LAST" 1 1
check "and at a line's start, it was a spoiler and no quote" marks "$START_MID" 0 1
"$DRIVE" shot spoiler_plain >/dev/null

say "== a cell past the table's columns"
check "a row with an extra cell is sent" send_plain '| a | b |\n|---|---|\n| x | y | >!EXTRACELL!< |' 'Table: 1 row'
check "the extra cell's spoiler is counted" marks "$LAST" 0 1
check "and the grid does not read it" bash -c "! grep -q 'tcell .*EXTRACELL' '$D'"
NEW="$LAST"
# The grid never draws that cell, so a "Show spoilers" here would change nothing.
if menu_on_new; then offered=1; else offered=0; fi
check "its menu offers no Show spoilers, having nothing to show" [ "$offered" = 0 ]
"$DRIVE" key escape >/dev/null

say "== in a table past those drawn as grids"
b=""; for i in $(seq 1 16); do b="$b| h$i |\n|---|\n| $i |\n\n"; done
"$DRIVE" sendraw "$b| a | b |\n|---|---|\n| x | y | >!PASTGRIDS!< |" >/dev/null
cut16() { snap; MID="$(grep '^  marks msg=' "$D" | grep -o 'msg=[0-9]*' | cut -d= -f2 | sort -n | tail -1)";
          [ "$MID" != "$LAST" ] && grep -q "^  marks msg=$MID quotes=0 spoilers=1\$" "$D" && grep -q "^  cutmsg mid=$MID " "$D"; }
check "seventeen tables are drawn cut" waitfor 10 cut16
click_rect "$(grep -m1 "^  cutmsg mid=$MID " "$D" | grep -o 'r=[-0-9,]*' | cut -d= -f2)"
NEW="$MID"
past() { snap; [ "$(mspl "$NEW" 0 shown)" = 0 ]; }
check "opened, the seventeenth's extra cell is covered, as text" waitfor 5 past
"$DRIVE" shot spoiler_past_grids >/dev/null
click_rect "$(mspl "$NEW" 0 r)"
past1() { snap; [ "$(mspl "$NEW" 0 shown)" = 1 ]; }
check "and a click reveals it" waitfor 5 past1
click_rect "$(mspl "$NEW" 0 r)"
check "and a second covers it" waitfor 5 past

say "== in a /me action"
send_new '/me >!ACTIONHIDDEN!< waves'
check "an action with one sends a spoiler" [ -n "$NEW" ]
cycle "in an action"
check "the preview hides it" bash -c "! grep -q 'prev=\"[^\"]*ACTIONHIDDEN' '$D'"
check "its menu offers it" menu_on_new
"$DRIVE" key enter >/dev/null
act1() { snap; [ "$(mspl "$NEW" 0 shown)" = 1 ]; }
check "and Enter shows it" waitfor 5 act1
"$DRIVE" shot spoiler_action >/dev/null
click_rect "$(mspl "$NEW" 0 r)"
act0() { snap; [ "$(mspl "$NEW" 0 shown)" = 0 ]; }
check "and it covers again" waitfor 5 act0

say "== the composer's Quote button"
fmt_quote() {  # click the fifth button of the formatting row: B I S code quote
  snap
  local rect; rect="$(grep -m1 '^fmtbar ' "$D" | cut -d' ' -f3- | cut -d' ' -f5)"
  click_rect "$rect"
}
draft_is() { snap; grep -qF "text=\"$1\"" <(grep '^ed len=' "$D"); }
clear_draft() { "$DRIVE" key ctrl+a >/dev/null; "$DRIVE" key delete >/dev/null; }
"$DRIVE" move 20 20 >/dev/null
"$DRIVE" key escape >/dev/null
"$DRIVE" typekeys '>!QUOTEBTN!<' >/dev/null
check "the spoiler is in the draft" waitfor 5 draft_is '>!QUOTEBTN!<'
fmt_quote
check "Quote quotes it, whole" waitfor 5 draft_is '> >!QUOTEBTN!<'
fmt_quote
check "and again takes off the marker it added, and not its >" waitfor 5 draft_is '>!QUOTEBTN!<'
"$DRIVE" key enter >/dev/null
sentq() { snap; L2="$(sent_mid)"; [ "$L2" != "$NEW" ] && grep -q "^  marks msg=$L2 quotes=0 spoilers=1\$" "$D"; }
check "sent, it is still a spoiler, hiding what it hid" waitfor 10 sentq
"$DRIVE" typekeys 'PLAINLINE' >/dev/null
fmt_quote
check "a plain line gets the marker" waitfor 5 draft_is '> PLAINLINE'
fmt_quote
check "and loses it" waitfor 5 draft_is 'PLAINLINE'
fmt_quote
"$DRIVE" key end >/dev/null
clear_draft
"$DRIVE" typekeys '>!UNCLOSEDQ' >/dev/null
fmt_quote
check "an unclosed one is a quote, and its marker comes off" waitfor 5 draft_is '!UNCLOSEDQ'
clear_draft

say "== Shift+Enter inside one in the composer"
"$DRIVE" typekeys '>!SHIFTAB!<' >/dev/null
check "the spoiler is in the draft" waitfor 5 draft_is '>!SHIFTAB!<'
"$DRIVE" key left >/dev/null; "$DRIVE" key left >/dev/null
"$DRIVE" key shift+enter >/dev/null
split() { snap; grep -q '^ed len=.*text=">!SHIFTA!<$' "$D" && grep -q '^>!B!<"$' "$D"; }
check "it splits into two spoilers, hiding both halves" waitfor 5 split
"$DRIVE" key enter >/dev/null
sents() { snap; L3="$(sent_mid)"; [ "$L3" != "$L2" ] && grep -q "^  marks msg=$L3 quotes=0 spoilers=2\$" "$D"; }
check "sent, both are spoilers" waitfor 10 sents
"$DRIVE" typekeys '>!QUIETNL!<' >/dev/null
"$DRIVE" key left >/dev/null; "$DRIVE" key right >/dev/null
"$DRIVE" typekeys '\' >/dev/null
"$DRIVE" key enter >/dev/null
sentb() { snap; L4="$(sent_mid)"; [ "$L4" != "$L3" ] && grep -q "^  marks msg=$L4 quotes=0 spoilers=1\$" "$D"; }
check "a backslash typed at its end does not bring it into the open" waitfor 10 sentb
rm -f "$D"

say ""
say "gui_spoiler: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
