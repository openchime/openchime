#!/usr/bin/env bash
# The web client's smoke test (docs/WEB.md): a daemon on a fresh database with
# one bootstrapped owner, the built page served with the headers wasm threads
# need (scripts/web/serve.py), and a headless Chromium that loads the client. The
# client's test hook answers through the browser's console log, which the test
# reads. Needs ./openchimed, build/web and a Chromium: CHROME names it, else
# Playwright's download is looked for.
set -uo pipefail
cd "$(dirname "$0")/../.."
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux*/chrome 2>/dev/null | sort | tail -1)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "webtest: no Chromium (set CHROME)" >&2; exit 2; }
[ -f build/web/openchime.js ] || { echo "webtest: build/web is not built (make web)" >&2; exit 2; }
./openchimed --version | grep -q 'demo build' || { echo "webtest: ./openchimed is not a demo build (make DEMO=1)" >&2; exit 2; }
W=build/webtest; rm -rf "$W"; mkdir -p "$W/db" "$W/blobs"
# The browser profiles are kept across runs: the compiled wasm is cached in them.
mkdir -p build/webtest-profile
PORT=18461; HPORT=18462; PAGE=18463
env OPENCHIME_DATA_DIR="$W/data" OPENCHIME_PORT=$PORT OPENCHIME_HEALTH_PORT=$HPORT \
    OPENCHIME_DEMO_USERS="alice:pw:owner" OPENCHIME_SUMMARY=off \
    ./openchimed > "$W/daemon.log" 2>&1 &
D=$!
python3 scripts/web/serve.py build/web $PAGE > "$W/webdev.log" 2>&1 &
S=$!
trap 'kill $D $S 2>/dev/null; wait $D $S 2>/dev/null' EXIT
for _ in $(seq 1 100); do grep -q "healthz listening" "$W/daemon.log" 2>/dev/null && break; sleep 0.1; done
# The client (openchime.html): the Windows application compiled for the
# browser, signed in with the bootstrapped owner. Its test hook dumps the model
# after a while and takes the client's own screenshot, which is left in
# $W/gui.bmp for a person to look at.
{
    URL2="http://127.0.0.1:$PAGE/openchime.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw&wait=authed&shot=1"
    timeout 90 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
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

    # Media (WEB.md): a video message recorded from the synthetic camera and the
    # browser's (fake) microphone through Web Audio, stopped, and the file taken
    # -- the same recorder, codecs and muxer the desktop runs, in wasm.
    URL3="http://127.0.0.1:$PAGE/openchime.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw&hook=1&env=OPENCHIME_TEST_CAPTURE%3Dsynthetic&script=sttest@0|vm%20open@1000|vm%20record@18000|vm%20stop@23000|dump%20/t/d.txt@33000"
    timeout 120 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --use-fake-device-for-media-stream --use-fake-ui-for-media-stream --autoplay-policy=no-user-gesture-required \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="build/webtest-profile/media" --no-first-run --disable-background-networking \
        "$URL3" > /dev/null 2> "$W/chrome-media.log" &
    C3=$!
    mok=0
    for _ in $(seq 1 400); do
        if grep -q '"DUMPALL:' "$W/chrome-media.log" 2>/dev/null; then mok=1; break; fi
        kill -0 $C3 2>/dev/null || break
        sleep 0.25
    done
    kill $C3 2>/dev/null; wait $C3 2>/dev/null
    grep -oE '"ACK [^"]*' "$W/chrome-media.log" | sed 's/^"/  /' | head -8
    taken=$(grep -oE '"ACK vm stop[^"]*' "$W/chrome-media.log" | head -1)
    # The text layer (sdltext/st_canvas.c): right-to-left order, clusters, line breaks.
    sttest=$(grep -oE '"ACK sttest: [^"]*' "$W/chrome-media.log" | head -1 | cut -c14-)
    echo "  text: ${sttest:-no answer}"
    # The recording's state after the stop: taken when the file has bytes.
    vm=$(grep -oE 'vm=[0-9]+ recphase=[0-9]+ [^"]*take_bytes=[0-9]+' "$W/chrome-media.log" | tail -1)
    [ -n "$vm" ] || vm=$(python3 - "$W" <<'PY'
import re, sys, base64
log = open(sys.argv[1] + '/chrome-media.log', errors='replace').read()
m = re.findall(r'"DUMPALL:([A-Za-z0-9+/=]+)', log)
if m:
    d = base64.b64decode(m[-1]).decode('utf-8', 'replace')
    for l in d.split('\n'):
        if l.startswith('vm='): print(l[:200])
PY
)
    echo "  media: $vm"
    # The audio ran on the worklet (audio_web.c): the fallback would say "scriptprocessor".
    audio=$(grep -oE 'oc-web: audio [a-z]+' "$W/chrome-media.log" | head -1)
    echo "  audio: ${audio:-not reported}"
    # The input method (plat_web.c): a composition played into the page's
    # hidden input ends in the composer, as a desktop IME's does.
    URL4="http://127.0.0.1:$PAGE/openchime.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw&hook=1&ime=1"
    timeout 120 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="build/webtest-profile/ime" --no-first-run --disable-background-networking \
        "$URL4" > /dev/null 2> "$W/chrome-ime.log" &
    C4=$!
    for _ in $(seq 1 400); do
        if grep -q '"IME:' "$W/chrome-ime.log" 2>/dev/null; then break; fi
        kill -0 $C4 2>/dev/null || break
        sleep 0.25
    done
    kill $C4 2>/dev/null; wait $C4 2>/dev/null
    ime=$(grep -m1 '"IME:' "$W/chrome-ime.log" | sed 's/.*"IME://; s/", source:.*//')
    composing=$(grep -m1 '"IME-COMPOSING:' "$W/chrome-ime.log" | sed 's/.*"IME-COMPOSING://; s/", source:.*//')
    echo "  ime: ${ime:-no answer}"
    # Two units composing (comp=2) and, committed, two characters in the box.
    imeok=0; echo "$composing" | grep -q 'comp=2' && echo "$ime" | grep -q 'len=2 ' && echo "$ime" | grep -q '日本' && imeok=1
    if [ $mok = 1 ] && echo "$taken" | grep -q ': ok' && echo "$vm" | grep -qE 'take_bytes=[1-9]' && [ "$audio" = "oc-web: audio worklet" ] && [ $imeok = 1 ] && echo "$sttest" | grep -q '^ok '; then
        echo "webtest: PASS -- the client signed in, drew itself and recorded a video message in the browser"; exit 0
    fi
    echo "webtest: FAIL (media) -- see $W/chrome-media.log" >&2
    exit 1
}
