#!/usr/bin/env bash
# Run the GUI feature scripts (tools/gui/*.sh) by name, on the Windows client
# or the browser's, and count the results (docs/TESTING.md, "Driving the Win32
# GUI" and "Driving the web client").
#
#   tools/gui/run.sh [--win32|--web] <name>... | all
#   tools/gui/run.sh --web smoke calls            # two scripts, in the browser
#   tools/gui/run.sh all                          # every script, on Windows
#
# Each script starts its own fixture daemon and prints a "gui_<name>: ..." result
# line ("N/M passed", "OK: N checks", "all passed", or its failures); this
# counts exit codes. On Windows, local only: the daemon is Linux's and the
# client needs a desktop (TESTING.md says why not CI); CI runs the quick ones on
# the web. On the web, the scripts whose checks are the Windows platform's alone
# are skipped by name.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lock.sh"
gui_lock                  # one GUI run on this machine at a time
ALL="smoke startup composer mentions drafts links keymenu members newmsg shortcuts chords reactions polls tables groups files uploads voice screenrec search_boxes calls calls_tcp camera balloon pdm workspaces navhistory permalink_w cursor emoji_admin preview timetoken spoiler collapse lightbox jumpdate whatsnew snippets admin_rows todos"
WEB_OK="smoke mentions reactions voice screenrec uploads files calls calls_tcp camera search_boxes pdm chords polls tables workspaces navhistory cursor emoji_admin preview timetoken spoiler collapse lightbox todos"
plat=win32
case "${1:-}" in --win32) plat=win32; shift ;; --web) plat=web; shift ;; esac
[ $# -gt 0 ] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
names="$*"; [ "$names" = all ] && names="$ALL"
# Take down what a script left running, whatever its own flag: the Windows
# client, every fixture daemon of this checkout (each carries OC_TEST_PORT),
# and every web driver with its browser. Left up, they hold memory the next
# build shares, and a build then crawls under the memory limit.
take_down() {
  "$HERE/tools/gui/drive.sh" kill >/dev/null 2>&1 || true
  local p pids="" i
  for p in $(pgrep -x openchimed 2>/dev/null); do
    [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = "$HERE/openchimed" ] || continue
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -q '^OC_TEST_PORT=' && pids="$pids $p"
  done
  pids="$pids $(pgrep -f "$HERE/tools/web/drive.mjs " 2>/dev/null) $(pgrep -f 'openchime-webdrive/' 2>/dev/null)"
  for p in $pids; do kill "$p" 2>/dev/null; done
  # Gone before the next script or build starts: asked, then up to 5 s, then
  # killed. A daemon's orderly shutdown takes a moment.
  for i in $(seq 1 50); do
    p=""; for p in $pids; do kill -0 "$p" 2>/dev/null && break; p=""; done
    [ -z "$p" ] && return 0
    sleep 0.1
  done
  for p in $pids; do kill -9 "$p" 2>/dev/null; done
  return 0
}
passed=0; failed=0; skipped=0; bad=""
for n in $names; do
  n="${n%.sh}"; n="${n#gui_}"
  s="$HERE/tools/gui/$n.sh"
  [ -x "$s" ] || { echo "run.sh: no script named $n (tools/gui/$n.sh)" >&2; failed=$((failed + 1)); bad="$bad $n"; continue; }
  if [ "$plat" = web ] && ! printf ' %s ' $WEB_OK | grep -q " $n "; then
    echo "$n: skipped (its checks are the Windows platform's)"; skipped=$((skipped + 1)); continue
  fi
  echo "== $n ($plat)"
  if OC_TEST_PLATFORM=$plat OC_TEST_NO_BUILD="${OC_TEST_NO_BUILD:-1}" "$s"; then passed=$((passed + 1)); else failed=$((failed + 1)); bad="$bad $n"; fi
  take_down
done
echo "run.sh ($plat): $passed passed, $failed failed, $skipped skipped${bad:+ --$bad}"
[ $failed = 0 ]
