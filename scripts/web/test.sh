#!/usr/bin/env bash
# The web client's smoke test (docs/WEB.md): a daemon on a fresh database with
# one bootstrapped owner, and a headless Chromium that loads the client from
# the daemon's own /app/ page, signing in on the daemon's page as a person does.
# The client's test hook answers through the browser's console log, which the
# test reads. Needs ./openchimed with the web client beside it (make web) and a
# Chrome: CHROME names it, else the installed google-chrome on PATH.
set -uo pipefail
cd "$(dirname "$0")/../.."
CHROME="${CHROME:-$(command -v google-chrome || true)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "webtest: no Chrome (set CHROME)" >&2; exit 2; }
[ -f build/web/openchime.js ] || { echo "webtest: build/web is not built (make web)" >&2; exit 2; }
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
# browser, signed in with the bootstrapped owner. Its test hook dumps the model
# after a while and takes the client's own screenshot, which is left in
# $W/gui.bmp for a person to look at.
{
    URL2="$APP&wait=authed&shot=1"
    timeout 90 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --ignore-certificate-errors \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="build/webtest-profile/gui" --no-first-run --disable-background-networking \
        "$URL2" > /dev/null 2> "$W/chrome-gui.log" &
    C2=$!
    gok=0
    for _ in $(seq 1 320); do
        if grep -q '"SHOT:' "$W/chrome-gui.log" 2>/dev/null; then gok=1; break; fi
        kill -0 $C2 2>/dev/null || break
        sleep 0.25
    done
    kill $C2 2>/dev/null; wait $C2 2>/dev/null
    dump=$(grep -oE '"DUMP:[^"]*' "$W/chrome-gui.log" | head -1 | cut -c7-)
    echo "  gui: $dump"
    python3 - "$W" <<'PY'
import re, sys, base64
w = sys.argv[1]
log = open(w + '/chrome-gui.log', errors='replace').read()
m = re.search(r'"SHOT:([A-Za-z0-9+/=]+)', log)
if m:
    open(w + '/gui.bmp', 'wb').write(base64.b64decode(m.group(1)))
    print('  gui: screenshot in ' + w + '/gui.bmp')
PY
    if ! { [ $gok = 1 ] && echo "$dump" | grep -q 'authed=1 connected=1'; }; then
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
    # opens and shows the camera; the stop comes once the recording has begun
    # (after the countdown), and is followed until the file has bytes. How long
    # it ran is the machine's business; that it was taken is the check.
    script='sttest|vm open~^vm=1 recphase=0 frame=[1-9]|vm record~^vm=1 recphase=2|vm stop~^vm=1 take_bytes=[1-9]|about'
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
    judge "text layer (sttest): ${sttest:-no answer}" "$(echo "$sttest" | grep -q '^ok ' && echo 1)"
    judge "video card opened on the camera" "$(grep -q '"UNTIL vm open: ok' "$W/chrome-media.log" && echo 1)"
    judge "recording began" "$(grep -q '"UNTIL vm record: ok' "$W/chrome-media.log" && echo 1)"
    judge "stopped and the file taken (take_bytes>0)" "$(grep -q '"UNTIL vm stop: ok' "$W/chrome-media.log" && echo "$vm" | grep -qE 'take_bytes=[1-9]' && echo 1)"
    judge "audio on the worklet: ${audio:-not reported}" "$([ "$audio" = "oc-web: audio worklet" ] && echo 1)"
    judge "about: ${about:-no answer}" "$(echo "$about" | grep -qE '^OpenChime client [^ ]+ \(web\), server [0-9a-z]+, protocol [0-9]+$' && echo 1)"

    # The input method (plat_web.c): a composition played into the page's
    # hidden input ends in the composer, as a desktop IME's does. The page
    # polls for each state; this reads what it found.
    run_page "$APP&hook=1&ime=1" ime "$W/chrome-ime.log" '"(IME|IME-ERR):'
    ime=$(grep -m1 '"IME:' "$W/chrome-ime.log" | sed 's/.*"IME://; s/", source:.*//')
    composing=$(grep -m1 '"IME-COMPOSING:' "$W/chrome-ime.log" | sed 's/.*"IME-COMPOSING://; s/", source:.*//')
    echo "  ime: ${ime:-$(grep -m1 -oE '"IME-ERR:[^"]*' "$W/chrome-ime.log" | cut -c10-)}"
    judge "composing: two units (comp=2)" "$(echo "$composing" | grep -q ' comp=2 ' && echo 1)"
    judge "committed: two characters in the composer (len=2, 日本)" "$(echo "$ime" | grep -q '^ed len=2 ' && echo "$ime" | grep -q '日本' && echo 1)"

    if [ $bad = 0 ]; then
        echo "webtest: PASS -- the client signed in, drew itself, recorded a video message and took a composition in the browser"; exit 0
    fi
    echo "webtest: FAIL -- see the FAIL lines above, and $W/chrome-media.log and $W/chrome-ime.log" >&2
    exit 1
}
