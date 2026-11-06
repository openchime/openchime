#!/usr/bin/env bash
# Time tokens (MARKDOWN.md §1c): a time written once, read in each reader's zone.
#
#   tools/gui/timetoken.sh          # run, and leave the client running
#   tools/gui/timetoken.sh --kill   # ... and shut it down afterwards
#
# Insert a time picks tomorrow at 15:00 in the client's own zone and writes the
# token, in UTC, at the caret. Sent, the message shows it as tomorrow's day, date
# and 15:00 -- the reader's zone is the writer's here, so it reads as picked -- in a box
# of its own in the text, and hovering it shows the whole date with the zone.
# However many there are, every one is drawn so: forty in one message and twenty
# in one table cell, custom emoji among them, each in its own box.
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9680}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-timetoken}"
export OC_TEST_WS="${OC_TEST_WS:-Time Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-timetoken-dump.txt"
snap() { "$DRIVE" dump timetoken 2>/dev/null | tail -n +2 > "$D"; }
tt() { grep -m1 '^timetoks ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
tok() { grep -m1 '^  timetok ' "$D" | grep -o "\b$1=\"[^\"]*\"" | head -1 | cut -d'"' -f2; }
tokr() { grep -m1 '^  timetok ' "$D" | grep -o ' r=[0-9,.-]*' | cut -d= -f2; }
# Polled on the outcome: each dump is a round trip to the client's tick.
waitfor() {  # waitfor <secs> <command...>
  local end=$((SECONDS + $1)); shift
  while :; do
    "$@" && return 0
    [ "$SECONDS" -ge "$end" ] && return 1
  done
}

say "== Insert a time writes the token"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
signed_in() { snap; grep -q 'authed=1' "$D"; }
waitfor 20 signed_in
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
snap
check "Insert a time is on the composer, and published" grep -q '^a11yitem composer.time ' "$D"
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
"$DRIVE" type "Standup at " >/dev/null
"$DRIVE" timeinsert "$DAY 15:00" >/dev/null
inserted() { snap; grep -m1 '^ed ' "$D" | grep -qF "text=\"Standup at $TOKEN\""; }
check "the composer holds the token, in UTC ($TOKEN)" waitfor 5 inserted

say "== sent, it reads in the reader's zone"
"$DRIVE" key enter >/dev/null
drawn() { snap; [ "$(tt n)" -ge 1 ] && [ "$(tok text)" = "$SHORT" ]; }
check "the message shows \"$SHORT\"" waitfor 10 drawn
check "the token is gone from the text: the excerpt reads as the message does" \
  grep -q "^  ch 1 .*prev=\"Standup at $SHORT\"" "$D"
"$DRIVE" shot timetoken_sent >/dev/null

say "== in a table's cell it reads in the reader's zone too"
"$DRIVE" sendraw "| When | What |\n|---|---|\n| $TOKEN | Standup |" >/dev/null
in_cell() { snap; [ "$(grep -c "^  timetok .*text=\"$SHORT\"" "$D")" -ge 2 ]; }
check "the cell shows \"$SHORT\", beside the message's" waitfor 10 in_cell
check "and the table is drawn as a grid, not as typed" grep -q '^  ch 1 .*prev="Table: 1 row' "$D"
"$DRIVE" shot timetoken_cell >/dev/null

say "== hovering it shows the whole of it"
IFS=, read -r l t r b <<<"$(tokr)"
"$DRIVE" move $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
tipped() { snap; [ "$(tt tip)" = 0 ]; }
check "the pointer on it shows its tooltip" waitfor 5 tipped
check "which says the date, the time and the zone (\"$(tok full)\")" \
  bash -c "case \"\$(grep -m1 '^  timetok ' '$D' | grep -o 'full=\"[^\"]*\"' | cut -d'\"' -f2)\" in \"$FULLP (UTC\"*) exit 0;; *) exit 1;; esac"
"$DRIVE" shot timetoken_tip >/dev/null

say "== however many there are, every one reads in the reader's zone"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it (tools/gui/emoji_admin.sh).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }
mkdir -p "$LIN_DIR"
python3 - "$LIN_DIR/tt-emoji.png" <<'PY'
import struct, sys, zlib
w = h = 32
rows = b''.join(b'\0' + bytes.fromhex('f59e0b') * w for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
"$DRIVE" emoji_add "ttmark $(cpath tt-emoji.png)" >/dev/null
registered() { snap; grep -q '^cemoji=1$' "$D"; }
check "a custom emoji is added to mix in" waitfor 30 registered
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
  snap
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
emoji_drawn() { snap; [ "$(tt emoji)" = "$1" ]; }
"$DRIVE" sendraw "Many: $(cat "$M/body.txt")" >/dev/null
check "forty in one message all read in the reader's zone, in order" waitfor 15 drawn_as "$M/body.want"
check "and the ten custom emoji among them are drawn" waitfor 10 emoji_drawn 10
"$DRIVE" shot timetoken_many >/dev/null
"$DRIVE" sendraw "| When | What |\n|---|---|\n| $(cat "$M/cell.txt") | Many |" >/dev/null
check "twenty in one table cell all read in the reader's zone, in order" waitfor 15 drawn_as "$M/cell.want"
check "and its five custom emoji are drawn, beside the message's ten" waitfor 10 emoji_drawn 15
"$DRIVE" shot timetoken_many_cell >/dev/null
rm -rf "$M" "$LIN_DIR/tt-emoji.png"
rm -f "$D"

say ""
say "gui_timetoken: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
