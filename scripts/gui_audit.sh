#!/usr/bin/env bash
# Walk every surface of the Win32 client, in every scale and theme, and check
# each one against properties it must hold ON ITS OWN.
#
#   scripts/gui_audit.sh              # the full matrix
#   scripts/gui_audit.sh --quick      # one state per scene, for a fast read
#   scripts/gui_audit.sh --scene home # one scene, every state
#   scripts/gui_audit.sh --keep       # leave the client running afterwards
#
# THIS IS NOT THE SMOKE, AND MUST NOT BECOME IT. `gui_smoke.sh` answers "does
# the client boot" in ten seconds and runs before every push; this walks a few
# hundred states and takes minutes. Both exist because a check nobody runs and a
# check that finds nothing are different failures. Run this when GUI chrome
# changes, and read it; run the smoke always.
#
# WHY THERE IS NO REFERENCE IMAGE. The audit this replaces diffed the SDL render
# against the Direct2D binary it was ported from, and finished the job a diff can
# do. A diff finds DIVERGENCE, so it is blind by construction to anything both
# renderers did the same way -- which is every defect the two of them inherited,
# and that is what is left. Everything here is a property of one scene: a string
# that fits its box, ink that can be read against what is behind it, one label
# not drawn over another. See scripts/audit/oracles.py.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
SCENES="$HERE/scripts/audit/scenes.tsv"
ORACLES="$HERE/scripts/audit/oracles.py"
WIN_DIR='C:\Windows\Temp\octest'
LIN_DIR='/mnt/c/Windows/Temp/octest'
OUT="${OC_AUDIT_OUT:-/tmp/oc-audit}"

# Its OWN daemon on its OWN port, for the reason the smoke gives at length:
# a run that adopts whatever is listening can be wrong about which machine it is
# testing, and then everything it reports is confident nonsense.
export OC_DEV_PORT="${OC_DEV_PORT:-9550}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-audit-dev}"
export OC_DEV_WS="${OC_DEV_WS:-Audit Fixture}"

quick=0 keep=0 only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) quick=1 ;;
    --keep)  keep=1 ;;
    --scene) only="${2:-}"; shift ;;
    *) echo "usage: gui_audit.sh [--quick] [--keep] [--scene <id>]" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$OUT"
