#!/usr/bin/env bash
# Build the MP3 encoder of the committed LAME (static) from third_party/lame --
# the 3.100 release tarball as published, committed (docs/VENDORS.md §1) -- into
# third_party/lame-<version>[-arch]. Nothing is downloaded.
#
#   scripts/build/lame.sh            native (Linux; `make EXPORT=1` links it)
#
# libmp3lame alone: no decoder (the bundled mpglib), no command-line frontend,
# no shared library. The daemon's export of a video message (ARCH-135) is the
# only user.
set -euo pipefail
# One compile at a time (docs/TESTING.md, "One build at a time").
unset MAKEFLAGS MFLAGS
NO_AUTOTOOLS="ACLOCAL=: AUTOCONF=: AUTOMAKE=: AUTOHEADER=:"

LAME_VERSION=3.100

cd "$(dirname "$0")/../.."
mkdir -p third_party
cd third_party

SRC="$(pwd)/lame"
[ -x "${SRC}/configure" ] || { echo "build_lame: third_party/lame is missing" >&2; exit 1; }

DEST="lame-${LAME_VERSION}${OC_ARCH:+-$OC_ARCH}"   # OC_ARCH: another architecture, with make's CC
HOST="${OC_ARCH:+--host=$OC_ARCH-linux-gnu}"

if [ ! -f "${DEST}/lib/libmp3lame.a" ]; then
  echo "build_lame: building ${DEST} (encoder only, static)"
  mkdir -p "${DEST}/build"
  (
    cd "${DEST}/build"
    # Out of tree: the committed sources are only read. The release tarball
    # ships a generated configure, so no autotools are needed.
    "${SRC}/configure" ${HOST} --prefix="$(cd .. && pwd)" \
      --disable-shared --enable-static --with-pic \
      --disable-decoder --disable-frontend --disable-gtktest \
      --disable-analyzer-hooks >/dev/null
    # The tarball's generated files are older than the sources a checkout
    # writes beside them, so make would regenerate them with automake, which
    # nothing here needs or installs: the regeneration commands are no-ops.
    make -j1 $NO_AUTOTOOLS -C libmp3lame >/dev/null
    make -j1 $NO_AUTOTOOLS -C include install >/dev/null
    make -j1 $NO_AUTOTOOLS -C libmp3lame install >/dev/null
  )
  rm -rf "${DEST}/build"
fi

echo "build_lame: done: third_party/${DEST}/lib/libmp3lame.a, headers in third_party/${DEST}/include"
