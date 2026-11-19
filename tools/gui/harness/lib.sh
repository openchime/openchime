# Sourced by every GUI feature script (tools/gui/features/*.sh): the platform under
# test, its drivers, and the helpers the scripts share. A script sets HERE (the
# repository root) and then `. "$HERE/tools/gui/harness/lib.sh"`.
#
# The platform is OC_TEST_PLATFORM=win32 (the default: the Windows client over
# tools/gui/harness/win32/drive.sh and pair.sh) or web (the browser client over
# tools/gui/harness/web/drive.sh and pair.sh). The verbs, the dump and the checks are
# the same on both; a script tests $PLAT where the platforms differ.
# tools/gui/run.sh runs the scripts by name on either.
# The harness's variables all begin OC_TEST_ (tools/gui/README.md); the older prefixes
# are refused by name so a stale shell does not quietly test the wrong thing.
for old in $(env | grep -oE '^OC_(DEV|PAIR|DRIVE|GUI|WEB|SMOKE)_[A-Z_]+' | sort -u); do
  echo "lib.sh: $old is not read any more; the harness's variables are OC_TEST_* (tools/gui/README.md, \"The variables\")" >&2
  exit 2
done
. "$(dirname "${BASH_SOURCE[0]}")/lock.sh"
gui_lock
# Every call to Windows' PowerShell, by name (win32/win.sh).
. "$(dirname "${BASH_SOURCE[0]}")/win32/win.sh"
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
  win32) DRIVE="$HERE/tools/gui/harness/win32/drive.sh"; PAIR="$HERE/tools/gui/harness/win32/pair.sh" ;;
  web)   DRIVE="$HERE/tools/gui/harness/web/drive.sh"; PAIR="$HERE/tools/gui/harness/web/pair.sh" ;;
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
# the redirect back to the client's callback (win_browser_post, win32/win.sh). The web client is its own
# browser (OPENCHIME_DEV_SIGNIN, tools/gui/harness/web/drive.sh) and needs none of this.
#   play_browser <lin-dir> <user> <password>   # waits for the URL, signs in
#   sign_in_card <drive...> -- <lin-dir> <user> <password> [workspace]
#       drives the card as the person would: Sign in (step 1 resolves and
#       probes the workspace), Trust and connect where the daemon's certificate
#       is asked about, Sign in again (step 2), then the page the browser is
#       played against. Each press once, from the card's own state in the dump,
#       so nothing is pressed twice -- a second press while the browser is
#       awaited cancels the sign-in. With a workspace, the card's field is set
#       to it before step 1 is submitted, so what is submitted is what was
#       meant, not whatever the field holds; and step 1 coming back with an
#       error is submitted again at once, up to five times. It fails as soon as
#       the client stops answering, not after a time: no verdict here is the
#       clock's.
sign_in_card() {
  local drv=(); while [ "$1" != "--" ]; do drv+=("$1"); shift; done; shift
  local ldir="$1" user="$2" pass="$3" ws="${4:-}" i S step br cn sent1=0 sent2=0 line dpi l t r b err tries=0
  # Until an outcome: signed in, the page played, step 1 refused five times,
  # the client silent, or its card the same fifty looks running -- stuck, by
  # what it shows, not by a clock.
  local same=0 last=""
  while :; do
    "${drv[@]}" dump sign_in_card >/dev/null 2>&1 ||
      { echo "sign_in_card: the client does not answer" >&2; return 1; }
    S="$(cat "$ldir/sign_in_card.txt" 2>/dev/null || true)"
    local now; now="$(grep -E '^signin |^startup|^confirm' <<<"$S")"
    if [ "$now" = "$last" ]; then same=$((same + 1)); else same=0; last="$now"; fi
    [ "$same" -ge 50 ] && break
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
    # Step 1 refused (unreachable, say): again, a second later, a few times.
    err="$(grep -o 'si_err="[^"]*"' <<<"$S" | head -1)"
    if [ "$step" = 1 ] && [ "$sent1" = 1 ] && [ -n "$ws" ] && [ "$err" != 'si_err=""' ] && [ -n "$err" ] &&
       [ "$tries" -lt 5 ]; then
      tries=$((tries + 1)); sent1=0; continue
    fi
    if [ "$step" = 1 ] && [ "$sent1" = 0 ]; then
      sent1=1
      [ -n "$ws" ] && { "${drv[@]}" siws "$ws" >/dev/null 2>&1 || true; }
      "${drv[@]}" sisubmit >/dev/null 2>&1 || true; sleep 0.3; continue
    fi
    if [ "$step" = 2 ] && [ "$sent2" = 0 ]; then sent2=1; rm -f "$ldir/signin_url.txt"; "${drv[@]}" sisubmit >/dev/null 2>&1 || true; sleep 0.3; continue; fi
    sleep 0.2
  done
  echo "sign_in_card: the card did not get to the page: $(grep -E '^signin |^startup|^confirm' <<<"$S" | tr '\n' ' ')" >&2
  return 1
}
play_browser() {
  local ldir="$1" user="$2" pass="$3" url=""
  # sign_in_card calls this once the client has published the URL.
  [ -s "$ldir/signin_url.txt" ] && url="$(tr -d '\r\n' < "$ldir/signin_url.txt")"
  [ -n "$url" ] || { echo "play_browser: the client published no sign-in URL in $ldir" >&2; return 1; }
  rm -f "$ldir/signin_url.txt"
  local base="${url%%\?*}"; base="${base%/*}"
  local origin; origin="$(python3 -c 'import sys,urllib.parse as u;p=u.urlparse(sys.argv[1]);print(p.scheme+"://"+p.netloc)' "$url")"
  local q="${url#*\?}"
  local body; body="$(python3 -c 'import sys,urllib.parse as u;q=dict(u.parse_qsl(sys.argv[1]));print(u.urlencode({"redirect_uri":q.get("redirect_uri",""),"nonce":q.get("nonce",""),"username":sys.argv[2],"password":sys.argv[3]}))' "$q" "$user" "$pass")"
  local out; out="$(win_browser_post "$url" "$base/signin" "$origin" "$body")"
  grep -qF 'data-oc-result="signed-in"' <<<"$out" || { echo "play_browser: the page did not sign $user in: $(grep -o 'class="msg"[^<]*' <<<"$out" | head -1)" >&2; return 1; }
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
# like "nothing to kill". Waits for the port to free and the daemon to be gone
# -- up to 15 s, since one with the speech models loaded takes seconds to leave
# -- and 1 if they are not.
#   dev_daemon_pids   # the pids of the daemons on $OC_TEST_PORT, one a line
dev_daemon_pids() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -x "OPENCHIME_PORT=$OC_TEST_PORT" >/dev/null && echo "$p"
  done
  return 0
}
kill_dev_daemon() {
  local p pids
  pids="$(dev_daemon_pids)"
  for p in $pids; do kill "$p" 2>/dev/null; done
  for _ in $(seq 1 60); do
    local alive=""
    for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done
    if [ -z "$alive" ] && ! (exec 3<>/dev/tcp/127.0.0.1/"$OC_TEST_PORT") 2>/dev/null; then return 0; fi
    sleep 0.25
  done
  return 1
}

# --- A FEATURE SCRIPT'S SHAPE (tools/gui/README.md, "A feature script") ------
# Every feature script under tools/gui/features/ is written the same way, and
# tools/gui/lint.sh holds it to that:
#
#   HERE=...; . "$HERE/tools/gui/harness/lib.sh"
#   gui_begin <name> <port> <dir> [workspace] "$@"
#   step "what this part shows"
#   gui_launch                     # one client, signed in -- or gui_pair_up
#   check "what is true" waitfor 5 <predicate>
#   ...
#   gui_end
#
# gui_begin takes down whatever an earlier run left on the script's port and
# wipes its directory; from then on, whatever the script started -- its client
# or its pair, and its fixture daemon -- is stopped when it exits, passed,
# failed, stopped by gui_abort or interrupted. `--keep` leaves them running, to
# look at afterwards. A wait is `waitfor` on what the client shows, never a
# fixed pause: a `sleep` in a feature script is a pause that is itself under
# test, and says so on its line or the line before (`# paced: <why>`).
# GUI_AT_EXIT: a command a script sets to undo what it left outside its own
# directory and processes (web_signin.sh: its workspace in the client's book),
# run on its way out, passed, failed or stopped -- while the client is still
# up, before the teardown; not with --keep, which leaves everything as it was.
GUI_NAME=""; GUI_STEP=""; GUI_KEEP=0; GUI_ENDED=0; GUI_AT_EXIT=""
gui_begin() {
  GUI_NAME="$1"; shift
  export OC_TEST_PORT="${OC_TEST_PORT:-$1}" OC_TEST_DIR="${OC_TEST_DIR:-$2}"; shift 2
  if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then export OC_TEST_WS="${OC_TEST_WS:-$1}"; shift; fi
  local a
  for a in "$@"; do
    case "$a" in
      --keep) GUI_KEEP=1 ;;
      --kill|--down) echo "$GUI_NAME: $a is the default now: a script stops what it started; --keep leaves it running" >&2 ;;
      *) echo "$GUI_NAME: unknown argument '$a' (--keep leaves the clients and daemon running)" >&2; exit 2 ;;
    esac
  done
  # Each its own dump files, in its own directory, so two scripts never read
  # each other's (a fixed /tmp name once let one script's check read another's).
  D="$OC_TEST_DIR/dump.txt"; DA="$OC_TEST_DIR/dump-a.txt"; DB="$OC_TEST_DIR/dump-b.txt"
  # Never wipe under a daemon still running: it would go on writing to the
  # deleted directory (kill_dev_daemon, above), and the run would test that.
  gui_teardown || { say "gui_$GUI_NAME: a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
  if [ "${OC_TEST_KEEP_DB:-0}" != 1 ]; then rm -rf "$OC_TEST_DIR"; fi
  mkdir -p "$OC_TEST_DIR"
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap _gui_exit EXIT
}
# What a script can have started: a pair (its clients by the pids `up`
# recorded, and its daemon), one client, and the fixture daemon on the port.
# Each is a no-op when there is nothing of its kind.
# A daemon the script holds stopped (SIGSTOP: todos.sh) is let go first, or it
# would act on no signal and the teardown would wait it out.
gui_teardown() {
  local p; for p in $(dev_daemon_pids); do kill -CONT "$p" 2>/dev/null; done
  "$PAIR" down >/dev/null 2>&1 || true
  "$DRIVE" kill >/dev/null 2>&1 || true
  kill_dev_daemon || { echo "gui_$GUI_NAME: a daemon still holds :$OC_TEST_PORT" >&2; return 1; }
}
_gui_exit() {
  local rc=$?
  trap - EXIT INT TERM
  if [ "$GUI_ENDED" != 1 ]; then
    [ "$rc" = 0 ] && rc=1      # a script that ends without gui_end has not finished
    say "gui_$GUI_NAME: STOPPED${GUI_STEP:+ at \"$GUI_STEP\"}: $((checks - fails))/$checks passed in $((SECONDS - START))s"
  fi
  if [ "$GUI_KEEP" = 1 ]; then say "(--keep: the clients and the daemon on :$OC_TEST_PORT are still running)"
  else
    [ -n "$GUI_AT_EXIT" ] && $GUI_AT_EXIT
    gui_teardown
  fi
  exit "$rc"
}
# One part of the script: printed, and named again if the script stops in it.
step() { GUI_STEP="$*"; say "== $*"; }
# The fixture, or something under it, did not come up: nothing after this
# could say anything about the feature, so the script stops here.
gui_abort() { say "  STOP $*"; exit 1; }
# The result line run.sh and a person read, and the script's status.
gui_end() {
  GUI_ENDED=1
  say ""
  if [ "$fails" = 0 ]; then say "gui_$GUI_NAME: $checks/$checks passed in $((SECONDS - START))s"; exit 0; fi
  say "gui_$GUI_NAME: FAILED: $((checks - fails))/$checks passed in $((SECONDS - START))s"; exit 1
}

