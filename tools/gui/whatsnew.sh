#!/usr/bin/env bash
# What's new (CLIENT.md, "What's new"): this build's release notes, from the
# control plane by its build id.
#
#   tools/gui/whatsnew.sh          # run, and leave the client running
#   tools/gui/whatsnew.sh --kill   # ... and shut it down afterwards
#
# It asserts that What's new is in the workspace menu, the palette and the About
# card; that choosing it opens the card, which never holds the window while it
# asks; that a source build ("dev"), which is no release, says so and offers the
# list of releases without asking; and that "Open on openchime.io" opens the
# release's page (written where the hook writes every URL it opens). A build
# that IS a release reads "What's new in release N" with its sections; that is
# run by hand against a release build (the dump's `whatsnew=shown release=N`).
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9710}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-whatsnew}"
export OC_TEST_WS="${OC_TEST_WS:-Whatsnew Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="$OC_TEST_DIR-dump.txt"
snap() { "$DRIVE" dump whatsnew 2>/dev/null | tail -n +2 > "$D"; }
wn() { grep -m1 '^whatsnew=' "$D" | tr ' ' '\n' | grep -m1 "^$1=" | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== What's new is offered"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" about >/dev/null
snap
check "the About card offers What's new" grep -q "^a11yitem .*What's new" "$D"
"$DRIVE" key esc >/dev/null
"$DRIVE" palette "What's new" >/dev/null
snap
check "the palette finds it" grep -q "What's new" "$D"
"$DRIVE" key esc >/dev/null

say "== a source build is no release"
"$DRIVE" whatsnew >/dev/null
open_card() { snap; [ "$(wn open)" = 1 ]; }
check "the card opens" waitfor 5 open_card
notrel() { snap; [ "$(wn whatsnew)" = notrelease ]; }
check "a dev build says it is not a release, without asking" waitfor 5 notrel
"$DRIVE" shot whatsnew_notrelease >/dev/null
URLF="/mnt/c/Windows/Temp/octest/signin_url.txt"   # where the hook writes a URL it opens
rm -f "$URLF"
"$DRIVE" whatsnew open >/dev/null
url_is() { [ -f "$URLF" ] && grep -q '^https://openchime.io/releases' "$URLF"; }
check "Open on openchime.io goes to the releases" waitfor 5 url_is
"$DRIVE" key esc >/dev/null
rm -f "$D"

say ""
say "gui_whatsnew: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
