#!/usr/bin/env bash
# Drive the web client as scripts/gui/drive.sh drives the Windows one (docs/
# TESTING.md, "Driving the web client"): the same subcommands, the same verbs,
# the same ack contract, so a feature script runs against either by setting
# OC_TEST_DRIVE. The client is the daemon's own /app/ page in a headless
# Chromium held by scripts/web/drive.mjs; the command channel is a directory
# of files, as the Windows client's is.
#
#   scripts/web/drive.sh launch [ws] [user:pass]   # the daemon, then a page signed in as alice
#   scripts/web/drive.sh devaddr                   # the workspace `launch` defaults to
#   scripts/web/drive.sh <cmd...>                  # one verb, wait for its ack
#   scripts/web/drive.sh shot <name> | dump <name> # into $GUI_DRIVE_OUT / to stdout
#   scripts/web/drive.sh kill
#
# OC_TEST_WHO=a|b names the client for a pair (its own page and channel);
# OC_TEST_PORT, OC_TEST_DIR, OC_TEST_WS, OC_TEST_USERS as scripts/gui/drive.sh; the
# OPENCHIME_DEV_* switches reach the page (scripts/web/drive.mjs). Needs node, the
# playwright-core package (npm install, from package.json) and a Chromium:
# CHROME names it, else Playwright's download is looked for.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot}"
OC_TEST_DIR="${OC_TEST_DIR:-/tmp/openchime-dev}"
OC_TEST_PORT="${OC_TEST_PORT:-8443}"
WHO="${OC_TEST_WHO:-a}"
DIR="$OC_TEST_DIR/web-$WHO"
mkdir -p "$DIR" "$OUT"

chromium() {
  if [ -n "${CHROME:-}" ]; then printf '%s\n' "$CHROME"; return; fi
  ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux*/chrome 2>/dev/null | sort | tail -1
}

daemon_pid() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PORT=$OC_TEST_PORT" && { echo "$p"; return; }
  done
}

