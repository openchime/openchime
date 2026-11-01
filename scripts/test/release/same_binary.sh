#!/usr/bin/env bash
# scripts/test/release/same_binary.sh <dist> <version> <arch>
#
# The .deb, the .rpm and the tarball each carry the one binary that was built.
# If this ever fails, the "one binary, packaged three ways" claim in the docs
# is false and the packages must not be published.
#
# Needs: dpkg-deb, rpm2cpio, cpio, tar; <dist>/openchimed and the three
# packages for <version> and <arch> (amd64, arm64).
# Run by: make test-release (PART=packages).
. "$(dirname "$0")/../lib/common.sh"
need dpkg-deb "dpkg"; need rpm2cpio "rpm"; need cpio "cpio"
dist=${1:?usage: same_binary.sh <dist> <version> <arch>} version=${2:?} arch=${3:?}
rpm_arch=$([ "$arch" = arm64 ] && echo aarch64 || echo x86_64)
deb="$dist/openchimed_${version}_${arch}.deb"
rpm="$dist/openchimed-${version}-1.${rpm_arch}.rpm"
tgz="$dist/openchimed-${version}-linux-${arch}.tar.gz"
for f in "$dist/openchimed" "$deb" "$rpm" "$tgz"; do have "$f" "the release build makes it"; done
want=$(sha256sum "$dist/openchimed" | cut -d' ' -f1)
v=$(mktemp -d)
trap 'rm -rf "$v"' EXIT
mkdir -p "$v/deb" "$v/rpm" "$v/tar"
dpkg-deb -x "$deb" "$v/deb"
( cd "$v/rpm" && rpm2cpio "$OLDPWD/$rpm" | cpio -idm --quiet )
tar xzf "$tgz" -C "$v/tar"
for f in "$v/deb/usr/bin/openchimed" "$v/rpm/usr/bin/openchimed" "$v/tar/openchimed-${version}-linux-${arch}/openchimed"; do
    got=$(sha256sum "$f" | cut -d' ' -f1)
    [ "$got" = "$want" ] || fail "${f#$v/} differs from the built binary"
done
ok "all three channels ship $want"
