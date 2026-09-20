#!/usr/bin/env bash
# The Win32 client's keyboard shortcuts, driven and observed (REQ-264).
#
#   scripts/gui_shortcuts.sh          # run, and leave the pair running
#   scripts/gui_shortcuts.sh --down   # ... and take it down afterwards
#
# A shortcut is only as good as the one path it shares with the menu that offers
# the same thing, so each check here drives the KEY and asserts the effect, not
# the dispatch. Today:
#
#   - Shift+Esc marks every conversation read, as the reference client binds it;
#     bare Esc still closes what is open rather than marking anything.
#
# Two clients over gui_pair.sh, so there is something unread to mark: bob says
# things in a conversation alice is not looking at.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
START=$SECONDS
fails=0
checks=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9570}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-shortcuts}"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

D="/tmp/oc-shortcuts-dump.txt"
snap() { "$PAIR" a dump shortcuts > "$D" 2>&1; }
# How many conversations have something unread, from the marks the client holds.
unread_convs() { snap; grep -c '^  marks ch .* unread=[1-9]' "$D"; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== fixture: alice and bob"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
"$PAIR" a channel 1 >/dev/null
"$PAIR" a mkchan quiet >/dev/null
sleep 1
"$PAIR" b channel 2 >/dev/null

say "== something unread"
# alice sits in #general; bob talks in #quiet, so alice has one unread there.
"$PAIR" a channel 1 >/dev/null
for i in 1 2 3; do "$PAIR" b send "unread message $i" >/dev/null; done
some_unread() { [ "$(unread_convs)" -ge 1 ]; }
check "alice has an unread conversation" waitfor 10 some_unread

say "== bare Esc does not mark anything read"
"$PAIR" a key esc >/dev/null
sleep 1
check "...it is still unread" some_unread

say "== Shift+Esc marks every conversation read"
"$PAIR" a key shift+esc >/dev/null
none_unread() { [ "$(unread_convs)" = 0 ]; }
check "nothing is unread any more" waitfor 10 none_unread
"$PAIR" a shot shortcuts_marked >/dev/null

say "== and it says so"
said() { snap; grep -qE '^toast\[[0-9]\] .*[Mm]arked' "$D"; }
check "a toast reports what it did" said
rm -f "$D"

say ""
say "gui_shortcuts: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
