#!/usr/bin/env bash
# How does the Win32 client START? Launch it four ways and read what it did.
#
#   tools/gui/features/startup.sh          # run; the client and daemon stop at the end
#   tools/gui/features/startup.sh --keep   # ... and leave them running
#
# The smoke suite (tools/gui/features/smoke.sh) asks whether a client that starts normally goes
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
# state it expects rather than reading once and hoping. It launches the client
# itself, four times, each with its own workspace.
#
# NOT IN CI, for the reason tools/gui/features/smoke.sh gives: the daemon is Linux-only and
# GitHub's Windows runners cannot host it. Run it before pushing a change to how
# the client starts.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
# Its own daemon, port and directory, wiped each run -- the isolation tools/gui/features/smoke.sh
# explains, so a run can never be testing somebody's real workspace.
gui_begin startup 9510 /tmp/oc-startup "Startup Fixture" "$@"

if [ "$PLAT" != win32 ]; then
  step "skipped: how the Windows client starts"
  gui_end
fi

VIEW_SIGNIN=100

# A key is read from the one dump line that owns it, picked by how that line
# begins: other lines carry keys of the same name (the table editor's
# `view=0,0,0,0` comes before the client's own `view=`), so a search of the
# whole dump reads whichever comes first.
#   view=     view, wsmgr          startup   visible, started, si_ws, si_err
#   authed=   authed               error_seq=  last_error
view_of()    { field "$D" '^view=' "$1"; }
startup_of() { field "$D" '^startup ' "$1"; }
si_of()      { fieldq "$D" '^startup ' "$1"; }   # a quoted value, which may hold spaces

# Launched with a workspace, the window is created hidden and shown once its
# placement arrives, or 1.5 s on: a check of `visible` waits for it as the other
# checks wait, and a window that never shows still fails.
shown() { gui_snap && [ "$(startup_of visible)" = "1" ]; }

# launch <workspace> <credential>: the first builds (unless OC_TEST_NO_BUILD=1),
# the rest use what it built. Only the first can sign in, so only its launch is
# held to it; each of the others must at least leave a client that answers.
launch() {
  local nb="${OC_TEST_NO_BUILD:-0}"; [ "${launched:-0}" = 1 ] && nb=1; launched=1
  OC_TEST_NO_BUILD="$nb" "$DRIVE" launch "$1" "$2" >/dev/null 2>&1
}
answers() { waitfor 10 gui_snap || gui_abort "the client launched at '$1' does not answer"; }

# --- 1. a workspace that resolves, with a credential ---------------------------
step "1. a reachable workspace"
launch "$("$DRIVE" devaddr)" "alice:pw" || gui_abort "the client did not launch and sign in at the fixture"
signed_in() { gui_snap && [ "$(field "$D" '^authed=' authed)" = "1" ]; }
check "signs in" waitfor 10 signed_in
check "window is visible" waitfor 10 shown
check "exactly one client started ($(startup_of started); 2 would be two net threads on one workspace)" \
  [ "$(startup_of started)" = "1" ]
check "shows the workspace, not the sign-in view (view=$(view_of view))" [ "$(view_of view)" != "$VIEW_SIGNIN" ]
check "does not also open the switcher (wsmgr=$(view_of wsmgr))" [ "$(view_of wsmgr)" = "0" ]

# --- 2. a workspace that does not resolve --------------------------------------
# `.invalid` is reserved never to resolve (RFC 6761), so this fails the same way
# on every machine, online or not.
step "2. a workspace that does not resolve"
launch "nosuch.invalid" "alice:pw"
answers nosuch.invalid
on_signin() { gui_snap && [ "$(view_of view)" = "$VIEW_SIGNIN" ]; }
check "lands on the sign-in view" waitfor 10 on_signin
check "window is visible (not a live process showing nothing)" waitfor 10 shown
check "keeps the workspace it was given (si_ws='$(si_of si_ws)')" [ "$(si_of si_ws)" = "nosuch.invalid" ]
says() { grep -E "^startup .*si_err=\"[^\"]*$1" "$D" >/dev/null; }
check "says why (si_err='$(si_of si_err)')" says "not found"

# --- 3. a workspace that is not a workspace ------------------------------------
step "3. a malformed workspace"
launch ":$OC_TEST_PORT" "alice:pw"
answers ":$OC_TEST_PORT"
check "lands on the sign-in view" waitfor 10 on_signin
check "window is visible" waitfor 10 shown
check "says why (si_err='$(si_of si_err)')" says "invalid workspace"

# --- 4. a workspace that resolves but where nothing is listening ---------------
# Resolving succeeds, so this is not an address the client refuses. The sign-in
# card asks the workspace how it signs people in before it starts a client, and
# nothing answers: it says why on the card and starts none. A client that did
# start would say why itself (last_error). What must hold is that the window
# shows, the reason is said, and never more than one client runs.
step "4. a workspace nothing answers at"
launch "127.0.0.1:1" "alice:pw"
answers "127.0.0.1:1"
reason_of() { local r; r="$(si_of si_err)"; [ -n "$r" ] || r="$(fieldq "$D" '^error_seq=' last_error)"; printf '%s' "$r"; }
has_reason() { gui_snap && [ -n "$(reason_of)" ]; }
waitfor 10 has_reason
check "says why it cannot connect ('$(reason_of)')" [ -n "$(reason_of)" ]
check "window is visible" waitfor 10 shown
check "no more than one client started ($(startup_of started))" [ "$(startup_of started)" -le 1 ] 2>/dev/null
gui_end
