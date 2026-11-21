#!/usr/bin/env bash
# scripts/test/full/glibc_floor.sh <binary>
#
# The release binary needs no glibc newer than 2.35. The floor is a shipped
# promise (README: Ubuntu 22.04+, Debian bookworm+, RHEL 10+; RHEL 9 runs the
# container image), and it is invisible in a passing build: a binary needing
# 2.38 links and runs perfectly on the build machine, then fails to start on
# the distro it was built for. The release holds it by building on Ubuntu
# 22.04 (docs/ARCHITECTURE.md, ARCH-20).
#
# Needs: readelf (any architecture's binary).
# Run by: make test-all (PART=packages).
. "$(dirname "$0")/../lib/common.sh"
need readelf "binutils"
floor=GLIBC_2.35
bin=${1:?usage: glibc_floor.sh <binary>}
have "$bin" "the release build makes it"
max=$(readelf --dyn-syms --wide "$bin" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)
[ -n "$max" ] || fail "$bin requires no versioned glibc symbol: not a dynamically linked glibc binary"
echo "$TEST: highest glibc symbol required: $max"
[ "$(printf '%s\n%s\n' "$max" "$floor" | sort -V | tail -1)" = "$floor" ] ||
    fail "$bin requires $max, above the supported floor $floor"
ok "$bin needs glibc ${floor#GLIBC_} at most"
