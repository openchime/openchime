#!/usr/bin/env bash
# Links in the Win32 client's transcript (REQ-220, MARKDOWN.md §4).
#
#   tools/gui/features/links.sh          # run; the client and daemon stop at the end
#   tools/gui/features/links.sh --keep   # ... and leave them running
#
# A labelled link's text can say anything while it points elsewhere, so the
# client never lets one open unseen. It asserts that
#
#   - hovering a labelled link's text reports its real ADDRESS, not the label,
#     and marks it labelled (the tip beside the pointer shows that address);
#   - clicking it opens a confirmation naming the address, and nothing else --
#     Esc dismisses it without opening anything;
#   - a bare address is its own label: hovering reports it, unlabelled.
#
# No link is ever opened: the bare one is only hovered, and the labelled one's
# confirmation is always dismissed. Its own daemon on its own port.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin links 9650 /tmp/oc-links-fixture "Links Fixture" "$@"

CONF_LINK_OPEN=12     # the confirmation's enum value, as the dump prints it

# The top-left of message row $1's body, from the dump.
body_xy() { field "$D" "^  msgrow $1 " body; }
hover_is() { gui_snap; grep -q "^link hover=\"$1\" labelled=$2 " "$D"; }

step "a labelled link and a bare one"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
# The labelled one back from the daemon before the bare one is sent, so they
# are drawn in that order.
send_stored "[the release notes](https://example.com/notes)"
send_stored "https://example.com/plain"
two() { gui_snap && [ "$(field "$D" '^msgrows ' n)" -ge 2 ] 2>/dev/null; }
check "both are drawn" waitfor 10 two
n="$(field "$D" '^msgrows ' n)"; n="${n:-2}"
IFS=, read -r lx ly <<< "$(body_xy $((n - 2)))"
IFS=, read -r bx by <<< "$(body_xy $((n - 1)))"

step "hovering the label shows where it goes"
"$DRIVE" move $((lx + 12)) $((ly + 8)) >/dev/null
check "the hover reports the ADDRESS, marked labelled" waitfor 5 hover_is "https://example.com/notes" 1
"$DRIVE" shot links_hover >/dev/null

step "clicking asks first"
"$DRIVE" click $((lx + 12)) $((ly + 8)) >/dev/null
asked() { gui_snap; grep -q "^confirm open=1 act=$CONF_LINK_OPEN url=\"https://example.com/notes\"" "$D"; }
check "a confirmation names the address" waitfor 5 asked
"$DRIVE" shotfull links_confirm >/dev/null
"$DRIVE" key esc >/dev/null
dismissed() { gui_snap; grep -q '^confirm open=0 ' "$D"; }
check "Esc dismisses it, and nothing is opened" waitfor 5 dismissed

step "a bare address is its own label"
"$DRIVE" move $((bx + 12)) $((by + 8)) >/dev/null
check "the hover reports it, unlabelled" waitfor 5 hover_is "https://example.com/plain" 0
"$DRIVE" move 20 20 >/dev/null
gui_end
