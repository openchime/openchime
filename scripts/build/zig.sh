#!/usr/bin/env bash
# Fetch the pinned zig into third_party/zig-<ver>/ -- the "fetched at build"
# vendoring class (docs/VENDORS.md §2): SHA-256-pinned tarball, gitignored
# output, reproduced by this script. zig's bundled clang is the release's C
# compiler (it ships glibc stubs for every version, so a 2.34 binary comes off
# any host) and CI's second compiler (make check-release-cc); the arm64 daemon
# is cross-built with it (make ARCH=aarch64). Needs curl and xz.
set -euo pipefail

ZIG_VERSION="${ZIG_VERSION:-0.13.0}"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in x86_64|aarch64) ;; *) echo "zig.sh: no pinned zig for a $HOST_ARCH host" >&2; exit 1 ;; esac

# Known-good SHA-256 of each host's tarball. Bump both with ZIG_VERSION; a
# version without a matching sum is refused rather than fetched unverified.
ZIG_SHA256_0_13_0_x86_64="d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea"
ZIG_SHA256_0_13_0_aarch64="041ac42323837eb5624068acd8b00cd5777dac4cf91179e8dad7a7e90dd0c556"
_sum_var="ZIG_SHA256_${ZIG_VERSION//./_}_${HOST_ARCH}"
ZIG_SHA256="${ZIG_SHA256:-${!_sum_var:-}}"
if [ -z "$ZIG_SHA256" ]; then
  echo "zig.sh: no known SHA-256 for zig ${ZIG_VERSION} on ${HOST_ARCH}; pass ZIG_SHA256=<sum>" >&2
  exit 1
fi

cd "$(dirname "$0")/../.."
DEST="third_party/zig-${ZIG_VERSION}"
if [ -x "${DEST}/zig" ]; then
  echo "zig.sh: up to date: ${DEST}/zig"
  exit 0
fi

TAR="third_party/zig-linux-${HOST_ARCH}-${ZIG_VERSION}.tar.xz"
mkdir -p third_party
echo "zig.sh: fetching zig ${ZIG_VERSION} for ${HOST_ARCH} (about 45 MB)"
curl -fsSL --retry 3 -o "${TAR}.part" "https://ziglang.org/download/${ZIG_VERSION}/zig-linux-${HOST_ARCH}-${ZIG_VERSION}.tar.xz"
got="$(sha256sum "${TAR}.part" | cut -d" " -f1)"
if [ "$got" != "$ZIG_SHA256" ]; then
  echo "zig.sh: SHA-256 MISMATCH: expected ${ZIG_SHA256}, got ${got}" >&2
  rm -f "${TAR}.part"
  exit 1
fi
echo "zig.sh: sha256 ok (${got})"
rm -rf "${DEST}.new"
mkdir -p "${DEST}.new"
tar -xJf "${TAR}.part" -C "${DEST}.new" --strip-components=1
rm -f "${TAR}.part"
rm -rf "${DEST}"
mv "${DEST}.new" "${DEST}"
"${DEST}/zig" version
