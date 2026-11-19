#!/usr/bin/env bash
# The daemon's own pages, from a real browser (AUTH.md §8, REQ-026, WEB.md
# "Loading").
#
#   tools/gui/features/web_pages.sh          # run; the daemon and browsers stop at the end
#   tools/gui/features/web_pages.sh --keep   # ... and leave the daemon running
#
# The daemon as shipped, with no owner yet, and headless Chromium driven by
# playwright-core (tools/gui/harness/web/pages.mjs, invite.mjs, load.mjs). What
# a browser sends -- its Origin under the pages' referrer policy, its handling
# of the 303, its cache -- is what is exercised, which a scripted post cannot
# show. It asserts that
#
#   - the first owner signs up with the setup token on the sign-up form (a
#     short password refused, saying why; the show-password toggle);
#   - a wrong password is refused on the sign-in page, and the right one signs in;
#   - the password page changes the password, the old one is refused after it
#     and the new one signs in;
#   - the token that last sign-in ended with is taken on AUTH (build/signin
#     present, with the sign-in's verifier);
#   - an invitation link opened cold in a second browser goes to the sign-up
#     form before the web client loads, and ends signed in to the web client
#     as the new member, the session kept when the tab is closed;
#   - the web client's load: the load screen counts the download, names its
#     stages in order and comes down; the wasm and loader arrive gzipped; a
#     right-click opens no browser menu; a second load downloads nothing again.
#
# Its own daemon on its own port, wiped each run, and a loopback listener on
# the next port for the sign-ins' callback (as a client's own is, AUTH.md
# §8.1): the browser lands there and the token is read off the address. Web
# only, whatever OC_TEST_PLATFORM says: there is no client of its own to drive.
# Needs ./openchimed with the web client in it (WEB=1), build/signin and
# build/demo_client, which tools/gui/run.sh builds, and node with
# playwright-core and Chrome (CHROME), which it checks for.
#
# platforms: web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export OC_TEST_PLATFORM=web
. "$HERE/tools/gui/harness/lib.sh"
gui_begin web_pages 9595 /tmp/oc-web-pages "$@"

PORT="$OC_TEST_PORT"
CBPORT=$((PORT + 1))
ORIGIN="https://127.0.0.1:$PORT"
CHROME_BIN="${CHROME:-$(command -v google-chrome || true)}"
[ -x "${CHROME_BIN:-/nonexistent}" ] || gui_abort "no Chrome (CHROME=<path>, or google-chrome on PATH)"
if [ "${OC_TEST_NO_BUILD:-0}" != 1 ]; then
  # Made here, not only when missing: one left from before a protocol bump
  # would be refused at HELLO and look like a page failure. The daemon is
  # run.sh's to build, with the switches it remembers.
  make -C "$HERE" REMEMBER=0 build/signin build/demo_client > "$OC_TEST_DIR/build.log" 2>&1 ||
    gui_abort "the build failed ($OC_TEST_DIR/build.log)"
fi
for b in openchimed build/signin build/demo_client; do
  [ -x "$HERE/$b" ] || gui_abort "no $b (tools/gui/run.sh builds it)"
done
[ -f "$HERE/web/openchime.wasm" ] || gui_abort "no web client beside the daemon (make openchimed WEB=1)"

# relay <file>: each "  ok   <what>" or "  FAIL <what>" line a browser script
# printed, as this script's own check; any other line said as it is.
relay() {
  local l
  while IFS= read -r l; do
    case "$l" in
      "  ok   "*) check "${l#  ok   }" true ;;
      "  FAIL "*) check "${l#  FAIL }" false ;;
      token\ *) ;;
      *) [ -n "$l" ] && say "  $l" ;;
    esac
  done < "$1"
}
# browser <name> <script.mjs> <args...>: the script run to its end; its lines
# relayed, and a script that ended in an error rather than a verdict is a
# failed check of its own, with the error.
browser() {
  local name="$1" mjs="$2" rc; shift 2
  timeout 600 node "$HERE/tools/gui/harness/web/$mjs" "$@" "$CHROME_BIN" \
    > "$OC_TEST_DIR/$name.out" 2> "$OC_TEST_DIR/$name.err" < /dev/null
  rc=$?
  relay "$OC_TEST_DIR/$name.out"
  if [ "$rc" != 0 ] && ! grep '^  FAIL ' "$OC_TEST_DIR/$name.out" >/dev/null; then
    check "$mjs ran to its verdict (exit $rc: $(tail -2 "$OC_TEST_DIR/$name.err" | tr '\n' ' '))" false
  fi
  return "$rc"
}

