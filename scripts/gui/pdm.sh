#!/usr/bin/env bash
# Encrypted DMs, two clients end to end (docs/PRIVATE-DM.md).
#
#   scripts/gui/pdm.sh            # run, and leave the pair running
#   scripts/gui/pdm.sh --down     # ... and take it down afterwards
#
# alice and bob, each a real client over scripts/gui/pair.sh (or the web's), against
# a real daemon. It asserts that
#
#   - alice writes to bob from New message with "End-to-end encrypted" ticked: bob
#     is asked, and accepts on this device;
#   - both are then in the same conversation, an ordinary DM marked encrypted, with
#     the same safety number, and no file, video, voice or Send later offered in it;
#   - what alice wrote in New message arrives once it is open, and bob's answer
#     from the normal composer reaches her;
#   - alice ends it: bob is told it was closed, and putting it away removes it;
#   - bob declines a second one: alice is told so.
#
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9640}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-pdm}"

dump() { "$PAIR" "$1" dump "pdm_$1" > "/tmp/oc-pdm-dump-$1.txt" 2>&1; }
# The value of key `k` on the first line starting with `prefix`.
field() {  # field <who> <line-prefix> <key>
  grep -m1 "^$2" "/tmp/oc-pdm-dump-$1.txt" | grep -o "$3=\"[^\"]*\"\|$3=[^ ]*" | head -1 | cut -d= -f2- | tr -d '"'
}
waitfor() {  # waitfor <who> <secs> <command...>
  local who="$1" secs="$2" t=0; shift 2
  while [ "$t" -lt $((secs * 4)) ]; do
    dump "$who"
    if "$@"; then return 0; fi
    sleep 0.25; t=$((t + 1))
  done
  return 1
}
# New message to bob, with the box ticked, and `text` in it, sent.
ask_bob() {
  "$PAIR" a view newmsg >/dev/null
  "$PAIR" a typekeys "bob" >/dev/null
  "$PAIR" a key enter >/dev/null
  "$PAIR" a pdm box >/dev/null
  dump a
  "$PAIR" a key tab >/dev/null
  "$PAIR" a typekeys "$1" >/dev/null
  "$PAIR" a key enter >/dev/null
}

rm -rf "$OC_TEST_DIR"
"$PAIR" up >/dev/null 2>&1

# 1 INVITED, 2 OPENING, 3 OPEN, 4 KEY_CHANGED, 7 ENDED (client/core/event.h)
ask_bob "a **private** hello"
box_a() { [ "$(field a 'pdms ' nm_box)" = 1 ] && [ "$(field a 'pdms ' nm_on)" = 1 ]; }
check "New message offers the box for one person, and it ticks" box_a
invited()   { [ "$(field b 'pdm ' state)" = 1 ]; }
check "bob is asked for an encrypted conversation" waitfor b 10 invited

"$PAIR" b pdm accept >/dev/null
open_b()    { [ "$(field b 'pdm ' state)" = 3 ] && [ "$(field b 'pdm ' chan)" = 1 ] && [ "$(field b 'pdms ' sel_e2e)" = 1 ]; }
open_a()    { [ "$(field a 'pdm ' state)" = 3 ] && [ "$(field a 'pdms ' sel_e2e)" = 1 ]; }
check "bob accepts: an encrypted DM, shown on his device" waitfor b 10 open_b
check "and on alice's" waitfor a 10 open_a
sa="$(field a 'pdm ' safety)"; sb="$(field b 'pdm ' safety)"
same_safety() { [ -n "$sa" ] && [ "$sa" = "$sb" ] && [ "${#sa}" = 71 ]; }
check "the same safety number on both, twelve groups of five" same_safety
no_files()  { [ "$(field a 'pdms ' attach)" = 0 ] && [ "$(field a 'pdms ' sched)" = 0 ] && [ "$(field a 'pdms ' files_tab)" = 0 ]; }
check "no file, Send later or Files tab in it" waitfor a 5 no_files

got_b()     { [ "$(field b 'pdms ' last)" = "a **private** hello" ]; }
check "what alice wrote in New message reaches bob" waitfor b 10 got_b
"$PAIR" b type "a private answer" >/dev/null
"$PAIR" b key enter >/dev/null
got_a()     { [ "$(field a 'pdms ' last)" = "a private answer" ]; }
check "bob's answer from the composer reaches alice" waitfor a 10 got_a

"$PAIR" a pdm close >/dev/null
closed_b()  { [ "$(field b 'pdm ' state)" = 7 ] && [ "$(field b 'pdm ' reason)" = 1 ]; }
check "alice ends it: bob is told it was closed" waitfor b 10 closed_b
"$PAIR" b pdm forget >/dev/null
gone_b()    { [ "$(field b 'pdms ' n)" = 0 ] && [ "$(field b 'pdms ' sel_e2e)" = 0 ]; }
check "put away, it is gone from bob's" waitfor b 5 gone_b
"$PAIR" a pdm forget >/dev/null

ask_bob "again"
check "a second invitation" waitfor b 10 invited
"$PAIR" b pdm decline >/dev/null
declined_a() { [ "$(field a 'pdm ' state)" = 7 ] && [ "$(field a 'pdm ' reason)" = 6 ]; }
check "bob declines: alice is told so" waitfor a 10 declined_a

say ""
say "gui_pdm: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
