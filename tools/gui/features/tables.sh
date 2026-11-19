#!/usr/bin/env bash
# Markdown tables in the transcript (REQ-220, MARKDOWN.md §1a).
#
#   tools/gui/features/tables.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/tables.sh --keep   # ... and leave them running
#
# A pipe table in a message is drawn as a grid, not as its pipes, and the text
# around it is drawn as ever. It asserts, on both clients, that
#
#   - a table is drawn as a grid of its rows and columns, with its header row
#     and each column's alignment, and the text before and after it still drawn;
#   - a cell carries the body's formatting -- bold without its asterisks, a
#     labelled link by its label, an emoji -- and a `<br>` is a line break,
#     which makes its row taller than a row of one line;
#   - a table whose header cells are all empty has no header row;
#   - a table wider than the transcript scrolls across inside its own frame,
#     and only it does;
#   - a labelled link in a cell asks before it opens, as one in the text does;
#   - a selection across a table copies its Markdown, and Copy table (offered by
#     the message's menu) puts it on the clipboard as tab-separated values;
#   - the sidebar's preview and assistive technology name a table for its size;
#   - a long table lays out a bounded number of rows and says how many more;
#   - a table that is not one (its separator short of the header) is text;
#
# and, in the composer (MARKDOWN.md §6), that
#
#   - Insert table on the formatting toolbar opens the table editor: a column
#     count, the header row, each column's alignment, cells typed row by row
#     (Tab across, Shift+Enter a line break, a row added past the last), Markdown
#     and emoji kept as typed;
#   - Insert puts the table in the field as a card, never as its pipes, between
#     the lines around it; sent, it is the grid on both clients;
#   - editing the sent message brings the table back as a card, the card's Edit
#     reopens the editor filled in, and Save then the edit's send change the
#     grid on both clients;
#   - a card's Remove takes it out, and undo puts it back;
#   - pasting cells copied from a sheet asks: Table (Enter), Text, or nothing
#     (Esc); Image always, in a conversation -- the clipboard's own picture when
#     it has one, else the cells drawn as one -- and it goes to the upload tray; Table keeps a cell's link and bold from the clipboard's HTML;
#   - a draft keeps a card's table.
#
# Two clients over the pair (tools/gui/harness/win32/pair.sh, or tools/gui/harness/web/pair.sh on the
# web): alice sends the tables, bob reads them.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"

gui_begin tables 9550 /tmp/oc-tables "$@"
CH=1
CONF_LINK_OPEN=12     # the link confirmation's enum value, as the dump prints it

ack_a() { ack_of a "$@"; }                         # the word alice's client acks a verb with
# The newest message's id, in a dump.
newest() { grep '^  msgrow ' "$1" | tail -1 | grep -o 'mid=[0-9]*' | cut -d= -f2; }
tline() { field "$1" "^  table msg=$2 i=$3 " "$4"; }                     # tline <file> <mid> <i> <field>
tcell() { field "$1" "^    tcell msg=$2 i=$3 row=$4 col=$5 " "$6"; }    # tcell <file> <mid> <i> <row> <col> <field>
rect_h() { local l t r b; IFS=, read -r l t r b <<< "$1"; echo $(( b - t )); }
rect_l() { echo "${1%%,*}"; }
# The centre of rect "l,t,r,b": "x y".
mid_of() { local l t r b; IFS=, read -r l t r b <<< "$1"; echo "$(( (l + r) / 2 )) $(( (t + b) / 2 ))"; }

step "fixture: alice and bob in one channel"
gui_pair_up
for c in a b; do
  "$PAIR" $c size 1100 820 >/dev/null && "$PAIR" $c theme 1 >/dev/null && "$PAIR" $c channel $CH >/dev/null ||
    gui_abort "client $c did not take its size, theme and channel"
done

