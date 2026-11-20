#!/usr/bin/env bash
# The speech data's archive tool (scripts/build/speech_sources.sh), offline: a
# stand-in archive of the same seven files, with fake contents and pins of its
# own, served from a file:// directory. `fetch` downloads, joins, checks and
# unpacks it, and marks it checked; a changed byte in a part, a joined bundle or
# an unpacked file is each refused naming that file, and leaves no mark; `check`
# agrees on the good copy and on each damaged one. The real archive is never
# touched.
#
# Needs: curl, sha256sum, tar, split. Run by: scripts/test/fast/source.sh (make test).
set -uo pipefail
cd "$(dirname "$0")/../../.."
tool="$(pwd)/scripts/build/speech_sources.sh"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail() { echo "speech_sources: FAIL: $*" >&2; exit 1; }

# The stand-in: two source bundles split as the archive splits them, the
# input list, the archive list and the pins.
src="$tmp/src"; srv="$tmp/srv"; mkdir -p "$src/voice-src" "$src/recognizer-src" "$srv"
for i in 1 2 3; do head -c 40000 /dev/urandom > "$src/voice-src/v$i.bin"; done
for i in 1 2; do head -c 30000 /dev/urandom > "$src/recognizer-src/r$i.bin"; done
( cd "$src" && sha256sum voice-src/* recognizer-src/* ) > "$srv/SHA256SUMS-inputs.txt"
tar -C "$src" -czf "$tmp/voice-src.tar.gz" voice-src
tar -C "$src" -czf "$tmp/recognizer-src.tar.gz" recognizer-src
( cd "$srv" && split -n 3 -d -a 1 "$tmp/voice-src.tar.gz" voice-src.part && split -n 2 -d -a 1 "$tmp/recognizer-src.tar.gz" recognizer-src.part )
for p in "$srv"/*.part?; do mv "$p" "$p.gz"; done
( cd "$srv" && sha256sum ./*.gz SHA256SUMS-inputs.txt | sed 's| \./| |' ) > "$srv/SHA256SUMS-archive.txt"
( cd "$srv" && sha256sum voice-src.part*.gz recognizer-src.part*.gz \
    SHA256SUMS-archive.txt SHA256SUMS-inputs.txt
  cd "$tmp" && sha256sum voice-src.tar.gz recognizer-src.tar.gz ) > "$tmp/pins"
[ "$(ls "$srv"/voice-src.part*.gz | wc -l)" = 3 ] && [ "$(ls "$srv"/recognizer-src.part*.gz | wc -l)" = 2 ] ||
  fail "the stand-in is not split as the archive is"

run() { OC_SPEECH_ARCHIVE="file://$srv" OC_SPEECH_SUMS="$tmp/pins" "$tool" "$@" > "$tmp/out" 2>&1; }

# Fetched whole: unpacked, every file as it was, marked checked; check agrees.
d="$tmp/good"
run fetch "$d" || { cat "$tmp/out" >&2; fail "fetch of a good archive failed"; }
[ -f "$d/.checked" ] || fail "fetch did not mark a good copy checked"
cmp -s "$d/voice-src/v2.bin" "$src/voice-src/v2.bin" && cmp -s "$d/recognizer-src/r1.bin" "$src/recognizer-src/r1.bin" ||
  fail "fetch did not unpack what the archive holds"
run check "$d" || { cat "$tmp/out" >&2; fail "check refused a good copy"; }

# expect_refused <what> <name>: the last run failed and named <name>.
expect_refused() {
  [ "$1" = 1 ] || fail "$3 passed"
  grep -q -- "$2" "$tmp/out" || { cat "$tmp/out" >&2; fail "$3 was refused without naming $2"; }
}
# flip <file>: invert one byte, so the file is always changed.
flip() {
  local b; b=$(od -An -tu1 -j100 -N1 "$1" | tr -d ' ')
  printf "\\$(printf '%03o' $((255 - b)))" | dd of="$1" bs=1 seek=100 conv=notrunc 2>/dev/null
}

# A changed byte in a part on the server: fetch refuses it, and marks nothing.
cp "$srv/voice-src.part1.gz" "$tmp/keep"; flip "$srv/voice-src.part1.gz"
d="$tmp/bad-part"; run fetch "$d"; rc=$?
expect_refused "$([ $rc != 0 ] && echo 1)" voice-src.part1.gz "a changed part"
[ ! -f "$d/.checked" ] || fail "a refused fetch was marked checked"
cp "$tmp/keep" "$srv/voice-src.part1.gz"

# A changed byte in a joined bundle, then in an unpacked file: check refuses each.
d="$tmp/bad-bundle"; cp -r "$tmp/good" "$d"; flip "$d/recognizer-src.tar.gz"
run check "$d"; rc=$?
expect_refused "$([ $rc != 0 ] && echo 1)" recognizer-src.tar.gz "a changed bundle"
d="$tmp/bad-file"; cp -r "$tmp/good" "$d"; flip "$d/voice-src/v3.bin"
run check "$d"; rc=$?
expect_refused "$([ $rc != 0 ] && echo 1)" voice-src/v3.bin "a changed unpacked file"

# The pins the tool ships cover the seven attachments and the two bundles.
n=$(grep -c . scripts/build/speech_sources.sha256)
[ "$n" = 9 ] || fail "scripts/build/speech_sources.sha256 holds $n pins, not 9"

echo "speech_sources: ok: fetch, join, check and unpack a stand-in archive; a changed part, bundle and file are each refused by name"