case "${1:-}" in
  devaddr) printf '127.0.0.1:%s\n' "$OC_TEST_PORT"; exit 0 ;;
  launch)
  # Development mode (CONFIG.md "Mode"): the fixture bootstraps its users,
  # which no other mode reads; the client signs in on the daemon's page, the
  # harness typing `cred` for the person (OPENCHIME_DEV_SIGNIN, plat_web.c).
  export OPENCHIME_MODE=development
    ws="${2:-127.0.0.1:$OC_TEST_PORT}"; cred="${3:-alice:pw}"
    # Both halves built, as scripts/gui/drive.sh builds both: the daemon carries the client.
    if [ "${OC_TEST_NO_BUILD:-0}" != "1" ]; then
      make -C "$HERE" >/dev/null || { echo "daemon build FAILED" >&2; exit 1; }
    fi
    [ -d "$HERE/node_modules/playwright-core" ] || (cd "$HERE" && npm install --no-audit --no-fund --silent)
    # The daemon on this port, restarted when stale or absent (scripts/gui/drive.sh's rule).
    if [ "${OC_TEST_NO_DAEMON:-0}" != "1" ]; then
      dpid="$(daemon_pid || true)"; stale=1
      if [ -n "$dpid" ] && (exec 3<>/dev/tcp/127.0.0.1/$OC_TEST_PORT) 2>/dev/null; then
        age=$(ps -o etimes= -p "$dpid" 2>/dev/null | tr -d ' '); started=$(( $(date +%s) - ${age:-1000000000} ))   # /proc/<pid>'s own time is its first lookup, not its start
        built=$(stat -c %Y "$HERE/openchimed" "$HERE/web/openchime.wasm" 2>/dev/null | sort -n | tail -1)   # the wasm too: read once, at start
        built=${built:-0}
        [ "$started" -ge "$built" ] && stale=0
      fi
      if [ "$stale" = 1 ]; then
        if [ -n "$dpid" ]; then
          kill "$dpid" 2>/dev/null || true
          # Gone before the next binds the port: a daemon with the speech
          # models loaded takes a few seconds to leave.
          for _ in $(seq 1 120); do kill -0 "$dpid" 2>/dev/null || break; sleep 0.25; done
        fi
        # And the port free, whoever held it.
        for _ in $(seq 1 120); do (exec 3<>/dev/tcp/127.0.0.1/$OC_TEST_PORT) 2>/dev/null || break; exec 3<&- 3>&-; sleep 0.25; done
        mkdir -p "$OC_TEST_DIR"
        env OPENCHIME_DATA_DIR="$OC_TEST_DIR/data" \
            OPENCHIME_PORT="$OC_TEST_PORT" OPENCHIME_HEALTH_PORT=0 \
            OPENCHIME_WORKSPACE_NAME="${OC_TEST_WS:-Acme HQ}" \
            OPENCHIME_DEV_USERS="${OC_TEST_USERS:-alice:pw:owner,bob:pw:member,carol:pw:member}" \
            OPENCHIME_DEV_CALL_MAX="${OC_TEST_CALL_MAX:-10}" OPENCHIME_SUMMARY=off \
            ${OC_TEST_ADVERTISE_PORT:+OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT="$OC_TEST_ADVERTISE_PORT"} \
            OPENCHIME_MANAGED_MAX_USERS=100 \
            setsid "$HERE/openchimed" > "$OC_TEST_DIR/daemon.log" 2>&1 < /dev/null &
        disown
        up=0
        for _ in $(seq 1 360); do
          (exec 3<>/dev/tcp/127.0.0.1/$OC_TEST_PORT) 2>/dev/null && { exec 3<&- 3>&-; up=1; break; }
          sleep 0.25
        done
        [ "$up" = 1 ] || { echo "the daemon is not listening on :$OC_TEST_PORT" >&2; exit 1; }
        echo "daemon restarted (was stale or absent)" >&2
      fi
    fi
    # One page per client name: the old one closed first.
    "$0" kill >/dev/null 2>&1 || true
    rm -f "$DIR"/cmd "$DIR"/ack "$DIR"/quit "$DIR"/drive.log
    host="${ws%:*}"; port="${ws##*:}"
    url="https://${ws}/app/?host=${host}&port=${port}&env=OPENCHIME_DEV_SIGNIN%3D$(python3 -c 'import sys,urllib.parse as u;print(u.quote(sys.argv[1],safe=""))' "$cred")"
    setsid node "$HERE/scripts/web/drive.mjs" "$DIR" "$url" "$(chromium)" > "$DIR/node.log" 2>&1 < /dev/null &
    echo $! > "$DIR/pid"; disown
    # Signed in before launch returns, as the Windows launch leaves a client that answers.
    for _ in $(seq 1 300); do
      grep -q 'serving' "$DIR/drive.log" 2>/dev/null && break
      # A driver that died (the page did not load) is a failed launch, said now.
      kill -0 "$(cat "$DIR/pid")" 2>/dev/null || { echo "launch failed: the driver exited (see $DIR/node.log)" >&2; exit 1; }
      sleep 0.2
    done
    for _ in $(seq 1 100); do
      "$0" dump launch-auth 2>/dev/null | grep -q 'authed=1' && { echo launched; exit 0; }
      sleep 0.3
    done
    echo "launched (not yet signed in)"; exit 0 ;;
  kill)
    # By the directory it serves, not a pid file alone: a fixture that wipes
    # its directory (fresh_fixture in the feature scripts) takes the pid file
    # with it, and a driver left behind would answer the next page's verbs.
    if [ -f "$DIR/pid" ]; then
      : > "$DIR/quit"
      for _ in $(seq 1 50); do kill -0 "$(cat "$DIR/pid")" 2>/dev/null || break; sleep 0.1; done
    fi
    for p in $(pgrep -f "scripts/web/drive.mjs $DIR " 2>/dev/null); do kill "$p" 2>/dev/null || true; done
    # The browser too, should the driver have gone without closing it: found
    # by the profile directory it was given (scripts/web/drive.mjs names it from $DIR).
    sleep 0.5
    prof="$(printf '%s' "$(cd "$DIR" 2>/dev/null && pwd || printf '%s' "$DIR")" | sed 's|^/*||; s|[^A-Za-z0-9_.-]\+|-|g')"
    for p in $(pgrep -f "openchime-webdrive/$prof" 2>/dev/null); do kill "$p" 2>/dev/null || true; done
    rm -f "$DIR/pid" "$DIR/quit" "$DIR/chrome.pid"
    echo killed; exit 0 ;;
  "") echo "usage: scripts/web/drive.sh launch|<cmd...>|kill" >&2; exit 2 ;;
esac

cmd="$*"
rm -f "$DIR/ack"
printf '%s' "$cmd" > "$DIR/cmd.tmp"
mv "$DIR/cmd.tmp" "$DIR/cmd"
t=0
while [ ! -f "$DIR/ack" ] && [ "$t" -lt 800 ]; do sleep 0.02; t=$((t + 1)); done
ack="$(cat "$DIR/ack" 2>/dev/null || echo TIMEOUT)"
echo "ack: $ack"
if [ "$ack" = TIMEOUT ]; then
  echo "web_drive: no ack for '$1' after 16s: the client did not answer" >&2
  exit 1
fi
if { [ "$1" = "shot" ] || [ "$1" = "shotfull" ]; } && [ $# -eq 2 ]; then
  cp "$DIR/${2}.bmp" "$OUT/${2}.bmp" 2>/dev/null && echo "$OUT/${2}.bmp"
fi
if [ "$1" = "dump" ] && [ $# -eq 2 ]; then
  cat "$DIR/${2}.txt" 2>/dev/null
fi
