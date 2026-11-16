#!/usr/bin/env bash
# How does the Win32 client START? Launch it four ways and read what it did.
#
#   tools/gui/startup.sh
#
# The smoke suite (tools/gui/smoke.sh) asks whether a client that starts normally goes
# on to work, and deliberately stops there. Nothing asked what happens between
# launch and the first frame -- reading the command line, deciding whether to
# connect or show the sign-in view, showing the window at all -- and two defects
# shipped through exactly that stretch: a workspace that did not resolve left a
# live process with an invisible window and nothing said, and the fix for it
# started the client twice, two net threads on one workspace. Both were found by
# a person noticing. This is the harness that notices instead.
#
# Each case launches the client fresh and asserts on its own dump (`startup
# visible= started= si_ws= si_err=`, plus `view=` and `authed=`), waiting for the
# state it expects rather than reading once and hoping.
#
# NOT IN CI, for the reason tools/gui/smoke.sh gives: the daemon is Linux-only and
# GitHub's Windows runners cannot host it. Run it before pushing a change to how
# the client starts.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'

# Its own daemon, port and directory, wiped each run -- the isolation tools/gui/smoke.sh
# explains, so a run can never be testing somebody's real workspace.
export OC_TEST_PORT="${OC_TEST_PORT:-9510}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-startup}"
export OC_TEST_WS="${OC_TEST_WS:-Startup Fixture}"

VIEW_SIGNIN=100
fails=0
checks=0
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); checks=$((checks + 1)); }
ok()   { printf '  ok   %s\n' "$*"; checks=$((checks + 1)); }

if ! kill_dev_daemon; then
  say "a daemon is still listening on :$OC_TEST_PORT: refusing to wipe underneath it."
  exit 1
fi
rm -rf "$OC_TEST_DIR"

snap() { "$DRIVE" dump startup >/dev/null 2>&1; cat "$LIN_DIR/startup.txt" 2>/dev/null; }
# A key is read from the one dump line that owns it, picked by how that line
# begins: other lines carry keys of the same name (the table editor's
# `view=0,0,0,0` comes before the client's own `view=`), so a search of the
# whole dump reads whichever comes first.
#   line_key  <dump> <line prefix> <key>     e.g. line_key "$d" 'startup ' started
#   line_quoted <dump> <line prefix> <key>   the same, for a "quoted" value
line_of() { printf '%s\n' "$1" | grep -m1 "^$2"; }
line_key() { line_of "$1" "$2" | grep -o "\b$3=[^ ]*" | head -1 | cut -d= -f2; }
line_quoted() { line_of "$1" "$2" | grep -o "\b$3=\"[^\"]*\"" | head -1 | sed "s/^$3=\"//; s/\"\$//"; }
# The lines this script reads, and the keys each one owns.
view_of()    { line_key "$1" 'view=' "$2"; }           # view, wsmgr
startup_of() { line_key "$1" 'startup ' "$2"; }        # visible, started
authed_of()  { line_key "$1" 'authed=' authed; }
si_of()      { line_quoted "$1" 'startup ' "$2"; }     # si_ws, si_err
error_of()   { line_quoted "$1" 'error_seq=' last_error; }

# Wait up to ~10 s for `pred` (a shell function over $d, the dump) to hold.
wait_for() {
  local pred="$1" i
  for i in $(seq 1 40); do
    d="$(snap)"
    [ -n "$d" ] && "$pred" && return 0
    sleep 0.25
  done
  return 1
}

# Launched with a workspace, the window is created hidden and shown once its
# placement arrives, or 1.5 s on: a check of `visible` waits for it as the other
# checks wait, and a window that never shows still fails.
shown() { [ "$(startup_of "$d" visible)" = "1" ]; }

launch() {   # launch <workspace> <credential>
  OC_TEST_NO_BUILD=1 "$DRIVE" launch "$1" "$2" >/dev/null 2>&1
}

