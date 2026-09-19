#!/usr/bin/env bash
# The Win32 composer's layout (REQ-220, REQ-223), driven and observed.
#
#   scripts/gui_composer.sh          # run, and leave the client running
#   scripts/gui_composer.sh --kill   # ... and shut it down afterwards
#
# It asserts that the message box is always tall enough for its text, however the
# text got there:
#
#   - a draft long enough to wrap, saved, is restored at the right height when the
#     client starts again -- not laid out for one line and clipped;
#   - leaving for a conversation with no draft shrinks the box to one line, and
#     coming back grows it again;
#   - a narrower or wider window re-wraps it, and the box follows.
#
# "Tall enough" is read from the dump's `ed` line: `lines=` is how many lines the
# text wraps to at the width it is drawn at, `fit_lines=` how many the box holds.
# The box grows to four lines and then scrolls, so it must hold min(lines, 4).
#
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9540}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-composer}"
export OC_DEV_WS="${OC_DEV_WS:-Composer Fixture}"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

# Stop this port's daemon before wiping its directory: SQLite would keep writing
# to the unlinked files. Found by its environment, which is where the port is.
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
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"

snap() { "$DRIVE" dump composer >/dev/null 2>&1; cat "$LIN_DIR/composer.txt" 2>/dev/null; }
ed_key() { snap | grep -m1 '^ed ' | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
MAX_LINES=4                                 # the box's tallest; past it the text scrolls
# Wait up to `ms` for the box to hold its text, with `lines` at least `min`.
fits() {  # fits <min lines> [ms]
  local min="$1" ms="${2:-6000}" t=0 l f want
  while :; do
    l="$(ed_key lines)"; f="$(ed_key fit_lines)"
    want="$l"; [ -n "$l" ] && [ "$l" -gt "$MAX_LINES" ] && want="$MAX_LINES"
    [ -n "$l" ] && [ "$l" -ge "$min" ] && [ "$want" = "$f" ] && return 0
    [ "$t" -ge "$ms" ] && { say "     lines=${l:-?} fit_lines=${f:-?}"; return 1; }
    sleep 0.1; t=$((t + 100))
  done
}
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

LONG="A draft long enough to wrap across several lines of the message box, so that the box has to be measured for more than one line when it comes back after the client starts again, which is when it used to be laid out for one line and clipped."

say "== a long draft, saved"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap | grep -q 'authed=1' && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan quiet >/dev/null           # channel 2: a conversation with no draft
for _ in $(seq 1 40); do snap | grep -q '^channels=2' && break; sleep 0.25; done
"$DRIVE" channel 1 >/dev/null
"$DRIVE" typekeys "$LONG" >/dev/null
check "typed, it wraps and the box holds it" fits 3
"$DRIVE" channel 2 >/dev/null              # leaving writes the draft
check "in a conversation with no draft, the box is one line" fits 1
[ "$(ed_key lines)" = 1 ] || fail "...but it is $(ed_key lines) lines"

say "== the client starts again"
OC_DRIVE_NO_BUILD=1 OC_DRIVE_NO_DAEMON=1 "$DRIVE" launch >/dev/null 2>&1
for _ in $(seq 1 80); do snap | grep -q 'authed=1' && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null
restored() { [ "$(ed_key len)" -gt 100 ] 2>/dev/null && fits 3; }
check "the draft comes back at its height, not clipped to one line" restored
"$DRIVE" shot composer_restored >/dev/null

say "== away and back, narrower and wider"
"$DRIVE" channel 2 >/dev/null
one() { fits 1 && [ "$(ed_key lines)" = 1 ]; }
check "away: one line" one
"$DRIVE" channel 1 >/dev/null
check "back: the draft's height again" fits 3
"$DRIVE" size 900 800 >/dev/null
n1="$(ed_key lines)"
check "narrower, the box follows the re-wrap" fits 3
"$DRIVE" size 1500 800 >/dev/null
wider() { fits 2 && [ "$(ed_key lines)" -lt "${n1:-99}" ]; }
check "wider, fewer lines, and the box follows" wider
"$DRIVE" size 1100 800 >/dev/null

say ""
say "gui_composer: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
