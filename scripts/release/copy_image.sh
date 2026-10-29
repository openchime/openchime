#!/usr/bin/env bash
# Copy an image registry to registry, byte for byte, and prove it arrived intact
# from the copy itself.
#
#   scripts/release/copy_image.sh <src creds> <src>@sha256:<hex> <dest creds> <dest>
#
# The source is named by digest. skopeo copies every platform with its manifest
# unchanged (--all --preserve-digests) and writes the digest of what it pushed
# to a file (--digestfile); that digest must be the source's. The destination
# is never read back: a registry does not always serve a tag the moment its
# push finishes, and the copy's own report is the stronger evidence anyway.
# Prints the digest. Exits 1 on a failed copy or a changed digest.
set -euo pipefail
src_creds="${1:?usage: scripts/release/copy_image.sh <src creds> <src>@sha256:<hex> <dest creds> <dest>}"
src="${2:?}"; dest_creds="${3:?}"; dest="${4:?}"
want=${src##*@}
[[ "$want" =~ ^sha256:[0-9a-f]{64}$ ]] || { echo "::error::the source ${src} is not named by digest" >&2; exit 1; }
out=$(mktemp)
trap 'rm -f "$out"' EXIT
skopeo copy --all --preserve-digests --retry-times 3 --digestfile "$out" \
  --src-creds "$src_creds" --dest-creds "$dest_creds" "docker://${src}" "docker://${dest}" >&2
got=$(tr -d '[:space:]' <"$out")
[ "$got" = "$want" ] || { echo "::error::the digest changed in the copy to ${dest}: ${want} -> ${got:-nothing}" >&2; exit 1; }
printf '%s\n' "$got"
