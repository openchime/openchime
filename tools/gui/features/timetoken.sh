#!/usr/bin/env bash
# Time tokens (MARKDOWN.md §1c): a time written once, read in each reader's zone.
#
#   tools/gui/features/timetoken.sh          # run; the client and daemon stop at the end
#   tools/gui/features/timetoken.sh --keep   # ... and leave them running
#
# Insert a time picks tomorrow at 15:00 in the client's own zone and writes the
# token, in UTC, at the caret. Sent, the message shows it as tomorrow's day, date
# and 15:00 -- the reader's zone is the writer's here, so it reads as picked -- in a box
# of its own in the text, and hovering it shows the whole date with the zone.
# However many there are, every one is drawn so: forty in one message and twenty
# in one table cell, custom emoji among them, each in its own box.
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin timetoken 9545 /tmp/oc-timetoken "Time Fixture" "$@"

tt() { field "$D" '^timetoks ' "$1"; }
tok() { fieldq "$D" '^  timetok ' "$1"; }
tokr() { grep -m1 '^  timetok ' "$D" | grep -o ' r=[0-9,.-]*' | cut -d= -f2; }

step "Insert a time writes the token"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
published() { gui_snap; grep '^a11yitem composer.time ' "$D" >/dev/null; }
check "Insert a time is on the composer, and published" waitfor 5 published
TZM="$(tt tz)"
# Tomorrow, by the client's zone: the token it must write, and what it must read as.
read -r DAY TOKEN SHORT FULLP <<<"$(python3 - "$TZM" <<'PY'
import datetime as d, sys
tz = d.timezone(d.timedelta(minutes=int(sys.argv[1])))
now = d.datetime.now(tz)
day = (now + d.timedelta(days=1)).replace(hour=15, minute=0, second=0, microsecond=0)
utc = day.astimezone(d.timezone.utc)
short = day.strftime('%a') + ' ' + str(day.day) + ' ' + day.strftime('%b')
if day.year != now.year: short += ' ' + str(day.year)
print(day.strftime('%Y-%m-%d'), utc.strftime('<time:%Y-%m-%dT%H:%MZ>'), (short + ', 15:00').replace(' ', '_'),
      (day.strftime('%A') + ' ' + str(day.day) + ' ' + day.strftime('%B %Y') + ', 15:00').replace(' ', '_'))
PY
)"
SHORT="${SHORT//_/ }"; FULLP="${FULLP//_/ }"
[ -n "$TZM" ] || gui_abort "the dump gives no zone for the client (timetoks tz=)"
"$DRIVE" type "Standup at " >/dev/null || gui_abort "the client did not take type"
"$DRIVE" timeinsert "$DAY 15:00" >/dev/null || gui_abort "the client did not take timeinsert"
inserted() { gui_snap; grep -m1 '^ed ' "$D" | grep -F "text=\"Standup at $TOKEN\"" >/dev/null; }
check "the composer holds the token, in UTC ($TOKEN)" waitfor 5 inserted

step "sent, it reads in the reader's zone"
"$DRIVE" key enter >/dev/null
drawn() { gui_snap; [ "$(tt n)" -ge 1 ] && [ "$(tok text)" = "$SHORT" ]; }
check "the message shows \"$SHORT\"" waitfor 10 drawn
check "the token is gone from the text: the excerpt reads as the message does" \
  grep -q "^  ch 1 .*prev=\"Standup at $SHORT\"" "$D"
"$DRIVE" shot timetoken_sent >/dev/null

step "in a table's cell it reads in the reader's zone too"
"$DRIVE" sendraw "| When | What |\n|---|---|\n| $TOKEN | Standup |" >/dev/null || gui_abort "the client did not send"
in_cell() { gui_snap; [ "$(grep -c "^  timetok .*text=\"$SHORT\"" "$D")" -ge 2 ]; }
check "the cell shows \"$SHORT\", beside the message's" waitfor 10 in_cell
check "and the table is drawn as a grid, not as typed" grep -q '^  ch 1 .*prev="Table: 1 row' "$D"
"$DRIVE" shot timetoken_cell >/dev/null

step "hovering it shows the whole of it"
IFS=, read -r l t r b <<<"$(tokr)"
[ -n "${b:-}" ] || gui_abort "the dump gives no rectangle for the token"
"$DRIVE" move $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
tipped() { gui_snap; [ "$(tt tip)" = 0 ]; }
check "the pointer on it shows its tooltip" waitfor 5 tipped
in_zone() { case "$(tok full)" in "$FULLP (UTC"*) return 0 ;; esac; return 1; }
check "which says the date, the time and the zone (\"$(tok full)\")" in_zone
"$DRIVE" shot timetoken_tip >/dev/null

