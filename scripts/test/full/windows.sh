#!/usr/bin/env bash
# scripts/test/full/windows.sh <bindir> <dist> <version>
#
# The Windows client as it ships. Both binaries are 64-bit x86 PEs (static, so
# nothing downstream chains a redistributable), and with BUILD_NUM and
# BUILD_ID set their version resources name the build: FileVersion
# 1.0.<BUILD_NUM>.0, ProductVersion "build <BUILD_ID>". The installer is a
# PE at 1.0.<version>.0, 32-bit or 64-bit (NSIS builds on a 32-bit stub), and
# the MSIX holds its manifest, block map, content types and both binaries, its
# Identity at 1.0.<version>.0. Read on Linux;
# whether they install is checked on Windows (docs/TESTING.md §4).
#
# Needs: python3 with pefile (apt: python3-pefile); <bindir>/openchime.exe and
# openchime-tui.exe, and the installer and MSIX in <dist>
# (packaging/build-windows.sh).
# Run by: make test-all (PART=windows).
. "$(dirname "$0")/../lib/common.sh"
bindir=${1:?usage: windows.sh <bindir> <dist> <version>} dist=${2:?} version=${3:?}
python3 -I -c 'import pefile' 2>/dev/null || { echo "$TEST: needs the Python module pefile (apt: python3-pefile)" >&2; exit 2; }
files=scripts/test/lib/windows_files.py
fv=${BUILD_NUM:+1.0.${BUILD_NUM}.0}
pv=${BUILD_ID:+build ${BUILD_ID}}
for f in openchime.exe openchime-tui.exe; do
    have "$bindir/$f" "make build-all PART=windows builds it"
    python3 -I "$files" pe "$bindir/$f" "$fv" "$pv" || fail "$bindir/$f"
done
setup="$dist/openchime-${version}-windows-x64-setup.exe"
pkg="$dist/openchime-${version}.msix"
have "$setup" "packaging/build-windows.sh makes it"
have "$pkg" "packaging/build-windows.sh makes it"
python3 -I "$files" installer "$setup" "1.0.${version}.0" || fail "$setup"
python3 -I "$files" msix "$pkg" "1.0.${version}.0" || fail "$pkg"
ok "the binaries, the installer and the MSIX are what ships"
