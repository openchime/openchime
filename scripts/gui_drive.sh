#!/usr/bin/env bash
# Drive the Win32 client through its in-app test hook (OPENCHIME_TEST_DIR) — a
# file command channel that works regardless of display state. Screenshots are
# rendered by the app itself (Direct2D DC render target), so no screen-scraping.
#
#   scripts/gui_drive.sh launch [ws] [user:pass]   # start the client with the hook on
#   scripts/gui_drive.sh devaddr                   # the workspace `launch` defaults to
#   scripts/gui_drive.sh <cmd...>                  # send one command, wait for ack
#   scripts/gui_drive.sh shotfull <name>           # WHOLE window, children included
#   scripts/gui_drive.sh shot <name>               # D2D scene only (no children)
#   scripts/gui_drive.sh kill
#
# Commands: shot <winpath> | send <text> | channel <name> | click x y |
#           rclick x y | members | scroll <dy> | size w h | dump <winpath> |
#           formnext <v1>|<v2>|... (arm the next modal form) |
#           clickform x y (a click that opens a form; acked first) |
#           formtype <field> <text> (into the open form) |
#           search [query] | find <text> | key [ctrl+|alt+|shift+]<key> |
#           keyup <key> | mousedown x y | mouseup x y |
#           dictate ptt-down|ptt-up|free-on|free-off |
#           attach <winpath> (into the message box's upload tray)
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

# Where the Windows client reaches a daemon this machine runs. Under WSL2 that is
# the WSL machine's own address: a Windows process reaches a WSL listener at
# 127.0.0.1 only where localhost forwarding is on, and without it the client sat
# on "could not reach the server" and every script built on `launch` failed its
# first check. Anywhere else -- the daemon on the Windows host itself, say --
# 127.0.0.1. OC_DEV_HOST overrides both.
dev_host() {
  if [ -n "${OC_DEV_HOST:-}" ]; then printf '%s\n' "$OC_DEV_HOST"; return; fi
  local a=""
  if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    a="$(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
  fi
  printf '%s\n' "${a:-127.0.0.1}"
}

# A daemon reached at an address that is not loopback presents its self-signed
# certificate, and the client asks whether to trust it (ARCH-10). The dev daemon
# is the one this harness started, so the harness answers for the person: Trust
# and connect, found in the accessibility list in pixels and clicked in scene
# units -- the same click gui_web_signin.sh makes. Done when the client signs in,
# or when there is no question to answer.
trust_dev_cert() {
  local S line dpi l t r b
  "$0" restore >/dev/null 2>&1 || true      # a window never shown has no buttons to find
  for _ in $(seq 1 100); do
    "$0" dump launch-trust >/dev/null 2>&1 || true
    S="$(cat "$LIN_DIR/launch-trust.txt" 2>/dev/null || true)"
    grep -q '^authed=1' <<<"$S" && return 0
    if grep -q '^confirm open=1 act=13' <<<"$S" &&
       line="$(grep '^a11yitem modal.button.trust and connect ' <<<"$S")"; then
      dpi="$(sed -n 's/.* dpi=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
      read -r _ _ _ _ l t r b <<<"$line"
      [ "${dpi:-0}" -gt 0 ] && "$0" click "$(( (l + r) * 48 / dpi ))" "$(( (t + b) * 48 / dpi ))" >/dev/null 2>&1
    fi
    sleep 0.2
  done
  return 0                                  # the caller's own checks say what happened
}

