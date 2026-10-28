#!/usr/bin/env bash
# Run the GUI feature scripts (scripts/gui/*.sh) by name, on the Windows client
# or the browser's, and count the results (docs/TESTING.md, "Driving the Win32
# GUI" and "Driving the web client").
#
#   scripts/gui/run.sh [--win32|--web] <name>... | all
#   scripts/gui/run.sh --web smoke calls            # two scripts, in the browser
#   scripts/gui/run.sh all                          # every script, on Windows
#
# Each script starts its own fixture daemon and prints a "gui_<name>: ..." result
# line ("N/M passed", "OK: N checks", "all passed", or its failures); this
# counts exit codes. On Windows, local only: the daemon is Linux's and the
# client needs a desktop (TESTING.md says why not CI); CI runs the quick ones on
# the web. On the web, the scripts whose checks are the Windows platform's alone
# are skipped by name.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ALL="smoke startup composer drafts links keymenu members newmsg shortcuts chords reactions polls tables groups files uploads voice screenrec search_boxes calls calls_tcp balloon pdm"
WEB_OK="smoke voice screenrec uploads files calls calls_tcp search_boxes pdm chords polls tables"
plat=win32
case "${1:-}" in --win32) plat=win32; shift ;; --web) plat=web; shift ;; esac
[ $# -gt 0 ] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
names="$*"; [ "$names" = all ] && names="$ALL"
passed=0; failed=0; skipped=0; bad=""
for n in $names; do
  n="${n%.sh}"; n="${n#gui_}"
  s="$HERE/scripts/gui/$n.sh"
  [ -x "$s" ] || { echo "run.sh: no script named $n (scripts/gui/$n.sh)" >&2; failed=$((failed + 1)); bad="$bad $n"; continue; }
  if [ "$plat" = web ] && ! printf ' %s ' $WEB_OK | grep -q " $n "; then
    echo "$n: skipped (its checks are the Windows platform's)"; skipped=$((skipped + 1)); continue
  fi
  echo "== $n ($plat)"
  if OC_TEST_PLATFORM=$plat OC_TEST_NO_BUILD="${OC_TEST_NO_BUILD:-1}" "$s"; then passed=$((passed + 1)); else failed=$((failed + 1)); bad="$bad $n"; fi
done
echo "run.sh ($plat): $passed passed, $failed failed, $skipped skipped${bad:+ --$bad}"
[ $failed = 0 ]
