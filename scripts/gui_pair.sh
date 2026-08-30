#!/usr/bin/env bash
# Two clients, two accounts, one workspace — for watching a message arrive.
#
# gui_drive.sh drives ONE client and enforces exactly one instance, which is
# right for a test but useless for the thing you most often want to see by eye:
# somebody else sending you something. This starts a pair, each with its own
# command channel, and leaves them running.
#
#   scripts/gui_pair.sh up            # daemon + alice + bob
#   scripts/gui_pair.sh a <cmd...>    # drive alice   (same verbs as gui_drive)
#   scripts/gui_pair.sh b <cmd...>    # drive bob
#   scripts/gui_pair.sh down
#
# IT DOES NOT KILL ANYTHING IT DID NOT START. gui_drive.sh's launch force-kills
# any running client, which is fine when the machine is a test rig and rude when
# somebody is using one — so the pair is tracked by pid and only those are
# stopped. Anything else you have open is left alone.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HERE/build/openchime.exe"
PORT="${OC_PAIR_PORT:-9600}"
DEV="${OC_PAIR_DIR:-/tmp/openchime-pair}"
PIDS="$DEV/pids"

# One command channel per client, or they read each other's commands and answer
# for the wrong window — which looks exactly like a client ignoring you.
WIN_A='C:\Windows\Temp\ocpair-a'; LIN_A='/mnt/c/Windows/Temp/ocpair-a'
WIN_B='C:\Windows\Temp\ocpair-b'; LIN_B='/mnt/c/Windows/Temp/ocpair-b'

