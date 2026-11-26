#!/usr/bin/env bash
# Fetch the pinned Freedesktop SDK and runtime for one architecture into a
# Flatpak installation of the tree's own, and lay a sysroot over the SDK: the
# files the Linux client's Flatpak compiles and links against (mk/linux.mk),
# natively for x86_64 and cross-compiled for aarch64, with no emulation.
# Gitignored, reproduced by this script; the "fetched at build" vendoring
# class (docs/details/VENDORS.md §2).
#
#   scripts/build/freedesktop_sdk.sh <x86_64|aarch64>
#
# The branch and each architecture's commits are pinned in
# packaging/linux-client/freedesktop-runtime.conf. Both come from Flathub with
# flatpak itself (Flathub serves its objects to flatpak alone), into
# third_party/flatpak-freedesktop-<branch>/ as FLATPAK_USER_DIR: a directory
# of the tree's, never the machine's or the user's installation, and nothing
# outside it is touched. Each commit is checked after the fetch: ostree names
# a commit by the checksum of everything in it, so the files are the ones
# pinned. `flatpak build-init` finds the SDK and the runtime there
# (packaging/build-client-flatpak.sh); the compilers find the SDK through the
# sysroot:
#
#   third_party/freedesktop-sdk-<branch>-<arch>/usr   the SDK's files
#
# with the top-level links of a merged-/usr system, which the SDK's linker
# scripts assume. About 950 MB per architecture, once.
set -euo pipefail

ARCH="${1:?usage: freedesktop_sdk.sh <x86_64|aarch64>}"
case "$ARCH" in x86_64|aarch64) ;; *) echo "freedesktop_sdk.sh: unsupported arch $ARCH" >&2; exit 2 ;; esac

cd "$(dirname "$0")/../.."
CONF=packaging/linux-client/freedesktop-runtime.conf
. "$CONF"
command -v flatpak >/dev/null || { echo "freedesktop_sdk.sh: flatpak is not installed" >&2; exit 1; }
mkdir -p third_party
export FLATPAK_USER_DIR="$(pwd)/third_party/flatpak-freedesktop-${FREEDESKTOP_BRANCH}"
SYSROOT="third_party/freedesktop-sdk-${FREEDESKTOP_BRANCH}-${ARCH}"

# fetch <Sdk|Platform> <pinned commit>: installed at the pin, or made so.
fetch() {
  local ref="runtime/org.freedesktop.$1/${ARCH}/${FREEDESKTOP_BRANCH}" pin="$2" have
  [ -n "$pin" ] || { echo "freedesktop_sdk.sh: $CONF pins no commit for $ref" >&2; exit 1; }
  have=$(flatpak info --user --show-commit "$ref" 2>/dev/null || true)
  [ "$have" = "$pin" ] && return 0
  echo "freedesktop_sdk.sh: fetching org.freedesktop.$1 ${FREEDESKTOP_BRANCH} for ${ARCH} at ${pin:0:12}"
  flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
  # Neither the extensions (translations, debug symbols) nor anything they
  # would pull in: the files the runtime holds are all the build reads.
  if [ -z "$have" ]; then
    flatpak install --user --noninteractive --no-related --no-deps --arch="$ARCH" flathub "$ref" >/dev/null
  fi
  flatpak update --user --noninteractive --no-related --no-deps --arch="$ARCH" --commit="$pin" "$ref" >/dev/null
  have=$(flatpak info --user --show-commit "$ref")
  [ "$have" = "$pin" ] || { echo "freedesktop_sdk.sh: $ref is at $have, not the pinned $pin" >&2; exit 1; }
}
sdk_pin="FREEDESKTOP_SDK_${ARCH}" platform_pin="FREEDESKTOP_PLATFORM_${ARCH}"
fetch Sdk "${!sdk_pin:-}"
fetch Platform "${!platform_pin:-}"

# The sysroot: the SDK's files checked out again from the installation's own
# repository, as hard links to the objects already there (no copy, no
# download), under a real usr/. A link to the deployment would not do: the
# linker reads absolute paths in a linker script (glibc's libc.so) inside the
# sysroot only when the script's real path is inside it, and through a link
# it is not, so the machine's own glibc would be linked instead. Laid out
# again when the pin moves.
pinned="${!sdk_pin} ${!platform_pin}"
if [ "$(cat "$SYSROOT/.pinned" 2>/dev/null)" != "$pinned" ]; then
  command -v ostree >/dev/null || { echo "freedesktop_sdk.sh: ostree is not installed" >&2; exit 1; }
  chmod -R u+w "$SYSROOT" 2>/dev/null || true
  rm -rf "$SYSROOT"
  mkdir -p "$SYSROOT"
  ostree --repo="$FLATPAK_USER_DIR/repo" checkout --user-mode --require-hardlinks --subpath=/files \
    "${!sdk_pin}" "$SYSROOT/usr"
  for d in lib lib64 bin sbin; do
    if [ -e "$SYSROOT/usr/$d" ]; then ln -sfn "usr/$d" "$SYSROOT/$d"; fi
  done
  echo "$pinned" > "$SYSROOT/.pinned"
fi
echo "freedesktop_sdk.sh: done: the ${FREEDESKTOP_BRANCH} SDK and runtime for ${ARCH}, sysroot ${SYSROOT}"
