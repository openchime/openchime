#!/usr/bin/env bash
# Lay out the Ubuntu 24.04 arm64 sysroot the Linux client's arm64 .deb and .rpm
# are cross-compiled against (mk/linux.mk), in
# third_party/sysroot-ubuntu-24.04-arm64/: the arm64 packages pinned in
# sysroot_ubuntu_arm64.list, beside this script, each downloaded from Ubuntu's
# ports mirror, checked against its pinned sha256 and unpacked with
# `dpkg-deb -x`. Gitignored, reproduced by this script; the "fetched at build"
# vendoring class (docs/details/VENDORS.md §2).
#
#   scripts/build/sysroot_ubuntu_arm64.sh
#
# Nothing is installed: no foreign architecture is added to dpkg, nothing goes
# through apt, and nothing outside third_party/ changes. The packages are
# files in a directory, as the toolchains are. Three things make a directory
# of unpacked packages a sysroot a cross compiler can use:
#   - the top-level links of a merged-/usr system (lib -> usr/lib, ...), which
#     base-files would make, and which glibc's linker scripts name;
#   - every absolute symbolic link made relative, so that it points inside the
#     sysroot rather than at the build machine's own files;
#   - the pkg-config files left as they are (prefix=/usr): the build asks
#     pkg-config with PKG_CONFIG_SYSROOT_DIR, which puts the sysroot in front
#     of every path they give.
# A changed list lays the sysroot out again from nothing. About 37 MB, once.
set -euo pipefail

cd "$(dirname "$0")/../.."
LIST=scripts/build/sysroot_ubuntu_arm64.list
MIRROR=https://ports.ubuntu.com/ubuntu-ports
DEST=third_party/sysroot-ubuntu-24.04-arm64
CACHE=third_party/sysroot-ubuntu-24.04-arm64.debs

command -v dpkg-deb >/dev/null || { echo "sysroot_ubuntu_arm64.sh: dpkg-deb is not installed" >&2; exit 1; }
want="$(sha256sum "$LIST" | cut -d' ' -f1)"
if [ "$(cat "$DEST/.pinned" 2>/dev/null)" = "$want" ]; then
  echo "sysroot_ubuntu_arm64.sh: up to date: $DEST"
  exit 0
fi

# The packages: each kept in the cache under its pool name once its sum is
# right, so a changed list fetches only what changed.
mkdir -p "$CACHE"
n=0
while read -r pkg version sum path; do
  case "$pkg" in ''|'#'*) continue ;; esac
  [ -n "$path" ] || { echo "sysroot_ubuntu_arm64.sh: $LIST: '$pkg' needs a version, a sha256 and a path" >&2; exit 1; }
  deb="$CACHE/$(basename "$path")"
  if ! echo "$sum  $deb" | sha256sum -c --status 2>/dev/null; then
    [ "$n" -gt 0 ] || echo "sysroot_ubuntu_arm64.sh: fetching the pinned Ubuntu 24.04 arm64 packages"
    curl -fsSL --retry 3 --max-time 300 -o "$deb.part" "$MIRROR/$path"
    echo "$sum  $deb.part" | sha256sum -c --status ||
      { echo "sysroot_ubuntu_arm64.sh: $pkg $version: $MIRROR/$path does not match its pinned sha256" >&2; rm -f "$deb.part"; exit 1; }
    mv "$deb.part" "$deb"
  fi
  n=$((n + 1))
done < "$LIST"

# Unpacked into a new directory, which replaces the old one only when whole.
rm -rf "$DEST.new"
mkdir -p "$DEST.new"
while read -r pkg version sum path; do
  case "$pkg" in ''|'#'*) continue ;; esac
  dpkg-deb -x "$CACHE/$(basename "$path")" "$DEST.new"
done < "$LIST"

# Merged /usr: what a package put under /lib, /bin or /sbin moves to its place
# under /usr, and the top-level name becomes the link base-files would make.
for d in lib bin sbin; do
  if [ -d "$DEST.new/$d" ] && [ ! -L "$DEST.new/$d" ]; then
    mkdir -p "$DEST.new/usr/$d"
    cp -a "$DEST.new/$d/." "$DEST.new/usr/$d/"
    rm -rf "${DEST:?}.new/$d"
  fi
  if [ -d "$DEST.new/usr/$d" ]; then ln -sfn "usr/$d" "$DEST.new/$d"; fi
done

# Absolute links point at the build machine's files: each is made relative to
# where it stands, so it points at the same path inside the sysroot.
root="$(cd "$DEST.new" && pwd)"
find "$DEST.new" -type l -lname '/*' -print0 | while IFS= read -r -d '' link; do
  target="$(readlink "$link")"
  ln -sfn "$(realpath -s -m --relative-to="$(dirname "$link")" "$root$target")" "$link"
done

# What everything else links against is there.
libc="$DEST.new/usr/lib/aarch64-linux-gnu/libc.so.6"
[ -f "$libc" ] || { echo "sysroot_ubuntu_arm64.sh: no arm64 glibc in $LIST" >&2; exit 1; }

echo "$want" > "$DEST.new/.pinned"
rm -rf "$DEST"
mv "$DEST.new" "$DEST"
echo "sysroot_ubuntu_arm64.sh: done: $n packages in $DEST"