drive() {  # drive <lin-dir> <win-dir> <cmd...>
  local dir="$1" wdir="$2"; shift 2
  # Same convenience gui_drive.sh has, for the same reason: a bare name is
  # written relative to the EXE's cwd, the command still acks "ok", and you read
  # a stale file from an earlier run without ever being told.
  local cmd="$*"
  if [ "$1" = dump ] && [ $# -eq 2 ]; then cmd="dump ${wdir}\\${2}.txt"
  elif { [ "$1" = shot ] || [ "$1" = shotfull ]; } && [ $# -eq 2 ]; then
    cmd="$1 ${wdir}\\${2}.bmp"
  fi
  rm -f "$dir/ack"
  printf '%s' "$cmd" > "$dir/cmd.tmp"; mv "$dir/cmd.tmp" "$dir/cmd"
  local t=0
  while [ ! -f "$dir/ack" ] && [ "$t" -lt 500 ]; do sleep 0.02; t=$((t + 1)); done
  local ack; ack="$(cat "$dir/ack" 2>/dev/null || echo TIMEOUT)"
  echo "ack: $ack"
  if [ "$1" = dump ] && [ $# -eq 2 ]; then cat "$dir/${2}.txt" 2>/dev/null; fi
  [ "$ack" != TIMEOUT ]
}

case "${1:-}" in
up)
  mkdir -p "$DEV" "$LIN_A" "$LIN_B"
  make -C "$HERE" >/dev/null || { echo "daemon build FAILED" >&2; exit 1; }
  make -C "$HERE" windows-gui >/dev/null || { echo "gui build FAILED" >&2; exit 1; }
  : > "$PIDS"

  if ! (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    env OPENCHIME_DB_PATH="$DEV/db" \
        OPENCHIME_TLS_CERT="$DEV/cert.pem" OPENCHIME_TLS_KEY="$DEV/key.pem" \
        OPENCHIME_BLOB_DIR="$DEV/blobs" \
        OPENCHIME_PROTO_PORT="$PORT" OPENCHIME_HEALTH_PORT=0 \
        OPENCHIME_WORKSPACE_NAME="Acme HQ" \
        OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner,bob:pw:member" \
        OPENCHIME_DEPLOYMENT_MODE=managed OPENCHIME_MAX_USERS=100 \
        setsid "$HERE/openchimed" > "$DEV/daemon.log" 2>&1 < /dev/null &
    disown
    for _ in $(seq 1 40); do
      (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && { exec 3<&- 3>&-; break; }
      sleep 0.25
    done
    echo "daemon up on :$PORT"
  else
    echo "daemon already listening on :$PORT"
  fi

  # Each client gets its OWN test dir, so the two command channels never cross.
  for who in a b; do
    case $who in
      a) user=alice; wdir=$WIN_A; ldir=$LIN_A; host=127.0.0.1 ;;
      b) user=bob;   wdir=$WIN_B; ldir=$LIN_B; host=127.0.0.1 ;;
    esac
    rm -f "$ldir"/cmd "$ldir"/ack

    # DROP THE STORED SESSION FIRST, and this is the whole trick.
    #
    # The client keeps nothing on disk (ARCH-88), so its session token lives in
    # Windows Credential Manager under "openchime:<workspace>". That is per
    # Windows user and therefore shared by every client on the machine: two
    # clients naming the same daemon share one stored session, and the second
    # signs in as whoever the first was, whatever credential it was handed.
    # Deleting the entry makes each authenticate with the credential it was
    # actually given.
    #
    # Through powershell, not cmd.exe: cmd refuses to run with a UNC working
    # directory, which is what a \\wsl.localhost path is, so the delete failed
    # silently and both clients came up as the same person.
    #
    # Naming the host differently instead does NOT work, and it is worth
    # recording why: "localhost" resolves to ::1 on Windows and the daemon is
    # IPv4, and no other 127.x address reaches it at all, because the daemon
    # runs under WSL and WSL forwards only 127.0.0.1.
    powershell.exe -NoProfile -Command "cmdkey /delete:openchime:${host}:${PORT}" \
      >/dev/null 2>&1 || true

    WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$wdir" \
      setsid "$EXE" "$host:$PORT" "$user:pw" >/dev/null 2>&1 &
    disown

    # WAIT FOR THIS ONE TO AUTHENTICATE before starting the next. The token is
    # written to the keyring a second or two AFTER auth, and the next client's
    # delete has to happen after that write or it deletes nothing. Sleeping a
    # guess raced -- and lost, in both directions depending on the machine.
    ok=0
    for _ in $(seq 1 40); do
      if drive "$ldir" "$wdir" dump settle 2>/dev/null | grep -q 'authed=1'; then ok=1; break; fi
      sleep 0.5
    done
    [ "$ok" = 1 ] || echo "warning: $user did not authenticate" >&2
  done
  sleep 2
  # Record the pair so `down` stops these and nothing else.
  powershell.exe -NoProfile -Command \
    "Get-Process openchime -EA SilentlyContinue | Sort-Object StartTime | Select-Object -Last 2 -ExpandProperty Id" \
    2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$' > "$PIDS" || true
  # Side by side, so both are visible at once — the whole point of a pair.
  # Positioned from the shell rather than through a verb: the client's `move`
  # is a mouse move, and there is no window-move verb (nor should there be —
  # nothing in the app needs to move its own window).
  { read -r pa; read -r pb; } < "$PIDS" || true
  for pair in "${pa:-} 40" "${pb:-} 980"; do
    set -- $pair
    [ -n "${1:-}" ] || continue
    powershell.exe -NoProfile -Command "
      \$sig='[DllImport(\"user32.dll\")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);'
      \$t=Add-Type -MemberDefinition \$sig -Name W -Namespace N -PassThru
      \$p=Get-Process -Id $1 -EA SilentlyContinue
      if (\$p) { \$t::MoveWindow(\$p.MainWindowHandle, $2, 60, 900, 760, \$true) | Out-Null }" \
      >/dev/null 2>&1 || true
  done
  echo "alice + bob up on :$PORT  (pids: $(tr '\n' ' ' < "$PIDS"))"
  ;;
a) shift; drive "$LIN_A" "$WIN_A" "$@" ;;
b) shift; drive "$LIN_B" "$WIN_B" "$@" ;;
down)
  # Only what `up` started. Through the app's own quit so the run ends CLEAN and
  # leaves no post-mortem: closing the window merely hides it now (REQ-138), so
  # CloseMainWindow would time out and force-kill every time.
  drive "$LIN_A" "$WIN_A" close quit >/dev/null 2>&1 || true
  drive "$LIN_B" "$WIN_B" close quit >/dev/null 2>&1 || true
  sleep 2
  if [ -f "$PIDS" ]; then
    while read -r p; do
      [ -n "$p" ] && powershell.exe -NoProfile -Command \
        "Get-Process -Id $p -EA SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1 || true
    done < "$PIDS"
  fi
  pkill -f "OPENCHIME_PROTO_PORT=$PORT" 2>/dev/null || true
  echo "pair down"
  ;;
*) echo "usage: gui_pair.sh up | a <cmd...> | b <cmd...> | down" >&2; exit 2 ;;
esac
