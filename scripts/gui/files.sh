#!/usr/bin/env bash
# Paging the Files view in the Win32 client (REQ-143).
#
#   scripts/gui/files.sh          # run, and leave the client running
#   scripts/gui/files.sh --kill   # ... and shut it down afterwards
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
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it (see scripts/gui/uploads.sh).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }

export OC_TEST_PORT="${OC_TEST_PORT:-9590}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-files}"
export OC_TEST_WS="${OC_TEST_WS:-Files Fixture}"
export OPENCHIME_DEMO_FILE_PAGE=2     # gui_drive passes the daemon its environment

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-files-dump.txt"
snap() { "$DRIVE" dump files 2>/dev/null | tail -n +2 > "$D"; }
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
  printf 'file %d\n' "$i" > "$LIN_DIR/paged$i.txt"
  "$DRIVE" upload "$(cpath paged$i.txt)" >/dev/null
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
