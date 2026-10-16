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
# The daemon and the clients these scripts start run in development mode
# (CONFIG.md "Mode"): the fixtures bootstrap their users (OPENCHIME_DEV_USERS)
# and the clients read their inputs (OPENCHIME_DEV_*), which no other mode
# does; they sign in as a person does, on the daemon's pages.
export OPENCHIME_MODE=development
case "$PLAT" in
  win32) DRIVE="$HERE/scripts/gui/drive.sh"; PAIR="$HERE/scripts/gui/pair.sh" ;;
  web)   DRIVE="$HERE/scripts/web/drive.sh"; PAIR="$HERE/scripts/web/pair.sh" ;;
  *) echo "lib.sh: OC_TEST_PLATFORM must be win32 or web, not '$PLAT'" >&2; exit 2 ;;
esac
# A driver of one's own, for a one-off (a copy with more logging, say).
DRIVE="${OC_TEST_DRIVE:-$DRIVE}"
PAIR="${OC_TEST_PAIR:-$PAIR}"

# THE BROWSER, PLAYED FOR THE PERSON (AUTH.md §8.10). A local account signs in
# on the daemon's own page, which the Win32 client opens through its loopback
# tunnel; under the test hook the client writes that URL to <test dir>/
# signin_url.txt instead of opening a tab, and PowerShell on Windows -- where
# the client's loopback is -- fetches the page and posts the form, following
# the redirect back to the client's callback. The web client is its own
# browser (OPENCHIME_DEV_SIGNIN, scripts/web/drive.sh) and needs none of this.
#   play_browser <lin-dir> <user> <password>   # waits for the URL, signs in
#   sign_in_card <drive...> -- <lin-dir> <user> <password>
#       drives the card as the person would: Sign in (step 1 resolves and
#       probes the workspace), Trust and connect where the daemon's certificate
#       is asked about, Sign in again (step 2), then the page the browser is
#       played against. Each press once, from the card's own state in the dump,
#       so nothing is pressed twice -- a second press while the browser is
#       awaited cancels the sign-in.
sign_in_card() {
  local drv=(); while [ "$1" != "--" ]; do drv+=("$1"); shift; done; shift
  local ldir="$1" user="$2" pass="$3" i S step br cn sent1=0 sent2=0 line dpi l t r b
  for i in $(seq 1 200); do
    "${drv[@]}" dump sign_in_card >/dev/null 2>&1 || true
    S="$(cat "$ldir/sign_in_card.txt" 2>/dev/null || true)"
    # Signed in, and not merely in another workspace with this one's card over
    # it (a stored session elsewhere is authed=1 too).
    grep -q '^authed=1' <<<"$S" && ! grep -qE 'si_overlay=1|^view=100 ' <<<"$S" && return 0
    step="$(sed -n 's/^signin step=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
    cn="$(sed -n 's/^signin .*connecting=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
    br="$(sed -n 's/^signin .*browser=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
    if grep -q '^confirm open=1 act=13' <<<"$S" &&
       line="$(grep '^a11yitem modal.button.trust and connect ' <<<"$S")"; then
      dpi="$(sed -n 's/.* dpi=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
      read -r _ _ _ _ l t r b <<<"$line"
      [ "${dpi:-0}" -gt 0 ] && "${drv[@]}" click "$(( (l + r) * 48 / dpi ))" "$(( (t + b) * 48 / dpi ))" >/dev/null 2>&1
      sleep 0.3; continue
    fi
    if [ "$br" = 1 ] && [ -s "$ldir/signin_url.txt" ]; then
      play_browser "$ldir" "$user" "$pass"; return $?
    fi
    if [ "$cn" = 1 ]; then sleep 0.2; continue; fi
    if [ "$step" = 1 ] && [ "$sent1" = 0 ]; then sent1=1; "${drv[@]}" sisubmit >/dev/null 2>&1 || true; sleep 0.3; continue; fi
    if [ "$step" = 2 ] && [ "$sent2" = 0 ]; then sent2=1; rm -f "$ldir/signin_url.txt"; "${drv[@]}" sisubmit >/dev/null 2>&1 || true; sleep 0.3; continue; fi
    sleep 0.2
  done
  echo "sign_in_card: the card did not get to the page: $(grep -E '^signin |^startup|^confirm' <<<"$S" | tr '\n' ' ')" >&2
  return 1
}
play_browser() {
  local ldir="$1" user="$2" pass="$3" url="" i
  for i in $(seq 1 150); do
    [ -s "$ldir/signin_url.txt" ] && { url="$(tr -d '\r\n' < "$ldir/signin_url.txt")"; break; }
    sleep 0.1
  done
  [ -n "$url" ] || { echo "play_browser: the client published no sign-in URL in $ldir" >&2; return 1; }
  rm -f "$ldir/signin_url.txt"
  local base="${url%%\?*}"; base="${base%/*}"
  local origin; origin="$(python3 -c 'import sys,urllib.parse as u;p=u.urlparse(sys.argv[1]);print(p.scheme+"://"+p.netloc)' "$url")"
  local q="${url#*\?}"
  local body; body="$(python3 -c 'import sys,urllib.parse as u;q=dict(u.parse_qsl(sys.argv[1]));print(u.urlencode({"redirect_uri":q.get("redirect_uri",""),"nonce":q.get("nonce",""),"username":sys.argv[2],"password":sys.argv[3]}))' "$q" "$user" "$pass")"
  local ps="$ldir/play_browser.ps1"
  cat > "$ps" <<'PS'
param([string]$Get, [string]$Post, [string]$Origin, [string]$Body)
$ErrorActionPreference = 'Stop'
$null = Invoke-WebRequest -UseBasicParsing -Uri $Get
$r = Invoke-WebRequest -UseBasicParsing -Uri $Post -Method POST -Body $Body `
     -ContentType 'application/x-www-form-urlencoded' -Headers @{ Origin = $Origin }
$r.Content
PS
  local wps; wps="$(wslpath -w "$ps" 2>/dev/null || printf '%s' "$ps")"
  local out; out="$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$wps" \
      -Get "$url" -Post "$base/signin" -Origin "$origin" -Body "$body" 2>&1 | tr -d '\r' || true)"
  grep -q 'You are signed in' <<<"$out" || { echo "play_browser: the page did not sign $user in: $(grep -o 'class="msg"[^<]*' <<<"$out" | head -1)" >&2; return 1; }
  return 0
}

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
# `env OPENCHIME_PORT=... openchimed`, so its own cmdline carries no port
# and `pkill -f OPENCHIME_PORT=9500` matches nothing while looking exactly
# like "nothing to kill". Waits for the port to free; 1 if it does not.
kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PORT=$OC_TEST_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_TEST_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
