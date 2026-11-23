#!/usr/bin/env bash
# Agents (docs/details/MCP.md §3.1, §6, §7): an owner makes one in Admin >
# Agents and sees its token once; its tokens are listed, another minted and one
# revoked; it is put in a channel, where the members pane shows it below the
# people under its own label, tagged; mention suggestions and the transcript
# tag it too; and removing it takes it out of the list and the channel.
#
#   tools/gui/features/agents.sh          # run; the client and daemon stop at the end
#   tools/gui/features/agents.sh --keep   # ... and leave them running
#
# New agent asks for its name, a first token's name and the scopes, each with
# what it allows; the script answers the form (`formnext`) with two of them,
# and the token's panel then names those two with their descriptions, never
# showing the token again after Done. An agent posts only through the Web
# API, which is not here yet, so its message is written into the fixture's
# database and the client started again to read it. Its own daemon on its own
# port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin agents 9830 /tmp/oc-agents "Agents Fixture" "$@"

aa() { field "$D" '^agentadmin ' "$1"; }
tag() { field "$D" '^agenttags ' "$1"; }
# The members pane: shown, whatever each platform's window opened with.
drawn() { gui_snap; [ "$(field "$D" '^members ' rows)" != 0 ]; }
show_members() { waitfor 3 drawn || act members; }
row_top() { grep "^memrow uid=$1 " "$D" | head -1 | grep -o ' r=[0-9,.-]*' | cut -d= -f2 | cut -d, -f2; }

step "an owner opens Admin > Agents"
gui_launch
act size 1400 900
act channel 1
act view admin
need "the Agents tab is published" press admin.tab.agents
shown() { gui_snap; [ "$(aa shown)" = 1 ]; }
check "Admin > Agents is open" waitfor 5 shown
new_pub() { gui_snap; grep -q '^a11yitem admin.agents.new ' "$D"; }
check "New agent is published" waitfor 5 new_pub

step "New agent, with two scopes: its token once, with what each allows"
act formnext 'Robo|first|1|0|1|0|0|0|0|0|0|0|0|0|0|0'   # chat:write, channels:history
need "New agent is pressed" press admin.agents.new
tok() { gui_snap; [ "$(aa token)" = 1 ] && [ "$(aa prefixed)" = 1 ]; }
check "the token is shown, with its prefix" waitfor 10 tok
two_scopes() { [ "$(fieldq "$D" '^agentadmin ' tokscopes)" = "chat:write channels:history" ] && [ "$(aa descs)" = 2 ]; }
check "its two scopes are shown, each with what it allows" two_scopes
copy_pub() { gui_snap; grep -q '^a11yitem admin.agents.token.copy ' "$D"; }
check "Copy is published" waitfor 5 copy_pub
act shot agents_token
need "Done is pressed" press admin.agents.token.done
listed_once() { gui_snap; [ "$(aa token)" = 0 ] && [ "$(aa agents)" = 1 ]; }
check "after Done the token is not shown again, and the agent is listed" waitfor 5 listed_once
AGENT="$(grep -m1 '^  agent uid=[0-9]* name="Robo"' "$D" | grep -o 'uid=[0-9]*' | cut -d= -f2)"
need "the agent has an id" [ -n "$AGENT" ]

step "its tokens: listed without the token, another minted, one revoked"
need "Tokens is pressed" press "admin.agent.$AGENT.tokens"
one_tok() { gui_snap; [ "$(aa sel)" = "$AGENT" ] && [ "$(aa atokens)" = 1 ]; }
check "its one token is listed" waitfor 10 one_tok
act formnext 'laptop|0|0|0|0|0|1|0|0|0|0|0|0|0|0'       # im:history
need "New token is pressed" press admin.agents.mint
check "the new token is shown once" waitfor 10 tok
need "Done is pressed" press admin.agents.token.done
two_tok() { gui_snap; [ "$(aa token)" = 0 ] && [ "$(aa atokens)" = 2 ]; }
check "the list has two" waitfor 10 two_tok
REV="$(grep -o '^  agentbtn admin\.token\.[0-9]*\.revoke' "$D" | tail -1 | cut -d. -f3)"
need "the second token has a Revoke" [ -n "$REV" ]
need "Revoke is pressed" press "admin.token.$REV.revoke"
asked() { gui_snap; grep -q '^confirm open=1 ' "$D"; }
check "Revoke asks first" waitfor 5 asked
act key enter
check "the token leaves the list" waitfor 10 one_tok
need "Back is pressed" press admin.agents.back

step "in the channel: below the people, under its label, tagged"
act view home
act channel 1
show_members
act chmenu 6                                             # Add someone: the members pane's picker
act grppick Rob
act grppick -                                            # Enter on an empty query adds
in_channel() { gui_snap; grep -q "^memrow uid=$AGENT .* agent=1\$" "$D"; }
check "the agent is in the channel's member list" waitfor 10 in_channel
below() {
  local mine top lab; mine="$(row_top "$AGENT")"
  [ -n "$mine" ] || return 1
  for top in $(grep '^memrow ' "$D" | grep -v "uid=$AGENT " | grep -o ' r=[0-9,.-]*' | cut -d, -f2); do
    [ "${top%.*}" -lt "${mine%.*}" ] || return 1
  done
  lab="$(field "$D" '^memagents ' label | cut -d, -f2)"
  [ -n "$lab" ] && [ "${lab%.*}" -gt 0 ] && [ "${lab%.*}" -lt "${mine%.*}" ]
}
check "below every person, under the Agents label" below
check "tagged there" [ "$(tag members)" -ge 1 ]
act shot agents_members

step "mention suggestions tag it"
act typekeys "@Rob"
mention() { gui_snap; [ "$(tag mention)" -ge 1 ] && grep -q '^  acrow 0 repl="@Robo"' "$D"; }
check "@Rob suggests it, tagged" waitfor 5 mention
act key escape
act key ctrl+a
act key backspace

step "its message in the transcript carries the tag"
python3 -I - "$OC_TEST_DIR/data/openchime.db" "$AGENT" <<'PY' || gui_abort "the agent's message was not written"
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1], timeout=5)
# The daemon's own SQL functions, which its search index's triggers call; for a
# plain body with no spoiler each is the identity (daemon/migrate.c).
db.create_function("oc_unspoiled", 1, lambda s: s)
db.create_function("oc_excerpt", 2, lambda s, n: s[:n] if s else s)
db.create_function("oc_has_spoiler", 1, lambda s: 0)
db.execute("INSERT INTO messages(channel_id, author_id, body, created_at_ms) VALUES(1, ?, 'hello from the agent', ?)",
           (int(sys.argv[2]), int(time.time() * 1000)))
db.commit()
PY
"$DRIVE" kill >/dev/null 2>&1
gui_launch
act size 1400 900
act channel 1
in_transcript() { gui_snap; [ "$(tag transcript)" -ge 1 ]; }
check "the transcript tags its author" waitfor 10 in_transcript

step "removed: out of the list and the channel"
act view admin
need "the Agents tab is pressed" press admin.tab.agents
need "Remove is pressed" press "admin.agent.$AGENT.remove"
check "Remove asks first" waitfor 5 asked
act key enter
none_left() { gui_snap; [ "$(aa agents)" = 0 ]; }
check "the agent leaves the list" waitfor 10 none_left
act view home
act channel 1
show_members
not_in() { gui_snap; ! grep -q "^memrow uid=$AGENT " "$D" && grep -q '^memrow ' "$D"; }
check "and the channel" waitfor 10 not_in

gui_end
