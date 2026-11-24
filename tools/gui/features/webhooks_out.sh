#!/usr/bin/env bash
# Outgoing webhooks (docs/details/WEBHOOKS.md): an owner opens Admin >
# Webhooks, makes one -- the daemon checks the address answers first -- and
# sees its signing secret once, with Copy; a message in its channel reaches the
# address, signed; it is turned off and on again, and deleted. Every control is
# published with a name.
#
#   tools/gui/features/webhooks_out.sh          # run; the client and daemon stop at the end
#   tools/gui/features/webhooks_out.sh --keep   # ... and leave them running
#
# The address is a receiver this script runs on the loopback over HTTPS, with a
# certificate made for the run and handed to the daemon as OPENCHIME_EXTRA_CA:
# it answers the check with its challenge and keeps every request it is sent.
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin webhooks_out 9840 /tmp/oc-webhooks-out "Webhooks Fixture" "$@"

wa() { field "$D" '^webhookadmin ' "$1"; }
RXMARK="oc-webhooks-out-receiver"
stop_rx() { pkill -f "python3 -I - $RXMARK" 2>/dev/null; return 0; }
GUI_AT_EXIT=stop_rx

step "a receiver on the loopback, over HTTPS the daemon trusts"
stop_rx
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
  -keyout "$OC_TEST_DIR/rx-key.pem" -out "$OC_TEST_DIR/rx-cert.pem" -subj /CN=localhost \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" -addext "basicConstraints=critical,CA:TRUE" \
  >/dev/null 2>&1 || gui_abort "the receiver's certificate was not made"
export OPENCHIME_EXTRA_CA="$OC_TEST_DIR/rx-cert.pem"
: > "$OC_TEST_DIR/rx.log"
setsid python3 -I - "$RXMARK" "$OC_TEST_DIR" > "$OC_TEST_DIR/rx.out" 2>&1 9>&- <<'PY' &
import http.server, json, ssl, sys
d = sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        b = self.rfile.read(n)
        sig = self.headers.get("X-Slack-Signature", "")
        with open(d + "/rx.log", "ab") as f:
            f.write(b"sig=" + sig.encode() + b" " + b + b"\n")
        try:
            j = json.loads(b)
        except ValueError:
            j = {}
        out = (j.get("challenge") or "ok").encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)
    def log_message(self, *a):
        pass
srv = http.server.HTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(d + "/rx-cert.pem", d + "/rx-key.pem")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
with open(d + "/rx.port", "w") as f:
    f.write(str(srv.server_address[1]))
srv.serve_forever()
PY
rx_up() { [ -s "$OC_TEST_DIR/rx.port" ]; }
waitfor 10 rx_up || gui_abort "the receiver did not start: $(tail -2 "$OC_TEST_DIR/rx.out" | tr '\n' ' ')"
RXURL="https://localhost:$(cat "$OC_TEST_DIR/rx.port")/hook"

step "an owner opens Admin > Webhooks"
gui_launch
act size 1400 900
act channel 1
act view admin
need "the Webhooks tab is published" press admin.tab.webhooks
shown() { gui_snap; [ "$(wa shown)" = 1 ] && [ "$(wa have)" = 1 ]; }
check "Admin > Webhooks is open, its list loaded" waitfor 10 shown
check "there are none yet" [ "$(wa count)" = 0 ]
new_pub() { gui_snap; grep -q '^a11yitem admin.webhooks.new ' "$D"; }
check "New webhook is published" waitfor 5 new_pub

step "a channel that is none: nothing is made, and it says why"
act formnext "$RXURL|#no-such-channel|1|0|0|0|0|0|0|0|0"
need "New webhook is pressed" press admin.webhooks.new
noted() { gui_snap; [ "$(wa note)" = 1 ] && [ "$(wa count)" = 0 ]; }
check "a note, and no webhook" waitfor 5 noted

step "New webhook: checked, then its secret once, with Copy"
act formnext "$RXURL|#general|1|0|0|0|0|0|0|0|1"          # message, user_change
need "New webhook is pressed" press admin.webhooks.new
secret() { gui_snap; [ "$(wa secret)" = 1 ] && [ "$(wa secretlen)" = 40 ] && [ "$(wa hex)" = 1 ]; }
check "the secret is shown: 40 hex digits" waitfor 15 secret
check "the address was checked first" grep -q '"type":"url_verification"' "$OC_TEST_DIR/rx.log"
copy_pub() { gui_snap; grep -q '^a11yitem admin.webhooks.secret.copy ' "$D" && grep -q '^a11yitem admin.webhooks.secret.done ' "$D"; }
check "Copy and Done are published" waitfor 5 copy_pub
act shot webhooks_secret
need "Done is pressed" press admin.webhooks.secret.done
listed() { gui_snap; [ "$(wa secret)" = 0 ] && [ "$(wa count)" = 1 ] && grep -q '^  whrow id=[0-9]* enabled=1 channels=1 ' "$D"; }
check "after Done the secret is gone, and the webhook is listed, on" waitfor 10 listed
HOOK="$(grep -m1 '^  whrow id=' "$D" | grep -o 'id=[0-9]*' | cut -d= -f2)"
need "the webhook has an id" [ -n "$HOOK" ]
named() { gui_snap; grep -q "^a11yitem admin.webhook.$HOOK.off " "$D" && grep -q "^a11yitem admin.webhook.$HOOK.delete " "$D"; }
check "its Turn off and Delete are published" waitfor 5 named

step "a message in its channel reaches the address, signed"
act view home
act channel 1
act send "hello from the webhooks fixture"
delivered() { grep -q '"type":"event_callback".*hello from the webhooks fixture' "$OC_TEST_DIR/rx.log"; }
check "the event arrives" waitfor 15 delivered
check "with a signature" grep -q '^sig=v0=[0-9a-f]\{64\} {"type":"event_callback"' "$OC_TEST_DIR/rx.log"

step "turned off, and on again"
act view admin
need "the Webhooks tab is pressed" press admin.tab.webhooks
check "its Turn off is published" waitfor 10 named
need "Turn off is pressed" press "admin.webhook.$HOOK.off"
off() { gui_snap; grep -q "^  whrow id=$HOOK enabled=0 " "$D" && grep -q "^a11yitem admin.webhook.$HOOK.on " "$D"; }
check "it is off, with Turn on" waitfor 10 off
act shot webhooks_off
need "Turn on is pressed" press "admin.webhook.$HOOK.on"
on() { gui_snap; grep -q "^  whrow id=$HOOK enabled=1 " "$D"; }
check "it is on again" waitfor 10 on

step "deleted, after asking"
need "Delete is pressed" press "admin.webhook.$HOOK.delete"
asked() { gui_snap; grep -q '^confirm open=1 ' "$D"; }
check "Delete asks first" waitfor 5 asked
act key enter
gone() { gui_snap; [ "$(wa count)" = 0 ]; }
check "the webhook leaves the list" waitfor 10 gone

gui_end
