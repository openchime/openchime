#!/usr/bin/env bash
# Paging the Files view in the Win32 client (REQ-143).
#
#   tools/gui/features/files.sh          # run; the client and daemon stop at the end
#   tools/gui/features/files.sh --keep   # ... and leave them running
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
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin files 9590 /tmp/oc-files "Files Fixture" "$@"
export OPENCHIME_DEV_FILE_PAGE=2     # gui_drive passes the daemon its environment

f() { gui_snap; field "$D" '^files ' "$1"; }

step "three files, two to a page"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
mkdir -p "$LIN_DIR"
# One at a time, each stored -- back from the daemon as a message's
# attachment -- before the next.
for i in 1 2 3; do
  printf 'file %d\n' "$i" > "$LIN_DIR/paged$i.txt"
  upload_stored "paged$i.txt"
done
"$DRIVE" view files >/dev/null || gui_abort "the client did not open Files"
first_page() { [ "$(f n)" = 2 ] && [ "$(f more)" = 1 ]; }
check "the first page holds two, and says more remain" waitfor 15 first_page
check "\"Load more\" is published to assistive technology" grep -q '^a11yitem files.loadmore ' "$D"
"$DRIVE" shot files_page1 >/dev/null

step "pressing it brings the rest"
click_rect "$(f more_btn)"
second_page() { [ "$(f n)" = 3 ] && [ "$(f more)" = 0 ]; }
check "the list holds all three, with no more to come" waitfor 15 second_page
gone() { [ "$(f more_btn)" = "0,0,0,0" ]; }
check "...and the button is gone" waitfor 5 gone
rows_match() { [ "$(f rows)" = 3 ]; }
check "every file is listed once" rows_match
"$DRIVE" shot files_page2 >/dev/null
rm -f "$LIN_DIR"/paged?.txt
gui_end
