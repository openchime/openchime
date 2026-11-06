#!/usr/bin/env bash
# scripts/release/smoke/indexes.sh
#
# Each index, read back from each bucket, lists one version more per
# architecture than before the release: the count recorded before publishing
# (APT_EXPECT, RPM_EXPECT), plus this release exactly once, every package it
# names present, and the signed top matching the files it names
# (scripts/release/repo_update.sh verify). Read from the buckets, not the
# public address.
#
# Needs: rclone; APT_EXPECT, RPM_EXPECT; DIST_S3_ACCESS_KEY,
# DIST_S3_SECRET_KEY, DIST_S3_ENDPOINT, DIST_S3_REGION, DIST_BUCKET,
# DIST_S3_MIRROR_ACCESS_KEY, DIST_S3_MIRROR_SECRET_KEY, DIST_MIRROR_BUCKET.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need rclone "rclone"
[ -n "${APT_EXPECT:-}" ] && [ -n "${RPM_EXPECT:-}" ] || fail "publish recorded no counts to check (APT_EXPECT, RPM_EXPECT)"
for v in DIST_S3_ACCESS_KEY DIST_S3_SECRET_KEY DIST_S3_ENDPOINT DIST_BUCKET \
         DIST_S3_MIRROR_ACCESS_KEY DIST_S3_MIRROR_SECRET_KEY DIST_MIRROR_BUCKET; do
    [ -n "${!v:-}" ] || fail "$v is not set"
done
conf=$(mktemp)
trap 'rm -f "$conf"' EXIT
cat > "$conf" <<CONF
[dist]
type = s3
provider = Other
access_key_id = ${DIST_S3_ACCESS_KEY}
secret_access_key = ${DIST_S3_SECRET_KEY}
endpoint = ${DIST_S3_ENDPOINT}
region = ${DIST_S3_REGION:-auto}

[dist-mirror]
type = s3
provider = Other
access_key_id = ${DIST_S3_MIRROR_ACCESS_KEY}
secret_access_key = ${DIST_S3_MIRROR_SECRET_KEY}
endpoint = ${DIST_S3_ENDPOINT}
region = ${DIST_S3_REGION:-auto}
CONF
export RCLONE_CONFIG="$conf"
for bucket in "dist:${DIST_BUCKET}" "dist-mirror:${DIST_MIRROR_BUCKET}"; do
    scripts/release/repo_update.sh verify apt "${bucket}/apt" "$APT_EXPECT"
    scripts/release/repo_update.sh verify rpm "${bucket}/rpm" "$RPM_EXPECT"
done
ok "each index lists this release once more, in both buckets"