# waitfor <secs> <command...>: until the command succeeds, polled four times
# a second; 1 if it never does. The command reads the client (gui_snap) itself,
# so each look is fresh.
waitfor() {
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do "$@" && return 0; sleep 0.25; t=$((t + 1)); done
  return 1
}
# gui_snap [a|b]: the client's dump, into $D -- or a pair's client's, into $DA
# or $DB. Through a file, not a pipe into grep -q (the note above). 1 when the
# client did not answer.
gui_snap() {
  case "${1:-}" in
    "") "$DRIVE" dump "$GUI_NAME" 2>/dev/null | tail -n +2 > "$D" ;;
    a)  "$PAIR" a dump "${GUI_NAME}_a" 2>/dev/null | tail -n +2 > "$DA" ;;
    b)  "$PAIR" b dump "${GUI_NAME}_b" 2>/dev/null | tail -n +2 > "$DB" ;;
  esac
}
# field <file> <line> <key>: the value of key= on the first line of a dump
# file ($D, $DA, $DB) matching the extended regex <line>, quotes taken off. A
# value with a space in it needs a reader of its own.
field() {
  grep -m1 -E "$2" "$1" 2>/dev/null | grep -o "\b$3=[^ ]*" | head -1 | cut -d= -f2- | tr -d '"'
}
# One client, signed in: `launch`, then the client's own word that it is.
# Stops the script, with the card's state, when it is not.
gui_launch() {
  "$DRIVE" launch "$@" >/dev/null 2>&1 || gui_abort "the client did not launch"
  _signed_in() { gui_snap && grep '^authed=1' "$D" >/dev/null; }
  waitfor 20 _signed_in ||
    gui_abort "the client did not sign in: $(grep -E '^signin ' "$D" 2>/dev/null | head -1)"
}
# Two clients, alice (a) and bob (b), each signed in, or the script stops.
gui_pair_up() {
  "$PAIR" up >/dev/null 2>"$OC_TEST_DIR/pair-up.err" ||
    gui_abort "the pair did not come up: $(tail -2 "$OC_TEST_DIR/pair-up.err" | tr '\n' ' ')"
}

