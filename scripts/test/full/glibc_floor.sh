#!/usr/bin/env bash
# scripts/test/full/glibc_floor.sh <binary>
#
# The release binary needs no glibc newer than 2.34. The floor is a shipped
# promise (README: glibc 2.34+, RHEL 9+), and it is invisible in a passing
# build: a binary needing 2.38 links and runs perfectly on the build machine,
# then fails to start on the distro it was built for.
#
# Needs: readelf (any architecture's binary).
# Run by: make test-all (PART=packages).
. "$(dirname "$0")/../lib/common.sh"
need readelf "binutils"
bin=${1:?usage: glibc_floor.sh <binary>}
have "$bin" "the release build makes it"
max=$(readelf --dyn-syms --wide "$bin" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)
echo "$TEST: highest glibc symbol required: $max"
[ "$max" = GLIBC_2.34 ] || fail "$bin requires $max, above the supported floor GLIBC_2.34"
ok "$bin needs glibc 2.34 at most"
