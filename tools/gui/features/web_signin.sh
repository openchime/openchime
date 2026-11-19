#!/usr/bin/env bash
# Local accounts sign in in the browser, on Windows, end to end (AUTH.md §8.10).
#
# The daemon as shipped -- it takes a password only on its own pages -- and
# the Win32 client started with no credentials. The client
# never shows a password field; it opens the daemon's pages through its loopback
# tunnel: the daemon is reached at this machine's WSL address with its
# self-signed certificate, which the person trusts in the client's question
# (ARCH-10) but a browser would not take. PowerShell on Windows plays the
# browser, from the URL the client publishes under the test hook.
#
#   1. The first owner signs up with the setup token: "Have an invite?", the
#      token typed into its dialog, the sign-up page, signed in.
#   2. Change password opens the password page; the password is changed there.
#   3. The workspace forgotten -- session and trust -- and signed in to again:
#      the certificate asked about again, the old password refused on the page,
#      the new one signing in.
#   4. A reset (AUTH.md §2): forgotten again, "Have a reset code?" cancelled
#      and the card still there, then the code typed into it, the reset page
#      opening through this client's own tunnel, setting the password and going
#      on to the sign-in, which signs in with it.
#
# Both dialogs are the real ones, opened by a click on the card's link and
# answered with Enter, and the card is checked to stay the view under them.
#
#   tools/gui/features/web_signin.sh          # run; the client and daemon stop at the end
#   tools/gui/features/web_signin.sh --keep   # ... and leave them running
#
# It builds nothing (run `make build` first) and starts the daemon and the
# client itself: the daemon as shipped, on OC_TEST_WEB_PORT (8551), and the
# client with no workspace, three times. Both are stopped on exit by lib.sh's
# teardown (the daemon by its port, the client by drive.sh kill); the
# workspace is forgotten in the client's book before it stops.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin web_signin "${OC_TEST_WEB_PORT:-8551}" /tmp/oc-web-signin "$@"

if [ "$PLAT" != win32 ]; then
  step "skipped: the Windows client's browser sign-in"
  gui_end
fi

EXE="$HERE/build/openchime.exe"
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot-web}"
PORT="$OC_TEST_PORT"
ADDR="$(ip -4 addr show eth0 | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)"
[ -n "$ADDR" ] || gui_abort "no WSL address"
WS="$ADDR:$PORT"
mkdir -p "$LIN_DIR" "$OUT" "$OC_TEST_DIR/blobs"