# The callback: answers HTML, since a page with no content type is a download
# to a browser, which then stays where it was. Marked on its command line, so
# one a --keep run left behind is found and stopped here.
CBMARK="oc-web-pages-callback"
stop_cb() { pkill -f "python3 -I - $CBPORT $CBMARK" 2>/dev/null; return 0; }
GUI_AT_EXIT=stop_cb

step "the daemon, as shipped, with no owner yet; the callback listening"
stop_cb
cb_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$CBPORT") 2>/dev/null; }
waitfor 5 cb_free || gui_abort "something else holds :$CBPORT, the callback's port"
setsid python3 -I - "$CBPORT" "$CBMARK" > "$OC_TEST_DIR/callback.log" 2>&1 9>&- <<'PY' &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"<!doctype html><p>callback</p>"
        self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): sys.stderr.write(self.path[:60] + "\n")
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
disown
# Started without the GUI lock's descriptor (9); OPENCHIME_PORT in its
# environment is what lib.sh's teardown finds it by.
env OPENCHIME_DATA_DIR="$OC_TEST_DIR/data" OPENCHIME_PORT="$PORT" OPENCHIME_HEALTH_PORT=0 \
    OPENCHIME_TTS=off OPENCHIME_STT=off OPENCHIME_WORKSPACE_NAME="Pages" \
    setsid "$HERE/openchimed" > "$OC_TEST_DIR/daemon.log" 2>&1 < /dev/null 9>&- &
disown
SETUP=""
token_printed() {
  SETUP="$(sed -n 's/.*first-run setup token.*: \([0-9a-f]\{64\}\).*/\1/p' "$OC_TEST_DIR/daemon.log" | head -1)"
  [ -n "$SETUP" ]
}
waitfor 40 token_printed || gui_abort "the daemon printed no setup token: $(tail -2 "$OC_TEST_DIR/daemon.log" | tr '\n' ' ')"
listening() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
waitfor 30 listening "$PORT" || gui_abort "the daemon is not listening on :$PORT"
waitfor 10 listening "$CBPORT" || gui_abort "the callback is not listening on :$CBPORT: $(tail -2 "$OC_TEST_DIR/callback.log" | tr '\n' ' ')"

step "sign-up with the setup token, a wrong password, the password changed"
# One PKCE pair for the run: the browser carries the challenge, this presents
# the verifier; the daemon binds each token to it.
read -r VERIFIER CHALLENGE < <(python3 -I -c '
import os, base64, hashlib
v = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
c = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b"=").decode()
print(v, c)')
browser pages pages.mjs "$ORIGIN" "$SETUP" "$CHALLENGE" "http://127.0.0.1:$CBPORT/cb"
TOKEN="$(sed -n 's/^token //p' "$OC_TEST_DIR/pages.out" | tail -1)"
need "the last sign-in ended with a token" [ -n "$TOKEN" ]

step "the browser's token, presented on AUTH"
WHO=""
presented() { WHO="$("$HERE/build/signin" present 127.0.0.1 "$PORT" "$TOKEN" "$VERIFIER" 2>"$OC_TEST_DIR/present.err")"; }
check "the browser's token signs in on AUTH" presented
say "  (user ${WHO:-none}$( [ -s "$OC_TEST_DIR/present.err" ] && echo "; $(tail -1 "$OC_TEST_DIR/present.err")"))"

step "an invitation link, opened cold in a second browser"
INVITE=""
minted() { INVITE="$("$HERE/build/demo_client" 127.0.0.1 "$PORT" owen granite-harbor-violet invite 0 2>"$OC_TEST_DIR/invite-mint.err")" && [ -n "$INVITE" ]; }
need "the owner mints an invitation link anyone may use" minted
browser invite invite.mjs "$ORIGIN" "$INVITE"

step "the web client's load, cold and warm"
# The certificate trusted by its key (the SPKI hash), not ignored: a browser
# keeps nothing in its HTTP cache from a certificate it was told to overlook.
SPKI="$(node -e '
const tls = require("tls"), crypto = require("crypto");
const s = tls.connect({ host: "127.0.0.1", port: +process.argv[1], rejectUnauthorized: false }, () => {
  const der = new crypto.X509Certificate(s.getPeerCertificate().raw).publicKey.export({ type: "spki", format: "der" });
  console.log(crypto.createHash("sha256").update(der).digest("base64")); s.end();
});
s.on("error", (e) => { console.error(e.message); process.exit(1); });' "$PORT" 2>"$OC_TEST_DIR/spki.err")"
need "the daemon's certificate key is read (${SPKI:-$(tail -1 "$OC_TEST_DIR/spki.err")})" [ -n "$SPKI" ]
browser load load.mjs "$ORIGIN" "$SPKI"

gui_end
