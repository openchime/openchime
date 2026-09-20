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
#     bare Esc still closes what is open rather than marking anything;
#   - Ctrl+<digit> goes to that workspace, counting from 1 in the rail's order,
#     and a digit past the last one does nothing.
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
# The clients reach the daemon at the WSL machine's own address, as the pair does.
OC_PAIR_HOST_ADDR="${OC_PAIR_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OC_PAIR_HOST="$OC_PAIR_HOST_ADDR"

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

say "== Ctrl+<digit> switches workspace"
# A SECOND daemon, so there is a second workspace to switch to: a client keys a
# workspace by its address, so signing in twice to one address is one workspace.
SECOND_PORT=$((OC_PAIR_PORT + 1))
SECOND_DIR="$OC_PAIR_DIR-2"
rm -rf "$SECOND_DIR"; mkdir -p "$SECOND_DIR"
env OPENCHIME_DB_PATH="$SECOND_DIR/db" \
    OPENCHIME_TLS_CERT="$SECOND_DIR/cert.pem" OPENCHIME_TLS_KEY="$SECOND_DIR/key.pem" \
    OPENCHIME_BLOB_DIR="$SECOND_DIR/blobs" \
    OPENCHIME_PROTO_PORT="$SECOND_PORT" OPENCHIME_HEALTH_PORT=0 \
    OPENCHIME_WORKSPACE_NAME="Second Fixture" \
    OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner" \
    OPENCHIME_DEPLOYMENT_MODE=managed OPENCHIME_MAX_USERS=100 \
    setsid "$HERE/openchimed" > "$SECOND_DIR/daemon.log" 2>&1 < /dev/null &
disown
for _ in $(seq 1 40); do
  (exec 3<>/dev/tcp/127.0.0.1/$SECOND_PORT) 2>/dev/null && { exec 3<&- 3>&-; break; }
  sleep 0.25
done
ws_n()     { snap; grep -m1 '^workspaces=' "$D" | grep -o 'workspaces=[0-9]*' | cut -d= -f2; }
ws_active(){ snap; grep -m1 '^workspaces=' "$D" | grep -o 'active=[0-9-]*' | cut -d= -f2; }
"$PAIR" a wsadd "$OC_PAIR_HOST_ADDR:$SECOND_PORT" "alice:pw" >/dev/null
two_ws() { [ "$(ws_n)" = 2 ]; }
check "alice's client holds two workspaces" waitfor 20 two_ws
on_second() { [ "$(ws_active)" = 1 ]; }
on_first()  { [ "$(ws_active)" = 0 ]; }
check "...and the new one is on screen" waitfor 5 on_second
# Each key must CHANGE where we are, so the starting point is set first through
# the switcher's own path (the wsgo verb): a check that begins where it means to
# end passes whether or not the key does anything.
"$PAIR" a wsgo 1 >/dev/null
"$PAIR" a key ctrl+1 >/dev/null
check "Ctrl+1 goes to the first" waitfor 5 on_first
"$PAIR" a shot shortcuts_ws1 >/dev/null
"$PAIR" a wsgo 0 >/dev/null
"$PAIR" a key ctrl+2 >/dev/null
check "Ctrl+2 goes to the second" waitfor 5 on_second
"$PAIR" a wsgo 0 >/dev/null
"$PAIR" a key ctrl+9 >/dev/null
sleep 0.5
check "Ctrl+9, with two signed in, leaves it where it was" on_first

say ""
say "gui_shortcuts: $((checks - fails))/$checks passed in $((SECONDS - START))s"
kill_second() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$SECOND_PORT" && kill "$p" 2>/dev/null
  done
}
kill_second
rm -rf "$SECOND_DIR"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