# The workspace out of the client's book, so the next run's client does not
# start into it. Forgetting the active workspace signs it out first and deletes
# its entry when that lands (forgetting=1 until then).
forgotten() { gui_snap && { grep -E '^workspaces=.* forgetting=0' "$D" >/dev/null || grep '^no model' "$D" >/dev/null; }; }
forget_ws() {
  "$DRIVE" wsforget "$WS" >/dev/null 2>&1 || return 0
  waitfor 10 forgotten || true
}
# On the way out, passed, failed or stopped: the workspace forgotten and the
# files this left on Windows gone (lib.sh's teardown then stops the client and
# the daemon).
cleanup() { forget_ws; rm -f "$LIN_DIR/signin_url.txt"; }
GUI_AT_EXIT=cleanup
# Every verb through lib.sh's act (a verb the client did not ack leaves nothing
# after it to read), its screenshots into $OUT; a check the rest depends on is
# lib.sh's need.
export GUI_DRIVE_OUT="$OUT"
# The dump's own lines, read off $D (each `<thing>()` takes a fresh dump).
si() { field "$D" '^signin step=' "$1"; }           # step, browser, invite, reset, copy, copied
center() {   # "l,t,r,b" -> "x y"; 1 when there is no rectangle
  local l t r b
  IFS=, read -r l t r b <<<"$1"
  [ -n "$l" ] && [ -n "$t" ] && [ -n "$r" ] && [ -n "$b" ] || return 1
  echo "$(( (${l%.*} + ${r%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 ))"
}
is_rect() { [ -n "$1" ] && [ "$1" != "0,0,0,0" ]; }
has() { grep -E "$1" "$D" >/dev/null; }             # has <regex>, in the last dump
shown()     { gui_snap && has '^startup visible=1 '; }
at_step2()  { gui_snap && [ "$(si step)" = 2 ]; }
on_card()   { gui_snap && has '^view=100 '; }
authed()    { gui_snap && has '^authed=1'; }
form_open() { gui_snap && has '^modal=form '; }
form_shut() { gui_snap && has '^modal=none '; }
url_ready() { [ -s "$LIN_DIR/signin_url.txt" ]; }
url() { tr -d '\r\n' < "$LIN_DIR/signin_url.txt"; }
# The certificate question (ARCH-10), answered Trust and connect: its button is
# found in the harness's accessibility list and pressed (lib.sh's press).
asked() { gui_snap && has '^confirm open=1 act=13'; }
trust_cert() {
  need "the certificate is asked about" waitfor 10 asked
  need "Trust and connect is there to press" press 'modal.button.trust and connect'
}
# The sign-in card's own dialogs, opened and answered as a person does: the
# link clicked, the dialog up OVER THE CARD (the attempt it starts is watched
# only while the card is the view), the value typed into its field, Enter.
# `formnext` would answer without opening it, which is how a dialog that took
# the card away went unnoticed.
real_dialog() {   # x y value shot-name
  act clickform "$1" "$2"
  need "the dialog opens" waitfor 5 form_open
  need "...over the sign-in card ($(grep -m1 '^view=' "$D"))" has '^view=100 '
  act formtype 0 "$3"
  act shotfull "$4"
  act key enter
  need "...and closes on Enter" waitfor 5 form_shut
  need "...leaving the sign-in card the view ($(grep -m1 '^view=' "$D"))" has '^view=100 '
  need "...and OK starts the sign-in in the browser ($(grep -m1 '^signin ' "$D"))" \
    has '^signin step=2 connecting=1 browser=1 '
}
# The client, started as a person starts it: no workspace, no credentials. A
# client with workspaces in its book starts hidden and shows once their
# settings arrive or a grace period ends; until then nothing is painted and the
# card's controls have no places to click. Waited for, not slept for. It is
# started without the GUI lock's descriptor (9), which it would otherwise hold
# for as long as it runs.
start_client() {
  "$DRIVE" kill >/dev/null 2>&1 || true
  rm -f "$LIN_DIR/cmd" "$LIN_DIR/ack" "$LIN_DIR/signin_url.txt"
  WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_MODE:OPENCHIME_DEV_DIR" OPENCHIME_DEV_DIR="$WIN_DIR" \
      setsid "$EXE" >/dev/null 2>&1 < /dev/null 9>&- &
  disown
  waitfor 20 shown || gui_abort "the window was never shown"
}
# Percent-encode for a form body.
enc() { python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"; }
qget() { python3 -c 'import sys,urllib.parse as u;print(u.parse_qs(u.urlparse(sys.argv[1]).query).get(sys.argv[2],[""])[0])' "$1" "$2"; }

# The browser, on Windows: GET the page, then POST its form from the page's own
# origin, following the redirect -- to the client's callback, for a sign-in
# (win_browser_post, win.sh). Prints the final page's text.
browser() {   # url form-action body
  local url="$1" action="$2" body="$3"
  local base="${url%%\?*}"; base="${base%/*}"
  local origin; origin="$(python3 -c 'import sys,urllib.parse as u;p=u.urlparse(sys.argv[1]);print(p.scheme+"://"+p.netloc)' "$url")"
  win_browser_post "$url" "$base/$action" "$origin" "$body"
}
says() { grep -F "$1" <<<"$OUTP" >/dev/null; }     # says <text>: the page the browser was left on
url_like() { case "$URL" in $1) return 0 ;; esac; return 1; }

step "the daemon, as shipped (no knob), with no owner yet"
# Started without the GUI lock's descriptor (9); OPENCHIME_PORT in its
# environment is what lib.sh's teardown finds it by.
env OPENCHIME_DATA_DIR="$OC_TEST_DIR/data" OPENCHIME_PORT="$PORT" \
    OPENCHIME_HEALTH_PORT=0 OPENCHIME_WORKSPACE_NAME="Web Sign-in" \
    OPENCHIME_TTS=0 OPENCHIME_STT=0 \
    setsid "$HERE/openchimed" > "$OC_TEST_DIR/daemon.log" 2>&1 < /dev/null 9>&- &
