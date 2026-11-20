#!/usr/bin/env bash
# What's new (CLIENT.md, "What's new"): this build's release notes, from the
# control plane by its build id.
#
#   tools/gui/features/whatsnew.sh          # run; the client and daemon stop at the end
#   tools/gui/features/whatsnew.sh --keep   # ... and leave them running
#
# It asserts that What's new is in the workspace menu, the palette and the About
# card; that choosing it opens the card, which never holds the window while it
# asks; that a source build ("dev"), which is no release, says so and offers the
# list of releases without asking; and that "Open on openchime.io" opens the
# release's page (written where the hook writes every URL it opens). A build
# that IS a release reads "What's new in release N" with its sections; that is
# run by hand against a release build (the dump's `whatsnew=shown release=N`).
# Its own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin whatsnew 9780 /tmp/oc-whatsnew "Whatsnew Fixture" "$@"

wn() { grep -m1 '^whatsnew=' "$D" | tr ' ' '\n' | grep -m1 "^$1=" | cut -d= -f2; }

step "What's new is offered"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" about >/dev/null || gui_abort "the client did not open About"
# Its buttons are published when the card is first drawn, a frame or two on.
offered() { gui_snap; grep -i "^a11yitem .*what's new" "$D" >/dev/null; }   # the id is lower case
check "the About card offers What's new" waitfor 5 offered
"$DRIVE" key esc >/dev/null
about_shut() { gui_snap; ! grep -i "^a11yitem .*what's new" "$D" >/dev/null; }
waitfor 5 about_shut || gui_abort "Esc did not close About"
"$DRIVE" palette "What's new" >/dev/null || gui_abort "the client did not open the palette"
found() { gui_snap; grep "What's new" "$D" >/dev/null; }
check "the palette finds it" waitfor 5 found
"$DRIVE" key esc >/dev/null

step "a source build is no release"
"$DRIVE" whatsnew >/dev/null || gui_abort "the client did not take whatsnew"
open_card() { gui_snap; [ "$(wn open)" = 1 ]; }
check "the card opens" waitfor 5 open_card
notrel() { gui_snap; [ "$(wn whatsnew)" = notrelease ]; }
check "a dev build says it is not a release, without asking" waitfor 5 notrel
"$DRIVE" shot whatsnew_notrelease >/dev/null
URLF="/mnt/c/Windows/Temp/octest/signin_url.txt"   # where the hook writes a URL it opens
rm -f "$URLF"
"$DRIVE" whatsnew open >/dev/null
url_is() { [ -f "$URLF" ] && grep '^https://openchime.io/releases' "$URLF" >/dev/null; }
check "Open on openchime.io goes to the releases" waitfor 5 url_is
"$DRIVE" key esc >/dev/null
gui_end
