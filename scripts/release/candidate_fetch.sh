#!/usr/bin/env bash
# Fetch a candidate from the mirror bucket (DIST_MIRROR_BUCKET, candidates/) and verify it.
#
#   candidate_fetch.sh <build-id> <dest-dir>
#
# Needs the `dist-mirror` rclone remote configured (as release-candidate.yml
# and release.yml do) and DIST_MIRROR_BUCKET set; the signing key imported,
# for the verification. Exits 1 when there is no stored build for the build id:
# the staging build never finished for that commit, or its build was pruned.
set -euo pipefail
id="${1:?build-id}"; dest="${2:?dest}"
src="dist-mirror:${DIST_MIRROR_BUCKET:?DIST_MIRROR_BUCKET}/candidates/${id}"
if ! rclone lsf "${src}/manifest.json" 2>/dev/null | grep -q .; then
  echo "candidate_fetch: no candidate for build ${id} in the mirror bucket: no staging build finished for this commit, or its build was pruned" >&2
  exit 1
fi
rm -rf "$dest" && mkdir -p "$dest"
rclone copy "$src" "$dest" --fast-list
"$(dirname "$0")/candidate_manifest.sh" verify "$dest" "$id"
