#!/usr/bin/env bash
# A stored build, fetched and verified: what release-candidate.yml publishes
# as a release-candidate and release.yml ships, each checking it the same way
# before it does. The build is fetched from the mirror bucket into <dir> and
# its signed manifest and every file's sha256 verified
# (scripts/release/candidate_fetch.sh); then the commit the manifest records
# must be <commit>, the amd64 daemon, unpacked and asked, must report the
# build id (the id the code carries, not merely the directory's name), and the
# image the manifest names must be in the build repository with the digest it
# recorded.
#
#   scripts/release/verify_candidate.sh <build-id> <commit> <dir>
#
# Needs: the `dist-mirror` rclone remote and DIST_MIRROR_BUCKET, the signing
# key imported (as candidate_fetch.sh); skopeo; BUILD_IMAGE,
# IMAGE_REGISTRY_USER and IMAGE_REGISTRY_TOKEN_BUILD. Exits 1, with an
# annotation, on the first thing that is not as it should be.
set -euo pipefail
id=${1:?usage: verify_candidate.sh <build-id> <commit> <dir>} sha=${2:?commit} dir=${3:?dir}
err() { echo "::error::$*"; exit 1; }
for v in BUILD_IMAGE IMAGE_REGISTRY_USER IMAGE_REGISTRY_TOKEN_BUILD; do
  [ -n "${!v:-}" ] || err "$v is not set"
done
"$(dirname "$0")/candidate_fetch.sh" "$id" "$dir"
recorded=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["sha"])' "$dir/manifest.json")
[ "$recorded" = "$sha" ] || err "build ${id} records the commit ${recorded}, not ${sha}"
check=$(mktemp -d)
trap 'rm -rf "$check"' EXIT
tar xzf "$dir/linux-amd64/openchimed-build-${id}-linux-amd64.tar.gz" -C "$check"
got=$("$check/openchimed-build-${id}-linux-amd64/openchimed" --version)
echo "$got"
grep -q "^openchimed ${id} " <<<"$got" || err "the build's daemon reports '${got}', not build ${id}"
want=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["images"]["build-" + sys.argv[2]])' "$dir/manifest.json" "$id")
have="sha256:$(skopeo inspect --raw --creds "${IMAGE_REGISTRY_USER}:${IMAGE_REGISTRY_TOKEN_BUILD}" \
                 "docker://${BUILD_IMAGE}:build-${id}" | sha256sum | cut -d' ' -f1)"
[ "$have" = "$want" ] || err "${BUILD_IMAGE}:build-${id} is ${have}, the manifest says ${want}"
echo "build ${id}: commit ${sha:0:7}, its files, its daemon and ${BUILD_IMAGE}:build-${id} as the manifest says"
