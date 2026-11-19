#!/usr/bin/env bash
# The Win32 client's keyboard shortcuts, driven and observed (REQ-264).
#
#   tools/gui/features/shortcuts.sh          # run; the pair and both daemons stop at the end
#   tools/gui/features/shortcuts.sh --keep   # ... and leave them running
#
# A shortcut is only as good as the one path it shares with the menu that offers
# the same thing, so each check here drives the KEY and asserts the effect, not
# the dispatch. Today:
#
#   - Shift+Esc marks every conversation read, as the reference client binds it;
#     bare Esc still closes what is open rather than marking anything;
#   - Ctrl+<digit> goes to that workspace, counting from 1 in the rail's order,
#     and a digit past the last one does nothing.
#
# Two clients over tools/gui/harness/win32/pair.sh, so there is something unread to mark: bob says
# things in a conversation alice is not looking at.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
# The second daemon (below) is stopped with whatever else the script started:
# lib.sh's teardown, run at the start and on exit by lib.sh's own EXIT trap,
# is wrapped to stop it too, so --keep leaves it as it leaves the rest.
eval "lib_$(declare -f gui_teardown)"
gui_teardown() {
  lib_gui_teardown
  OC_TEST_PORT=$((OC_TEST_PORT + 1)) kill_dev_daemon ||
    echo "$GUI_NAME: a daemon still holds :$((OC_TEST_PORT + 1))" >&2
}
gui_begin shortcuts 9570 /tmp/oc-shortcuts "$@"
# The clients reach the daemon at the WSL machine's own address, as the pair does.
HOST_ADDR="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OC_TEST_HOST="$HOST_ADDR"

# How many conversations have something unread, from the marks the client holds.
unread_convs() { gui_snap a; grep -c '^  marks ch .* unread=[1-9]' "$DA"; }

step "fixture: alice and bob"
gui_pair_up
# A channel of alice's own to sit in. Only she is in it: bob, never asked in,
# does not have it, so he talks where they both are.
"$PAIR" a mkchan quiet >/dev/null || gui_abort "alice's client did not take mkchan"
has_quiet() { gui_snap a; grep -q '^  ch 2 ' "$DA"; }
waitfor 10 has_quiet || gui_abort "alice's client never had #quiet"
"$PAIR" b channel 1 >/dev/null || gui_abort "bob did not open #general"

step "something unread"
# alice sits in #quiet; bob talks in #general, so alice has one unread there.
"$PAIR" a channel 2 >/dev/null || gui_abort "alice did not open #quiet"
in_quiet() { gui_snap a; grep -q '^  ch 2 .* \*$' "$DA"; }
waitfor 5 in_quiet || gui_abort "alice's client is not showing #quiet: $(grep -m1 '^  ch 2 ' "$DA")"
for i in 1 2 3; do "$PAIR" b send "unread message $i" >/dev/null || gui_abort "bob's client did not take message $i"; done
some_unread() { [ "$(unread_convs)" -ge 1 ]; }
check "alice has an unread conversation" waitfor 10 some_unread

step "bare Esc does not mark anything read"
"$PAIR" a key esc >/dev/null
# paced: an absence -- the time Esc would have taken to mark something read
sleep 1
check "...it is still unread" some_unread

step "Shift+Esc marks every conversation read"
"$PAIR" a key shift+esc >/dev/null
none_unread() { [ "$(unread_convs)" = 0 ]; }
check "nothing is unread any more" waitfor 10 none_unread
"$PAIR" a shot shortcuts_marked >/dev/null

step "and it says so"
said() { gui_snap a; grep -qE '^toast\[[0-9]\] .*[Mm]arked' "$DA"; }
check "a toast reports what it did" waitfor 5 said

step "Ctrl+<digit> switches workspace"
# A SECOND daemon, so there is a second workspace to switch to: a client keys a
# workspace by its address, so signing in twice to one address is one workspace.
# Its directory is inside the script's, so gui_begin wipes it with the rest;
# 9>&- so it does not hold the GUI lock (fd 9) for as long as it runs.
SECOND_PORT=$((OC_TEST_PORT + 1))
SECOND_DIR="$OC_TEST_DIR/second"
mkdir -p "$SECOND_DIR"
env OPENCHIME_DATA_DIR="$SECOND_DIR/data" \
    OPENCHIME_PORT="$SECOND_PORT" OPENCHIME_HEALTH_PORT=0 \
    OPENCHIME_WORKSPACE_NAME="Second Fixture" \
    OPENCHIME_DEV_USERS="alice:pw:owner" \
    OPENCHIME_MANAGED_MAX_USERS=100 \
    setsid "$HERE/openchimed" > "$SECOND_DIR/daemon.log" 2>&1 < /dev/null 9>&- &
disown
second_listens() { (exec 3<>/dev/tcp/127.0.0.1/$SECOND_PORT) 2>/dev/null; }
waitfor 10 second_listens || gui_abort "the second daemon did not listen: $(tail -2 "$SECOND_DIR/daemon.log" | tr '\n' ' ')"
ws_n()     { gui_snap a; field "$DA" '^workspaces=' workspaces; }
ws_active(){ gui_snap a; field "$DA" '^workspaces=' active; }
"$PAIR" a wsadd "$HOST_ADDR:$SECOND_PORT" >/dev/null || gui_abort "alice's client did not take wsadd"
# The second workspace's card: Sign in, its certificate trusted, Sign in, the page.
sign_in_card "$PAIR" a -- "/mnt/c/Windows/Temp/ocpair-$OC_TEST_PORT-a" alice pw ||
  gui_abort "alice did not sign in to the second workspace"
two_ws() { [ "$(ws_n)" = 2 ]; }
check "alice's client holds two workspaces" waitfor 20 two_ws
on_second() { [ "$(ws_active)" = 1 ]; }
on_first()  { [ "$(ws_active)" = 0 ]; }
check "...and the new one is on screen" waitfor 5 on_second
# Each key must CHANGE where we are, so the starting point is set first through
# the switcher's own path (the wsgo verb), and seen to be set: a check that
# begins where it means to end passes whether or not the key does anything.
start_at() {  # start_at <index> <predicate>
  "$PAIR" a wsgo "$1" >/dev/null && waitfor 5 "$2" || gui_abort "the switcher did not go to workspace $1"
}
start_at 1 on_second
"$PAIR" a key ctrl+1 >/dev/null
check "Ctrl+1 goes to the first" waitfor 5 on_first
"$PAIR" a shot shortcuts_ws1 >/dev/null
start_at 0 on_first
"$PAIR" a key ctrl+2 >/dev/null
check "Ctrl+2 goes to the second" waitfor 5 on_second
start_at 0 on_first
"$PAIR" a key ctrl+9 >/dev/null
# paced: an absence -- the time Ctrl+9 would have taken to switch, were it going to
sleep 0.5
check "Ctrl+9, with two signed in, leaves it where it was" on_first
gui_end
