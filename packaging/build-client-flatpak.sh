#!/usr/bin/env bash
# Assemble io.openchime.OpenChime-<version>-<arch>.flatpak, the Linux client
# as a Flatpak, from an already-built binary: the GUI compiled against the
# pinned Freedesktop SDK (make linux-gui LINUX_SYSROOT=<the SDK's sysroot>,
# mk/linux.mk), its desktop entry, AppStream metadata and icons
# (packaging/linux-client/).
#
# The shape of build-deb.sh and build-rpm.sh: a binary packaged, not built.
# flatpak-builder is not used, because it compiles inside the SDK, and for
# arm64 on an x86 runner that means emulation; the commands below only arrange
# files, so either architecture's are assembled here (ARCH-20):
#   flatpak build-init     an application directory for the pinned
#                          org.freedesktop.Platform, at its branch
#                          (packaging/linux-client/freedesktop-runtime.conf)
#   flatpak build-finish   the command, and the sandbox the client needs
#                          (packaging/linux-client/flatpak-finish-args)
#   flatpak build-export   committed to <outdir>/flatpak-repo, a local
#                          repository, as app/io.openchime.OpenChime/<arch>/stable
#   flatpak build-bundle   that commit as the single-file bundle
# Every library the client links is in the runtime, libsecret included, so
# the application is the binary and its files alone.
#
# build-init reads the SDK and the runtime from the tree's own Flatpak
# installation (scripts/build/freedesktop_sdk.sh fetches both, pinned), never
# the machine's or the user's.
#
#   build-client-flatpak.sh <version> <arch> <binary> [outdir]   arch: x86_64|aarch64
set -euo pipefail

VERSION="${1:?usage: build-client-flatpak.sh <version> <arch> <binary> [outdir]}"
ARCH="${2:?missing arch (x86_64|aarch64)}"
BINARY="${3:?missing path to the client built against the SDK}"
OUTDIR="${4:-dist}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
files="$root/packaging/linux-client"
APP_ID=io.openchime.OpenChime
BRANCH=stable

[ -x "$BINARY" ] || { echo "build-client-flatpak.sh: $BINARY is not an executable" >&2; exit 1; }
case "$ARCH" in x86_64|aarch64) ;; *) echo "build-client-flatpak.sh: unsupported arch $ARCH" >&2; exit 1 ;; esac
command -v flatpak >/dev/null || { echo "build-client-flatpak.sh: flatpak not found" >&2; exit 1; }
. "$files/freedesktop-runtime.conf"
export FLATPAK_USER_DIR="$root/third_party/flatpak-freedesktop-${FREEDESKTOP_BRANCH}"
for r in Sdk Platform; do
  flatpak info --user "runtime/org.freedesktop.$r/$ARCH/$FREEDESKTOP_BRANCH" >/dev/null 2>&1 ||
    { echo "build-client-flatpak.sh: org.freedesktop.$r $FREEDESKTOP_BRANCH for $ARCH is not fetched (scripts/build/freedesktop_sdk.sh $ARCH)" >&2; exit 1; }
done

app="$(mktemp -d)"
trap 'rm -rf "$app"' EXIT
rmdir "$app"
flatpak build-init --arch="$ARCH" "$app" "$APP_ID" org.freedesktop.Sdk org.freedesktop.Platform "$FREEDESKTOP_BRANCH" >/dev/null

# The files, under /app as the runtime sees them.
install -D -m 0755 "$BINARY"                       "$app/files/bin/openchime"
install -D -m 0644 "$files/$APP_ID.desktop"        "$app/files/share/applications/$APP_ID.desktop"
install -D -m 0644 "$files/$APP_ID.metainfo.xml"   "$app/files/share/metainfo/$APP_ID.metainfo.xml"
mkdir -p "$app/files/share/icons"
cp -r "$files/icons/hicolor" "$app/files/share/icons/"
install -D -m 0644 "$root/LICENSE"                 "$app/files/share/licenses/$APP_ID/LICENSE"

# The sandbox: one argument a line, comments and blank lines aside.
mapfile -t finish < <(grep -vE '^[[:space:]]*(#|$)' "$files/flatpak-finish-args")
flatpak build-finish --command=openchime "${finish[@]}" "$app" >/dev/null

mkdir -p "$OUTDIR"
repo="$OUTDIR/flatpak-repo"
flatpak build-export --arch="$ARCH" --subject="OpenChime $VERSION" "$repo" "$app" "$BRANCH" >/dev/null
out="$OUTDIR/${APP_ID}-${VERSION}-${ARCH}.flatpak"
# The runtime's repository travels in the bundle, so installing it fetches the
# runtime from Flathub.
flatpak build-bundle --arch="$ARCH" --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$repo" "$out" "$APP_ID" "$BRANCH"

echo "$out"