step "however many there are, every one reads in the reader's zone"
make_png "$LIN_DIR/tt-emoji.png" 32x32 f59e0b
"$DRIVE" emoji_add "ttmark $(cpath tt-emoji.png)" >/dev/null || gui_abort "the client did not take emoji_add"
registered() { gui_snap; grep '^cemoji=1$' "$D" >/dev/null; }
waitfor 30 registered || gui_abort "the custom emoji to mix in was not added"
# Forty tokens two days on and twenty three days on, a quarter-hour apart, so
# each reads differently; a custom emoji after every fourth. Written out with
# what each must read as in the client's zone and on its clock, in order.
M="$(mktemp -d)"
python3 - "$(tt tz)" "$(tt time24)" "$M" <<'PY'
import datetime as d, sys
tz = d.timezone(d.timedelta(minutes=int(sys.argv[1])))
h24, out = sys.argv[2] == '1', sys.argv[3]
now = d.datetime.now(tz)
def short(t):
    s = t.strftime('%a') + ' ' + str(t.day) + ' ' + t.strftime('%b')
    if t.year != now.year: s += ' ' + str(t.year)
    if h24: return s + ', %02d:%02d' % (t.hour, t.minute)
    return s + ', %d:%02d %s' % (t.hour % 12 or 12, t.minute, 'AM' if t.hour < 12 else 'PM')
def run(name, days, n):
    start = (now + d.timedelta(days=days)).replace(hour=8, minute=0, second=0, microsecond=0)
    text, want = [], []
    for k in range(n):
        t = start + d.timedelta(minutes=15 * k)
        text.append(t.astimezone(d.timezone.utc).strftime('<time:%Y-%m-%dT%H:%MZ>'))
        if k % 4 == 3: text.append(':ttmark:')
        want.append(short(t))
    open(out + '/' + name + '.txt', 'w').write(' '.join(text))
    open(out + '/' + name + '.want', 'w').write('\n'.join(want) + '\n')
run('body', 2, 40)
run('cell', 3, 20)
PY
# Is there a message whose tokens, as drawn, read exactly as <want>, in order?
drawn_as() {  # drawn_as <want file>
  gui_snap
  python3 - "$D" "$1" <<'PY'
import re, sys
want = open(sys.argv[2]).read().split('\n')[:-1]
got = {}
for ln in open(sys.argv[1]):
    m = re.match(r'  timetok mid=(\d+) .*?text="([^"]*)"', ln)
    if m: got.setdefault(m.group(1), []).append(m.group(2))
sys.exit(0 if want in got.values() else 1)
PY
}
emoji_drawn() { gui_snap; [ "$(tt emoji)" = "$1" ]; }
"$DRIVE" sendraw "Many: $(cat "$M/body.txt")" >/dev/null || gui_abort "the client did not send"
check "forty in one message all read in the reader's zone, in order" waitfor 15 drawn_as "$M/body.want"
check "and the ten custom emoji among them are drawn" waitfor 10 emoji_drawn 10
"$DRIVE" shot timetoken_many >/dev/null
"$DRIVE" sendraw "| When | What |\n|---|---|\n| $(cat "$M/cell.txt") | Many |" >/dev/null || gui_abort "the client did not send"
check "twenty in one table cell all read in the reader's zone, in order" waitfor 15 drawn_as "$M/cell.want"
check "and its five custom emoji are drawn, beside the message's ten" waitfor 10 emoji_drawn 15
"$DRIVE" shot timetoken_many_cell >/dev/null
rm -rf "$M" "$LIN_DIR/tt-emoji.png"

