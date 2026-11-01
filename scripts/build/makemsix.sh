#!/usr/bin/env bash
# Build Microsoft's makemsix (msix-packaging) at a pinned commit into
# third_party/makemsix-<rev>/ -- the "built from pinned source" vendoring class
# (docs/VENDORS.md §2): it packs the Windows client's MSIX on Linux
# (packaging/build-windows.sh), so no Windows machine is in the build.
#
# The tagged releases (v1.7, 2019) no longer compile with a current clang
# ("templates must have C++ linkage"); the pinned commit does, and needs
# CMake 3.29 or newer, which Ubuntu 24.04 does not ship, so a pinned CMake
# from Kitware is fetched beside it. Needs git, curl, clang and clang++ (the
# project's toolchain file names them), make.
set -euo pipefail

MAKEMSIX_REV="${MAKEMSIX_REV:-25a65f5c1690930813bcc10cdf1d59fa865f2bb1}"
CMAKE_VERSION=3.31.6
CMAKE_SHA256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf

[ "$(uname -m)" = x86_64 ] || { echo "makemsix.sh: built on an x86-64 host only (its toolchain file says so)" >&2; exit 1; }
cd "$(dirname "$0")/../.."
DEST="third_party/makemsix-${MAKEMSIX_REV:0:7}"
# --dir: where it is, for the callers (packaging/build-windows.sh).
if [ "${1:-}" = --dir ]; then echo "$DEST"; exit 0; fi
if [ -x "$DEST/bin/makemsix" ]; then
  echo "makemsix.sh: up to date: $DEST/bin/makemsix"
  exit 0
fi
for t in git curl clang clang++ make; do
  command -v "$t" >/dev/null || { echo "makemsix.sh: needs $t" >&2; exit 1; }
done

work="third_party/makemsix-build"
rm -rf "$work"
mkdir -p "$work"
echo "makemsix.sh: fetching CMake ${CMAKE_VERSION}"
curl -fsSL --retry 3 -o "$work/cmake.tgz" \
  "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"
got=$(sha256sum "$work/cmake.tgz" | cut -d' ' -f1)
[ "$got" = "$CMAKE_SHA256" ] || { echo "makemsix.sh: CMake SHA-256 MISMATCH: expected ${CMAKE_SHA256}, got ${got}" >&2; exit 1; }
tar xzf "$work/cmake.tgz" -C "$work"

echo "makemsix.sh: building makemsix at ${MAKEMSIX_REV:0:7}"
git -C "$work" init -q src
git -C "$work/src" fetch -q --depth 1 https://github.com/microsoft/msix-packaging "$MAKEMSIX_REV"
git -C "$work/src" checkout -q FETCH_HEAD
( cd "$work/src" && PATH="$PWD/../cmake-${CMAKE_VERSION}-linux-x86_64/bin:$PATH" \
    ./makelinux.sh --pack --skip-samples --skip-tests > ../build.log 2>&1 ) \
  || { tail -30 "$work/build.log" >&2; echo "makemsix.sh: the build failed ($work/build.log)" >&2; exit 1; }

mkdir -p "$DEST/bin" "$DEST/lib"
cp "$work/src/.vs/bin/makemsix" "$DEST/bin/"
cp "$work/src/.vs/lib/libmsix.so" "$DEST/lib/"
cp "$work/src/LICENSE" "$DEST/LICENSE"
rm -rf "$work"
"$DEST/bin/makemsix" >/dev/null 2>&1 || true
echo "makemsix.sh: built $DEST/bin/makemsix"
