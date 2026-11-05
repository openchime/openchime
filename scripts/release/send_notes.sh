#!/usr/bin/env bash
# Send the control plane what a release shipped, for its release notes.
#
#   scripts/release/send_notes.sh <release-number> <build-id> [--dry-run]
#
# The issues are scripts/release/shipped_issues.sh's, each as filed: its title,
# and its type from its label -- bug or enhancement, other when it has neither.
# Nothing is written by hand; the control plane formats them. The payload,
#
#   { "release": N, "build_id": "...", "published_at": "<UTC>",
#     "issues": [ { "number": N, "title": "...", "type": "bug" } ] }
#
# is posted to $CONTROL_PLANE_URL/api/releases/intake with the job's GitHub
# OIDC token (audience $CONTROL_PLANE_URL), so no secret is shared with the
# control plane. The issues are read from openchime/openchime (ISSUES_REPO
# overrides it) with GH_TOKEN; GH names the gh to run (a test's fixture).
# --dry-run prints the payload and posts nothing.
#
# NOTHING HERE FAILS THE RELEASE. By the time this runs the packages are live --
# every path warns and exits 0.
set -uo pipefail

warn() { echo "::warning::$*"; }
usage() {
  echo "usage: scripts/release/send_notes.sh <release-number> <build-id> [--dry-run]" >&2
  exit 2
}

n="${1:-}" build="${2:-}"
[ -n "$n" ] && [ -n "$build" ] || usage
printf '%s' "$n" | grep -qE '^[0-9]+$' || usage
dry=0
[ "${3:-}" = "--dry-run" ] && dry=1
gh="${GH:-gh}"
repo="${ISSUES_REPO:-openchime/openchime}"
here=$(cd "$(dirname "$0")" && pwd)

if ! issues=$("$here/shipped_issues.sh" "release-${n}"); then
  warn "could not list the issues release-${n} shipped; sent nothing"
  exit 0
fi

entries="[]"
while read -r num; do
  [ -z "$num" ] && continue
  if ! meta=$("$gh" issue view "$num" -R "$repo" --json title,labels 2>/dev/null); then
    warn "OC-${num}: could not read the issue; left out of the notes"
    continue
  fi
  if ! entries=$(jq -c --argjson num "$num" --argjson meta "$meta" '. + [{
        number: $num,
        title: $meta.title,
        type: ([$meta.labels[].name] as $l
               | if ($l | index("bug")) then "bug"
                 elif ($l | index("enhancement")) then "enhancement"
                 else "other" end)
      }]' <<<"$entries"); then
    warn "OC-${num}: could not read the issue's title and labels; sent nothing"
    exit 0
  fi
done <<<"$issues"

payload=$(jq -cn --argjson release "$n" --arg build "$build" \
  --arg at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --argjson issues "$entries" \
  '{release: $release, build_id: $build, published_at: $at, issues: $issues}')

if [ "$dry" = 1 ]; then
  printf '%s\n' "$payload"
  exit 0
fi

url="${CONTROL_PLANE_URL:-}"
[ -n "$url" ] || { warn "CONTROL_PLANE_URL is not set; the release's notes were not sent"; exit 0; }
url="${url%/}"
if [ -z "${ACTIONS_ID_TOKEN_REQUEST_URL:-}" ] || [ -z "${ACTIONS_ID_TOKEN_REQUEST_TOKEN:-}" ]; then
  warn "no OIDC token to ask for (the job needs id-token: write); the release's notes were not sent"
  exit 0
fi
if ! oidc=$(curl -fsS -H "Authorization: bearer ${ACTIONS_ID_TOKEN_REQUEST_TOKEN}" \
              "${ACTIONS_ID_TOKEN_REQUEST_URL}&audience=$(jq -rn --arg u "$url" '$u | @uri')" | jq -er .value); then
  warn "could not get an OIDC token; the release's notes were not sent"
  exit 0
fi

out=$(mktemp); trap 'rm -f "$out"' EXIT
status=$(curl -sS -o "$out" -w '%{http_code}' -X POST \
  -H "Authorization: Bearer ${oidc}" -H "Content-Type: application/json" \
  --data "$payload" "${url}/api/releases/intake" 2>/dev/null || true)
case "$status" in
  2??) echo "sent release ${n}'s notes ($(jq '.issues | length' <<<"$payload") issues): ${status}" ;;
  *)   warn "the control plane did not take release ${n}'s notes: ${status:-no answer} $(head -c 300 "$out" 2>/dev/null)" ;;
esac
exit 0
