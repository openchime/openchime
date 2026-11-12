#!/usr/bin/env bash
# Drive the web client as tools/gui/drive.sh drives the Windows one (docs/
# TESTING.md, "Driving the web client"): the same subcommands, the same verbs,
# the same ack contract, so a feature script runs against either by setting
# OC_TEST_DRIVE. The client is the daemon's own /app/ page in a headless
# Chromium held by tools/web/drive.mjs; the command channel is a directory
# of files, as the Windows client's is.
#
#   tools/web/drive.sh launch [ws] [user:pass]   # the daemon, then a page signed in as alice
#   tools/web/drive.sh devaddr                   # the workspace `launch` defaults to
#   tools/web/drive.sh <cmd...>                  # one verb, wait for its ack
#   tools/web/drive.sh sysclip text <w>|png <p>  # the browser's clipboard, as another program sets it
#   tools/web/drive.sh syspaste | sysclipread    # the keyboard's Ctrl+V; what the clipboard holds
#   tools/web/drive.sh shot <name> | dump <name> # into $GUI_DRIVE_OUT / to stdout
#   tools/web/drive.sh kill                      # every driver for this client, and its browser
#   tools/web/drive.sh held                      # 1 and the pids if a driver still serves it
#
# OC_TEST_WHO=a|b names the client for a pair (its own page and channel);
# OC_TEST_PORT, OC_TEST_DIR, OC_TEST_WS, OC_TEST_USERS as tools/gui/drive.sh; the
# OPENCHIME_DEV_* switches reach the page (tools/web/drive.mjs). Needs node, the
# playwright-core package (npm install, from package.json) and a Chromium:
# CHROME names it, else the installed google-chrome on PATH.
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
  command -v google-chrome || true
}

daemon_pid() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PORT=$OC_TEST_PORT" && { echo "$p"; return; }
  done
}

# The drivers serving directory $1: by their command line, which names it.
drivers_of() { pgrep -f "tools/web/drive.mjs $1 " 2>/dev/null || true; }
# End the processes named: SIGTERM, up to 3 s for each to exit, then SIGKILL.
# `forced` counts those that had to be killed.
stop_pids() {
  [ $# -gt 0 ] || return 0
  kill "$@" 2>/dev/null || true
  for _ in $(seq 1 30); do
    left=""; for p in "$@"; do kill -0 "$p" 2>/dev/null && left="$left $p"; done
    [ -z "$left" ] && return 0
    sleep 0.1
  done
  for p in $left; do kill -9 "$p" 2>/dev/null && forced=$((forced + 1)); done
  for _ in $(seq 1 20); do
    alive=0; for p in $left; do kill -0 "$p" 2>/dev/null && alive=1; done
    [ "$alive" = 0 ] && return 0
    sleep 0.1
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
    # Both halves built, as tools/gui/drive.sh builds both: the daemon carries the client.
    if [ "${OC_TEST_NO_BUILD:-0}" != "1" ]; then
      make -C "$HERE" daemon >/dev/null || { echo "daemon build FAILED" >&2; exit 1; }
    fi
    [ -d "$HERE/node_modules/playwright-core" ] || (cd "$HERE" && npm install --no-audit --no-fund --silent)
    # The daemon on this port, restarted when stale or absent (tools/gui/drive.sh's rule).
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
    # One page per client name: the old one closed first, and none left after:
    # two drivers on one directory race for each command.
    "$0" kill >/dev/null 2>&1 || true
    "$0" held >/dev/null || { echo "launch refused: drivers still serve $DIR (pids $("$0" held))" >&2; exit 1; }
    rm -f "$DIR"/cmd "$DIR"/ack "$DIR"/quit "$DIR"/drive.log
    host="${ws%:*}"; port="${ws##*:}"
    url="https://${ws}/app/?host=${host}&port=${port}&env=OPENCHIME_DEV_SIGNIN%3D$(python3 -c 'import sys,urllib.parse as u;print(u.quote(sys.argv[1],safe=""))' "$cred")"
    setsid node "$HERE/tools/web/drive.mjs" "$DIR" "$url" "$(chromium)" > "$DIR/node.log" 2>&1 < /dev/null &
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
  held)
    # Whether a driver still serves this client's directory: 1 and their pids.
    pids="$(drivers_of "$DIR" | tr '\n' ' ')"
    [ -z "${pids// /}" ] && exit 0
    echo "$pids"; exit 1 ;;
  kill)
    # By the directory it serves, not a pid file alone: a fixture that wipes
    # its directory (fresh_fixture in the feature scripts) takes the pid file
    # with it, and a driver left behind would answer the next page's verbs.
    # Every driver for $DIR, and its browser, is gone before this returns:
    # asked (`quit`), then SIGTERM, then SIGKILL for what is left after 3 s.
    mkdir -p "$DIR" 2>/dev/null || true
    : > "$DIR/quit"
    forced=0
    stop_pids $(drivers_of "$DIR")
    prof="$(printf '%s' "$(cd "$DIR" 2>/dev/null && pwd || printf '%s' "$DIR")" | sed 's|^/*||; s|[^A-Za-z0-9_.-]\+|-|g')"
    stop_pids $(pgrep -f "openchime-webdrive/$prof" 2>/dev/null || true)
    rm -f "$DIR/pid" "$DIR/quit" "$DIR/chrome.pid"
    [ "$forced" = 0 ] && echo killed || echo "killed ($forced forced)"
    exit 0 ;;
  "") echo "usage: tools/web/drive.sh launch|<cmd...>|kill" >&2; exit 2 ;;
esac

cmd="$*"
rm -f "$DIR/ack"
printf '%s' "$cmd" > "$DIR/cmd.tmp"
mv "$DIR/cmd.tmp" "$DIR/cmd"
# 16 s for a verb; a file carried onto the page first (attach, upload,
# emoji_add) gets 5 minutes, since the copy takes as long as the file is large
# -- a 120 MB one well over 16 s -- and a verb given up on while the driver is
# still copying is overwritten by the next, which is then all the client sees.
t=0; limit=800
case "${1:-}" in attach|upload|emoji_add) limit=15000 ;; esac
while [ ! -f "$DIR/ack" ] && [ "$t" -lt "$limit" ]; do sleep 0.02; t=$((t + 1)); done
ack="$(cat "$DIR/ack" 2>/dev/null || echo TIMEOUT)"
echo "ack: $ack"
if [ "$ack" = TIMEOUT ]; then
  echo "web_drive: no ack for '$1' after $((limit / 50))s: the client did not answer" >&2
  exit 1
fi
if { [ "$1" = "shot" ] || [ "$1" = "shotfull" ]; } && [ $# -eq 2 ]; then
  cp "$DIR/${2}.bmp" "$OUT/${2}.bmp" 2>/dev/null && echo "$OUT/${2}.bmp"
fi
if [ "$1" = "dump" ] && [ $# -eq 2 ]; then
  cat "$DIR/${2}.txt" 2>/dev/null
fi
