#!/usr/bin/env bash
# tools/gui/lint.sh
#
# The GUI feature scripts keep one shape (tools/gui/README.md, "A feature
# script"), which is what lets a person read any of them, and what makes each
# stop what it started and say where it stopped. A feature script is every
# tools/gui/features/*.sh. Each:
#
#   shape      sources tools/gui/harness/lib.sh, calls gui_begin, and ends
#              with gui_end;
#   own        defines none of what lib.sh gives it (waitfor, gui_snap, snap,
#              field, fieldq, check, need, ok, fail, say, act, ack_of, dfile,
#              seen, click_rect, aid_xy, press, cpath, make_png, bmp_px,
#              newest_is, send_stored, upload_stored, dev_daemon_pids),
#              handles no --kill or --down of its own and prints no result line
#              of its own (`gui_<name>: ...`);
#   sleep      calls sleep only where the pause is itself under test, and says
#              so on the line or the line before: # paced: <why>
#   platforms  declares the platforms it runs on in its header, the words
#              win32 and web only: # platforms: win32 web
#
# And across tools/gui:
#
#   names      every OC_TEST_* name set or used is read somewhere in the
#              harness or the scripts (${NAME:-...}, ${NAME}, process.env.NAME):
#              a name nothing reads is a setting that does nothing.
#
# Prints file:line: rule: text for each, and exits 1 if there is any. Before
# the tree, it is run on a stand-in tree that breaks every rule, and must
# report each, and nothing in a stand-in that keeps them.
#
# Run by: tools/gui/run.sh, before anything else; by hand as it is.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 2

lint() {   # lint <root>: the findings, one per line
  local root="$1" f n names
  for f in "$root"/tools/gui/features/*.sh; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .sh)"
    awk -v F="tools/gui/features/$n.sh" '
      { line[NR] = $0 }
      /^[ \t]*\. "\$HERE\/tools\/gui\/harness\/lib\.sh"/ { lib = 1 }
      /^[ \t]*gui_begin[ \t]/ { begin = 1 }
      /^# platforms:/ && !plat {
        plat = NR; p = $0; sub(/^# platforms:[ \t]*/, "", p)
        k = split(p, w, /[ \t]+/); good = 0
        for (i = 1; i <= k; i++) {
          if (w[i] == "win32" || w[i] == "web") good++
          else if (w[i] != "") printf "%s:%d: platforms: names only win32 and web, not \"%s\": %s\n", F, NR, w[i], $0
        }
        if (!good) printf "%s:%d: platforms: names no platform: %s\n", F, NR, $0
      }
      !/^[ \t]*#/ && /^[ \t]*(waitfor|gui_snap|snap|field|fieldq|check|need|ok|fail|say|act|ack_of|dfile|seen|click_rect|aid_xy|press|cpath|make_png|bmp_px|newest_is|send_stored|upload_stored|dev_daemon_pids)[ \t]*\(\)/ {
        printf "%s:%d: own: lib.sh gives this; use it: %s\n", F, NR, $0 }
      !/^[ \t]*#/ && /"--(kill|down)"/ {
        printf "%s:%d: own: gui_begin stops what the script started (--keep leaves it): %s\n", F, NR, $0 }
      !/^[ \t]*#/ && /say[ \t]+"gui_[a-z_]+:/ {
        printf "%s:%d: own: gui_end prints the result line: %s\n", F, NR, $0 }
      !/^[ \t]*#/ && /(^|[^A-Za-z0-9_.-])sleep[ \t]/ && $0 !~ /# paced: *[^ ]/ && line[NR-1] !~ /# paced: *[^ ]/ {
        printf "%s:%d: sleep: waitfor what the client shows, or say # paced: <why>: %s\n", F, NR, $0 }
      NF { last = $0; lastn = NR }
      END {
        if (!lib) printf "%s:1: shape: sources tools/gui/harness/lib.sh\n", F
        if (!begin) printf "%s:1: shape: calls gui_begin\n", F
        if (last !~ /^[ \t]*gui_end[ \t]*$/) printf "%s:%d: shape: ends with gui_end: %s\n", F, lastn, last
        if (!plat) printf "%s:1: platforms: no # platforms: line (win32, web or both)\n", F
      }' "$f"
  done
  # Every OC_TEST_* name in tools/gui, read by something there: the harness, a
  # driver, run.sh or a script that takes it. Not this file's: its stand-ins.
  local files=() fl
  while IFS= read -r fl; do files+=("$fl"); done < <(find "$root/tools/gui" -type f \( -name '*.sh' -o -name '*.mjs' \) ! -path "$root/tools/gui/lint.sh" | sort)
  [ "${#files[@]}" -gt 0 ] || return 0
  names="$(grep -ohE 'OC_TEST_[A-Z0-9_]+' "${files[@]}" | sort -u)"
  for n in $names; do
    grep -qE "\\$\{$n(:|\}|-)|process\.env\.$n\b" "${files[@]}" && continue
    grep -nE "\b$n\b" "${files[@]}" | head -1 | sed "s#^$root/##; s#^\([^:]*:[0-9]*\):.*#\1: names: $n is set, and nothing in tools/gui reads it#"
  done
}

