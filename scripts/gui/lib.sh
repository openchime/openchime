# Sourced by every GUI feature script (scripts/gui/*.sh): the platform under
# test, its drivers, and the helpers the scripts share. A script sets HERE (the
# repository root) and then `. "$HERE/scripts/gui/lib.sh"`.
#
# The platform is OC_TEST_PLATFORM=win32 (the default: the Windows client over
# scripts/gui/drive.sh and pair.sh) or web (the browser client over
# scripts/web/drive.sh and pair.sh). The verbs, the dump and the checks are
# the same on both; a script tests $PLAT where the platforms differ.
# scripts/gui/run.sh runs the scripts by name on either.
# The harness's variables all begin OC_TEST_ (TESTING.md); the older prefixes
# are refused by name so a stale shell does not quietly test the wrong thing.
for old in $(env | grep -oE '^OC_(DEV|PAIR|DRIVE|GUI|WEB|SMOKE)_[A-Z_]+' | sort -u); do
  echo "lib.sh: $old is not read any more; the harness's variables are OC_TEST_* (TESTING.md, \"Driving the Win32 GUI\")" >&2
  exit 2
done
PLAT="${OC_TEST_PLATFORM:-}"
if [ -z "$PLAT" ]; then
  if [ -n "${OC_TEST_DRIVE:-}${OC_TEST_PAIR:-}" ]; then PLAT=web; else PLAT=win32; fi
fi
# The daemon these scripts start must be a demo build (make DEMO=1): the
# fixtures sign in by password in frames and bootstrap their users, which a
# production build refuses.
if [ -x "$HERE/openchimed" ] && ! "$HERE/openchimed" --version | grep -q 'demo build'; then
  echo "lib.sh: $HERE/openchimed is not a demo build; the harness needs one: make DEMO=1" >&2
  exit 2
fi
case "$PLAT" in
  win32) DRIVE="$HERE/scripts/gui/drive.sh"; PAIR="$HERE/scripts/gui/pair.sh" ;;
  web)   DRIVE="$HERE/scripts/web/drive.sh"; PAIR="$HERE/scripts/web/pair.sh" ;;
  *) echo "lib.sh: OC_TEST_PLATFORM must be win32 or web, not '$PLAT'" >&2; exit 2 ;;
esac
# A driver of one's own, for a one-off (a copy with more logging, say).
DRIVE="${OC_TEST_DRIVE:-$DRIVE}"
PAIR="${OC_TEST_PAIR:-$PAIR}"

# A poll is `snap | grep PATTERN >/dev/null`, never `grep -q`: these scripts
# run under pipefail, and -q quits at the first match, which leaves the dump's
# writer with a closed pipe and the pipeline failing on a match it found. The
# dumps grew until that race was lost every time.
START=$SECONDS
fails=0
checks=0
say()   { printf '%s\n' "$*"; }
fail()  { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()    { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

# STOP THE DAEMON BEFORE WIPING ITS DIRECTORY. Deleting the directory under a
# running daemon does not reset it: SQLite keeps writing to the unlinked inode,
# so the workspace stays live while its files are gone. And killing it needs the
# ENVIRONMENT, not the command line -- the daemon is started as
# `env OPENCHIME_PROTO_PORT=... openchimed`, so its own cmdline carries no port
# and `pkill -f OPENCHIME_PROTO_PORT=9500` matches nothing while looking exactly
# like "nothing to kill". Waits for the port to free; 1 if it does not.
kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_TEST_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_TEST_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