# --- WHAT THE SCRIPTS SHARE ---------------------------------------------------
# Each of these was once written into two scripts or more; a script uses them
# and defines none of them (tools/gui/lint.sh holds it to that).
# Where one takes [a|b] first, that is a pair's client (alice or bob, over
# "$PAIR"); without it, the one client (over "$DRIVE").

# act [a|b] <verb...>: the verb, its output dropped; a client that does not
# answer it stops the script, naming the verb. Returns 0 when it answered
# (whatever it answered: `ack_of` reads the word).
act() {
  case "${1:-}" in
    a|b) "$PAIR" "$@" >/dev/null 2>&1 || gui_abort "$1's client did not answer '${*:2}'" ;;
    *)   "$DRIVE" "$@" >/dev/null 2>&1 || gui_abort "the client did not answer '$*'" ;;
  esac
}
# ack_of [a|b] <verb...>: prints the word the verb was acked with (ok, err,
# none ...); nothing when the client did not answer. Never stops the script:
# what the word must be is the caller's check.
ack_of() {
  local out
  case "${1:-}" in
    a|b) out="$("$PAIR" "$@" 2>/dev/null)" ;;
    *)   out="$("$DRIVE" "$@" 2>/dev/null)" ;;
  esac
  grep -o -m1 '^ack: [a-z]*' <<<"$out" | cut -d' ' -f2
}
# need <what> <command...>: a check (as `check`) that the rest of the script
# depends on: when it fails, the script stops there.
need() {
  local f="$fails"
  check "$@"
  [ "$fails" = "$f" ] || gui_abort "the rest depends on it: $1"
}
# dfile <a|b>: the path of that pair client's dump file ($DA or $DB).
dfile() { if [ "$1" = a ]; then printf '%s' "$DA"; else printf '%s' "$DB"; fi; }
# seen <a|b> <command...>: a fresh dump of that client (gui_snap), then the
# command over it; the command's status, 1 when the client did not answer.
seen() { local w="$1"; shift; gui_snap "$w" && "$@"; }
# fieldq <file> <line-ERE> <key>: as `field`, for a value that can hold
# spaces: key="..." read whole, quotes taken off (an unquoted value as field
# reads it). Empty when there is no such line or key.
fieldq() {
  grep -m1 -E "$2" "$1" 2>/dev/null | grep -o "\b$3=\"[^\"]*\"\|\b$3=[^ \"]*" | head -1 | cut -d= -f2- |
    sed 's/^"//; s/"$//'
}

