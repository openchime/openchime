#!/usr/bin/env bash
# A person's AI clients (docs/details/AUTH.md §8.13): a client registers and
# sends its person to the daemon's authorize page, which shows a code; the
# person approves it in the app, in Preferences > AI clients (Approve an AI
# client, the code, the client and where it returns, a lifetime) and the page
# sends them back with a code the client exchanges; their list there shows the
# connection; a post through it reads "via <client>" beside the author; an
# owner sees another person's connection in Admin's everyone's and revokes it;
# and the person revokes their own, after which the client is refused.
#
#   tools/gui/features/connections.sh          # run; the client and daemon stop at the end
#   tools/gui/features/connections.sh --keep   # ... and leave them running
#
# The client is played by curl against the fixture daemon, with MCP on. bob's
# connection, for the owner's view, is written into the fixture's database.
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
export OPENCHIME_MCP=on
gui_begin connections 9832 /tmp/oc-connections "Connections Fixture" "$@"

BASE="https://127.0.0.1:$OC_TEST_PORT"
CB="http%3A%2F%2F127.0.0.1%3A4000%2Fcb"
RES="https%3A%2F%2F127.0.0.1%3A$OC_TEST_PORT%2Fmcp"
CH="E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"   # RFC 7636 appendix B
VER="dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
cn() { field "$D" '^connections ' "$1"; }
post_mcp() {   # <token> <json params>: the HTTP status
  curl -sk -o "$OC_TEST_DIR/mcp.out" -w '%{http_code}' -X POST "$BASE/mcp" \
    -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' -H "Authorization: Bearer $1" \
    -H 'Mcp-Method: tools/call' -H 'Mcp-Name: post' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{$2,\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}"
}

step "the owner opens AI clients from the profile menu"
gui_launch
act size 1400 900
act channel 1
act menu 76
shown() { gui_snap; [ "$(cn shown)" = 1 ] && grep -q '^modal=prefs ' "$D"; }
check "Preferences opens at AI clients" waitfor 5 shown
cat_pub() { gui_snap; grep -q '^a11yitem prefs.cat.aiclients ' "$D"; }
check "the AI clients category is published" waitfor 5 cat_pub
approve_pub() { gui_snap; grep -q '^a11yitem settings.conn.approve ' "$D"; }
check "Approve an AI client is published" waitfor 5 approve_pub

step "a client registers and its page shows a code"
CLIENT="$(curl -sk -X POST "$BASE/oauth/register" -H 'Content-Type: application/json' \
  --data '{"client_name":"Desk AI","redirect_uris":["http://127.0.0.1:4000/cb"]}' | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["client_id"])')"
need "the client is registered" [ -n "$CLIENT" ]
PAGE="$(curl -sk "$BASE/oauth/authorize?response_type=code&client_id=$CLIENT&redirect_uri=$CB&code_challenge=$CH&code_challenge_method=S256&state=s1&resource=$RES")"
REQ="$(printf '%s' "$PAGE" | grep -o 'request=ocrq_[A-Za-z0-9_-]*' | head -1 | cut -d= -f2)"
CODE="$(printf '%s' "$PAGE" | grep -o ';code=[A-Z]*' | head -1 | cut -d= -f2)"
need "the page has its handle and a code" [ -n "$REQ" ] && [ "${#CODE}" = 8 ]

step "the owner approves it in the app, for 90 days"
act formnext "${CODE:0:4}-${CODE:4:4}"
need "Approve an AI client is pressed" press settings.conn.approve
asking() { gui_snap; [ "$(cn open)" = 1 ] && [ "$(fieldq "$D" '^connections ' client)" = "Desk AI" ]; }
check "the app names the client asking" waitfor 10 asking
need "90 days is pressed" press settings.conn.life.90
ninety() { gui_snap; [ "$(cn life)" = 90 ]; }
check "90 days is chosen" waitfor 5 ninety
act shot connections_request
need "Approve is pressed" press settings.conn.allow
decided() { gui_snap; [ "$(cn decided)" = 1 ] && [ "$(cn open)" = 0 ]; }
check "it is approved" waitfor 10 decided

