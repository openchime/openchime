#!/usr/bin/env bash
# Resolve release numbers from the `release-N` tags (ARCH-20).
#
# The scheme is the sibling control plane's: a monotonically increasing integer,
# tagged after the release succeeds. The one difference is *when* the number is
# computed. openchime-saas tags after the fact, because its artifact is
# identified by a commit sha; here the number IS the Debian package version, so
# it has to exist before anything is built. The release workflow therefore asks
# for `next` up front, builds and publishes with it, and tags only on success.
#
#   version.sh next     -> the number a new release would take
#   version.sh current  -> the newest released number (0 if none)
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
    echo "usage: version.sh [next|current]" >&2
    exit 2
    ;;
esac