step "a table between two lines of text"
APPLE="$(printf '\xF0\x9F\x8D\x8E')"
"$PAIR" a sendraw "Before the table\n\n| Name | Qty | Note |\n|:--|:-:|--:|\n| *Apple* $APPLE | 3 | [the notes](https://example.com/notes) |\n| Pear | 12 | two<br>lines |\n\nAfter the table" >/dev/null
drawn() {  # drawn <a|b>: the newest message is a drawn table
  gui_snap "$1"; local f mid; f="$(dfile "$1")"; mid="$(newest "$f")"
  [ -n "$mid" ] && [ "$(tline "$f" "$mid" 0 rows)" = 2 ] && [ "$(tline "$f" "$mid" 0 r)" != 0,0,0,0 ]
}
check "alice's client draws it as a grid" waitfor 15 drawn a
M1="$(newest "$DA")"
check "bob's client draws it too" waitfor 15 drawn b
has() { grep -qF -- "$2" "$1"; }       # has <file> <fixed text>
for side in a b; do
  F="$(dfile $side)"
  check "($side) 2 rows, 3 columns, a header row, aligned left, centre, right" \
    has "$F" "  table msg=$M1 i=0 rows=2 cols=3 header=1 align=l,c,r "
  check "($side) the header's cells read Name, Qty, Note" \
    eval 'has "$F" "row=0 col=0 text=\"Name\"" && has "$F" "row=0 col=1 text=\"Qty\"" && has "$F" "row=0 col=2 text=\"Note\""'
  check "($side) a bold cell reads without its asterisks, its emoji kept" \
    has "$F" "    tcell msg=$M1 i=0 row=1 col=0 text=\"Apple $APPLE\""
  check "($side) a labelled link reads as its label" \
    has "$F" "    tcell msg=$M1 i=0 row=1 col=2 text=\"the notes\""
  check "($side) <br> is a line break" \
    has "$F" "    tcell msg=$M1 i=0 row=2 col=2 text=\"two\\nlines\""
  h1="$(rect_h "$(tcell "$F" "$M1" 0 1 1 r)")"; h2="$(rect_h "$(tcell "$F" "$M1" 0 2 1 r)")"
  check "($side) ...and its row is taller than a row of one line ($h2 > $h1)" [ "${h2:-0}" -gt "${h1:-0}" ]
  x0="$(rect_l "$(tcell "$F" "$M1" 0 0 0 r)")"; x1="$(rect_l "$(tcell "$F" "$M1" 0 0 1 r)")"
  x2="$(rect_l "$(tcell "$F" "$M1" 0 0 2 r)")"
  check "($side) the columns sit side by side, in order" [ "${x0:-0}" -lt "${x1:-0}" -a "${x1:-0}" -lt "${x2:-0}" ]
  IFS=, read -r _ ft _ fb <<< "$(tline "$F" "$M1" 0 r)"
  IFS=, read -r btop bbot <<< "$(tline "$F" "$M1" 0 body)"
  check "($side) the text before the table is drawn above it" [ $((ft - btop)) -ge 14 ]
  check "($side) ...and the text after it below it" [ $((bbot - fb)) -ge 14 ]
  check "($side) the table is published to assistive technology, row by row" \
    eval 'has "$F" "a11yitem message.$M1.table.0 " && has "$F" "a11yitem message.$M1.table.0.row.0 " && has "$F" "a11yitem message.$M1.table.0.row.2 "'
done
prev_b() { gui_snap b; tr '\n' ' ' < "$DB" | grep "  ch $CH .*prev=\"Before the table *Table: 2 rows × 3 columns *After the table\"" >/dev/null; }
check "bob's sidebar previews it as a table, not as pipes" waitfor 5 prev_b
"$PAIR" a shot tables_grid >/dev/null
"$PAIR" b shot tables_grid_b >/dev/null