case "${1:-}" in
  devaddr)
    # The default workspace `launch` uses, for a script that names it itself.
    printf '%s:%s\n' "$(dev_host)" "$OC_DEV_PORT"; exit 0 ;;
  launch)
    # The default follows OC_DEV_PORT, so pointing a run at a second dev daemon
    # (a smoke run that must not touch the workspace you are using) does not also
    # require repeating the address on the command line.
    ws="${2:-$(dev_host):$OC_DEV_PORT}"; cred="${3:-alice:pw}"

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
      # Named in its environment: the port is not on its command line.
      dpid=""
      for p in $(pgrep -x openchimed || true); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && { dpid=$p; break; }
      done
      # Only the daemon on THIS port is ours to restart: falling back to any
      # openchimed stopped whichever daemon another run happened to have up.
      stale=1
      if [ "$listening" = "1" ] && [ -n "$dpid" ]; then
        started=$(stat -c %Y "/proc/$dpid" 2>/dev/null || echo 0)
        built=$(stat -c %Y "$HERE/openchimed" 2>/dev/null || echo 0)
        [ "$started" -ge "$built" ] && stale=0
      fi
      if [ "$stale" = "1" ]; then
        if [ -n "$dpid" ]; then
          kill "$dpid" 2>/dev/null || true
          # Gone from the port before the next binds it, or the new one fails to
          # bind and the wait below finds the old one on its way out.
          for _ in $(seq 1 40); do
            (exec 3<>/dev/tcp/127.0.0.1/$OC_DEV_PORT) 2>/dev/null || break
            exec 3<&- 3>&-; sleep 0.25
          done
        fi
        mkdir -p "$OC_DEV_DIR"
        env OPENCHIME_DB_PATH="$OC_DEV_DIR/db" \
            OPENCHIME_TLS_CERT="$OC_DEV_DIR/cert.pem" OPENCHIME_TLS_KEY="$OC_DEV_DIR/key.pem" \
            OPENCHIME_BLOB_DIR="$OC_DEV_DIR/blobs" \
            OPENCHIME_PROTO_PORT="${OC_DEV_PORT}" OPENCHIME_HEALTH_PORT=8080 \
            OPENCHIME_TEST_PASSWORD_AUTH=1 \
            OPENCHIME_WORKSPACE_NAME="${OC_DEV_WS:-Acme HQ}" \
            OPENCHIME_BOOTSTRAP_USERS="${OC_DEV_USERS:-alice:pw:owner,bob:pw:member,carol:pw:member}" \
            OPENCHIME_DEPLOYMENT_MODE=managed OPENCHIME_MAX_USERS=100 \
            setsid "$HERE/openchimed" > "$OC_DEV_DIR/daemon.log" 2>&1 < /dev/null &
        disown
        # Wait for the listener rather than sleeping a guess -- and long enough:
        # the bootstrap accounts are hashed before it listens, so a fixture of
        # sixty people (gui_members.sh) takes half a minute, and a client started
        # before then fails its one attempt and never tries again.
        up=0
        for _ in $(seq 1 360); do
          (exec 3<>/dev/tcp/127.0.0.1/$OC_DEV_PORT) 2>/dev/null && { exec 3<&- 3>&-; up=1; break; }
          sleep 0.25
        done
        [ "$up" = 1 ] || { echo "the daemon is not listening on :$OC_DEV_PORT" >&2; exit 1; }
        echo "daemon restarted (was stale or absent)" >&2
      fi
    fi

    # Exactly one instance, always. A leftover client reads the same command file
    # and answers for the one under test — which once produced a "crash" that was
    # really an orphan from an earlier run.
    # CLOSE it, and only force it if that fails. Stop-Process -Force is a
    # TerminateProcess, which the client correctly reports as a death that ran
    # no handler — so force-killing on every launch would fill the test dir with
    # post-mortems for kills the harness did on purpose, and bury the one that
    # matters. CloseMainWindow posts WM_CLOSE, which reaches WM_DESTROY and marks
    # the run clean.
    powershell.exe -NoProfile -Command "\$p = Get-Process openchime -EA SilentlyContinue; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(3000)) { \$p | Stop-Process -Force } }" >/dev/null 2>&1 || true
    sleep 1
    rm -f "$LIN_DIR"/cmd "$LIN_DIR"/ack
    # OC_DRIVE_LOCAL=1 runs a copy from a local Windows directory: Windows refuses
    # a real camera to an executable started from the WSL share (TESTING.md).
    if [ "${OC_DRIVE_LOCAL:-0}" = "1" ]; then
      mkdir -p /mnt/c/Temp/octest-bin
      cp "$EXE" /mnt/c/Temp/octest-bin/openchime.exe
      EXE=/mnt/c/Temp/octest-bin/openchime.exe
    fi
    # WSLENV is required for the env var to cross into the Windows process. The
    # test switches cross too when set: OPENCHIME_TEST_AUDIO=synthetic,
    # OPENCHIME_TEST_MIC=<windows path to a WAV> for a microphone that speaks,
    # OPENCHIME_TEST_CAPTURE=synthetic|synthetic-camera|denied, and the rest.
    WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR:OPENCHIME_TEST_AUDIO:OPENCHIME_TEST_MIC:OPENCHIME_TEST_MIC_ECHO:OPENCHIME_TEST_CAPTURE:OPENCHIME_TEST_VIDEO_CAP_MS:OPENCHIME_TEST_SCREEN_GONE_MS" OPENCHIME_TEST_DIR="$WIN_DIR" \
        setsid "$EXE" "$ws" "$cred" >/dev/null 2>&1 < /dev/null &
    disown; sleep 3
    case "$ws" in
      127.*|localhost|localhost:*|\[::1\]*) ;;
      *) [ -n "$cred" ] && trust_dev_cert ;;
    esac
    echo "launched"; exit 0 ;;
  kill)
    # Graceful first, for the reason the launch path gives.
    powershell.exe -NoProfile -Command "\$p = Get-Process openchime -EA SilentlyContinue; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(3000)) { \$p | Stop-Process -Force } }" >/dev/null 2>&1 || true
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

# Poll finely. The client answers on its 30 ms tick, so a 100 ms sleep spent, on
# average, half of every round trip waiting for an answer that had already
# arrived — and a suite is nothing but round trips: the smoke makes tens of them
# and the regression suite hundreds. The timeout is unchanged at 10s (500 x 20ms);
# this buys latency, not patience.
t=0
while [ ! -f "$LIN_DIR/ack" ] && [ "$t" -lt 500 ]; do sleep 0.02; t=$((t + 1)); done
ack="$(cat "$LIN_DIR/ack" 2>/dev/null || echo TIMEOUT)"
echo "ack: $ack"

# The contract of this wrapper is "ack means the handler ran". On timeout it means
# exactly the opposite, and exiting 0 said it ran.
#
# CHECK THIS STATUS. gui_smoke.sh does, and it is why its failures can be read at
# face value. A caller that parses state and discards status instead lets a verb
# the client never answered go silent, and it then reports itself as the NEXT
# assertion failing — a defect blamed on whatever ran after the dropped command.
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
