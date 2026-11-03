#!/usr/bin/env bash
# build-windows.sh <version> <bindir> <outdir>
#
# The Windows client's two packages, built on Linux from the binaries the
# Linux build cross-compiled (make windows): the NSIS installer, the download
# channel (packaging/windows/openchime.nsi, makensis), and the MSIX, the Store
# channel (packaging/windows/msix/, makemsix). <version> is the release number
# (0 in the staging build); <bindir> holds openchime.exe and openchime-tui.exe.
# Signing is the release's (scripts/release/sign_windows.sh).
#
# Needs makensis (apt: nsis), python3, and what scripts/build/makemsix.sh and
# nsis_plugins.sh need on first use.
set -euo pipefail
version=${1:?usage: build-windows.sh <version> <bindir> <outdir>}
bindir=${2:?usage: build-windows.sh <version> <bindir> <outdir>}
outdir=${3:?usage: build-windows.sh <version> <bindir> <outdir>}
cd "$(dirname "$0")/.."
command -v makensis >/dev/null || { echo "build-windows.sh: needs makensis (apt: nsis)" >&2; exit 1; }
for f in openchime.exe openchime-tui.exe; do
  [ -f "$bindir/$f" ] || { echo "build-windows.sh: $bindir/$f is missing (make windows)" >&2; exit 1; }
done
scripts/build/nsis_plugins.sh
scripts/build/makemsix.sh
mkdir -p "$outdir"

# -NOCD: makensis otherwise changes into the script's directory, and every
# path given here is from the repository root.
makensis -V2 -NOCD -DVERSION="$version" -DBINDIR="$bindir" -DOUTDIR="$outdir" \
  -DPLUGINDIR=third_party/nsis-plugins -DSRCDIR=. packaging/windows/openchime.nsi

# The MSIX: the manifest and its assets, the two binaries under bin/, and the
# release number in the package version (1.0.<release>.0, as the installer).
# There is no resources.pri: the manifest names its strings literally, and
# Windows uses the unqualified tile images at every scale.
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -r packaging/windows/msix/. "$stage/"
mkdir -p "$stage/bin"
cp "$bindir/openchime.exe" "$bindir/openchime-tui.exe" "$stage/bin/"
sed -i "s/Version=\"1\.0\.0\.0\"/Version=\"1.0.${version}.0\"/" "$stage/AppxManifest.xml"
grep -q "Version=\"1.0.${version}.0\"" "$stage/AppxManifest.xml" \
  || { echo "build-windows.sh: the manifest's version was not set" >&2; exit 1; }
mm=$(scripts/build/makemsix.sh --dir)
LD_LIBRARY_PATH="$mm/lib" "$mm/bin/makemsix" pack -d "$stage" -p "$outdir/openchime-${version}.msix"
ls -la "$outdir"
