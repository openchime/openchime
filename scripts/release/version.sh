#!/usr/bin/env bash
# Resolve release numbers from the `release-N` tags (ARCH-20).
#
# A monotonically increasing integer, assigned to one candidate build (which
# is identified by its commit, the build id) when it is released: the number
# IS the Debian package version, so the release asks for `next`, reserves it
# as a tag, packages the candidate's binaries under it and publishes.
#
#   scripts/release/version.sh next     -> the number a new release would take
#   scripts/release/version.sh current  -> the newest released number (0 if none)
#
# A gap in the sequence is not an error: a failed release leaves its number
# unused, and reusing it would mean two different artifacts sharing a version.
# So `next` is always "highest + 1", never "lowest unused".
set -euo pipefail

mode="${1:-next}"

# --merged is deliberately absent: a release tag is a fact about what was
# published, not about the current branch's history. Asking only this branch
# would let a release on another branch be silently reissued with the same
# number, and two .debs sharing a version is the one thing apt cannot recover
# from.
current=$(git tag -l 'release-*' \
          | sed 's/^release-//' \
          | grep -E '^[0-9]+$' \
          | sort -n | tail -1 || true)
current="${current:-0}"

case "$mode" in
  current) echo "$current" ;;
  next)    echo "$(( current + 1 ))" ;;
  *)
    echo "usage: scripts/release/version.sh [next|current]" >&2
    exit 2
    ;;
esac