step "the page returns the client a code, exchanged for a token"
LOC="$(curl -sk -o /dev/null -w '%{redirect_url}' "$BASE/oauth/authorize?request=$REQ&code=$CODE")"
AUTH="$(printf '%s' "$LOC" | grep -o 'code=ocac_[A-Za-z0-9_-]*' | cut -d= -f2)"
need "the page sent the client back with a code" [ -n "$AUTH" ]
ACCESS="$(curl -sk -X POST "$BASE/oauth/token" -H 'Content-Type: application/x-www-form-urlencoded' \
  --data "grant_type=authorization_code&code=$AUTH&code_verifier=$VER&redirect_uri=$CB&client_id=$CLIENT&resource=$RES" \
  | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("access_token",""))')"
need "the client holds an access token" [ "${ACCESS:0:5}" = ocpa_ ]
need "the AI clients category is pressed again" press prefs.cat.aiclients
mine() { gui_snap; [ "$(cn mine)" = 1 ]; }
check "the owner's own list shows it" waitfor 10 mine
own_revoke() { gui_snap; grep -q '^a11yitem settings.conn.[0-9]*.revoke ' "$D"; }
check "its Revoke is published" waitfor 5 own_revoke

step "a post through it reads via the client"
need "the client posts" [ "$(post_mcp "$ACCESS" '"name":"post","arguments":{"channel":"#general","text":"drafted by the client"}')" = 200 ]
act key esc
closed() { gui_snap; ! grep -q '^modal=prefs ' "$D"; }
check "Preferences closes" waitfor 5 closed
act view home
act channel 1
via_shown() { gui_snap; [ "$(cn vias)" -ge 1 ]; }
check "the transcript says via the client" waitfor 10 via_shown
act shot connections_via

step "the owner sees bob's connection, and revokes it"
python3 -I - "$OC_TEST_DIR/data/openchime.db" <<'PY' || gui_abort "bob's connection was not written"
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1], timeout=5)
bob = db.execute("SELECT id FROM users WHERE subject='local:bob'").fetchone()[0]
db.execute("INSERT INTO mcp_connections(user_id,client_id,client_name,resource,lifetime_days,expires_at_ms,created_at_ms)"
           " VALUES(?, 'occl_bob', 'Bob Helper', 'x', 30, ?, ?)", (bob, int(time.time() * 1000) + 30 * 86400000, int(time.time() * 1000)))
db.commit()
PY
act view admin
need "the tab is pressed" press admin.tab.connections
on_tab() { gui_snap; [ "$(cn admin)" = 1 ]; }
check "Admin's AI clients is open" waitfor 5 on_tab
own_none() { ! grep -q '^a11yitem settings.conn.approve ' "$D"; }
check "the tab holds no person's own controls" own_none
both() { gui_snap; [ "$(cn all)" = 2 ] && grep -q '^  conn id=[0-9]* uid=[0-9]* client="Bob Helper"' "$D"; }
check "everyone's lists both" waitfor 10 both
BOBCONN="$(grep -m1 '^  conn id=[0-9]* uid=[0-9]* client="Bob Helper"' "$D" | grep -o 'id=[0-9]*' | head -1 | cut -d= -f2)"
need "Revoke is pressed on bob's" press "admin.conn.$BOBCONN.revoke"
asked() { gui_snap; grep -q '^confirm open=1 ' "$D"; }
check "Revoke asks first" waitfor 5 asked
act key enter
one_left() { gui_snap; [ "$(cn all)" = 1 ] && ! grep -q 'client="Bob Helper"' "$D"; }
check "bob's leaves everyone's list" waitfor 10 one_left

step "the owner revokes their own; the client is refused"
act menu 76
check "Preferences opens at AI clients" waitfor 5 shown
check "its Revoke is published" waitfor 5 own_revoke
MINE="$(grep -o '^a11yitem settings\.conn\.[0-9]*\.revoke' "$D" | head -1 | cut -d. -f3)"
need "Revoke is pressed on their own" press "settings.conn.$MINE.revoke"
check "Revoke asks first" waitfor 5 asked
act key enter
none() { gui_snap; [ "$(cn mine)" = 0 ]; }
check "their list is empty" waitfor 10 none
check "the client is refused" [ "$(post_mcp "$ACCESS" '"name":"post","arguments":{"channel":"#general","text":"again"}')" = 401 ]

gui_end
