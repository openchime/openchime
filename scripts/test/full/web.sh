#!/usr/bin/env bash
# scripts/test/full/web.sh
#
# The browser client's checks (docs/TESTING.md §3.4), against a daemon built
# with the web client in it, in two parts:
#
#   client  a daemon on a fresh database with one bootstrapped owner, and a
#           headless Chromium that loads the client from the daemon's own
#           /app/ page, signing in on the daemon's page as a person does; then
#           a video message recorded, the text layer, About, and a composition
#           from the input method. The client's test hook answers through the
#           browser's console log, which this reads.
#   pages   the daemon's sign-in pages from a real browser
#           (scripts/test/lib/web/pages.mjs): the first owner signed up with
#           the setup token, a wrong password refused, a sign-in, the password
#           changed and the new one signing in, the last token presented on
#           AUTH by build/signin; then an invitation link opened cold in a
#           second browser ends signed in to the web client (invite.mjs), and
#           the client's load is watched cold and warm (load.mjs). What a
#           browser sends -- its Origin under the pages' referrer policy, its
#           handling of the 303, its cache -- is what is exercised, which the
#           suites' scripted posts cannot show.
#
# Needs: node (NODE, the pinned emsdk's) with playwright-core, Chrome (CHROME,
# else google-chrome), ./openchimed with the web client beside it (WEB=1),
# build/signin, build/demo_client, openssl, python3.
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need "${NODE:-node}" "Node.js; make passes the pinned emsdk's"
cd "$(dirname "$0")/../../.."
set +e

