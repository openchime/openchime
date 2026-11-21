#!/usr/bin/env bash
# Fetch and check the archived inputs of the speech data (docs/VENDORS.md): the
# published files the read-aloud voice model and the voice-input recognizer are
# converted from.
#
#   scripts/build/speech_sources.sh fetch DIR   download, join, check, unpack
#   scripts/build/speech_sources.sh check DIR   check a copy already in DIR
#
# The archive is seven files: two source bundles split under the attachment
# size limit (voice-src.part*.gz, recognizer-src.part*.gz) and two checksum
# lists.
# `fetch` downloads them into DIR, joins each bundle's parts, and checks every
# attachment and both bundles against scripts/build/speech_sources.sha256. It
# then unpacks DIR/voice-src and DIR/recognizer-src, and checks every unpacked
# source file against
# SHA256SUMS-inputs.txt, itself pinned above; those are the SHA-256s
# docs/VENDORS.md records. A file that does not match stops it, named. DIR/.checked
# is written last, only when everything matched, and removed first.
#
# `check` repeats every check on what DIR holds, with no network, so a copy of the
# archive kept elsewhere can be checked as it stands; a bundle not yet joined is
# joined from its parts first.
#
# A maintainer's tool: not run by make, the suites or CI. OC_SPEECH_ARCHIVE
# replaces the archive with a directory or URL holding the seven files side by
# side (a mirror, or the test's stand-in); OC_SPEECH_SUMS replaces the pin file.
# Needs curl, sha256sum and tar.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
SUMS="${OC_SPEECH_SUMS:-$here/speech_sources.sha256}"

# Each attachment, where it is archived.
ATTACHMENTS=(
  "35148170|voice-src.part0.gz"
  "35148174|voice-src.part1.gz"
  "35148175|voice-src.part2.gz"
  "35148176|recognizer-src.part0.gz"
  "35148172|recognizer-src.part1.gz"
  "35148169|SHA256SUMS-archive.txt"
  "35148171|SHA256SUMS-inputs.txt"
)
BASE="https://github.com/user-attachments/files"

die() { echo "speech_sources: $*" >&2; exit 1; }
usage() { echo "usage: speech_sources.sh fetch|check DIR" >&2; exit 2; }

# Every pinned file present in DIR and matching; the first that is not, named.
check_pins() {
  local dir="$1" sum name got
  [ -f "$SUMS" ] || die "no pin file $SUMS"
  while read -r sum name; do
    [ -n "$sum" ] || continue
    [ -f "$dir/$name" ] || die "$name is missing from $dir"
    got="$(sha256sum "$dir/$name" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || die "$name does not match its pin (expected $sum, got $got)"
  done < "$SUMS"
}

# Every unpacked source file against SHA256SUMS-inputs.txt (pinned by check_pins).
check_inputs() {
  local dir="$1" sum name got
  while read -r sum name; do
    [ -n "$sum" ] || continue
    [ -f "$dir/$name" ] || die "$name is missing from $dir"
    got="$(sha256sum "$dir/$name" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || die "$name does not match SHA256SUMS-inputs.txt (expected $sum, got $got)"
  done < "$dir/SHA256SUMS-inputs.txt"
}

# Each bundle from its parts, in order. With `missing`, only a bundle not there.
join_parts() {
  local dir="$1" only="${2:-}" b parts
  for b in voice-src recognizer-src; do
    [ "$only" = missing ] && [ -f "$dir/$b.tar.gz" ] && continue
    compgen -G "$dir/$b.part*.gz" >/dev/null || die "no parts of $b in $dir"
    mapfile -t parts < <(printf '%s\n' "$dir"/$b.part*.gz | sort -V)
    cat "${parts[@]}" > "$dir/$b.tar.gz"
  done
}

[ $# -eq 2 ] || usage
mode="$1"; dir="$2"
case "$mode" in
fetch)
  mkdir -p "$dir"
  rm -f "$dir/.checked"
  for a in "${ATTACHMENTS[@]}"; do
    id="${a%%|*}"; name="${a#*|}"
    if [ -n "${OC_SPEECH_ARCHIVE:-}" ]; then url="${OC_SPEECH_ARCHIVE%/}/$name"; else url="$BASE/$id/$name"; fi
    echo "speech_sources: fetching $name"
    curl -fsSL -o "$dir/$name.dl" "$url" || die "cannot fetch $name from $url"
    mv "$dir/$name.dl" "$dir/$name"
  done
  join_parts "$dir"
  check_pins "$dir"
  rm -rf "$dir/voice-src" "$dir/recognizer-src"
  tar -xzf "$dir/voice-src.tar.gz" -C "$dir"
  tar -xzf "$dir/recognizer-src.tar.gz" -C "$dir"
  check_inputs "$dir"
  touch "$dir/.checked"
  echo "speech_sources: fetched and checked into $dir"
  ;;
check)
  [ -d "$dir" ] || die "no directory $dir"
  join_parts "$dir" missing
  check_pins "$dir"
  check_inputs "$dir"
  echo "speech_sources: every file in $dir matches"
  ;;
*) usage ;;
esac