rm -f "$OUT"/*.tsv "$OUT"/*.txt "$OUT"/*.bmp 2>/dev/null

# --- the state matrix ------------------------------------------------------
# Theme, DPI and text size, because those are the three axes on which a fixed
# number in a layout stops being right. 96 is the unscaled case, 144 is the
# commonest laptop, 192 is where anything with slack left in it runs out.
if [ "$quick" = 1 ]; then
  STATES=("light 96 1")
else
  STATES=(
    "light 96 1"  "dark 96 1"
    "light 144 1" "dark 144 1"
    "light 96 3"  "dark 96 3"
    "light 192 3" "dark 192 3"
  )
fi

pass=0; fail=0; scenes_run=0
declare -A FINDINGS

drive() { "$DRIVE" "$@" >/dev/null 2>&1; }

# Poll for the state being asserted rather than sleeping a guess -- the rule the
# smoke states and the reason this audit's first version misread a loading pane
# as a layout defect.
settle() {   # settle <key> <want>
  local key="$1" want="$2" t=0 got
  while [ "$t" -lt 4000 ]; do
    drive dump settle
    got=$(grep -o "\b$key=[^ ]*" "$LIN_DIR/settle.txt" 2>/dev/null | head -1 | cut -d= -f2)
    [ "$got" = "$want" ] && return 0
    sleep 0.05; t=$((t + 50))
  done
  return 1
}

# --- fixture ---------------------------------------------------------------
# Scenes need something to draw. An empty workspace exercises the empty states
# and nothing else, and the defects this audit is for live in rows, avatars and
# wrapped text.
seed() {
  local c
  [ -x "$HERE/build/demo_client" ] || make -C "$HERE" demo-client >/dev/null 2>&1
  for c in engineering design releases; do drive mkchan "$c 1"; done
  sleep 1
  send() { "$HERE/build/demo_client" 127.0.0.1 "$OC_DEV_PORT" "$1" pw send "$2" "$3" >/dev/null 2>&1; }
  send alice 1 "Welcome to the audit fixture."
  send bob   1 "A reply, so a row has a neighbour."
  send carol 1 "And a third, so grouping has something to group."
  send bob   2 "A message long enough to wrap at every text size in the matrix, which is the state that finds a box somebody sized for one of them."
  send alice 2 "Short."
  send carol 3 "One more channel with content."
  # A group DM and a one-to-one, so the DM index has both shapes in it: a stack
  # of faces with no name, and a single face with a presence marker.
  drive groupdm bob,carol
  drive groupdm bob
  sleep 1
}

echo "audit: launching on :$OC_DEV_PORT"
"$DRIVE" launch >/dev/null 2>&1 || { echo "audit: the client did not start" >&2; exit 1; }
seed
# 1024x768 of CLIENT area. The window is larger by its frame; the audit measures
# the scene, so the number that matters is the one the ledger reports.
drive size 1042 815

for state in "${STATES[@]}"; do
  set -- $state
  theme="$1"; dpi="$2"; ts="$3"
  drive theme "$([ "$theme" = dark ] && echo 0 || echo 1)"
  drive dpi "$dpi"
  drive textsize "$ts"
  sleep 0.4
  tag="$theme-${dpi}dpi-ts$ts"

  while IFS=$'\t' read -r id verbs key want; do
    case "$id" in ''|\#*) continue ;; esac
    [ -n "$only" ] && [ "$only" != "$id" ] && continue

    # Every scene starts from the same place. Without this a modal left open by
    # the previous row becomes part of the next one's scene, and the finding is
    # filed against the wrong surface.
    drive key esc; drive view home
    ok=1
    IFS=';' read -ra steps <<< "$verbs"
    for v in "${steps[@]}"; do
      if ! "$DRIVE" $v >/dev/null 2>&1; then
        echo "  FAIL $tag/$id — the client did not answer '$v'"
        ok=0; break
      fi
    done
    [ "$ok" = 1 ] || { fail=$((fail + 1)); continue; }

    if ! settle "$key" "$want"; then
      echo "  FAIL $tag/$id — never reached $key=$want"
      fail=$((fail + 1)); continue
    fi

    drive "ledger ${WIN_DIR}\\aud.tsv"
    drive dump aud
    # The picture too. The pixel checks need it, and so do the contact sheets --
    # the sheets are the only instrument for the findings no oracle reaches, and
    # they cannot be built after the fact from a ledger alone.
    drive shotfull "aud"
    cp "$LIN_DIR/aud.tsv" "$OUT/$tag.$id.ledger.tsv" 2>/dev/null
    cp "$LIN_DIR/aud.txt" "$OUT/$tag.$id.dump.txt" 2>/dev/null
    cp "$LIN_DIR/aud.bmp" "$OUT/$tag.$id.bmp" 2>/dev/null
    scenes_run=$((scenes_run + 1))

    px=""
    [ -f "$OUT/$tag.$id.bmp" ] && px=$(python3 "$HERE/scripts/audit/pixels.py" \
        "$OUT/$tag.$id.ledger.tsv" "$OUT/$tag.$id.bmp" 2>&1)
    if out=$(python3 "$ORACLES" "$OUT/$tag.$id.ledger.tsv" "$OUT/$tag.$id.dump.txt" 2>&1) \
       && [ -z "$px" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "  $tag/$id"
      echo "$out" | sed 's/^/    /'
      [ -n "$px" ] && echo "$px" | sed 's/^/    /'
      # Count by check, so the summary says what KIND of thing is wrong rather
      # than only how much of it there is.
      while read -r k n; do
        [ -n "$k" ] && FINDINGS[$k]=$(( ${FINDINGS[$k]:-0} + n ))
      done < <(echo "$out" | sed -n 's/^  \([a-z-]*\): \([0-9]*\)$/\1 \2/p')
    fi
  done < "$SCENES"
done

[ "$keep" = 1 ] || drive kill

echo
echo "audit: $scenes_run scenes captured, $pass clean, $fail with findings"
if [ ${#FINDINGS[@]} -gt 0 ]; then
  echo "by check:"
  for k in "${!FINDINGS[@]}"; do echo "  $k: ${FINDINGS[$k]}"; done
fi
echo "captures in $OUT"
[ "$fail" = 0 ]
