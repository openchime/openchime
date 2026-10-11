#!/usr/bin/env bash
# Fetch the pinned llvm-mingw into third_party/llvm-mingw-<ver>/ -- the
# "fetched at build" vendoring class (docs/VENDORS.md §2): SHA-256-pinned
# tarball, gitignored output, reproduced by this script. It is the Windows
# toolchain: clang, lld and the LLVM binutils behind the x86_64-w64-mingw32-*
# names the Windows rules and the cross-build scripts call (gcc, g++, windres,
# objcopy, ar, ranlib, strip, addr2line, dlltool), with current mingw-w64
# headers and winpthreads. The UCRT variant: the C runtime every supported
# Windows ships. Needs curl and xz.
set -euo pipefail

LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-20261006}"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in x86_64|aarch64) ;; *) echo "llvm_mingw.sh: no pinned llvm-mingw for a $HOST_ARCH host" >&2; exit 1 ;; esac

# Known-good SHA-256 of each host's tarball (the ucrt-ubuntu-22.04 builds run
# on any glibc 2.35+ host). Bump with LLVM_MINGW_VERSION; a version without a
# matching sum is refused rather than fetched unverified.
LLVM_MINGW_SHA256_20261006_x86_64="5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
LLVM_MINGW_SHA256_20261006_aarch64=""
_sum_var="LLVM_MINGW_SHA256_${LLVM_MINGW_VERSION}_${HOST_ARCH}"
LLVM_MINGW_SHA256="${LLVM_MINGW_SHA256:-${!_sum_var:-}}"
if [ -z "$LLVM_MINGW_SHA256" ]; then
  echo "llvm_mingw.sh: no known SHA-256 for llvm-mingw ${LLVM_MINGW_VERSION} on ${HOST_ARCH}; pass LLVM_MINGW_SHA256=<sum>" >&2
  exit 1
fi

cd "$(dirname "$0")/../.."
DEST="third_party/llvm-mingw-${LLVM_MINGW_VERSION}"
if [ -x "${DEST}/bin/x86_64-w64-mingw32-gcc" ]; then
  echo "llvm_mingw.sh: up to date — ${DEST}"
  exit 0
fi

NAME="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-${HOST_ARCH}"
TAR="third_party/${NAME}.tar.xz"
mkdir -p third_party
echo "llvm_mingw.sh: fetching llvm-mingw ${LLVM_MINGW_VERSION} for ${HOST_ARCH} (about 85 MB)"
curl -fsSL --retry 3 -o "${TAR}.part" "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${NAME}.tar.xz"
got="$(sha256sum "${TAR}.part" | cut -d" " -f1)"
if [ "$got" != "$LLVM_MINGW_SHA256" ]; then
  echo "llvm_mingw.sh: SHA-256 MISMATCH: expected ${LLVM_MINGW_SHA256}, got ${got}" >&2
  rm -f "${TAR}.part"
  exit 1
fi
echo "llvm_mingw.sh: sha256 ok (${got})"
rm -rf "${DEST}.new"
mkdir -p "${DEST}.new"
tar -xJf "${TAR}.part" -C "${DEST}.new" --strip-components=1
rm -f "${TAR}.part"
rm -rf "${DEST}"
mv "${DEST}.new" "${DEST}"
"${DEST}/bin/x86_64-w64-mingw32-gcc" --version | head -1
