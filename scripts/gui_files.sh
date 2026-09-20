#!/usr/bin/env bash
# Paging the Files view in the Win32 client (REQ-143).
#
#   scripts/gui_files.sh          # run, and leave the client running
#   scripts/gui_files.sh --kill   # ... and shut it down afterwards
#
# The daemon answers a page at a time and says when more remain. It asserts that
#
#   - a first page shows its rows and offers "Load more", published to assistive
#     technology;
#   - pressing it appends the next page rather than replacing what is shown;
#   - when the last page arrives the button goes, and the rows are the files that
#     were uploaded -- each one once.
#
# Its own daemon on its own port, with a page of two, so three uploads are enough
# to see paging; a real daemon pages at two hundred.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9590}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-files}"
export OC_DEV_WS="${OC_DEV_WS:-Files Fixture}"
export OPENCHIME_FILE_PAGE=2          # gui_drive passes the daemon its environment

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"

D="/tmp/oc-files-dump.txt"
snap() { "$DRIVE" dump files >/dev/null 2>&1; cp "$LIN_DIR/files.txt" "$D" 2>/dev/null; }
f() { snap; grep -m1 '^files ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
click_more() {
  local l t r b; IFS=, read -r l t r b <<< "$(f more_btn)"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt 0 ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== three files, two to a page"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
for i in 1 2 3; do
  p="/mnt/c/Windows/Temp/octest/paged$i.txt"
  printf 'file %d\n' "$i" > "$p"
  "$DRIVE" upload "C:\\Windows\\Temp\\octest\\paged$i.txt" >/dev/null
  sleep 1
done
"$DRIVE" view files >/dev/null
first_page() { [ "$(f n)" = 2 ] && [ "$(f more)" = 1 ]; }
check "the first page holds two, and says more remain" waitfor 15 first_page
snap
check "\"Load more\" is published to assistive technology" bash -c "grep -q '^a11yitem files.loadmore ' $D"
"$DRIVE" shot files_page1 >/dev/null

say "== pressing it brings the rest"
click_more
second_page() { [ "$(f n)" = 3 ] && [ "$(f more)" = 0 ]; }
check "the list holds all three, with no more to come" waitfor 15 second_page
gone() { [ "$(f more_btn)" = "0,0,0,0" ]; }
check "...and the button is gone" waitfor 5 gone
rows_match() { [ "$(f rows)" = 3 ]; }
check "every file is listed once" rows_match
"$DRIVE" shot files_page2 >/dev/null
rm -f "$D" /mnt/c/Windows/Temp/octest/paged?.txt

say ""
say "gui_files: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
