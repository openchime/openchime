#!/usr/bin/env bash
# The daemon's sign-in pages from a real browser (docs/TESTING.md): a headless
# Chromium, driven by scripts/web/pages.mjs, signs up the first owner with the
# setup token, is refused a wrong password, signs in, changes the password on
# its page and signs in with the new one; the token that last sign-in ends
# with is then presented on AUTH by build/signin, which proves the page's
# answer is one the daemon takes. Then an invitation link, opened cold in a
# second browser, ends signed in to the web client (scripts/web/invite.mjs). What a browser sends -- its Origin under the
# pages' referrer policy, its handling of the 303 -- is what is exercised,
# which the suites' scripted posts cannot show. Needs ./openchimed (a demo
# build) with the web client beside it (make web), build/signin,
# build/demo_client, node with playwright-core, and a Chromium (CHROME, else
# Playwright's download).
set -uo pipefail
cd "$(dirname "$0")/../.."
CHROME="${CHROME:-$(ls -d "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux*/chrome 2>/dev/null | sort | tail -1)}"
[ -x "${CHROME:-/nonexistent}" ] || { echo "pages: no Chromium (set CHROME)" >&2; exit 2; }
[ -d node_modules/playwright-core ] || npm install --no-audit --no-fund --silent
./openchimed --version | grep -q 'demo build' || { echo "pages: ./openchimed is not a demo build (make DEMO=1)" >&2; exit 2; }
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
OUT="$(node scripts/web/pages.mjs "https://127.0.0.1:$PORT" "$SETUP" "$CHALLENGE" "http://127.0.0.1:$CBPORT/cb" "$CHROME" 2>"$W/pages.err")"
rc=$?
printf '%s\n' "$OUT" | grep -v '^token '
[ $rc = 0 ] || { echo "pages: FAIL (see $W/pages.err)"; tail -5 "$W/pages.err"; exit 1; }
TOKEN="$(printf '%s\n' "$OUT" | sed -n 's/^token //p' | tail -1)"
WHO="$(./build/signin present 127.0.0.1 $PORT "$TOKEN" "$VERIFIER")" || { echo "  FAIL the browser's token is refused on AUTH"; exit 1; }
echo "  ok   the browser's token signs in on AUTH (user $WHO)"
# An invitation link (REQ-026): the owner mints an invite anyone may use, and a
# second browser, which has never seen the workspace, opens the link cold and
# ends signed in to the web client as a new member (scripts/web/invite.mjs).
# The daemon serves the web client from web/ beside it (make web).
[ -f web/openchime.wasm ] || { echo "pages: no web client beside the daemon (make web)" >&2; exit 2; }
INVITE="$(./build/demo_client 127.0.0.1 $PORT owen pw-new invite 0 2>>"$W/pages.err")" || { echo "  FAIL minting the invitation link"; tail -3 "$W/pages.err"; exit 1; }
OUT2="$(node scripts/web/invite.mjs "https://127.0.0.1:$PORT" "$INVITE" "$CHROME" 2>>"$W/pages.err")"
rc=$?
printf '%s\n' "$OUT2"
[ $rc = 0 ] || { echo "pages: FAIL (see $W/pages.err)"; tail -5 "$W/pages.err"; exit 1; }
echo "pages: OK: the sign-in pages and an invitation link from a real browser"