step "in a table past those drawn as grids, and in a /me line"
# Each drawn token is a hit with its tip (the dump's timetok line), so a new one
# reading "$SHORT" is one shown in the reader's zone, with its tip.
count_short() { gui_snap; grep -c "^  timetok .*text=\"$SHORT\"" "$D"; }
# By the message it is on, not a count of those on screen: a message of
# seventeen tables is tall, and sending it scrolls earlier tokens out of view.
newest_mid() { gui_snap; grep '^  msgrow ' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2 | sort -n | tail -1; }
before_mid="$(newest_mid)"
b=""; for i in $(seq 1 16); do b="$b| h$i |\n|---|\n| $i |\n\n"; done
"$DRIVE" sendraw "$b| When |\n|---|\n| $TOKEN |" >/dev/null || gui_abort "the client did not send"
# A message that long is drawn cut, its seventeenth table behind "Show more":
# opened, as a reader opens it.
cut_new() { gui_snap; NEWM="$(newest_mid)"; [ "${NEWM:-0}" -gt "${before_mid:-0}" ] && grep -q "^  cutmsg mid=$NEWM " "$D"; }
if waitfor 10 cut_new; then
  click_rect "$(grep -m1 "^  cutmsg mid=$NEWM " "$D" | grep -o 'r=[-0-9,]*' | cut -d= -f2)"
fi
past16() {
  gui_snap
  local m
  for m in $(grep "^  timetok .*text=\"$SHORT\"" "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2); do
    [ "$m" -gt "${before_mid:-0}" ] && return 0
  done
  return 1
}
check "a token in the seventeenth table reads \"$SHORT\", with its tip" waitfor 15 past16
"$DRIVE" shot timetoken_past_grids >/dev/null
before="$(count_short)"
"$DRIVE" sendraw "/me starts at $TOKEN" >/dev/null || gui_abort "the client did not send"
in_me() { [ "$(count_short)" -gt "$before" ]; }
check "a token in a /me line reads \"$SHORT\", with its tip" waitfor 10 in_me
check "and the /me line's preview reads it too" grep -q "prev=\"[^\"]*starts at $SHORT\"" "$D"
"$DRIVE" shot timetoken_action >/dev/null

step "the clock setting, changed, changes both"
ME_MID="$(newest_mid)"
# Preferences > Messages, then the Time format row's choice, as a person clicks it.
# The choice is clicked once Preferences has drawn it, and kept with Save:
# Escape is Cancel, which puts the sheet's values back -- so the 12-hour
# clock was never on, and "back on 24 hours" held because nothing had moved.
set_clock() {  # set_clock <0 12-hour | 1 24-hour>
  "$DRIVE" prefs 1 >/dev/null || gui_abort "the client did not open Preferences"
  row_drawn() { gui_snap; grep "^  prefhit row=1 val=$1 " "$D" >/dev/null; }
  waitfor 5 row_drawn "$1" || gui_abort "Preferences drew no Time format choice $1"
  click_rect "$(grep -m1 "^  prefhit row=1 val=$1 " "$D" | grep -o 'r=[-0-9,]*' | cut -d= -f2)" ||
    gui_abort "the dump gives no rectangle for Time format choice $1"
  chosen() { gui_snap; grep "^modal=prefs .*time24=$1 " "$D" >/dev/null; }
  waitfor 5 chosen "$1" || gui_abort "the sheet did not take Time format choice $1"
  press modal.button.save || gui_abort "Preferences has no Save to press"
  saved() { gui_snap; ! grep '^modal=prefs ' "$D" >/dev/null && [ "$(tt time24)" = "$1" ]; }
  waitfor 5 saved "$1" || gui_abort "Save did not close Preferences with time24=$1"
}
reads() {  # reads <pattern>: the seventeenth table's token and the /me line's both match
  gui_snap
  local m
  for m in "$NEWM" "$ME_MID"; do grep "^  timetok mid=$m .*text=\"$1\"" "$D" >/dev/null || return 1; done
}
set_clock 0
twelve() { reads '[^"]*[0-9] [AP]M'; }
check "on a 12-hour clock, both read in it" waitfor 10 twelve
set_clock 1
back() { reads "$SHORT"; }
check "and back on 24 hours, both read \"$SHORT\" again" waitfor 10 back
gui_end