step "a link in a cell asks before it opens"
# Where the link is NOW, each try: the transcript can still settle between a
# dump and the pointer (a read receipt's line arriving under the newest
# message moves every row up), so a miss is measured and tried again.
link_xy() {
  gui_snap a; local ll lt lr lb
  IFS=, read -r ll lt lr lb <<< "$(tcell "$DA" "$M1" 0 1 2 r)"
  # The column is right-aligned: the label ends at the cell's right padding.
  [ -n "${lb:-}" ] && echo "$(( lr - 20 )) $(( (lt + lb) / 2 ))"
}
hover_ok() {
  "$PAIR" a move $(link_xy) >/dev/null
  gui_snap a; grep -q '^link hover="https://example.com/notes" labelled=1 ' "$DA"
}
check "hovering it reports its address, labelled" waitfor 10 hover_ok
asked() { gui_snap a; grep -q "^confirm open=1 act=$CONF_LINK_OPEN url=\"https://example.com/notes\"" "$DA"; }
# A click, then a second to show the question, before it is measured again.
click_link() { asked && return 0; "$PAIR" a click $(link_xy) >/dev/null; waitfor 1 asked; }
check "clicking it asks first, naming the address" waitfor 10 click_link
"$PAIR" a shot tables_link_confirm >/dev/null
"$PAIR" a key esc >/dev/null
dismissed() { gui_snap a; grep -q '^confirm open=0 ' "$DA"; }
check "Esc dismisses it" waitfor 5 dismissed
"$PAIR" a move 20 20 >/dev/null

step "selecting across the table copies its Markdown"
gui_snap a
BODY="$(grep "^  msgrow .* mid=$M1 " "$DA" | grep -o 'body=[0-9]*,[0-9]*' | cut -d= -f2)"
IFS=, read -r bx by <<< "$BODY"
FR="$(tline "$DA" "$M1" 0 r)"
IFS=, read -r fl ft fr fb <<< "$FR"
"$PAIR" a drag $((bx + 1)) $((by + 8)) $((bx + 300)) $((fb + 30)) >/dev/null
"$PAIR" a key ctrl+c >/dev/null
copied_md() {
  gui_snap a
  grep -qF 'clipboard set="Before the table\n\n| Name | Qty | Note |\n|:--|:-:|--:|\n' "$DA" &&
    grep -qF 'After the table"' "$DA"
}
check "the selection copies the text and the table's Markdown between it" waitfor 5 copied_md
"$PAIR" a shot tables_selected >/dev/null
"$PAIR" a click $((bx + 1)) $((by + 8)) >/dev/null

step "Copy table"
MENU_PT="$(mid_of "$(tcell "$DA" "$M1" 0 1 1 r)")"
"$PAIR" a rclick $MENU_PT >/dev/null
offered() { gui_snap a; grep -q '^a11yitem menu.109 ' "$DA"; }
check "the message's menu offers Copy table" waitfor 5 offered
"$PAIR" a key esc >/dev/null
check "copytable is taken" [ "$(ack_a copytable "$M1" 0)" = ok ]
TSV='clipboard set="Name\tQty\tNote\r\nApple '"$APPLE"'\t3\tthe notes\r\nPear\t12\t\"two\nlines\""'
tsv_ok() { gui_snap a; grep -qF "$TSV" "$DA"; }
check "it puts the table on the clipboard as TSV, a line break quoted" waitfor 5 tsv_ok

step "a table with no header row"
"$PAIR" a sendraw "|  |  |\n|---|---|\n| a | b |\n| c | d |" >/dev/null
headless() { gui_snap b; local mid; mid="$(newest "$DB")"; [ "$mid" != "$M1" ] && [ "$(tline "$DB" "$mid" 0 header)" = 0 ] \
               && [ "$(tline "$DB" "$mid" 0 laid)" = 2 ] && [ "$(tline "$DB" "$mid" 0 rows)" = 2 ]; }
