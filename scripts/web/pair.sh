#!/usr/bin/env bash
# Two web clients at once, as scripts/gui/pair.sh runs two Windows clients:
# alice as `a` and bob as `b`, each its own page and command channel
# (scripts/web/drive.sh with OC_TEST_WHO), against one daemon. The same
# subcommands and variables as scripts/gui/pair.sh, so a paired feature script runs
# against either with OC_TEST_PAIR (docs/TESTING.md, "Driving the web client").
#
#   scripts/web/pair.sh up | a <cmd...> | b <cmd...> | down
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export OPENCHIME_MODE=development   # the fixtures (CONFIG.md "Mode")
PORT="${OC_TEST_PORT:-9600}"
HOST="${OC_TEST_HOST:-127.0.0.1}"
DEV="${OC_TEST_DIR:-/tmp/openchime-pair}"
DRIVE="$HERE/scripts/web/drive.sh"

# The driver's variables, from the pair's.
export OC_TEST_DIR="$DEV" OC_TEST_PORT="$PORT"
export OC_TEST_USERS="${OC_TEST_USERS:-alice:pw:owner,bob:pw:member}"
export OC_TEST_CALL_MAX="${OC_TEST_CALL_MAX:-10}"

case "${1:-}" in
up)
  mkdir -p "$DEV"
  # The daemon once (the first launch starts it), then the pages; a tone of
  # its own for each client's synthetic microphone, as scripts/gui/pair.sh gives them.
  OC_TEST_WHO=a OPENCHIME_DEV_TONE="${OC_TEST_TONE_A:-}" "$DRIVE" launch "$HOST:$PORT" alice:pw >/dev/null
  OC_TEST_WHO=b OPENCHIME_DEV_TONE="${OC_TEST_TONE_B:-}" OC_TEST_NO_DAEMON=1 OC_TEST_NO_BUILD=1 "$DRIVE" launch "$HOST:$PORT" bob:pw >/dev/null
  for who in a b; do
    ok=0
    for _ in $(seq 1 60); do
      if OC_TEST_WHO=$who "$DRIVE" dump settle 2>/dev/null | grep -q 'authed=1'; then ok=1; break; fi
      sleep 0.5
    done
    [ "$ok" = 1 ] || echo "warning: $who did not authenticate" >&2
  done
  echo "alice + bob up on :$PORT (web)"
  ;;
a) shift; OC_TEST_WHO=a "$DRIVE" "$@" ;;
b) shift; OC_TEST_WHO=b "$DRIVE" "$@" ;;
down)
  OC_TEST_WHO=a "$DRIVE" kill >/dev/null 2>&1 || true
  OC_TEST_WHO=b "$DRIVE" kill >/dev/null 2>&1 || true
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PORT=$PORT" || continue
    kill "$p" 2>/dev/null || true
    for _ in $(seq 1 120); do kill -0 "$p" 2>/dev/null || break; sleep 0.25; done
  done
  echo "down"
  ;;
*) echo "usage: scripts/web/pair.sh up | a <cmd...> | b <cmd...> | down" >&2; exit 2 ;;
esac
