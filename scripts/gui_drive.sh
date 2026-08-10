#!/usr/bin/env bash
# Drive the Win32 client through its in-app test hook (OPENCHIME_TEST_DIR) — a
# file command channel that works regardless of display state. Screenshots are
# rendered by the app itself (Direct2D DC render target), so no screen-scraping.
#
#   scripts/gui_drive.sh launch [ws] [user:pass]   # start the client with the hook on
#   scripts/gui_drive.sh <cmd...>                  # send one command, wait for ack
#   scripts/gui_drive.sh shotfull <name>           # WHOLE window, children included
#   scripts/gui_drive.sh shot <name>               # D2D scene only (no children)
#   scripts/gui_drive.sh kill
#
# Commands: shot <winpath> | send <text> | channel <name> | click x y |
#           rclick x y | members | scroll <dy> | size w h | dump <winpath> |
#           formnext <v1>|<v2>|... (arm the next modal form) |
#           search [query] | find <text>
#
# `shot <name>` and `dump <name>` take a bare name; every other path is a
# Windows path.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HERE/build/openchime.exe"
WIN_DIR='C:\Windows\Temp\octest'
LIN_DIR='/mnt/c/Windows/Temp/octest'
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot}"
OC_DEV_DIR="${OC_DEV_DIR:-/tmp/openchime-dev}"
OC_DEV_PORT="${OC_DEV_PORT:-8443}"

mkdir -p "$LIN_DIR" "$OUT"

case "${1:-}" in
  launch)
    # The default follows OC_DEV_PORT, so pointing a run at a second dev daemon
    # (a smoke run that must not touch the workspace you are using) does not also
    # require repeating the address on the command line.
    ws="${2:-127.0.0.1:$OC_DEV_PORT}"; cred="${3:-alice:pw}"

    # --- Build BOTH sides, then restart the daemon on the new binary. ---------
    #
    # The client and the daemon share a wire (ARCH-61 ships them together), so a
    # client built from source N talking to a daemon still running binary N-1 is
    # not a test, it is a trap: frames decode into garbage and the only symptom
    # is "connection lost — reconnecting", which points nowhere near the cause.
    # That cost real time three times in one day before this guard existed.
    #
    # `make` is incremental, so this is free when nothing changed. Skip with
    # OC_DRIVE_NO_BUILD=1 when you deliberately want a mismatched pair (testing
    # the version-reject path, say).
    if [ "${OC_DRIVE_NO_BUILD:-0}" != "1" ]; then
      make -C "$HERE" >/dev/null || { echo "daemon build FAILED" >&2; exit 1; }
      make -C "$HERE" windows-gui >/dev/null || { echo "gui build FAILED" >&2; exit 1; }
    fi

    # Restart the daemon unless it is already running the binary we just built.
    # Comparing start time to the binary's mtime is enough and avoids bouncing a
    # daemon (and its in-memory presence) on every launch.
    if [ "${OC_DRIVE_NO_DAEMON:-0}" != "1" ]; then
      # The PORT is the question, not whether any openchimed exists: a daemon
      # started for something else (the e2e run uses 9443) satisfied a pgrep and
      # left the client with nothing to connect to — "launched" with authed=0 and
      # no explanation. Ask what the client will ask.
      listening=0
      (exec 3<>/dev/tcp/127.0.0.1/$OC_DEV_PORT) 2>/dev/null && { exec 3<&- 3>&-; listening=1; }
      dpid=$(pgrep -f "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" | head -1 || true)
      [ -z "$dpid" ] && dpid=$(pgrep -x openchimed | head -1 || true)
      stale=1
      if [ "$listening" = "1" ] && [ -n "$dpid" ]; then
        started=$(stat -c %Y "/proc/$dpid" 2>/dev/null || echo 0)
        built=$(stat -c %Y "$HERE/openchimed" 2>/dev/null || echo 0)
        [ "$started" -ge "$built" ] && stale=0
      fi
      if [ "$stale" = "1" ]; then
        [ -n "$dpid" ] && { kill "$dpid" 2>/dev/null || true; sleep 1; }
        mkdir -p "$OC_DEV_DIR"
        env OPENCHIME_DB_PATH="$OC_DEV_DIR/db" \
            OPENCHIME_TLS_CERT="$OC_DEV_DIR/cert.pem" OPENCHIME_TLS_KEY="$OC_DEV_DIR/key.pem" \
            OPENCHIME_BLOB_DIR="$OC_DEV_DIR/blobs" \
            OPENCHIME_PROTO_PORT="${OC_DEV_PORT}" OPENCHIME_HEALTH_PORT=8080 \
            OPENCHIME_WORKSPACE_NAME="${OC_DEV_WS:-Acme HQ}" \
            OPENCHIME_BOOTSTRAP_USERS="${OC_DEV_USERS:-alice:pw:owner,bob:pw:member,carol:pw:member}" \
            OPENCHIME_DEPLOYMENT_MODE=managed OPENCHIME_MAX_USERS=100 \
            setsid "$HERE/openchimed" > "$OC_DEV_DIR/daemon.log" 2>&1 < /dev/null &
        disown
        # Wait for the listener rather than sleeping a guess.
        for _ in $(seq 1 40); do
          (exec 3<>/dev/tcp/127.0.0.1/$OC_DEV_PORT) 2>/dev/null && { exec 3<&- 3>&-; break; }
          sleep 0.25
        done
        echo "daemon restarted (was stale or absent)" >&2
      fi
    fi

    # Exactly one instance, always. A leftover client reads the same command file
    # and answers for the one under test — which once produced a "crash" that was
    # really an orphan from an earlier run.
    powershell.exe -NoProfile -Command "Get-Process openchime -EA SilentlyContinue|Stop-Process -Force" >/dev/null 2>&1 || true
    sleep 1
    rm -f "$LIN_DIR"/cmd "$LIN_DIR"/ack
    # WSLENV is required for the env var to cross into the Windows process.
    WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$WIN_DIR" \
        setsid "$EXE" "$ws" "$cred" >/dev/null 2>&1 < /dev/null &
    disown; sleep 3; echo "launched"; exit 0 ;;
  kill)
    powershell.exe -NoProfile -Command "Get-Process openchime -EA SilentlyContinue|Stop-Process -Force" >/dev/null 2>&1 || true
    echo killed; exit 0 ;;
  "") echo "usage: gui_drive.sh launch|<cmd...>|kill" >&2; exit 2 ;;