say "build"
make -C "$HERE" daemon >/dev/null 2>&1 || { say "  FAIL daemon build"; exit 1; }
make -C "$HERE" windows >/dev/null 2>&1 || { say "  FAIL gui build"; exit 1; }

# --- 1. a workspace that resolves, with a credential ---------------------------
say "1. a reachable workspace"
launch "$("$DRIVE" devaddr)" "alice:pw"
signed_in() { [ "$(authed_of "$d")" = "1" ]; }
if wait_for signed_in; then ok "signs in"; else fail "never signed in"; fi
wait_for shown && ok "window is visible" || fail "window is not visible"
[ "$(startup_of "$d" started)" = "1" ] && ok "exactly one client started" \
  || fail "started $(startup_of "$d" started) clients, not 1: two net threads on one workspace"
[ "$(view_of "$d" view)" != "$VIEW_SIGNIN" ] && ok "shows the workspace, not the sign-in view" \
  || fail "landed on the sign-in view"
[ "$(view_of "$d" wsmgr)" = "0" ] && ok "does not also open the switcher" \
  || fail "opened the workspace switcher as well"

# --- 2. a workspace that does not resolve --------------------------------------
# `.invalid` is reserved never to resolve (RFC 6761), so this fails the same way
# on every machine, online or not.
say "2. a workspace that does not resolve"
launch "nosuch.invalid" "alice:pw"
on_signin() { [ "$(view_of "$d" view)" = "$VIEW_SIGNIN" ]; }
if wait_for on_signin; then ok "lands on the sign-in view"; else fail "never reached the sign-in view (view=$(view_of "$d" view))"; fi
wait_for shown && ok "window is visible" \
  || fail "window is NOT visible: a live process showing nothing"
[ "$(si_of "$d" si_ws)" = "nosuch.invalid" ] && ok "keeps the workspace it was given" \
  || fail "sign-in view shows workspace '$(si_of "$d" si_ws)'"
case "$(si_of "$d" si_err)" in
  *"not found"*) ok "says why: $(si_of "$d" si_err)" ;;
  *) fail "gives no reason (si_err='$(si_of "$d" si_err)')" ;;
esac

# --- 3. a workspace that is not a workspace ------------------------------------
say "3. a malformed workspace"
launch ":$OC_TEST_PORT" "alice:pw"
if wait_for on_signin; then ok "lands on the sign-in view"; else fail "never reached the sign-in view"; fi
wait_for shown && ok "window is visible" || fail "window is not visible"
case "$(si_of "$d" si_err)" in
  *"invalid workspace"*) ok "says why: $(si_of "$d" si_err)" ;;
  *) fail "gives no reason (si_err='$(si_of "$d" si_err)')" ;;
esac

# --- 4. a workspace that resolves but where nothing is listening ---------------
# Resolving succeeds, so this is not an address the client refuses. The sign-in
# card asks the workspace how it signs people in before it starts a client, and
# nothing answers: it says why on the card and starts none. A client that did
# start would say why itself (last_error). What must hold is that the window
# shows, the reason is said, and never more than one client runs.
say "4. a workspace nothing answers at"
launch "127.0.0.1:1" "alice:pw"
reason_of() { local r; r="$(si_of "$1" si_err)"; [ -n "$r" ] || r="$(error_of "$1")"; printf '%s' "$r"; }
has_reason() { [ -n "$(reason_of "$d")" ]; }
if wait_for has_reason; then ok "says why: $(reason_of "$d")"; else fail "gives no reason it cannot connect"; fi
wait_for shown && ok "window is visible" || fail "window is not visible"
[ "$(startup_of "$d" started)" -le 1 ] 2>/dev/null && ok "no more than one client started ($(startup_of "$d" started))" \
  || fail "started $(startup_of "$d" started) clients, more than 1"

"$DRIVE" kill >/dev/null 2>&1
kill_dev_daemon >/dev/null 2>&1
say ""
if [ "$fails" -eq 0 ]; then say "startup: $checks checks, all ok"; exit 0; fi
say "startup: $fails of $checks checks FAILED"
exit 1