# --- client -----------------------------------------------------------------
(
CHROME="${CHROME:-$(command -v google-chrome || true)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "webtest: no Chrome (set CHROME)" >&2; exit 2; }
[ -f build/web/openchime.js ] || { echo "webtest: build/web is not built (make build-all PART=plain)" >&2; exit 2; }
export OPENCHIME_MODE=development   # the fixture's users (CONFIG.md "Mode")
W=build/webtest; rm -rf "$W"; mkdir -p "$W/db" "$W/blobs"
# Fresh browser profiles each run: a session kept from an earlier run is one a
# fresh database does not know, and the client would open signed out.
rm -rf build/webtest-profile; mkdir -p build/webtest-profile
PORT=18461; HPORT=18462
env OPENCHIME_DATA_DIR="$W/data" OPENCHIME_PORT=$PORT OPENCHIME_HEALTH_PORT=$HPORT \
    OPENCHIME_DEV_USERS="alice:pw:owner" OPENCHIME_SUMMARY=off \
    ./openchimed > "$W/daemon.log" 2>&1 &
D=$!
trap 'kill $D 2>/dev/null; wait $D 2>/dev/null' EXIT
# The page each check opens, with what it is asked to do after it; the page
# keeps those instructions across the sign-in (openchime.html). The daemon's
# certificate is its own, self-signed one.
APP="https://127.0.0.1:$PORT/app/?host=127.0.0.1&port=$PORT&env=OPENCHIME_DEV_SIGNIN%3Dalice%3Apw"
for _ in $(seq 1 100); do grep -q "healthz listening" "$W/daemon.log" 2>/dev/null && break; sleep 0.1; done
# The client (openchime.html): the Windows application compiled for the
# browser, signed in with the bootstrapped owner. Its test hook polls the model
# until it is signed in and logs the dump's sign-in line.
{
    URL2="$APP&hook=1&wait=authed"
    timeout 90 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --ignore-certificate-errors \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="build/webtest-profile/gui" --no-first-run --disable-background-networking \
        "$URL2" > /dev/null 2> "$W/chrome-gui.log" &
    C2=$!
    gok=0
    for _ in $(seq 1 320); do
        if grep -q '"DUMP:' "$W/chrome-gui.log" 2>/dev/null; then gok=1; break; fi
        kill -0 $C2 2>/dev/null || break
        sleep 0.25
    done
    kill $C2 2>/dev/null; wait $C2 2>/dev/null
    dump=$(grep -oE '"DUMP:[^"]*' "$W/chrome-gui.log" | head -1 | cut -c7-)
    echo "  gui: $dump"
    if ! { [ $gok = 1 ] && grep -q 'authed=1 connected=1' <<<"$dump"; }; then
        echo "webtest: FAIL (gui) -- see $W/chrome-gui.log" >&2
        exit 1
    fi
    echo "webtest: the client signed in and drew itself in the browser"
    # Each check below is judged on its own and said on its own line; the run
    # fails at the end if any did. What they wait on is the client's state,
    # never a schedule: the page takes each step when the last has happened.
    bad=0
    judge() {   # judge <what> <0|1>
        if [ "$2" = 1 ]; then echo "  ok   $1"; else echo "  FAIL $1"; bad=1; fi
    }
    # A failed page's log, to stderr: in CI the runner's workspace is gone once
    # the job ends, so its job log is the only place a failure's cause is kept.
    show_log() {   # show_log <file>
        {
            echo "----- last 60 lines of $1 -----"
            tail -n 60 "$1" 2>/dev/null || echo "(no log)"
            echo "----- end of $1 -----"
        } >&2
    }
    # Run Chrome on the page until its log says one of the patterns, the page
    # gives up on the sign-in, or Chrome ends; the outer bound only stops a hang.
    run_page() {   # run_page <url> <profile> <log> <done-pattern> [chrome flags...]
        local url=$1 prof=$2 log=$3 done=$4; shift 4
        timeout 600 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --ignore-certificate-errors \
            "$@" --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
            --user-data-dir="build/webtest-profile/$prof" --no-first-run --disable-background-networking \
            "$url" > /dev/null 2> "$log" &
        local c=$!
        while kill -0 $c 2>/dev/null; do
            grep -qE "$done|\"SIGNIN-FAIL:" "$log" 2>/dev/null && break
            sleep 0.25
        done
        kill $c 2>/dev/null; wait $c 2>/dev/null
        if grep -q '"SIGNIN-FAIL:' "$log"; then
            echo "webtest: FAIL ($prof) -- the client did not sign in: $(grep -m1 -oE '"SIGNIN-FAIL:[^"]*' "$log" | cut -c14-) -- see $log" >&2
            exit 1
        fi
    }

    # Media (WEB.md): a video message recorded from the synthetic camera and the
    # browser's (fake) microphone through Web Audio, stopped, and the file taken
    # -- the same recorder, codecs and muxer the desktop runs, in wasm. The card
    # opens and shows the camera; once the recording has begun (after the
    # countdown) the dump is polled through it until rec_elapsed has passed
    # 3000 ms, since dumping while the recorder runs has trapped the page before;
    # then the stop, followed until the file has bytes. A step's patterns take no
    # "|" or space: the lookahead is "four digits or more, none of 0000-2999".
    # The card opens only in a selected channel, and a signed-in client may
    # not have selected one yet: the first step waits for one, so `vm open`
    # is never sent to a client that would drop it.
    script='sttest~authed=1 sel=[1-9]|vm open~^vm=1 recphase=0 frame=[1-9]|vm record~^vm=1 recphase=2 rec_elapsed=(?![0-2][0-9]{3}\b)[0-9]{4,}|vm stop~^vm=1 take_bytes=[1-9]|about'
    URL3="$APP&hook=1&env=OPENCHIME_DEV_CAPTURE%3Dsynthetic&script=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$script")"
    run_page "$URL3" media "$W/chrome-media.log" '"(DUMPALL|DUMP-ERR):' \
        --use-fake-device-for-media-stream --use-fake-ui-for-media-stream --autoplay-policy=no-user-gesture-required
    grep -oE '"(ACK|UNTIL) [^"]*' "$W/chrome-media.log" | sed 's/^"/  /'
    # The text layer (sdltext/st_canvas.c): right-to-left order, clusters, line breaks.
    sttest=$(grep -oE '"ACK sttest: [^"]*' "$W/chrome-media.log" | head -1 | cut -c14-)
    # The recording's state at the end: taken when the file has bytes.
    vm=$(python3 - "$W" <<'PY'
import re, sys, base64
log = open(sys.argv[1] + '/chrome-media.log', errors='replace').read()
m = re.findall(r'"DUMPALL:([A-Za-z0-9+/=]+)', log)
if m:
    d = base64.b64decode(m[-1]).decode('utf-8', 'replace')
    for l in d.split('\n'):
        if l.startswith('vm='): print(l[:200])
PY
)
    echo "  media: ${vm:-no dump}"
    # The audio ran on the worklet (audio_web.c): the fallback would say "scriptprocessor".
    audio=$(grep -oE 'oc-web: audio [a-z]+' "$W/chrome-media.log" | head -1)
    # About OpenChime: both builds on one line, the daemon's told by SERVER_BUILD.
    about=$(grep -oE '"ACK about: [^"]*' "$W/chrome-media.log" | head -1 | cut -c13-)
    before=$bad; bad=0
    judge "text layer (sttest): ${sttest:-no answer}" "$(grep -q '^ok ' <<<"$sttest" && echo 1)"
    judge "video card opened on the camera" "$(grep -q '"UNTIL vm open: ok' "$W/chrome-media.log" && echo 1)"
    judge "recording began" "$(grep -q '"UNTIL vm record: ok' "$W/chrome-media.log" && echo 1)"
    judge "stopped and the file taken (take_bytes>0)" "$(grep -q '"UNTIL vm stop: ok' "$W/chrome-media.log" && grep -qE 'take_bytes=[1-9]' <<<"$vm" && echo 1)"
    judge "audio on the worklet: ${audio:-not reported}" "$([ "$audio" = "oc-web: audio worklet" ] && echo 1)"
    judge "about: ${about:-no answer}" "$(grep -qE '^OpenChime client [^ ]+ \(web\), server [0-9a-z]+, protocol [0-9]+$' <<<"$about" && echo 1)"
    media_bad=$bad; [ "$before" = 1 ] && bad=1

    # The input method (plat_web.c): a composition played into the page's
    # hidden input ends in the composer, as a desktop IME's does. The page
    # polls for each state; this reads what it found.
    run_page "$APP&hook=1&ime=1" ime "$W/chrome-ime.log" '"(IME|IME-ERR):'
    ime=$(grep -m1 '"IME:' "$W/chrome-ime.log" | sed 's/.*"IME://; s/", source:.*//')
    composing=$(grep -m1 '"IME-COMPOSING:' "$W/chrome-ime.log" | sed 's/.*"IME-COMPOSING://; s/", source:.*//')
    echo "  ime: ${ime:-$(grep -m1 -oE '"IME-ERR:[^"]*' "$W/chrome-ime.log" | cut -c10-)}"
    before=$bad; bad=0
    judge "composing: two units (comp=2)" "$(grep -q ' comp=2 ' <<<"$composing" && echo 1)"
    judge "committed: two characters in the composer (len=2, 日本)" "$(grep -q '^ed len=2 ' <<<"$ime" && grep -q '日本' <<<"$ime" && echo 1)"
    ime_bad=$bad; [ "$before" = 1 ] && bad=1

    if [ $bad = 0 ]; then
        echo "webtest: PASS -- the client signed in, drew itself, recorded a video message and took a composition in the browser"; exit 0
    fi
    [ "$media_bad" = 1 ] && show_log "$W/chrome-media.log"
    [ "$ime_bad" = 1 ] && show_log "$W/chrome-ime.log"
    echo "webtest: FAIL -- see the FAIL lines above, and $W/chrome-media.log and $W/chrome-ime.log" >&2
    exit 1
}
)
[ $? = 0 ] || fail "the web client failed in a headless Chrome (above)"

# --- pages ------------------------------------------------------------------
(
CHROME="${CHROME:-$(command -v google-chrome || true)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "pages: no Chrome (set CHROME)" >&2; exit 2; }
[ -d node_modules/playwright-core ] || npm install --no-audit --no-fund --silent
export OPENCHIME_MODE=development   # the fixture's users (CONFIG.md "Mode")
# Made here, not only when missing: one left from before a protocol bump would
# be refused at HELLO and look like a page failure.
make build/signin build/demo_client >/dev/null || exit 2
W=build/webpages; rm -rf "$W"; mkdir -p "$W/data"
PORT=18471; CBPORT=18473
# The sign-in's callback: a loopback listener the browser can land on, as the
# client's own listener is (AUTH.md §8.1); the token is read off the address.
# It answers HTML: a page with no content type is a download to a browser,
# which then stays where it was.
python3 - "$CBPORT" > "$W/cb.log" 2>&1 <<'PYCB' &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<!doctype html><p>callback</p>"
        self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): sys.stderr.write(self.path[:60] + "\n")
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PYCB
CB=$!
env OPENCHIME_DATA_DIR="$W/data" OPENCHIME_PORT=$PORT OPENCHIME_HEALTH_PORT=0 OPENCHIME_TTS=off OPENCHIME_STT=off \
    OPENCHIME_WORKSPACE_NAME="Pages" ./openchimed > "$W/daemon.log" 2>&1 &
D=$!
trap 'kill $D $CB 2>/dev/null; wait $D $CB 2>/dev/null' EXIT
SETUP=""
for _ in $(seq 1 200); do
    SETUP="$(sed -n 's/.*first-run setup token.*: \([0-9a-f]\{64\}\).*/\1/p' "$W/daemon.log" | head -1)"
    if [ -n "$SETUP" ] && (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then exec 3<&- 3>&-; break; fi
    sleep 0.2
done
[ -n "$SETUP" ] || { echo "pages: the daemon printed no setup token"; exit 1; }
# One PKCE pair for the run: the browser carries the challenge, the script
# presents the verifier; the daemon binds each token to it.
read -r VERIFIER CHALLENGE < <(python3 -c '
import os, base64, hashlib
v = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
c = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b"=").decode()
print(v, c)')
OUT="$(node scripts/test/lib/web/pages.mjs "https://127.0.0.1:$PORT" "$SETUP" "$CHALLENGE" "http://127.0.0.1:$CBPORT/cb" "$CHROME" 2>"$W/pages.err")"
rc=$?
printf '%s\n' "$OUT" | grep -v '^token '
[ $rc = 0 ] || { echo "pages: FAIL (see $W/pages.err)"; tail -5 "$W/pages.err"; exit 1; }
TOKEN="$(printf '%s\n' "$OUT" | sed -n 's/^token //p' | tail -1)"
WHO="$(./build/signin present 127.0.0.1 $PORT "$TOKEN" "$VERIFIER")" || { echo "  FAIL the browser's token is refused on AUTH"; exit 1; }
echo "  ok   the browser's token signs in on AUTH (user $WHO)"
# An invitation link (REQ-026): the owner mints an invite anyone may use, and a
# second browser, which has never seen the workspace, opens the link cold and
# ends signed in to the web client as a new member (scripts/test/lib/web/invite.mjs).
# The daemon serves the web client from web/ beside it (make build-all PART=plain).
[ -f web/openchime.wasm ] || { echo "pages: no web client beside the daemon (make build-all PART=plain)" >&2; exit 2; }
INVITE="$(./build/demo_client 127.0.0.1 $PORT owen granite-harbor-violet invite 0 2>>"$W/pages.err")" || { echo "  FAIL minting the invitation link"; tail -3 "$W/pages.err"; exit 1; }
OUT2="$(node scripts/test/lib/web/invite.mjs "https://127.0.0.1:$PORT" "$INVITE" "$CHROME" 2>>"$W/pages.err")"
rc=$?
printf '%s\n' "$OUT2"
[ $rc = 0 ] || { echo "pages: FAIL (see $W/pages.err)"; tail -5 "$W/pages.err"; exit 1; }
# The web client's load (WEB.md, "Loading"): the load screen's stages, gzip on
# the wire, and a second load that downloads nothing again. The certificate is
# trusted by its key, since a browser caches nothing from one it overlooks.
SPKI="$(echo | openssl s_client -connect 127.0.0.1:$PORT 2>/dev/null | openssl x509 -pubkey -noout |
        openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64)"
OUT3="$(node scripts/test/lib/web/load.mjs "https://127.0.0.1:$PORT" "$SPKI" "$CHROME" 2>>"$W/pages.err")"
rc=$?
printf '%s\n' "$OUT3"
[ $rc = 0 ] || { echo "pages: FAIL (see $W/pages.err)"; tail -5 "$W/pages.err"; exit 1; }
echo "pages: OK: the sign-in pages, an invitation link and the client's load from a real browser"
)
[ $? = 0 ] || fail "the sign-in pages failed in a real browser (above)"
ok "the web client passed in a headless Chrome"
