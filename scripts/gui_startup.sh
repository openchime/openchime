#!/usr/bin/env bash
# How does the Win32 client START? Launch it four ways and read what it did.
#
#   scripts/gui_startup.sh
#
# The smoke suite (gui_smoke.sh) asks whether a client that starts normally goes
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
# NOT IN CI, for the reason gui_smoke.sh gives: the daemon is Linux-only and
# GitHub's Windows runners cannot host it. Run it before pushing a change to how
# the client starts.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'

# Its own daemon, port and directory, wiped each run -- the isolation gui_smoke.sh
# explains, so a run can never be testing somebody's real workspace.
export OC_DEV_PORT="${OC_DEV_PORT:-9510}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-startup}"
export OC_DEV_WS="${OC_DEV_WS:-Startup Fixture}"

VIEW_SIGNIN=100
fails=0
checks=0
say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); checks=$((checks + 1)); }
ok()   { printf '  ok   %s\n' "$*"; checks=$((checks + 1)); }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
if ! kill_dev_daemon; then
  say "a daemon is still listening on :$OC_DEV_PORT — refusing to wipe underneath it."
  exit 1
fi
rm -rf "$OC_DEV_DIR"

snap() { "$DRIVE" dump startup >/dev/null 2>&1; cat "$LIN_DIR/startup.txt" 2>/dev/null; }
key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }
quoted_of() { printf '%s' "$1" | grep -o "\b$2=\"[^\"]*\"" | head -1 | sed "s/^$2=\"//; s/\"\$//"; }

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

launch() {   # launch <workspace> <credential>
  OC_DRIVE_NO_BUILD=1 "$DRIVE" launch "$1" "$2" >/dev/null 2>&1
}

say "build"
make -C "$HERE" >/dev/null 2>&1 || { say "  FAIL daemon build"; exit 1; }
make -C "$HERE" windows-gui >/dev/null 2>&1 || { say "  FAIL gui build"; exit 1; }

# --- 1. a workspace that resolves, with a credential ---------------------------
say "1. a reachable workspace"
launch "127.0.0.1:$OC_DEV_PORT" "alice:pw"
signed_in() { [ "$(key_of "$d" authed)" = "1" ]; }
if wait_for signed_in; then ok "signs in"; else fail "never signed in"; fi
[ "$(key_of "$d" visible)" = "1" ] && ok "window is visible" || fail "window is not visible"
[ "$(key_of "$d" started)" = "1" ] && ok "exactly one client started" \
  || fail "started $(key_of "$d" started) clients, not 1 — two net threads on one workspace"
[ "$(key_of "$d" view)" != "$VIEW_SIGNIN" ] && ok "shows the workspace, not the sign-in view" \
  || fail "landed on the sign-in view"
[ "$(key_of "$d" wsmgr)" = "0" ] && ok "does not also open the switcher" \
  || fail "opened the workspace switcher as well"

# --- 2. a workspace that does not resolve --------------------------------------
# `.invalid` is reserved never to resolve (RFC 6761), so this fails the same way
# on every machine, online or not.
say "2. a workspace that does not resolve"
launch "nosuch.invalid" "alice:pw"
on_signin() { [ "$(key_of "$d" view)" = "$VIEW_SIGNIN" ]; }
if wait_for on_signin; then ok "lands on the sign-in view"; else fail "never reached the sign-in view (view=$(key_of "$d" view))"; fi
[ "$(key_of "$d" visible)" = "1" ] && ok "window is visible" \
  || fail "window is NOT visible — a live process showing nothing"
[ "$(quoted_of "$d" si_ws)" = "nosuch.invalid" ] && ok "keeps the workspace it was given" \
  || fail "sign-in view shows workspace '$(quoted_of "$d" si_ws)'"
case "$(quoted_of "$d" si_err)" in
  *"not found"*) ok "says why: $(quoted_of "$d" si_err)" ;;
  *) fail "gives no reason (si_err='$(quoted_of "$d" si_err)')" ;;
esac

# --- 3. a workspace that is not a workspace ------------------------------------
say "3. a malformed workspace"
launch ":$OC_DEV_PORT" "alice:pw"
if wait_for on_signin; then ok "lands on the sign-in view"; else fail "never reached the sign-in view"; fi
[ "$(key_of "$d" visible)" = "1" ] && ok "window is visible" || fail "window is not visible"
case "$(quoted_of "$d" si_err)" in
  *"invalid workspace"*) ok "says why: $(quoted_of "$d" si_err)" ;;
  *) fail "gives no reason (si_err='$(quoted_of "$d" si_err)')" ;;
esac

# --- 4. a workspace that resolves but where nothing is listening ---------------
# Resolving succeeds, so this is not a sign-in failure: the client starts and its
# net thread reports that it cannot connect, and keeps retrying. What must hold
# is that the window shows, one client runs, and the reason is said.
say "4. a workspace nothing answers at"
launch "127.0.0.1:1" "alice:pw"
has_error() { [ -n "$(quoted_of "$d" last_error)" ]; }
if wait_for has_error; then ok "says why: $(quoted_of "$d" last_error)"; else fail "gives no reason it cannot connect"; fi
[ "$(key_of "$d" visible)" = "1" ] && ok "window is visible" || fail "window is not visible"
[ "$(key_of "$d" started)" = "1" ] && ok "exactly one client started" \
  || fail "started $(key_of "$d" started) clients, not 1"

"$DRIVE" kill >/dev/null 2>&1
kill_dev_daemon >/dev/null 2>&1
say ""
if [ "$fails" -eq 0 ]; then say "startup: $checks checks, all ok"; exit 0; fi
say "startup: $fails of $checks checks FAILED"
exit 1
