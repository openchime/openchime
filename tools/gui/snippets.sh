#!/usr/bin/env bash
# Snippets (REQ-226): one posted, shown as a card with its first lines, and
# opened whole, coloured.
#
#   tools/gui/snippets.sh          # run, and leave the client running
#   tools/gui/snippets.sh --kill   # ... and shut it down afterwards
#
# The test hook's "snippet" verb posts one as Create snippet does; the card
# fetches its text; a click on the card opens the viewer, whose spans are the
# lexer's; Copy and Close are its buttons. Its own daemon on its own port, wiped
# each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9720}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-snippets}"
export OC_TEST_WS="${OC_TEST_WS:-Snippet Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-snippets-dump.txt"
snap() { "$DRIVE" dump snippets 2>/dev/null | tail -n +2 > "$D"; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
card() { grep -m1 '^snipcard ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
view() { grep -m1 '^snipview ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }

say "== a snippet posted is a card with its first lines"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" snippet 'rotate.py|python|import os\n# rotate the keys\ndef rotate(n):\n    return n + 1\n' >/dev/null
loaded() { snap; [ "$(card loaded)" = 1 ]; }
check "the card is drawn, and its text fetched" waitfor 15 loaded
"$DRIVE" shot snippets_card >/dev/null

say "== a click opens it whole, coloured"
R="$(card rect)"; IFS=, read -r l t r b <<< "$R"
"$DRIVE" click $(( (${l%.*} + ${r%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null
opened() { snap; [ "$(view open)" = 1 ] && [ "$(view spans)" != 0 ]; }
check "the viewer opens on it" waitfor 5 opened
check "its words are coloured (spans $(view spans))" [ "$(view spans)" -ge 4 ]
check "as Python" [ "$(view lang)" = 2 ]
"$DRIVE" shot snippets_view >/dev/null
"$DRIVE" key escape >/dev/null
closed() { snap; [ "$(view open)" = 0 ]; }
check "Esc closes it" waitfor 5 closed
rm -f "$D"

say ""
say "gui_snippets: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
