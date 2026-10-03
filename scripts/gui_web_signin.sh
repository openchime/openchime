#!/usr/bin/env bash
# Local accounts sign in in the browser, on Windows, end to end (AUTH.md §8.10).
#
# A daemon WITHOUT the test knob -- as shipped: it takes a password only on its
# own pages -- and the Win32 client started with no credentials. The client
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
#   scripts/gui_web_signin.sh            # builds nothing; run `make windows-gui` first
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HERE/build/openchime.exe"
WIN_DIR='C:\Windows\Temp\octest'
LIN_DIR='/mnt/c/Windows/Temp/octest'
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot-web}"
PORT="${OC_WEB_PORT:-8551}"
D=/tmp/oc-web-signin
ADDR="$(ip -4 addr show eth0 | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)"
[ -n "$ADDR" ] || { echo "FAIL: no WSL address" >&2; exit 1; }
WS="$ADDR:$PORT"
mkdir -p "$LIN_DIR" "$OUT"

drive() { GUI_DRIVE_OUT="$OUT" "$HERE/scripts/gui_drive.sh" "$@" >/dev/null; }
state() { drive dump "$1"; cat "$LIN_DIR/$1.txt"; }
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }
field() { sed -n "s/.*$2=\([^ ]*\).*/\1/p" <<<"$1" | head -1; }
center() {   # "l,t,r,b" -> "x y"
    IFS=, read -r l t r b <<<"$1"
    echo "$(( (${l%.*} + ${r%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 ))"
}
# The URL the client published for the browser (the test hook's file).
await_url() {
    for _ in $(seq 1 100); do
        [ -s "$LIN_DIR/signin_url.txt" ] && { tr -d '\r\n' < "$LIN_DIR/signin_url.txt"; return 0; }
        sleep 0.1
    done
    return 1
}
# The certificate question (ARCH-10), answered Trust and connect: its button is
# found in the harness's accessibility list, in pixels, and clicked in scene
# units.
trust_cert() {
    local S line dpi
    for _ in $(seq 1 50); do S="$(state wt)"; grep -q '^confirm open=1 act=13' <<<"$S" && break; sleep 0.2; done
    grep -q '^confirm open=1 act=13' <<<"$S" || fail "the certificate was not asked about"
    line="$(grep '^a11yitem modal.button.trust and connect ' <<<"$S")" || fail "no Trust and connect button"
    dpi="$(sed -n 's/.* dpi=\([0-9]*\).*/\1/p' <<<"$S" | head -1)"
    read -r _ _ _ _ l t r b <<<"$line"
    drive click "$(( (l + r) * 48 / dpi ))" "$(( (t + b) * 48 / dpi ))"
}
# The sign-in card's own dialogs, opened and answered as a person does: the
# link clicked, the dialog up OVER THE CARD (the attempt it starts is watched
# only while the card is the view), the value typed into its field, Enter.
# `formnext` would answer without opening it, which is how a dialog that took
# the card away went unnoticed.
await_form() {
    for _ in $(seq 1 50); do
        S="$(state wf)"; grep -q '^modal=form ' <<<"$S" && return 0; sleep 0.1
    done
    return 1
}
real_dialog() {   # x y value shot-name
    drive clickform "$1" "$2"
    await_form || fail "the dialog did not open"
    grep -q '^view=100 ' <<<"$S" || fail "the dialog took the sign-in card away: $(grep '^view=' <<<"$S")"
    drive formtype 0 "$3"
    drive shotfull "$4"
    drive key enter
    for _ in $(seq 1 50); do S="$(state wd)"; grep -q '^modal=none ' <<<"$S" && break; sleep 0.1; done
    grep -q '^modal=none ' <<<"$S" || fail "the dialog did not close on Enter"
    grep -q '^view=100 ' <<<"$S" || fail "OK took the sign-in card away: $(grep '^view=' <<<"$S")"
    grep -q '^signin step=2 connecting=1 browser=1 ' <<<"$S" ||
        fail "OK did not start the sign-in in the browser: $(grep '^signin ' <<<"$S")"
}
# The window, on screen. A client with workspaces in its book starts hidden and
# shows once their settings arrive or a grace period ends, which takes longer
# the more it has to start; until then nothing is painted and the card's
# controls have no places to click. Waited for, not slept for.
await_shown() {
    for _ in $(seq 1 100); do
        S="$(state ws0 2>/dev/null || true)"
        grep -q '^startup visible=1 ' <<<"$S" && return 0
        sleep 0.2
    done
    fail "the window was never shown"
}
# Percent-encode for a form body.
enc() { python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"; }
qget() { python3 -c 'import sys,urllib.parse as u;print(u.parse_qs(u.urlparse(sys.argv[1]).query).get(sys.argv[2],[""])[0])' "$1" "$2"; }

# The browser, on Windows: GET the page, then POST its form from the page's own
# origin, following the redirect -- to the client's callback, for a sign-in.
# Prints the final page's text.
cat > "$LIN_DIR/web.ps1" <<'PS'
param([string]$Get, [string]$Post, [string]$Origin, [string]$Body)
$ErrorActionPreference = 'Stop'
$null = Invoke-WebRequest -UseBasicParsing -Uri $Get
$r = Invoke-WebRequest -UseBasicParsing -Uri $Post -Method POST -Body $Body `
     -ContentType 'application/x-www-form-urlencoded' -Headers @{ Origin = $Origin }
$r.Content
PS
browser() {   # url form-action body
    local url="$1" action="$2" body="$3"
    local base="${url%%\?*}"; base="${base%/*}"
    local origin; origin="$(python3 -c 'import sys,urllib.parse as u;p=u.urlparse(sys.argv[1]);print(p.scheme+"://"+p.netloc)' "$url")"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WIN_DIR\\web.ps1" \
        -Get "$url" -Post "$base/$action" -Origin "$origin" -Body "$body" 2>&1 | tr -d '\r' || true
}

cleanup() {
    drive wsforget "$WS" 2>/dev/null || true
    "$HERE/scripts/gui_drive.sh" kill >/dev/null 2>&1 || true
    for p in $(pgrep -x openchimed || true); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PROTO_PORT=$PORT" && kill "$p" 2>/dev/null
    done
    rm -f "$LIN_DIR/web.ps1" "$LIN_DIR/signin_url.txt"
}

# --- the daemon, as shipped (no knob), with no owner yet ---------------------
for p in $(pgrep -x openchimed || true); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PROTO_PORT=$PORT" && kill "$p" 2>/dev/null
done
# A daemon told to stop takes a moment to let go of the port; one started into
# it fails to bind, and the client then reaches the old one on its way out.
for _ in $(seq 1 50); do
    (exec 3<>"/dev/tcp/$ADDR/$PORT") 2>/dev/null || break
    exec 3<&- 3>&-; sleep 0.2
done
rm -rf "$D"; mkdir -p "$D/blobs"
env -u OPENCHIME_TEST_PASSWORD_AUTH OPENCHIME_DB_PATH="$D/db" OPENCHIME_TLS_CERT="$D/cert.pem" \
    OPENCHIME_TLS_KEY="$D/key.pem" OPENCHIME_BLOB_DIR="$D/blobs" OPENCHIME_PROTO_PORT="$PORT" \
    OPENCHIME_HEALTH_PORT=0 OPENCHIME_WORKSPACE_NAME="Web Sign-in" OPENCHIME_DEPLOYMENT_MODE=standalone \
    OPENCHIME_TTS=0 OPENCHIME_STT=0 \
    setsid "$HERE/openchimed" > "$D/daemon.log" 2>&1 < /dev/null &
disown
TOKEN=""
for _ in $(seq 1 150); do     # the speech models load first: seconds on a busy host
    TOKEN="$(sed -n 's/.*first-run setup token.*: \([0-9a-f]\{64\}\).*/\1/p' "$D/daemon.log" | head -1)"
    [ -n "$TOKEN" ] && break; sleep 0.2
done
[ -n "$TOKEN" ] || fail "the daemon printed no setup token"
grep -q 'OPENCHIME_TEST_PASSWORD_AUTH' "$D/daemon.log" && fail "the daemon has the test knob on"
# The token is printed before the listener is up (the speech models load in
# between), so wait for the port the client will ask, not for the log.
for _ in $(seq 1 150); do
    (exec 3<>"/dev/tcp/$ADDR/$PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }
    sleep 0.2
done
(exec 3<>"/dev/tcp/$ADDR/$PORT") 2>/dev/null || fail "the daemon is not listening on $PORT"

# --- the client, with no credentials -----------------------------------------
"$HERE/scripts/gui_drive.sh" kill >/dev/null 2>&1 || true
rm -f "$LIN_DIR/cmd" "$LIN_DIR/ack" "$LIN_DIR/signin_url.txt"
WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$WIN_DIR" \
    setsid "$EXE" >/dev/null 2>&1 < /dev/null &
disown
await_shown

# 1. The owner signs up with the setup token.
drive menu 80                                    # Add a workspace (the card, over any other)
drive siws "$WS"
drive sisubmit
trust_cert
sleep 2
S="$(state w1)"
[ "$(field "$S" step)" = 2 ] || fail "no second step: $(grep '^signin' <<<"$S")"
drive shotfull web-1-card
read -r IX IY < <(center "$(field "$S" invite)")
rm -f "$LIN_DIR/signin_url.txt"
real_dialog "$IX" "$IY" "$TOKEN" web-1-invite
URL="$(await_url)" || fail "the client published no sign-up URL"
case "$URL" in http://127.0.0.1:*/p/*/signin\?*invite=$TOKEN*) ;; *) fail "not a tunnel URL with the invitation: $URL" ;; esac
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" signup "invite=$TOKEN&username=owen&password=pw-owen&confirm=pw-owen&redirect_uri=$(enc "$R")&nonce=$N")"
grep -q 'You are signed in' <<<"$OUTP" || fail "sign-up did not reach the callback: $OUTP"
for _ in $(seq 1 50); do S="$(state w2)"; grep -q '^authed=1' <<<"$S" && break; sleep 0.2; done
grep -q '^authed=1' <<<"$S" || fail "not signed in after sign-up"
drive shotfull web-2-signed-in
echo "ok: signed up through the page, signed in"

# 2. Change password, on its page.
rm -f "$LIN_DIR/signin_url.txt"
drive menu 31
URL="$(await_url)" || fail "no password page URL"
case "$URL" in http://127.0.0.1:*/p/*/account/password*) ;; *) fail "not a tunnel password URL: $URL" ;; esac
OUTP="$(browser "$URL" password "username=owen&current=pw-owen&password=pw-new&confirm=pw-new")"
grep -q 'Password changed' <<<"$OUTP" || fail "the password was not changed: $OUTP"
echo "ok: password changed on its page"

# 3. Forget the workspace -- its session and the trust -- and sign in again.
drive wsforget "$WS"
"$HERE/scripts/gui_drive.sh" kill >/dev/null 2>&1 || true
rm -f "$LIN_DIR/cmd" "$LIN_DIR/ack"
WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$WIN_DIR" \
    setsid "$EXE" >/dev/null 2>&1 < /dev/null &
disown
await_shown
drive menu 80
drive siws "$WS"
drive sisubmit
trust_cert
sleep 2
rm -f "$LIN_DIR/signin_url.txt"
drive sisubmit                                   # step 2's Sign in
URL="$(await_url)" || fail "no sign-in URL"
case "$URL" in http://127.0.0.1:*/p/*/signin\?*) ;; *) fail "not a tunnel sign-in URL: $URL" ;; esac
drive shotfull web-3-waiting
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" signin "username=owen&password=pw-owen&redirect_uri=$(enc "$R")&nonce=$N")"
grep -q 'isn&#39;t right' <<<"$OUTP" || fail "the old password was not refused: $OUTP"
OUTP="$(browser "$URL" signin "username=owen&password=pw-new&redirect_uri=$(enc "$R")&nonce=$N")"
grep -q 'You are signed in' <<<"$OUTP" || fail "the new password did not sign in: $OUTP"
for _ in $(seq 1 50); do S="$(state w3)"; grep -q '^authed=1' <<<"$S" && break; sleep 0.2; done
grep -q '^authed=1' <<<"$S" || fail "not signed in with the new password"
echo "ok: old password refused on the page, new one signed in"

# 4. A reset, made as RESET_CREDENTIAL makes one (its hash kept, a day to run),
#    entered on the sign-in card as the code the administrator sent.
RESET="$(python3 - "$D/db" <<'PY'
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
[ ${#RESET} = 64 ] || fail "no reset made"
drive wsforget "$WS"
"$HERE/scripts/gui_drive.sh" kill >/dev/null 2>&1 || true
rm -f "$LIN_DIR/cmd" "$LIN_DIR/ack"
WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$WIN_DIR" \
    setsid "$EXE" >/dev/null 2>&1 < /dev/null &
disown
await_shown
drive menu 80
drive siws "$WS"
drive sisubmit
trust_cert
sleep 2
S="$(state w4)"
[ "$(field "$S" step)" = 2 ] || fail "no second step: $(grep '^signin' <<<"$S")"
[ "$(field "$S" reset)" != "0,0,0,0" ] || fail "no reset-code link on the card"
# Named by its address, the workspace is in the heading already, so the line
# under it says nothing rather than repeating the heading or a button's label.
grep -qx 'signinsub ""' <<<"$S" || fail "the card's subtitle repeats something: $(grep '^signinsub' <<<"$S")"
drive shotfull web-4-card
read -r XX XY < <(center "$(field "$S" reset)")
# Cancel first: the card is still there to use afterwards.
drive clickform "$XX" "$XY"
await_form || fail "the reset-code dialog did not open"
drive key esc
S="$(state w4c)"
grep -q '^view=100 ' <<<"$S" || fail "Cancel in the reset-code dialog left the card: $(grep '^view=' <<<"$S")"
[ "$(field "$(grep '^signin ' <<<"$S")" step)" = 2 ] || fail "Cancel in the reset-code dialog lost step 2"
rm -f "$LIN_DIR/signin_url.txt"
real_dialog "$XX" "$XY" "$RESET" web-4-reset
URL="$(await_url)" || fail "the client published no reset URL"
case "$URL" in http://127.0.0.1:*/p/*/account/reset\?t=$RESET\&redirect_uri=*) ;;
               *) fail "not a tunnel reset URL: $URL" ;; esac
R="$(qget "$URL" redirect_uri)"; N="$(qget "$URL" nonce)"
OUTP="$(browser "$URL" reset "t=$RESET&password=pw-reset&confirm=pw-reset&redirect_uri=$(enc "$R")&nonce=$N")"
grep -q 'Your new password is set' <<<"$OUTP" || fail "the reset did not go on to the sign-in: $OUTP"
SIGNIN="${URL%%/account/reset*}/signin?redirect_uri=$(enc "$R")&nonce=$N"
OUTP="$(browser "$SIGNIN" signin "username=owen&password=pw-reset&redirect_uri=$(enc "$R")&nonce=$N")"
grep -q 'You are signed in' <<<"$OUTP" || fail "the new password did not sign in: $OUTP"
for _ in $(seq 1 50); do S="$(state w5)"; grep -q '^authed=1' <<<"$S" && break; sleep 0.2; done
grep -q '^authed=1' <<<"$S" || fail "not signed in after the reset"
echo "ok: reset code entered, password set on the page, signed in"

cleanup
echo "PASS: local sign-in in the browser (sign-up, password change, sign-in, reset)"
