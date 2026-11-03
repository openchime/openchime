#!/usr/bin/env bash
# Fetch the pinned jsign into third_party/jsign-<ver>.jar -- the "fetched at
# build" vendoring class (docs/VENDORS.md §2), SHA-256-pinned, gitignored.
# jsign signs Windows executables, installers and MSIX packages from Linux
# against Azure Trusted Signing (scripts/release/sign_windows.sh), so no
# Windows machine signs a release. Apache 2.0. Needs curl; running it needs
# a Java runtime.
set -euo pipefail

JSIGN_VERSION=7.6
JSIGN_SHA256=6cf58aa080e7ff82656f320cff8d4a3bff30408df8766806dcba3740dd839fa5

cd "$(dirname "$0")/../.."
DEST="third_party/jsign-${JSIGN_VERSION}.jar"
# --path: where it is, for the callers (scripts/release/sign_windows.sh).
if [ "${1:-}" = --path ]; then echo "$DEST"; exit 0; fi
if [ -f "$DEST" ]; then echo "jsign.sh: up to date: $DEST"; exit 0; fi
mkdir -p third_party
curl -fsSL --retry 3 -o "$DEST.part" "https://github.com/ebourg/jsign/releases/download/${JSIGN_VERSION}/jsign-${JSIGN_VERSION}.jar"
got=$(sha256sum "$DEST.part" | cut -d' ' -f1)
if [ "$got" != "$JSIGN_SHA256" ]; then
  rm -f "$DEST.part"
  echo "jsign.sh: SHA-256 MISMATCH: expected ${JSIGN_SHA256}, got ${got}" >&2
  exit 1
fi
mv "$DEST.part" "$DEST"
echo "jsign.sh: $DEST"