disown
TOKEN=""
token_printed() {
  TOKEN="$(sed -n 's/.*first-run setup token.*: \([0-9a-f]\{64\}\).*/\1/p' "$OC_TEST_DIR/daemon.log" | head -1)"
  [ -n "$TOKEN" ]
}
# The speech models load first: seconds on a busy host. The token is printed
# before the listener is up, so then wait for the port the client will ask.
waitfor 30 token_printed || gui_abort "the daemon printed no setup token: $(tail -2 "$OC_TEST_DIR/daemon.log" | tr '\n' ' ')"
listening() { (exec 3<>"/dev/tcp/$ADDR/$PORT") 2>/dev/null; }
waitfor 30 listening || gui_abort "the daemon is not listening on $ADDR:$PORT"

step "1. the owner signs up with the setup token"
start_client
act menu 80                                    # Add a workspace (the card, over any other)
act siws "$WS"
act sisubmit
trust_cert
waitfor 10 at_step2
need "the card goes on to its second step ($(grep -m1 '^signin' "$D"))" [ "$(si step)" = 2 ]
act shotfull web-1-card
INV="$(si invite)"
need "the card shows its Have an invite? link (invite=${INV:-<none>})" is_rect "$INV"
read -r IX IY < <(center "$INV")
rm -f "$LIN_DIR/signin_url.txt"
real_dialog "$IX" "$IY" "$TOKEN" web-1-invite
need "the client publishes the sign-up URL" waitfor 10 url_ready
URL="$(url)"
need "...a tunnel URL with the invitation ($URL)" url_like "http://127.0.0.1:*/p/*/signin\?*invite=$TOKEN*"
# While it waits the card offers the link to copy (a browser that did not open):
# the button is on the card, and pressing it puts the URL on the clipboard.
gui_snap
check "the card offers Copy sign-in link while it waits ($(grep -m1 '^signin' "$D"))" has '^signin .*browser=1 .*copy=[1-9]'
act sicopy
CLIP=""
clip_is_url() { CLIP="$(win_clipboard_read | tr -d '\n')"; [ "$CLIP" = "$URL" ]; }
check "...and the copied link is the sign-in URL" waitfor 5 clip_is_url
copied() { gui_snap && has '^signin .*copied=1'; }
check "...and the card says it was copied" waitfor 5 copied
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" signup "invite=$TOKEN&username=owen&password=lantern-orchard-copper&confirm=lantern-orchard-copper&redirect_uri=$(enc "$R")&nonce=$N")"
need "the sign-up page reaches the callback" says 'data-oc-result="signed-in"'
need "...and the client is signed in" waitfor 10 authed
act shotfull web-2-signed-in

step "2. Change password, on its page"
rm -f "$LIN_DIR/signin_url.txt"
act menu 31
need "the client publishes the password page's URL" waitfor 10 url_ready
URL="$(url)"
need "...a tunnel password URL ($URL)" url_like "http://127.0.0.1:*/p/*/account/password*"
OUTP="$(browser "$URL" password "username=owen&current=lantern-orchard-copper&password=granite-harbor-violet&confirm=granite-harbor-violet")"
need "the password is changed on the page" says 'data-oc-result="password-changed"'
# The change signs every device out, this one too: the client goes to the
# sign-in card and says why.
check "the client goes back to sign-in after the change" waitfor 10 on_card
check "...and says why ($(grep -m1 '^startup' "$D"))" has 'si_err="You were signed out of '

