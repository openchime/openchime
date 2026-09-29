#!/usr/bin/env bash
# Links in the Win32 client's transcript (REQ-220, MARKDOWN.md §4).
#
#   scripts/gui_links.sh          # run, and leave the client running
#   scripts/gui_links.sh --kill   # ... and shut it down afterwards
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
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9650}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-links-fixture}"
export OC_DEV_WS="${OC_DEV_WS:-Links Fixture}"

CONF_LINK_OPEN=12     # the confirmation's enum value, as the dump prints it

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"

D="/tmp/oc-links-dump.txt"
snap() { "$DRIVE" dump links >/dev/null 2>&1; cp "$LIN_DIR/links.txt" "$D" 2>/dev/null; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# The top-left of message row $1's body, from the dump.
body_xy() { grep "^  msgrow $1 " "$D" | grep -o 'body=[0-9]*,[0-9]*' | cut -d= -f2; }
hover_is() { snap; grep -q "^link hover=\"$1\" labelled=$2 " "$D"; }

say "== a labelled link and a bare one"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" send "[the release notes](https://example.com/notes)" >/dev/null; sleep 0.4
"$DRIVE" send "https://example.com/plain" >/dev/null
two() { snap; [ "$(grep -m1 '^msgrows ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)" -ge 2 ]; }
check "both are drawn" waitfor 10 two
n=$(grep -m1 '^msgrows ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)
IFS=, read -r lx ly <<< "$(body_xy $((n - 2)))"
IFS=, read -r bx by <<< "$(body_xy $((n - 1)))"

say "== hovering the label shows where it goes"
"$DRIVE" move $((lx + 12)) $((ly + 8)) >/dev/null
check "the hover reports the ADDRESS, marked labelled" waitfor 5 hover_is "https://example.com/notes" 1
"$DRIVE" shot links_hover >/dev/null

say "== clicking asks first"
"$DRIVE" click $((lx + 12)) $((ly + 8)) >/dev/null
asked() { snap; grep -q "^confirm open=1 act=$CONF_LINK_OPEN url=\"https://example.com/notes\"" "$D"; }
check "a confirmation names the address" waitfor 5 asked
"$DRIVE" shotfull links_confirm >/dev/null
"$DRIVE" key esc >/dev/null
dismissed() { snap; grep -q '^confirm open=0 ' "$D"; }
check "Esc dismisses it, and nothing is opened" waitfor 5 dismissed

say "== a bare address is its own label"
"$DRIVE" move $((bx + 12)) $((by + 8)) >/dev/null
check "the hover reports it, unlabelled" waitfor 5 hover_is "https://example.com/plain" 0
"$DRIVE" move 20 20 >/dev/null
rm -f "$D"

say ""
say "gui_links: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
