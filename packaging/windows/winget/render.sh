#!/usr/bin/env bash
# Render the WinGet manifests for a release, ready for `wingetcreate submit`.
#
# Unlike apt and dnf, this publishes NO infrastructure: Microsoft hosts the
# index and the GitHub release serves the installer. All that ships is three
# YAML files submitted as a pull request to microsoft/winget-pkgs.
#
# The templates next to this script are the source of truth, rather than
# `wingetcreate update` editing whatever is currently live. That keeps the
# ProductCode wiring -- the part that silently breaks upgrade detection when it
# is wrong -- reviewed in this repository alongside the .iss it has to match,
# instead of in a file nobody here can see.
#
#   render.sh <version> <installer> <url> [outdir]
#
# <installer> is the built setup .exe, hashed here so the submitted manifest
# cannot disagree with the artifact actually attached to the release.
set -euo pipefail

VERSION="${1:?usage: render.sh <version> <installer> <url> [outdir]}"
INSTALLER="${2:?missing path to the built setup .exe}"
URL="${3:?missing installer download URL}"
OUTDIR="${4:-dist/winget}"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ID="OpenChime.OpenChime"

[ -f "$INSTALLER" ] || { echo "render.sh: $INSTALLER does not exist" >&2; exit 1; }

# The one failure this whole file exists to prevent, checked rather than
# reviewed. If the manifest's ProductCode stops matching the installer's AppId,
# WinGet cannot correlate an installed copy with the package: `winget upgrade`
# omits it entirely, with no error anywhere, and every install is stranded.
# Nothing else in the pipeline notices -- the manifest validates, the PR merges,
# and the package looks fine.
iss_appid="$(grep -oE 'AppId=\{\{[0-9A-Fa-f-]+' "$here/../openchime.iss" | sed 's/.*{{//')"
wg_appid="$(grep -oE 'ProductCode: "\{[0-9A-Fa-f-]+' "$here/$ID.installer.yaml" | sed 's/.*{//')"
if [ -z "$iss_appid" ] || [ "$iss_appid" != "$wg_appid" ]; then
  echo "render.sh: the WinGet ProductCode does not match the Inno AppId" >&2
  echo "  openchime.iss  AppId       = {${iss_appid:-<not found>}}" >&2
  echo "  installer.yaml ProductCode = {${wg_appid:-<not found>}}_is1" >&2
  exit 1
fi

# Uppercase: the manifest schema requires it, and sha256sum emits lowercase.
SHA256="$(sha256sum "$INSTALLER" | cut -d' ' -f1 | tr 'a-f' 'A-F')"

mkdir -p "$OUTDIR"
for t in "$ID.yaml" "$ID.installer.yaml" "$ID.locale.en-US.yaml"; do
  [ -f "$here/$t" ] || { echo "render.sh: missing template $t" >&2; exit 1; }
  sed -e "s|@VERSION@|${VERSION}|g" \
      -e "s|@SHA256@|${SHA256}|g" \
      -e "s|@URL@|${URL}|g" \
      "$here/$t" > "$OUTDIR/$t"
done

# A leftover placeholder means a template gained a field this script does not
# fill. Submitting that produces a manifest that validates and then points at
# nothing, so it is caught here instead.
if grep -l '@[A-Z0-9_]*@' "$OUTDIR"/*.yaml 2>/dev/null | grep -q .; then
  echo "render.sh: unsubstituted placeholders remain:" >&2
  grep -n '@[A-Z0-9_]*@' "$OUTDIR"/*.yaml >&2
  exit 1
fi

echo "rendered $ID $VERSION into $OUTDIR (sha256 $SHA256)"
