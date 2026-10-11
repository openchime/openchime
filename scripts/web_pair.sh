#!/usr/bin/env bash
# Two web clients at once, as scripts/gui_pair.sh runs two Windows clients:
# alice as `a` and bob as `b`, each its own page and command channel
# (scripts/web_drive.sh with OC_WEB_WHO), against one daemon. The same
# subcommands and variables as gui_pair.sh, so a paired feature script runs
# against either with OC_GUI_PAIR (docs/TESTING.md, "Driving the web client").
#
#   scripts/web_pair.sh up | a <cmd...> | b <cmd...> | down
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${OC_PAIR_PORT:-9600}"
HOST="${OC_PAIR_HOST:-127.0.0.1}"
DEV="${OC_PAIR_DIR:-/tmp/openchime-pair}"
DRIVE="$HERE/scripts/web_drive.sh"

# The driver's variables, from the pair's.
export OC_DEV_DIR="$DEV" OC_DEV_PORT="$PORT"
export OC_DEV_USERS="${OC_PAIR_USERS:-alice:pw:owner,bob:pw:member}"
export OC_DEV_CALL_MAX="${OC_PAIR_CALL_MAX:-10}"

case "${1:-}" in
up)
  mkdir -p "$DEV"
  # The daemon once (the first launch starts it), then the pages; a tone of
  # its own for each client's synthetic microphone, as gui_pair.sh gives them.
  OC_WEB_WHO=a OPENCHIME_TEST_TONE="${OC_PAIR_TONE_A:-}" "$DRIVE" launch "$HOST:$PORT" alice:pw >/dev/null
  OC_WEB_WHO=b OPENCHIME_TEST_TONE="${OC_PAIR_TONE_B:-}" OC_DRIVE_NO_DAEMON=1 OC_DRIVE_NO_BUILD=1 "$DRIVE" launch "$HOST:$PORT" bob:pw >/dev/null
  for who in a b; do
    ok=0
    for _ in $(seq 1 60); do
      if OC_WEB_WHO=$who "$DRIVE" dump settle 2>/dev/null | grep -q 'authed=1'; then ok=1; break; fi
      sleep 0.5
    done
    [ "$ok" = 1 ] || echo "warning: $who did not authenticate" >&2
  done
  echo "alice + bob up on :$PORT (web)"
  ;;
a) shift; OC_WEB_WHO=a "$DRIVE" "$@" ;;
b) shift; OC_WEB_WHO=b "$DRIVE" "$@" ;;
down)
  OC_WEB_WHO=a "$DRIVE" kill >/dev/null 2>&1 || true
  OC_WEB_WHO=b "$DRIVE" kill >/dev/null 2>&1 || true
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PROTO_PORT=$PORT" || continue
    kill "$p" 2>/dev/null || true
    for _ in $(seq 1 120); do kill -0 "$p" 2>/dev/null || break; sleep 0.25; done
  done
  echo "down"
  ;;
*) echo "usage: web_pair.sh up | a <cmd...> | b <cmd...> | down" >&2; exit 2 ;;
esac
