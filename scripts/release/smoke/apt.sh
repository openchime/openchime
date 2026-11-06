#!/usr/bin/env bash
# scripts/release/smoke/apt.sh
#
# The published apt repository installs openchimed the way an operator does:
# the published keyring, the published source line, apt-get install, and the
# binary runs. Read from DIST_VERIFY_URL, which serves the same packages as
# DIST_BASE_URL.
#
# Needs: sudo, apt-get, curl; DIST_VERIFY_URL. Changes the machine: adds the
# repository and installs the package system-wide.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need sudo "sudo"; need apt-get "a Debian or Ubuntu machine"; need curl "curl"
: "${DIST_VERIFY_URL:?DIST_VERIFY_URL is the address the published repository is verified at}"
key=$(mktemp)
trap 'rm -f "$key"' EXIT
curl -fsSL "$DIST_VERIFY_URL/openchime-archive-keyring.gpg" -o "$key"
sudo install -m 0644 "$key" /usr/share/keyrings/openchime-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/openchime-archive-keyring.gpg] $DIST_VERIFY_URL/apt stable main" \
    | sudo tee /etc/apt/sources.list.d/openchime.list >/dev/null
# Through apt.sh: the index fetched first (install alone can find nothing),
# bounded and retried.
scripts/build/apt.sh openchimed
openchimed --version
ok "the apt repository installs openchimed"
