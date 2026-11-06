#!/usr/bin/env bash
# scripts/test/full/package.sh <dist> <version> <arch>
#
# The real .deb installs and what it put on disk runs: its dependencies
# resolve, the binary executes, and the read-aloud and voice-input data it
# installed are where an installed daemon looks, and verify
# (scripts/test/full/speech.sh on /usr/bin/openchimed). The .rpm is covered by
# same_binary.sh and the published dnf check. An arm64 package on an x86
# machine is not installed.
#
# Needs: sudo, apt-get (Debian or Ubuntu); the .deb for <version> and <arch>.
# Changes the machine: installs the package system-wide.
# Run by: make test-all (PART=packages).
. "$(dirname "$0")/../lib/common.sh"
need sudo "sudo"; need apt-get "a Debian or Ubuntu machine"
dist=${1:?usage: package.sh <dist> <version> <arch>} version=${2:?} arch=${3:?}
deb="$dist/openchimed_${version}_${arch}.deb"
have "$deb" "make build-all PART=packages makes it"
if ! runnable "$dist/openchimed"; then echo "$TEST: $arch package on $(uname -m); not installed"; exit 0; fi
case "$deb" in /*) ;; *) deb="./$deb" ;; esac
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$deb"
/usr/bin/openchimed --version
scripts/test/full/speech.sh /usr/bin/openchimed
ok "the package installs and its binary speaks and hears"
