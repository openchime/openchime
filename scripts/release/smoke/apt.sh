#!/usr/bin/env bash
# scripts/release/smoke/apt.sh
#
# The published apt repository installs openchimed the way an operator does:
# the published keyring, the published source line, apt-get install, and the
# binary runs. Then the Linux client, openchime, the way a desktop user
# installs it, on ubuntu:24.04, the client's floor (ARCH-20), in a machine
# that has never seen this project: the same keyring and source line, and its
# libraries from Ubuntu's own archive. podman, not docker: the pipeline is
# daemon-free. Read from DIST_VERIFY_URL, which serves the same packages as
# DIST_BASE_URL.
#
# Needs: sudo, apt-get, curl, podman; DIST_VERIFY_URL; network. Changes the
# machine: adds the repository and installs the daemon's package
# system-wide.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need sudo "sudo"; need apt-get "a Debian or Ubuntu machine"; need curl "curl"; need podman "podman"
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

# The client. The image has no CA certificates, which an https source needs,
# so they come first, from Ubuntu's archive; then only our index is fetched
# again, not Ubuntu's a second time. Every fetch bounded and retried.
chmod 0644 "$key"
podman run --rm -v "$key:/usr/share/keyrings/openchime-archive-keyring.gpg:ro" ubuntu:24.04 bash -euo pipefail -c "
  export DEBIAN_FRONTEND=noninteractive
  net='-o Acquire::Retries=3 -o Acquire::http::Timeout=60 -o Acquire::https::Timeout=60'
  apt-get \$net update -qq
  apt-get \$net install -y -qq --no-install-recommends ca-certificates >/dev/null
  echo 'deb [signed-by=/usr/share/keyrings/openchime-archive-keyring.gpg] ${DIST_VERIFY_URL}/apt stable main' \
    > /etc/apt/sources.list.d/openchime.list
  apt-get \$net update -qq -o Dir::Etc::sourcelist=sources.list.d/openchime.list \
    -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0
  apt-get \$net install -y -qq --no-install-recommends openchime >/dev/null
  openchime --version
"
ok "the apt repository installs openchime on ubuntu:24.04"
