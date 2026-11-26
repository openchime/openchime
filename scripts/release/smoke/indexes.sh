#!/usr/bin/env bash
# scripts/release/smoke/indexes.sh
#
# Each index, read back from each bucket, lists one version more of each
# package name per architecture than before the release, the daemon's,
# openchimed, and the Linux client's, openchime: the count recorded before
# publishing (APT_EXPECT, RPM_EXPECT: "openchime:amd64=2 openchimed:amd64=5
# ..."), each package name's version this release exactly once, every
# package it names present, and the signed top matching the files it names
# (scripts/release/repo_update.sh verify). The Flatpak repository's signed
# summaries list the client for each architecture at the commit recorded
# before publishing (FLATPAK_EXPECT: "aarch64=<commit> x86_64=<commit>"),
# each commit there and this release's (scripts/release/flatpak_repo.sh
# verify). Read from the buckets, not the public address.
#
# Needs: rclone, flatpak, ostree, gpg with the signing key imported;
# APT_EXPECT, RPM_EXPECT, FLATPAK_EXPECT, VERSION, GPG_KEY_ID;
# DIST_S3_ACCESS_KEY, DIST_S3_SECRET_KEY, DIST_S3_ENDPOINT, DIST_S3_REGION,
# DIST_BUCKET, DIST_S3_MIRROR_ACCESS_KEY, DIST_S3_MIRROR_SECRET_KEY,
# DIST_MIRROR_BUCKET.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need rclone "rclone"; need flatpak "flatpak"; need ostree "ostree"
[ -n "${APT_EXPECT:-}" ] && [ -n "${RPM_EXPECT:-}" ] && [ -n "${FLATPAK_EXPECT:-}" ] \
    || fail "publish recorded no counts to check (APT_EXPECT, RPM_EXPECT, FLATPAK_EXPECT)"
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
    scripts/release/flatpak_repo.sh verify "${bucket}/flatpak" "$FLATPAK_EXPECT"
done
ok "each index lists this release once more, and the Flatpak repository this release's client, in both buckets"