check "(b) it is drawn with its two rows and no header" waitfor 15 headless
M2="$(newest "$DB")"
first_row_first() { grep -q "^    tcell msg=$M2 i=0 row=1 col=0 text=\"a\"" "$DB" && ! grep -q "^    tcell msg=$M2 i=0 row=0 " "$DB"; }
check "(b) ...its first row the first drawn" first_row_first

step "a wide table scrolls across, inside its frame"
HDR="|"; SEP="|"; ROW="|"
for i in $(seq 1 12); do HDR="$HDR Column number $i |"; SEP="$SEP---|"; ROW="$ROW a fairly long value $i |"; done
"$PAIR" a sendraw "$HDR\n$SEP\n$ROW" >/dev/null
wide() {  # wide <a|b>
  gui_snap "$1"; local f mid max; f="$(dfile "$1")"; mid="$(newest "$f")"
  [ "$(tline "$f" "$mid" 0 cols)" = 12 ] || return 1
  max="$(tline "$f" "$mid" 0 scroll | cut -d/ -f2)"; [ "${max:-0}" -gt 0 ]
}
check "(a) twelve columns, wider than the transcript: it scrolls" waitfor 15 wide a
check "(b) ...on bob's client too" waitfor 15 wide b
M3="$(newest "$DA")"
FR3="$(tline "$DA" "$M3" 0 r)"
IFS=, read -r wl wt wr wb <<< "$FR3"
MSGX="$(grep -m1 '^msgrows ' "$DA" | grep -o 'x=[0-9]*\.\.[0-9]*' | cut -d. -f3)"
check "(a) its frame stays inside the transcript" [ "$wr" -le "${MSGX:-0}" ]
X0="$(rect_l "$(tcell "$DA" "$M3" 0 0 3 r)")"
"$PAIR" a shot tables_wide >/dev/null
for side in a b; do
  check "($side) tscroll is taken" [ "$(ack_of $side tscroll "$M3" 0 200)" = ok ]
done
scrolled() { gui_snap a; [ "$(tline "$DA" "$M3" 0 scroll | cut -d/ -f1)" = 200 ] && \
               [ "$(rect_l "$(tcell "$DA" "$M3" 0 0 3 r)")" -eq $((X0 - 200)) ]; }
check "(a) it moves the grid 200 across, and its cells with it" waitfor 5 scrolled
IFS=, read -r sl _ sr _ <<< "$(tline "$DA" "$M3" 0 r)"
check "(a) ...and the frame stays where it was" [ "$sl,$sr" = "$wl,$wr" ]
scrolled_b() { gui_snap b; [ "$(tline "$DB" "$M3" 0 scroll | cut -d/ -f1)" = 200 ]; }
check "(b) bob's moves too, by his own offset" waitfor 5 scrolled_b
"$PAIR" a tscroll "$M3" 0 100000 >/dev/null
at_end() { gui_snap a; local s; s="$(tline "$DA" "$M3" 0 scroll)"; [ "${s%/*}" = "${s#*/}" ]; }
check "(a) scrolling past the end stops at it" waitfor 5 at_end
"$PAIR" a shot tables_wide_scrolled >/dev/null

step "a long table is bounded"
LONG="| n | sq |\n|--:|--:|"
for i in $(seq 1 210); do LONG="$LONG\n| $i | $((i * i)) |"; done
"$PAIR" a sendraw "$LONG" >/dev/null
# Past 25 lines a message is drawn cut, its first lines and Show more
# (CLIENT.md "Long messages"), and the table in it as far as those go; the
# bound is the whole message's, so bob opens it first.
long_cut() { gui_snap b; ML="$(newest "$DB")"; [ "$(tline "$DB" "$ML" 0 rows)" = 210 ] \
               && grep -q "^  cutmsg mid=$ML kind=2 " "$DB"; }