esac

# Convenience: `shot foo` -> render into the scratch dir and copy back as PNG-able BMP.
if [ "$1" = "shot" ] && [ $# -eq 2 ]; then
  name="$2"; cmd="shot ${WIN_DIR}\\${name}.bmp"
elif [ "$1" = "shotfull" ] && [ $# -eq 2 ]; then
  # The whole window, native children included (PrintWindow/DWM). Use this by
  # default: `shot` re-renders the D2D scene only and cannot see the composer,
  # the find/search boxes, the sign-in fields or the emoji picker.
  name="$2"; cmd="shotfull ${WIN_DIR}\\${name}.bmp"
elif [ "$1" = "dump" ] && [ $# -eq 2 ]; then
  # Same convenience as `shot`. Without it a bare name is written relative to the
  # exe's cwd, the command still acks "ok", and you read a STALE file from an
  # earlier run without ever being told — which is exactly the kind of silent
  # wrong answer this harness exists to prevent.
  name="$2"; cmd="dump ${WIN_DIR}\\${name}.txt"
else
  cmd="$*"
fi

rm -f "$LIN_DIR/ack"
printf '%s' "$cmd" > "$LIN_DIR/cmd.tmp"
mv "$LIN_DIR/cmd.tmp" "$LIN_DIR/cmd"          # atomic handoff

for _ in $(seq 1 100); do [ -f "$LIN_DIR/ack" ] && break; sleep 0.1; done
ack="$(cat "$LIN_DIR/ack" 2>/dev/null || echo TIMEOUT)"
echo "ack: $ack"

# The contract of this wrapper is "ack means the handler ran". On timeout it means
# exactly the opposite, and exiting 0 said it ran. gui_smoke.sh parses state rather
# than status so it never noticed, but anything checking the exit code was told the
# command succeeded when the client never answered.
if [ "$ack" = TIMEOUT ]; then
  echo "gui_drive: no ack for '$1' after 10s — the client did not answer" >&2
  exit 1
fi

if { [ "$1" = "shot" ] || [ "$1" = "shotfull" ]; } && [ $# -eq 2 ]; then
  cp "$LIN_DIR/${2}.bmp" "$OUT/${2}.bmp" 2>/dev/null && echo "$OUT/${2}.bmp"
fi
if [ "$1" = "dump" ] && [ $# -eq 2 ]; then
  cat "$LIN_DIR/${2}.txt" 2>/dev/null
fi
