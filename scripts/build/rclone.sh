#!/usr/bin/env bash
# rclone, pinned and checksummed, instead of an `apt-get update` for one
# package (docs/CONTRIBUTING.md, "Billed minutes"): fetched once into the
# user's cache and put on the PATH -- GitHub's, in a workflow.
#
#   scripts/build/rclone.sh
#
# The checksum is the one rclone publishes in the release's SHA256SUMS.
# Needs: curl, python3, sha256sum.
# Run by: the workflows' steps that copy to object storage.
set -euo pipefail
v=v1.76.0
sum=b00f00b3266c3764413afe599d3ec316f6f43b3e0ea8a0bc1d186d2133e46df7
dir="${XDG_CACHE_HOME:-$HOME/.cache}/openchime/rclone-$v"
if [ ! -x "$dir/rclone" ]; then
  mkdir -p "$dir"
  curl -fsSL --retry 3 --max-time 120 -o "$dir/r.zip" "https://downloads.rclone.org/$v/rclone-$v-linux-amd64.zip"
  echo "$sum  $dir/r.zip" | sha256sum -c --quiet - || { rm -f "$dir/r.zip"; echo "rclone.sh: checksum mismatch" >&2; exit 1; }
  python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); open(sys.argv[2], "wb").write(z.read(sys.argv[3]))' \
    "$dir/r.zip" "$dir/rclone" "rclone-$v-linux-amd64/rclone"
  chmod +x "$dir/rclone"
  rm -f "$dir/r.zip"
fi
if [ -n "${GITHUB_PATH:-}" ]; then echo "$dir" >> "$GITHUB_PATH"; fi
# The first line of its version, read whole first: a pipe into head would close
# on rclone, and under pipefail its SIGPIPE fails the script.
version=$("$dir/rclone" version)
echo "${version%%$'\n'*}"