check "(b) the long table's message arrives, cut, with Show more" waitfor 15 long_cut
"$PAIR" b click $(mid_of "$(field "$DB" "^  cutmsg mid=$ML " r)") >/dev/null
bounded() { gui_snap b; local mid; mid="$(newest "$DB")"; [ "$(tline "$DB" "$mid" 0 rows)" = 210 ] \
              && [ "$(tline "$DB" "$mid" 0 laid)" = 201 ] && [ "$(tline "$DB" "$mid" 0 more)" = 10 ]; }
check "(b) 210 rows: the header and 200 laid out, the other 10 said in a line" waitfor 15 bounded

step "a table that is not one is text"
M4="$(newest "$DB")"     # the long table, newest in bob's last look
"$PAIR" a sendraw "| a | b |\n| -- |\n| c | d |" >/dev/null
as_text() { gui_snap b; local mid; mid="$(newest "$DB")"; [ "$mid" != "$M4" ] && [ -n "$mid" ] \
              && ! grep -q "^  table msg=$mid " "$DB"; }
check "(b) its separator is short of its header: no grid, the message drawn as text" waitfor 15 as_text

step "Insert table opens the table editor"
PEAR="$(printf '\xF0\x9F\x8D\x90')"
"$PAIR" a channel $CH >/dev/null
"$PAIR" a type "Text before the table" >/dev/null
gui_snap a
check "Insert table is on the formatting toolbar" grep -q '^a11yitem composer.format.table ' "$DA"
check "tablenew is taken" [ "$(ack_a tablenew)" = ok ]
gui_snap a
check "a new table: three columns, two rows and a header row" \
  has "$DA" "tableeditor open=1 mode=new slot=-1 cols=3 rows=2 header=1 focus=0,0 "
"$PAIR" a tablecols 5 >/dev/null; gui_snap a; c5="$(field "$DA" '^tableeditor ' cols)"
"$PAIR" a tablecols 3 >/dev/null; gui_snap a; c3="$(field "$DA" '^tableeditor ' cols)"
check "the column count adds columns and takes them away ($c5, then $c3)" [ "$c5" = 5 -a "$c3" = 3 ]
"$PAIR" a tableheader 0 >/dev/null; gui_snap a; h0="$(field "$DA" '^tableeditor ' header)"
"$PAIR" a tableheader 1 >/dev/null; gui_snap a; h1="$(field "$DA" '^tableeditor ' header)"
check "the header row turns off and on ($h0, then $h1)" [ "$h0" = 0 -a "$h1" = 1 ]
for k in 0:l 1:c 2:r; do "$PAIR" a tablealign "${k%:*}" "${k#*:}" >/dev/null; done
"$PAIR" a tablecell 0 0 Item >/dev/null
"$PAIR" a tablecell 0 1 Qty >/dev/null
"$PAIR" a tablecell 0 2 Note >/dev/null
"$PAIR" a tablecell 1 0 "*Pear* $PEAR" >/dev/null
"$PAIR" a tablecell 1 1 4 >/dev/null
"$PAIR" a tablecell 1 2 'first\nsecond' >/dev/null
"$PAIR" a tablecell 2 0 Fig >/dev/null
"$PAIR" a tablecell 2 1 12 >/dev/null
"$PAIR" a tablecell 2 2 "[the notes](https://example.com/notes)" >/dev/null
gui_snap a
check "each column takes its alignment" has "$DA" " align=l,c,r fieldfocus=1 "
check "a cell keeps its Markdown and emoji as typed" grep -qE "^  tecell row=1 col=0 r=[0-9,]+ text=\"\\*Pear\\* $PEAR\"" "$DA"
check "a cell holds a line break" grep -qF 'text="first\nsecond"' "$DA"
"$PAIR" a key tab >/dev/null        # the last cell: Tab goes on into a new row
gui_snap a
check "Tab past the last cell adds a row and moves into it" has "$DA" " rows=3 header=1 focus=3,0 "
"$PAIR" a typekeys "x" >/dev/null
"$PAIR" a key shift+enter >/dev/null
"$PAIR" a typekeys "y" >/dev/null
gui_snap a
check "Shift+Enter is a line break inside the cell" grep -qF '  tecell row=3 col=0 ' "$DA"
check "...the cell reads x, a line break, y" grep -qF 'text="x\ny"' "$DA"
"$PAIR" a tablermrow 3 >/dev/null
gui_snap a
check "a row is removed, and the row above it takes the keys" has "$DA" " rows=2 header=1 focus=2,0 "
"$PAIR" a key shift+tab >/dev/null
gui_snap a
check "Shift+Tab from a row's first cell goes back to the end of the row above" has "$DA" " rows=2 header=1 focus=1,2 "
"$PAIR" a shot tables_editor_new >/dev/null
check "Insert is taken" [ "$(ack_a tableok)" = ok ]
gui_snap a
check "the editor closes" has "$DA" "tableeditor open=0 "
check "the table is in the composer as a card" has "$DA" "composer tables=1 "
check "...three columns, two rows, a header, aligned left, centre, right" \
  grep -qE '^  ctable slot=[0-9]+ at=[0-9]+ rows=2 cols=3 header=1 align=l,c,r r=[1-9]' "$DA"