# --- the stand-in: a script breaking each rule, which must each be found -------
scratch="$(mktemp -d)"; trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/tools/gui/features" "$scratch/tools/gui/harness"
cat > "$scratch/tools/gui/harness/lib.sh" <<'SH'
gui_begin() { :; }
port="${OC_TEST_READ:-1}"
SH
cat > "$scratch/tools/gui/features/good.sh" <<'SH'
# platforms: win32 web
. "$HERE/tools/gui/harness/lib.sh"
gui_begin good 9999 /x
OC_TEST_READ=2 "$DRIVE" launch
# paced: the hold is what is tested
sleep 1
check "x" waitfor 5 true
gui_end
SH
cat > "$scratch/tools/gui/features/bad.sh" <<'SH'
waitfor() { :; }
[ "${1:-}" = "--kill" ] && x
sleep 0.3
say "gui_bad: all passed"
OC_TEST_UNREAD=1 x
SH
cat > "$scratch/tools/gui/features/badplat.sh" <<'SH'
# platforms: win32 mac
. "$HERE/tools/gui/harness/lib.sh"
gui_begin badplat 9998 /y
gui_end
SH
cat > "$scratch/tools/gui/features/noplat.sh" <<'SH'
# platforms:
. "$HERE/tools/gui/harness/lib.sh"
gui_begin noplat 9997 /z
gui_end
SH
got="$(lint "$scratch")"
want=(
  'bad.sh:1: own:'
  'bad.sh:2: own:'
  'bad.sh:3: sleep:'
  'bad.sh:4: own:'
  'bad.sh:1: shape: sources'
  'bad.sh:1: shape: calls gui_begin'
  'bad.sh:5: shape: ends with gui_end'
  'bad.sh:1: platforms: no # platforms: line'
  'badplat.sh:1: platforms: names only win32 and web, not "mac"'
  'noplat.sh:1: platforms: names no platform'
  'bad.sh:5: names: OC_TEST_UNREAD'
)
selfbad=0
grep -F 'good.sh:' <<<"$got" >/dev/null && { echo "lint: the stand-in that keeps every rule was reported: $(grep -F good.sh: <<<"$got")" >&2; selfbad=1; }
grep -F 'OC_TEST_READ' <<<"$got" >/dev/null && { echo "lint: a name the harness reads was reported: $(grep -F OC_TEST_READ <<<"$got")" >&2; selfbad=1; }
grep -F 'harness/lib.sh:' <<<"$got" >/dev/null && { echo "lint: the harness was held to the feature shape" >&2; selfbad=1; }
for w in "${want[@]}"; do
  grep -F -- "$w" <<<"$got" >/dev/null || { echo "lint: the stand-in breaking a rule was not reported: $w" >&2; selfbad=1; }
done
[ "$selfbad" = 0 ] || { echo "lint: the self-test failed; the tree was not checked" >&2; exit 1; }

# --- the tree -----------------------------------------------------------------
out="$(lint .)"
if [ -n "$out" ]; then
  while IFS= read -r l; do echo "lint: $l" >&2; done <<<"$out"
  echo "lint: the GUI feature scripts keep one shape (tools/gui/README.md, \"A feature script\")" >&2
  exit 1
fi
echo "lint: $(ls tools/gui/features/*.sh | wc -l) feature scripts, every one in shape"