# CLICKING WHAT THE DUMP PLACES. Rects in the dump's own lines (l,t,r,b) are
# in the units `click` takes; an `a11yitem <aid> l t r b` line, what assistive
# technology is given, is in device pixels and is scaled by the dump's dpi=.
#   click_rect [a|b] <l,t,r,b>   # a click at its centre; 1 (no click) when the
#                                # rect is empty or missing
#   aid_xy <file> <aid>          # prints "x y", the centre of that published
#                                # item in click units; 1 when it is not there
#   press [a|b] <aid>            # waits up to 10 s, on fresh dumps, for the
#                                # item to be published and clicks its centre,
#                                # as a person presses it; 1 (no click) when it
#                                # never is
# A click the client does not answer stops the script (act).
click_rect() {
  local who=() l t r b
  case "${1:-}" in a|b) who=("$1"); shift ;; esac
  IFS=, read -r l t r b <<< "${1:-}"
  l="${l%.*}"; t="${t%.*}"; r="${r%.*}"; b="${b%.*}"
  [ -n "$b" ] && [ "$r" -gt "$l" ] 2>/dev/null || return 1
  act "${who[@]}" click $(( (l + r) / 2 )) $(( (t + b) / 2 ))
}
aid_xy() {
  local line dpi l t r b
  line="$(grep '^a11yitem ' "$1" 2>/dev/null | grep -m1 -F "a11yitem $2 ")"
  [ -n "$line" ] && [ "${line#"a11yitem $2 "}" != "$line" ] || return 1
  dpi="$(grep -m1 -o '\bdpi=[0-9]*' "$1" | cut -d= -f2)"
  read -r l t r b <<<"$(awk '{print $(NF-3), $(NF-2), $(NF-1), $NF}' <<<"$line")"
  [ -n "$b" ] && [ "${dpi:-0}" -gt 0 ] || return 1
  echo "$(( (l + r) * 48 / dpi )) $(( (t + b) * 48 / dpi ))"
}
press() {
  local who=() f="$D" xy
  case "${1:-}" in a|b) who=("$1"); f="$(dfile "$1")"; shift ;; esac
  _published() { gui_snap "${who[@]}" && aid_xy "$f" "$1" >/dev/null; }
  waitfor 10 _published "$1" || return 1
  xy="$(aid_xy "$f" "$1")"
  act "${who[@]}" click ${xy}
}