check "...its Markdown is the table as typed" \
  grep -qF "md=\"| Item | Qty | Note |\n| :--- | :---: | ---: |\n| *Pear* $PEAR | 4 | first<br>second |\n| Fig | 12 | [the notes](https://example.com/notes) |\"" "$DA"
"$PAIR" a key shift+enter >/dev/null
"$PAIR" a typekeys "Text after the table" >/dev/null
gui_snap a
check "the composer shows the card between the two lines, and no pipes" \
  has "$DA" 'composer tables=1 text="Text before the table\n[table]\nText after the table"'
IFS=, read -r _ ct _ cb <<< "$(field "$DA" '^  ctable ' r)"
cr_ok() { local t b; IFS=, read -r _ t _ b <<< "$(field "$DA" '^  ctable ' edit)"; [ "${b:-0}" -gt 0 ] && [ "$t" -ge "${ct:-0}" ]; }
check "the card's Edit and Remove are drawn on it" cr_ok
check "...and published to assistive technology" grep -qE '^a11yitem composer\.table\.[0-9]+\.(edit|remove) ' "$DA"
"$PAIR" a shot tables_composer_card >/dev/null

step "sent, it is a grid"
"$PAIR" a key enter >/dev/null
sent_grid() {  # sent_grid <a|b>
  gui_snap "$1"; local f mid; f="$(dfile "$1")"; mid="$(newest "$f")"
  [ -n "$mid" ] && grep -q "^  table msg=$mid i=0 rows=2 cols=3 header=1 align=l,c,r " "$f"
}
check "(a) the message is drawn with the grid" waitfor 15 sent_grid a
check "(b) ...and on bob's client" waitfor 15 sent_grid b
M5="$(newest "$DB")"
for side in a b; do
  F="$(dfile $side)"
  check "($side) its cells read as they will: bold without asterisks, the emoji, a line break, a link's label" \
    eval 'has "$F" "    tcell msg=$M5 i=0 row=1 col=0 text=\"Pear $PEAR\"" && has "$F" "    tcell msg=$M5 i=0 row=1 col=2 text=\"first\\nsecond\"" && has "$F" "    tcell msg=$M5 i=0 row=2 col=2 text=\"the notes\""'
  IFS=, read -r _ ft _ fb <<< "$(tline "$F" "$M5" 0 r)"
  IFS=, read -r btop bbot <<< "$(tline "$F" "$M5" 0 body)"
  check "($side) the text before it is above it, the text after below" [ $((ft - btop)) -ge 14 -a $((bbot - fb)) -ge 14 ]
done
gui_snap a
check "the composer is empty again" has "$DA" 'composer tables=0 text=""'

step "editing the message brings the table back as a card"
check "editmsg is taken" [ "$(ack_a editmsg 0)" = ok ]
gui_snap a
check "the composer holds the text and the table as a card" \
  has "$DA" 'composer tables=1 text="Text before the table\n[table]\nText after the table"'
