#!/usr/bin/env bash
# scripts/release/smoke/dnf.sh
#
# The published dnf repository installs openchimed the way an operator does,
# over HTTPS with the published key and .repo file, on rockylinux:9 -- the
# oldest RHEL the glibc 2.34 floor exists to support -- in a machine that has
# never seen this project. podman, not docker: the pipeline is daemon-free.
#
# Needs: podman; DIST_VERIFY_URL, DIST_BASE_URL; network.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need podman "podman"
: "${DIST_VERIFY_URL:?DIST_VERIFY_URL is the address the published repository is verified at}"
: "${DIST_BASE_URL:?DIST_BASE_URL is the published repository address}"
podman run --rm rockylinux:9 bash -euo pipefail -c "
  curl -fsSL '${DIST_VERIFY_URL}/openchime-archive-keyring.asc' -o /tmp/oc.asc
  rpm --import /tmp/oc.asc
  curl -fsSL '${DIST_VERIFY_URL}/openchime.repo' -o /etc/yum.repos.d/openchime.repo
  # The published file names DIST_BASE_URL; read it from where it is verified.
  sed -i 's|${DIST_BASE_URL}|${DIST_VERIFY_URL}|g' /etc/yum.repos.d/openchime.repo
  # Only our repository: the package needs nothing beyond glibc and a shell,
  # which the image has, so the distribution's metadata (some 78 MB) is not
  # fetched for it (docs/CONTRIBUTING.md, \"Billed minutes\").
  dnf install -y --disablerepo='*' --enablerepo=openchime openchimed
  openchimed --version
"
ok "the dnf repository installs openchimed on rockylinux:9"