# FILES THE CLIENT IS GIVEN. They are written to the Windows share: the
# Windows client is given them by its Windows path, the web client by this
# machine's (its driver carries the file onto the page).
#   cpath <name>                          # the path of $LIN_DIR/<name> as the client names it
#   make_png <file> <w>x<h> <rrggbb> [<rrggbb>]
#       a PNG of that size in one colour -- or, given two, its left half in
#       the first and its right half in the second; 1 when it is not written
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }
make_png() {
  mkdir -p "$(dirname "$1")"
  python3 -I - "$1" "${2%x*}" "${2#*x}" "$3" "${4:-$3}" <<'PY'
import struct, sys, zlib
w, h = int(sys.argv[2]), int(sys.argv[3])
a, b = bytes.fromhex(sys.argv[4]), bytes.fromhex(sys.argv[5])
rows = b''.join(b'\0' + b''.join(a if x < w // 2 else b for x in range(w)) for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
}
# bmp_px <file.bmp> <x> <y> [n]: the colours, as RRGGBB one a line, of n
# pixels (1 if not given) from (x, y) rightward, in a screenshot the client
# wrote (`shot`: 32-bit BGRA, rows top-down when its height is negative). No
# imaging library: the file's own header says where each pixel is. x and y may
# be fractional (pixel i is at int(x + i)). 1 when the file cannot be read.
bmp_px() {
  python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
off = struct.unpack_from("<I", d, 10)[0]
w, h = struct.unpack_from("<ii", d, 18)
bpp = struct.unpack_from("<H", d, 28)[0]
x, y, n = float(sys.argv[2]), int(float(sys.argv[3])), int(sys.argv[4])
row = y if h < 0 else abs(h) - 1 - y
for i in range(n):
    p = off + row * (((w * bpp + 31) // 32) * 4) + int(x + i) * (bpp // 8)
    print("%02X%02X%02X" % (d[p + 2], d[p + 1], d[p]))
' "$1" "$2" "$3" "${4:-1}" 2>/dev/null
}

# MESSAGES AND FILES, BACK FROM THE DAEMON. What the client sends is stored
# when it comes back: a conversation's line in the dump (`  ch <id> ...
# prev="<text>"`) says what its newest message is, and a message's attachment
# has its own line (`  attach msg=<mid> id=<id> name="<name>"`) once stored.
#   newest_is [a|b] <ch> <text>    # a fresh dump; 0 when conversation <ch>'s
#                                  # newest message is exactly <text>
#   send_stored [a|b] <text>       # sends <text> and waits up to 10 s for a
#                                  # conversation's newest to be it; stops the
#                                  # script when it is not
#   upload_stored <name>           # uploads $LIN_DIR/<name> (cpath) and waits
#                                  # up to 15 s for it to be stored as an
#                                  # attachment; stops the script when it is not
newest_is() {
  local who=() f="$D" line
  case "${1:-}" in a|b) who=("$1"); f="$(dfile "$1")"; shift ;; esac
  gui_snap "${who[@]}" || return 1
  line="$(grep -m1 "^  ch $1 " "$f")"
  [[ "$line" == *" prev=\"$2\""* ]]
}
send_stored() {
  local who=() f="$D"
  case "${1:-}" in a|b) who=("$1"); f="$(dfile "$1")"; shift ;; esac
  act "${who[@]}" send "$1"
  _stored() { gui_snap "${who[@]}" && grep -F "prev=\"$1\"" "$f" >/dev/null; }
  waitfor 10 _stored "$1" || gui_abort "the message \"$1\" did not come back from the daemon"
}
upload_stored() {
  act upload "$(cpath "$1")"
  _stored() { gui_snap && grep -F "name=\"$1\"" "$D" | grep -E '^  attach msg=[0-9]+ id=[1-9][0-9]* ' >/dev/null; }
  waitfor 15 _stored "$1" || gui_abort "$1 did not come back from the daemon"
}