step "3. the workspace forgotten -- session and trust -- and signed in to again"
act wsforget "$WS"
# Killing the client before the forgetting lands would leave the entry,
# session and trust.
gone() { gui_snap && has '^workspaces=.* forgetting=0' && has '^authed=0'; }
need "the workspace is forgotten" waitfor 10 gone
start_client
act menu 80
act siws "$WS"
act sisubmit
trust_cert
waitfor 10 at_step2
need "the card goes on to its second step ($(grep -m1 '^signin' "$D"))" [ "$(si step)" = 2 ]
rm -f "$LIN_DIR/signin_url.txt"
act sisubmit                                   # step 2's Sign in
need "the client publishes the sign-in URL" waitfor 10 url_ready
URL="$(url)"
need "...a tunnel sign-in URL ($URL)" url_like "http://127.0.0.1:*/p/*/signin\?*"
act shotfull web-3-waiting
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" signin "username=owen&password=lantern-orchard-copper&redirect_uri=$(enc "$R")&nonce=$N")"
check "the old password is refused on the page" says 'data-oc-error="bad-credentials"'
OUTP="$(browser "$URL" signin "username=owen&password=granite-harbor-violet&redirect_uri=$(enc "$R")&nonce=$N")"
need "the new one signs in on the page" says 'data-oc-result="signed-in"'
need "...and the client is signed in" waitfor 10 authed

step "4. a reset code, entered on the card, the password set on its page"
# Made as RESET_CREDENTIAL makes one (its hash kept, a day to run), entered on
# the sign-in card as the code the administrator sent.
RESET="$(python3 - "$OC_TEST_DIR/data/openchime.db" <<'PY'
import hashlib, os, sqlite3, sys, time
raw = os.urandom(32)
db = sqlite3.connect(sys.argv[1], timeout=5)
now = int(time.time() * 1000)
db.execute("INSERT INTO credential_resets(token_hash, user_id, created_by, created_at_ms, expires_at_ms) "
           "SELECT ?, id, id, ?, ? FROM users WHERE subject='local:owen'",
           (hashlib.sha256(raw).digest(), now, now + 86400000))
db.commit()
print(raw.hex())
PY
)"
[ ${#RESET} = 64 ] || gui_abort "no reset made"
act wsforget "$WS"
need "the workspace is forgotten" waitfor 10 gone
start_client
act menu 80
# The managed suffix goes beside a bare name, and beside nothing that already
# names a host and port.
act siws acme
suffix() { gui_snap && grep -x "sisuffix shown=$1" "$D" >/dev/null; }
check "the managed suffix is beside a bare name" waitfor 5 suffix 1
act siws "$WS"
check "...and not beside the address $WS" waitfor 5 suffix 0
act sisubmit
trust_cert
waitfor 10 at_step2
need "the card goes on to its second step ($(grep -m1 '^signin' "$D"))" [ "$(si step)" = 2 ]
RST="$(si reset)"
need "the card shows its reset-code link (reset=${RST:-<none>})" is_rect "$RST"
# Named by its address, the workspace is in the heading already, so the line
# under it says nothing rather than repeating the heading or a button's label.
check "the card's subtitle repeats nothing ($(grep -m1 '^signinsub' "$D"))" has '^signinsub ""$'
act shotfull web-4-card
read -r XX XY < <(center "$RST")
# Cancel first: the card is still there to use afterwards.
act clickform "$XX" "$XY"
need "the reset-code dialog opens" waitfor 5 form_open
act key esc
need "...and Cancel closes it" waitfor 5 form_shut
check "...leaving the card the view ($(grep -m1 '^view=' "$D"))" has '^view=100 '
check "...still at its second step" [ "$(si step)" = 2 ]
rm -f "$LIN_DIR/signin_url.txt"
real_dialog "$XX" "$XY" "$RESET" web-4-reset
need "the client publishes the reset URL" waitfor 10 url_ready
URL="$(url)"
need "...a tunnel reset URL ($URL)" url_like "http://127.0.0.1:*/p/*/account/reset\?t=$RESET\&redirect_uri=*"
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" reset "t=$RESET&password=pw-reset&confirm=pw-reset&redirect_uri=$(enc "$R")&nonce=$N")"
need "the reset sets the password and goes on to the sign-in" says 'Your new password is set'
SIGNIN="${URL%%/account/reset*}/signin?redirect_uri=$(enc "$R")&nonce=$N"
OUTP="$(browser "$SIGNIN" signin "username=owen&password=pw-reset&redirect_uri=$(enc "$R")&nonce=$N")"
need "the new password signs in on the page" says 'data-oc-result="signed-in"'
check "...and the client is signed in" waitfor 10 authed

gui_end
