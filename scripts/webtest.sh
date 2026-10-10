#!/usr/bin/env bash
# The web client's smoke test (docs/WEB.md): a daemon on a fresh database with
# one bootstrapped owner, the built page served with the headers wasm threads
# need (scripts/webdev.py), and a headless Chromium that loads it. The page's
# console says when the core has signed in and how many channels it was given;
# the test reads that from the browser's log. Needs ./openchimed, build/web and
# a Chromium: CHROME names it, else Playwright's download is looked for.
set -uo pipefail
cd "$(dirname "$0")/.."
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux*/chrome 2>/dev/null | sort | tail -1)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "webtest: no Chromium (set CHROME)" >&2; exit 2; }
[ -f build/web/openchime-core.js ] || { echo "webtest: build/web is not built (make web-core)" >&2; exit 2; }
W=build/webtest; rm -rf "$W"; mkdir -p "$W/db" "$W/blobs"
PORT=18461; HPORT=18462; PAGE=18463
env OPENCHIME_DB_PATH="$W/db/oc.db" OPENCHIME_TLS_CERT="$W/cert.pem" OPENCHIME_TLS_KEY="$W/key.pem" \
    OPENCHIME_BLOB_DIR="$W/blobs" OPENCHIME_PROTO_PORT=$PORT OPENCHIME_HEALTH_PORT=$HPORT \
    OPENCHIME_TEST_PASSWORD_AUTH=1 OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner" OPENCHIME_SUMMARY=off \
    ./openchimed > "$W/daemon.log" 2>&1 &
D=$!
python3 scripts/webdev.py build/web $PAGE > "$W/webdev.log" 2>&1 &
S=$!
trap 'kill $D $S 2>/dev/null; wait $D $S 2>/dev/null' EXIT
for _ in $(seq 1 100); do grep -q "healthz listening" "$W/daemon.log" 2>/dev/null && break; sleep 0.1; done
URL="http://127.0.0.1:$PAGE/index.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw"
timeout 40 "$CHROME" --headless=new --no-sandbox --disable-gpu --enable-logging=stderr --v=0 \
    --user-data-dir="$W/chrome" --no-first-run --disable-background-networking \
    "$URL" > "$W/chrome.out" 2> "$W/chrome.log" &
C=$!
ok=0
for _ in $(seq 1 150); do
    if grep -q 'oc-web: signed in' "$W/chrome.log" 2>/dev/null && grep -qE 'oc-web: [1-9][0-9]* channels' "$W/chrome.log" 2>/dev/null; then ok=1; break; fi
    kill -0 $C 2>/dev/null || break
    sleep 0.2
done
kill $C 2>/dev/null; wait $C 2>/dev/null
grep -oE 'oc-web: [^"]*' "$W/chrome.log" | sed 's/^/  /' | head -20
if [ $ok != 1 ]; then
    echo "webtest: FAIL (core) -- see $W/chrome.log and $W/daemon.log" >&2
    grep -iE 'error|uncaught|refused|failed' "$W/chrome.log" | head -5 >&2
    exit 1
fi
echo "webtest: the core signed in from the browser over the WebSocket transport"

# The client itself (openchime.html, WEB.md step three): the Windows application
# compiled for the browser, signed in with the same credentials. Its test hook
# dumps the model after a while and takes the client's own screenshot, which is
# left in $W/gui.bmp for a person to look at.
if [ -f build/web/openchime.js ]; then
    URL2="http://127.0.0.1:$PAGE/openchime.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw&wait=authed&shot=1"
    timeout 90 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="$W/chrome-gui" --no-first-run --disable-background-networking \
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
    URL3="http://127.0.0.1:$PAGE/openchime.html?host=127.0.0.1&port=$HPORT&user=alice&pass=pw&hook=1&env=OPENCHIME_TEST_CAPTURE%3Dsynthetic&script=vm%20open@0|vm%20record@10000|vm%20stop@14000|dump%20/t/d.txt@24000"
    timeout 120 "$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --use-fake-device-for-media-stream --use-fake-ui-for-media-stream --autoplay-policy=no-user-gesture-required \
        --hide-scrollbars --window-size=1280,800 --enable-logging=stderr --v=0 \
        --user-data-dir="$W/chrome-media" --no-first-run --disable-background-networking \
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
    if [ $mok = 1 ] && echo "$taken" | grep -q ': ok' && echo "$vm" | grep -qE 'take_bytes=[1-9]'; then
        echo "webtest: PASS -- the client signed in, drew itself and recorded a video message in the browser"; exit 0
    fi
    echo "webtest: FAIL (media) -- see $W/chrome-media.log" >&2
    exit 1
fi
echo "webtest: PASS (core only; build/web/openchime.js not built)"
exit 0
