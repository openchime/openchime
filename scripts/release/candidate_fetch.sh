#!/usr/bin/env bash
# Fetch a candidate from the staging bucket and verify it (docs/RELEASING.md).
#
#   candidate_fetch.sh <build-id> <dest-dir>
#
# Needs the `dist-staging` rclone remote configured (as release.yml does) and
# DIST_STAGING_BUCKET set; the signing key imported, for the verification.
# Exits 1 when there is no candidate for the build id, which is the one way a
# commit reaches main without staging having built it: say so plainly.
set -euo pipefail
id="${1:?build-id}"; dest="${2:?dest}"
src="dist-staging:${DIST_STAGING_BUCKET:?DIST_STAGING_BUCKET}/candidates/${id}"
if ! rclone lsf "${src}/manifest.json" 2>/dev/null | grep -q .; then
  echo "candidate_fetch: no candidate for build ${id} in the staging bucket: staging's candidate build did not finish for this commit" >&2
  exit 1
fi
rm -rf "$dest" && mkdir -p "$dest"
rclone copy "$src" "$dest" --fast-list
"$(dirname "$0")/candidate_manifest.sh" verify "$dest" "$id"
