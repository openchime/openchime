#!/usr/bin/env bash
# scripts/release/smoke/dnf.sh
#
# The published dnf repository installs openchimed the way an operator does,
# over HTTPS with the published key and .repo file, on rockylinux:9 -- the
# oldest RHEL the glibc 2.34 floor exists to support -- in a machine that has
# never seen this project. Then the Linux client, openchime, the same way, on
# rockylinux:10, the oldest RHEL its floor supports (ARCH-20), its libraries
# from the distribution's own repositories. podman, not docker: the pipeline
# is daemon-free.
#
# Needs: podman; DIST_VERIFY_URL, DIST_BASE_URL; network.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need podman "podman"
: "${DIST_VERIFY_URL:?DIST_VERIFY_URL is the address the published repository is verified at}"
: "${DIST_BASE_URL:?DIST_BASE_URL is the published repository address}"

# install_on <image> <package> [dnf options]: the repository added as an
# operator adds it, the package installed, and its binary, of the same name,
# asked its version.
install_on() {
    podman run --rm "$1" bash -euo pipefail -c "
      curl -fsSL '${DIST_VERIFY_URL}/openchime-archive-keyring.asc' -o /tmp/oc.asc
      rpm --import /tmp/oc.asc
      curl -fsSL '${DIST_VERIFY_URL}/openchime.repo' -o /etc/yum.repos.d/openchime.repo
      # The published file names DIST_BASE_URL; read it from where it is verified.
      sed -i 's|${DIST_BASE_URL}|${DIST_VERIFY_URL}|g' /etc/yum.repos.d/openchime.repo
      dnf install -y ${3:-} $2
      $2 --version
    "
}
# Only our repository: the daemon needs nothing beyond glibc and a shell,
# which the image has, so the distribution's metadata (some 78 MB) is not
# fetched for it (docs/details/CONTRIBUTING.md, "Billed minutes").
install_on rockylinux:9 openchimed "--disablerepo='*' --enablerepo=openchime"
ok "the dnf repository installs openchimed on rockylinux:9"
# The client needs FreeType, HarfBuzz, fontconfig, PipeWire, GLib and
# libsecret, which only the distribution's repositories have.
install_on rockylinux:10 openchime
ok "the dnf repository installs openchime on rockylinux:10"