check "...the card's table as it was sent" grep -qF '| Fig | 12 | [the notes](https://example.com/notes) |"' "$DA"
check "the card's Edit opens the editor" [ "$(ack_a tableedit)" = ok ]
gui_snap a
check "...filled in, as the card's table" \
  eval 'has "$DA" "tableeditor open=1 mode=edit " && has "$DA" " cols=3 rows=2 header=1 " && grep -qF "  tecell row=2 col=1 " "$DA" && grep -qF "text=\"12\"" "$DA"'
"$PAIR" a theme 0 >/dev/null
"$PAIR" a shot tables_editor_edit_dark >/dev/null
"$PAIR" a theme 1 >/dev/null
"$PAIR" a tablecell 2 1 13 >/dev/null
check "Save is taken" [ "$(ack_a tableok)" = ok ]
gui_snap a
check "the card has the change" grep -qF '| Fig | 13 | [the notes]' "$DA"
"$PAIR" a key enter >/dev/null
edited() { gui_snap "$1"; grep -q "^    tcell msg=$M5 i=0 row=2 col=1 text=\"13\"" "$(dfile "$1")"; }
check "(a) the edit's send changes the grid" waitfor 15 edited a
check "(b) ...on bob's client too" waitfor 15 edited b

step "a card's Remove, and undo"
"$PAIR" a type "keep" >/dev/null
"$PAIR" a tablenew >/dev/null
"$PAIR" a tablecell 0 0 A >/dev/null
"$PAIR" a tableok >/dev/null
gui_snap a
RM="$(field "$DA" '^  ctable ' remove)"
"$PAIR" a click $(mid_of "$RM") >/dev/null
gui_snap a
check "clicking Remove takes the card out" has "$DA" 'composer tables=0 text="keep"'
"$PAIR" a key ctrl+z >/dev/null
gui_snap a
check "...and Ctrl+Z puts it back" has "$DA" 'composer tables=1 text="keep\n[table]"'
"$PAIR" a type "" >/dev/null

step "pasting cells from a sheet asks"
"$PAIR" a clipset 'Name\tScore\nAnn\t10\nBo\t7' >/dev/null
"$PAIR" a paste >/dev/null
gui_snap a
check "the chooser opens at the caret, for three rows of two cells" has "$DA" "pastechoice open=1 focus=table rows=3 cols=2 "
check "...offering Table, Image and Text" grep -qE '^pastechoice .* table=[1-9][0-9]*,[0-9]+,[0-9]+,[0-9]+ image=[1-9][0-9]*,[0-9]+,[0-9]+,[0-9]+ text=[1-9]' "$DA"
"$PAIR" a shot tables_paste_choice >/dev/null
"$PAIR" a key enter >/dev/null
gui_snap a
check "Enter pastes a table: a card" has "$DA" 'composer tables=1 text="[table]"'
check "...of the cells" grep -qF 'md="| Name | Score |\n| --- | --- |\n| Ann | 10 |\n| Bo | 7 |"' "$DA"
"$PAIR" a type "" >/dev/null
"$PAIR" a paste >/dev/null
check "Text is taken" [ "$(ack_a pastechoose text)" = ok ]
gui_snap a
check "Text pastes the cells as they came, tabs and all" has "$DA" 'composer tables=0 text="Name\tScore\nAnn\t10\nBo\t7"'
"$PAIR" a type "" >/dev/null
"$PAIR" a paste >/dev/null
"$PAIR" a key esc >/dev/null
gui_snap a
check "Esc pastes nothing" eval 'has "$DA" "pastechoice open=0 " && has "$DA" "composer tables=0 text=\"\""'
# A sheet in a browser puts no picture on the clipboard: Image draws the cells.
"$PAIR" a paste >/dev/null
check "Image is taken with no picture on the clipboard" [ "$(ack_a pastechoose image)" = ok ]
pic_drawn() { gui_snap a; grep -qE '^  fchip 0 name="pasted-table-[0-9]{8}-[0-9]{6}\.png" here=1 pic=1 ' "$DA"; }
check "...the cells drawn as a picture, in the upload tray" waitfor 15 pic_drawn
"$PAIR" a shot tables_drawn_image >/dev/null
"$PAIR" a click $(mid_of "$(field "$DA" '^  fchip 0 ' x)") >/dev/null
gone_chip() { gui_snap a; ! grep -q '^  fchip 0 ' "$DA"; }
waitfor 10 gone_chip || gui_abort "the drawn picture's chip did not go"
"$PAIR" a cliphtml '<table><tr><td><b>Name</b></td><td>Score</td></tr><tr><td>Ann</td><td><a href="https://example.com/ann">10</a></td></tr><tr><td>Bo</td><td>7</td></tr></table>' >/dev/null
"$PAIR" a clippicture 1 >/dev/null
"$PAIR" a paste >/dev/null
gui_snap a
check "with a picture on the clipboard too, Image is offered" grep -qE '^pastechoice open=1 .* image=[1-9]' "$DA"
"$PAIR" a theme 0 >/dev/null
"$PAIR" a shot tables_paste_choice_dark >/dev/null
"$PAIR" a theme 1 >/dev/null
check "Table is taken" [ "$(ack_a pastechoose table)" = ok ]
gui_snap a
check "Table keeps the bold and the link the clipboard's HTML has" \
  grep -qF 'md="| *Name* | Score |\n| --- | --- |\n| Ann | [10](https://example.com/ann) |\n| Bo | 7 |"' "$DA"
