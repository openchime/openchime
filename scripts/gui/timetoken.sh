#!/usr/bin/env bash
# Time tokens (MARKDOWN.md §1c): a time written once, read in each reader's zone.
#
#   scripts/gui/timetoken.sh          # run, and leave the client running
#   scripts/gui/timetoken.sh --kill   # ... and shut it down afterwards
#
# Insert a time picks tomorrow at 15:00 in the client's own zone and writes the
# token, in UTC, at the caret. Sent, the message shows it as tomorrow's day, date
# and 15:00 -- the reader's zone is the writer's here, so it reads as picked -- in a box
# of its own in the text, and hovering it shows the whole date with the zone.
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

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
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== Insert a time writes the token"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
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
rm -f "$D"

say ""
say "gui_timetoken: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
