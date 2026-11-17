#!/usr/bin/env bash
# What a release shipped, read the way the release reads it: in a scratch
# repository with two release-N tags, scripts/release/shipped_issues.sh lists
# exactly the issues the commits between them name, each once, with the pull
# request's squash suffix not mistaken for one, plus the issue of the
# documentation the Release N snapshot carried; close_shipped_issues.sh would
# close the open ones of that list, and would remove each workflow label an
# issue of it carries, closed already or not, `parked` included, leaving its type;
# and send_notes.sh --dry-run, with the tracker replaced by a fixture, prints
# the payload the control plane takes, its types following the labels.
#
# Needs: git and jq. Run by: scripts/test/fast/source.sh (make test).
set -uo pipefail
cd "$(dirname "$0")/../../.."
root=$(pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail() { echo "shipped_issues: FAIL: $*" >&2; exit 1; }

# The tracker's prefix and a squash suffix, assembled so this file does not
# cite an issue (scripts/policy/refs.sh).
p=$(printf 'O%s' C)
pr=$(printf ' (%s%s)' '#' 90)

repo="$tmp/repo"
git init -q -b staging "$repo"
g() { git -C "$repo" -c user.name=t -c user.email=t@example.invalid "$@"; }
c() { g commit -q --allow-empty -m "$1"; }
c "${p}-1: First"
g tag release-1
c "${p}-2: Second${pr}"
c "${p}-3: Third"
c "Merge without an issue"
c "${p}-2: Second again"
g tag release-2
c "${p}-5: After the release"
# main: the Release 2 snapshot carries a documentation commit.
g checkout -q -b docs release-1
c "${p}-4: Documentation"
doc=$(g rev-parse HEAD)
g checkout -q -b main release-1
g commit -q --allow-empty -m "Release 2" -m "Docs: ${doc}"
g checkout -q staging

run() { (cd "$repo" && MAIN_REF=main "$@"); }

got=$(run "$root/scripts/release/shipped_issues.sh" release-2 2>/dev/null | tr '\n' ' ')
[ "$got" = "2 3 4 " ] || fail "release-2 shipped '$got', not '2 3 4 '"
got=$(run "$root/scripts/release/shipped_issues.sh" release-1 2>/dev/null | tr '\n' ' ')
[ "$got" = "1 " ] || fail "release-1 shipped '$got', not '1 '"
run "$root/scripts/release/shipped_issues.sh" release-9 >/dev/null 2>&1 \
  && fail "an unknown tag was read as nothing shipped"

# The tracker: 2 is a bug in review, 3 an enhancement with no workflow label,
# 4 neither, parked and waiting for a release, and closed already. An issue is
# answered as the API gives it and read with the caller's own --jq, so the
# script's choice of labels is what is tested.
cat > "$tmp/gh" <<'EOF'
#!/usr/bin/env bash
case "$1 $2" in
  "api repos/"*)
    case "${2##*/}" in
      2) j='{"state":"open","labels":[{"name":"bug"},{"name":"in-review"}]}' ;;
      3) j='{"state":"open","labels":[{"name":"enhancement"}]}' ;;
      *) j='{"state":"closed","labels":[{"name":"parked"},{"name":"pending-deployment"}]}' ;;
    esac
    q=.
    while [ $# -gt 0 ]; do [ "$1" = --jq ] && q="$2"; shift; done
    jq -r "$q" <<<"$j"
    exit 0 ;;
esac
case "$3" in
  2) l='[{"name":"bug"},{"name":"in-review"}]' ;;
  3) l='[{"name":"enhancement"}]' ;;
  *) l='[{"name":"in-review"}]' ;;
esac
printf '{"title":"Issue %s \\"quoted\\"","labels":%s}\n' "$3" "$l"
EOF
chmod +x "$tmp/gh"

dry=$(run env PATH="$tmp:$PATH" "$root/scripts/release/close_shipped_issues.sh" release-2 --dry-run 2>/dev/null)
got=$(sed -n 's/^would close //p' <<<"$dry" | tr '\n' ' ')
[ "$got" = "${p}-2 ${p}-3 " ] || fail "close_shipped_issues would close '$got', not the two open issues"
got=$(sed -n 's/^would remove //p' <<<"$dry" | tr '\n' ',')
[ "$got" = "in-review from ${p}-2,parked from ${p}-4,pending-deployment from ${p}-4," ] \
  || fail "close_shipped_issues would remove '$got': each workflow label an issue carries, parked included, and no type"

payload=$(run env GH="$tmp/gh" "$root/scripts/release/send_notes.sh" 2 abc1234 --dry-run 2>/dev/null)
shape=$(jq -c '[.release, .build_id, (.published_at | test("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z$")),
               [.issues[] | [.number, .title, .type]]]' <<<"$payload" 2>/dev/null) \
  || fail "send_notes printed no JSON: $payload"
want='[2,"abc1234",true,[[2,"Issue 2 \"quoted\"","bug"],[3,"Issue 3 \"quoted\"","enhancement"],[4,"Issue 4 \"quoted\"","other"]]]'
[ "$shape" = "$want" ] || fail "send_notes payload $shape, not $want"

# Not sent anywhere without the control plane's address, and never a failure.
out=$(run env GH="$tmp/gh" CONTROL_PLANE_URL= "$root/scripts/release/send_notes.sh" 2 abc1234 2>&1) \
  || fail "send_notes failed without CONTROL_PLANE_URL"
case "$out" in *"CONTROL_PLANE_URL is not set"*) ;; *) fail "send_notes did not say CONTROL_PLANE_URL is unset: $out" ;; esac
echo "shipped_issues: ok"