"$PAIR" a type "" >/dev/null
"$PAIR" a paste >/dev/null
check "Image is taken" [ "$(ack_a pastechoose image)" = ok ]
gui_snap a
check "Image puts the picture in the upload tray, as a pasted table" \
  grep -qE '^  fchip 0 name="pasted-table-[0-9]{8}-[0-9]{6}\.png" ' "$DA"
"$PAIR" a click $(mid_of "$(field "$DA" '^  fchip 0 ' x)") >/dev/null
"$PAIR" a clipset "" >/dev/null

step "a draft keeps its table"
"$PAIR" a mkchan tdraft >/dev/null || gui_abort "alice's client did not take mkchan"
has_tdraft() { gui_snap a; grep -q '^  ch 2 ' "$DA"; }
waitfor 10 has_tdraft || gui_abort "the second conversation did not appear on alice's client"
"$PAIR" a channel $CH >/dev/null
"$PAIR" a type "draft words" >/dev/null
"$PAIR" a tablenew >/dev/null
"$PAIR" a tablecell 0 0 Kept >/dev/null
"$PAIR" a tablecell 1 0 "in the draft" >/dev/null
"$PAIR" a tableok >/dev/null
"$PAIR" a channel 2 >/dev/null
gui_snap a
check "leaving the conversation leaves an empty composer elsewhere" has "$DA" 'composer tables=0 text=""'
"$PAIR" a channel $CH >/dev/null
gui_snap a
check "coming back, the draft has the card again" has "$DA" 'composer tables=1 text="draft words\n[table]"'
check "...with its table" grep -qF '| Kept |  |  |\n| --- | --- | --- |\n| in the draft |' "$DA"
"$PAIR" a type "" >/dev/null
"$PAIR" a channel 2 >/dev/null
"$PAIR" a channel $CH >/dev/null

step "dark"
for c in a b; do "$PAIR" $c theme 0 >/dev/null; done
"$PAIR" a tscroll "$M3" 0 -100000 >/dev/null
"$PAIR" a shot tables_dark >/dev/null
"$PAIR" b shot tables_dark_b >/dev/null
for c in a b; do "$PAIR" $c theme 1 >/dev/null; done
gui_end
