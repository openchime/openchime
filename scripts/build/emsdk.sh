#!/usr/bin/env bash
# Fetch the pinned Emscripten into third_party/emsdk-<ver>/ -- the "fetched at
# build" vendoring class (docs/VENDORS.md §2), gitignored, reproduced by this
# script. emsdk is a git repository whose tag names the Emscripten release and
# whose own table pins the SHA-256 of every binary it installs (LLVM, Binaryen,
# Node, Python); `emsdk install` downloads and checks them. The result needs no
# environment: emcc by its path finds the embedded configuration, and the
# Makefile puts the directory on PATH for the scripts that call emcc, emar,
# emcmake and emmake. Needs git, curl and python3; about 400 MB, once.
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-6.0.8}"

cd "$(dirname "$0")/../.."
DEST="third_party/emsdk-${EMSDK_VERSION}"
if [ -x "${DEST}/upstream/emscripten/emcc" ] && [ -x "${DEST}/node-bin/node" ]; then
  echo "emsdk.sh: up to date: ${DEST}"
  exit 0
fi

mkdir -p third_party
echo "emsdk.sh: fetching Emscripten ${EMSDK_VERSION} (about 400 MB, once)"
if [ ! -d "${DEST}/.git" ]; then
  rm -rf "${DEST}"
  git clone -q --depth 1 --branch "${EMSDK_VERSION}" https://github.com/emscripten-core/emsdk.git "${DEST}"
fi
# Quiet, and with its state under its own directory: nothing in $HOME.
( cd "${DEST}" && EMSDK_QUIET=1 ./emsdk install "${EMSDK_VERSION}" && EMSDK_QUIET=1 ./emsdk activate "${EMSDK_VERSION}" ) >/dev/null
# The bundled node, at a stable path (its directory carries node's version).
nodebin="$(ls -d "${DEST}"/node/*/bin | head -1)"
[ -n "$nodebin" ] || { echo "emsdk.sh: emsdk installed no node" >&2; exit 1; }
rm -f "${DEST}/node-bin"; ln -s "node/$(basename "$(dirname "$nodebin")")/bin" "${DEST}/node-bin"
"${DEST}/upstream/emscripten/emcc" --version | head -1
