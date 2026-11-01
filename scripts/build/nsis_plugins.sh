#!/usr/bin/env bash
# Fetch the pinned NSIS plugins the Windows installer uses into
# third_party/nsis-plugins/x86-unicode/ -- the "fetched at build" vendoring
# class (docs/VENDORS.md §2), SHA-256-pinned, gitignored. makensis itself is
# Ubuntu's (apt: nsis), run natively, so no Windows machine is in the build.
#
#   WinShell  sets the Start-menu shortcut's AppUserModelID, without which
#             Windows cannot raise the client's toast notifications
#             (packaging/windows/openchime.nsi). zlib licence.
#
# Needs curl and python3 (to read the zip, so unzip is not required).
set -euo pipefail

WINSHELL_URL=https://nsis.sourceforge.io/mediawiki/images/5/54/WinShell.zip
WINSHELL_SHA256=34e111f8aacf64c540d848fd06b9d6f3e2c10cb825ec9329a01d1141973e749b

cd "$(dirname "$0")/../.."
DEST=third_party/nsis-plugins/x86-unicode
if [ -f "$DEST/WinShell.dll" ]; then
  echo "nsis_plugins.sh: up to date: $DEST"
  exit 0
fi
mkdir -p "$DEST"
zip="third_party/nsis-plugins/WinShell.zip"
curl -fsSL --retry 3 -o "$zip.part" "$WINSHELL_URL"
got=$(sha256sum "$zip.part" | cut -d' ' -f1)
if [ "$got" != "$WINSHELL_SHA256" ]; then
  rm -f "$zip.part"
  echo "nsis_plugins.sh: SHA-256 MISMATCH for WinShell.zip: expected ${WINSHELL_SHA256}, got ${got}" >&2
  exit 1
fi
mv "$zip.part" "$zip"
python3 -I -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("Plugins/x86-unicode/WinShell.dll"))' \
  "$zip" "$DEST/WinShell.dll"
echo "nsis_plugins.sh: WinShell.dll in $DEST"
