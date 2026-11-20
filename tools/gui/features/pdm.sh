#!/usr/bin/env bash
# Encrypted DMs, two clients end to end (docs/PRIVATE-DM.md).
#
#   tools/gui/features/pdm.sh            # run; the pair and daemon stop at the end
#   tools/gui/features/pdm.sh --keep     # ... and leave them running
#
# alice and bob, each a real client over tools/gui/harness/win32/pair.sh (or the web's), against
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
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin pdm 9515 /tmp/oc-pdm "$@"

# The value of key `k` on alice's (a) or bob's (b) newest dump's first line
# starting with `prefix`; a quoted value whole, spaces and all (`last=` is a
# message).
fv() { fieldq "$(dfile "$1")" "^$2" "$3"; }   # fv <a|b> <line-prefix> <key>
# New message to bob, with the box ticked, not yet sent.
compose_to_bob() {
  "$PAIR" a view newmsg >/dev/null || gui_abort "alice's client did not open New message"
  "$PAIR" a typekeys "bob" >/dev/null
  "$PAIR" a key enter >/dev/null
  "$PAIR" a pdm box >/dev/null || gui_abort "alice's client did not take the encrypted box"
}
send_to_bob() {
  "$PAIR" a key tab >/dev/null
  "$PAIR" a typekeys "$1" >/dev/null
  "$PAIR" a key enter >/dev/null
}

step "fixture: alice and bob"
gui_pair_up

# 1 INVITED, 2 OPENING, 3 OPEN, 4 KEY_CHANGED, 7 ENDED (client/core/event.h)
step "alice asks bob for an encrypted conversation, and he accepts"
compose_to_bob
box_a() { gui_snap a; [ "$(fv a 'pdms ' nm_box)" = 1 ] && [ "$(fv a 'pdms ' nm_on)" = 1 ]; }
check "New message offers the box for one person, and it ticks" waitfor 5 box_a
send_to_bob "a **private** hello"
invited()   { gui_snap b; [ "$(fv b 'pdm ' state)" = 1 ]; }
check "bob is asked for an encrypted conversation" waitfor 10 invited

"$PAIR" b pdm accept >/dev/null
open_b()    { gui_snap b; [ "$(fv b 'pdm ' state)" = 3 ] && [ "$(fv b 'pdm ' chan)" = 1 ] && [ "$(fv b 'pdms ' sel_e2e)" = 1 ]; }
open_a()    { gui_snap a; [ "$(fv a 'pdm ' state)" = 3 ] && [ "$(fv a 'pdms ' sel_e2e)" = 1 ]; }
check "bob accepts: an encrypted DM, shown on his device" waitfor 10 open_b
check "and on alice's" waitfor 10 open_a
sa="$(fv a 'pdm ' safety)"; sb="$(fv b 'pdm ' safety)"
same_safety() { [ -n "$sa" ] && [ "$sa" = "$sb" ] && [ "${#sa}" = 71 ]; }
check "the same safety number on both, twelve groups of five" same_safety
no_files()  { gui_snap a; [ "$(fv a 'pdms ' attach)" = 0 ] && [ "$(fv a 'pdms ' sched)" = 0 ] && [ "$(fv a 'pdms ' files_tab)" = 0 ]; }
check "no file, Send later or Files tab in it" waitfor 5 no_files

step "messages both ways"
got_b()     { gui_snap b; [ "$(fv b 'pdms ' last)" = "a **private** hello" ]; }
check "what alice wrote in New message reaches bob" waitfor 10 got_b
"$PAIR" b type "a private answer" >/dev/null
"$PAIR" b key enter >/dev/null
got_a()     { gui_snap a; [ "$(fv a 'pdms ' last)" = "a private answer" ]; }
check "bob's answer from the composer reaches alice" waitfor 10 got_a

step "alice ends it"
"$PAIR" a pdm close >/dev/null
closed_b()  { gui_snap b; [ "$(fv b 'pdm ' state)" = 7 ] && [ "$(fv b 'pdm ' reason)" = 1 ]; }
check "alice ends it: bob is told it was closed" waitfor 10 closed_b
"$PAIR" b pdm forget >/dev/null
gone_b()    { gui_snap b; [ "$(fv b 'pdms ' n)" = 0 ] && [ "$(fv b 'pdms ' sel_e2e)" = 0 ]; }
check "put away, it is gone from bob's" waitfor 5 gone_b
# alice puts hers away too, so the next invitation is the only one either
# side's `pdm` line can be about.
"$PAIR" a pdm forget >/dev/null
gone_a()    { gui_snap a; [ "$(fv a 'pdms ' n)" = 0 ]; }
waitfor 5 gone_a || gui_abort "alice's ended conversation did not go when put away: $(grep -m1 '^pdms ' "$DA")"

step "bob declines a second one"
compose_to_bob
waitfor 5 box_a || gui_abort "New message did not tick the box for the second invitation"
send_to_bob "again"
check "a second invitation" waitfor 10 invited
"$PAIR" b pdm decline >/dev/null
declined_a() { gui_snap a; [ "$(fv a 'pdm ' state)" = 7 ] && [ "$(fv a 'pdm ' reason)" = 6 ]; }
check "bob declines: alice is told so" waitfor 10 declined_a
gui_end
