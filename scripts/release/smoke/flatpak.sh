#!/usr/bin/env bash
# scripts/release/smoke/flatpak.sh
#
# The published Flatpak repository is added the way a desktop user adds it,
# from the published io.openchime.OpenChime.flatpakrepo, and lists the Linux
# client, io.openchime.OpenChime, for x86_64 and aarch64. The .flatpakrepo
# carries the signing key, so the remote it adds verifies the summary's
# signature, and `flatpak remote-ls` reads nothing from a summary that does
# not verify. Each architecture's commit is then pulled, its metadata alone,
# into that installation's repository with the remote's signature checking
# on, which is what an install verifies first: a commit with no valid
# signature from the key fails here as `flatpak install` would. Nothing is
# installed, and no runtime is needed: the remote is added to a Flatpak
# installation of this check's own (FLATPAK_USER_DIR, in a scratch directory
# removed after), never the machine's or the user's. Read from
# DIST_VERIFY_URL, which serves the same repository as DIST_BASE_URL, which
# the .flatpakrepo names.
#
# Needs: flatpak, ostree, curl; DIST_VERIFY_URL; network.
# Run by: scripts/release/smoke.sh.
. "$(dirname "$0")/../../test/lib/common.sh"
need flatpak "flatpak"; need ostree "ostree"; need curl "curl"
: "${DIST_VERIFY_URL:?DIST_VERIFY_URL is the address the published repository is verified at}"
app=io.openchime.OpenChime
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export FLATPAK_USER_DIR="$tmp/flatpak"
curl -fsSL "$DIST_VERIFY_URL/flatpak/$app.flatpakrepo" -o "$tmp/$app.flatpakrepo"
grep -q '^GPGKey=.' "$tmp/$app.flatpakrepo" || fail "the published $app.flatpakrepo carries no signing key"
# The published file names DIST_BASE_URL; read it from where it is verified.
sed -i "s|^Url=.*|Url=$DIST_VERIFY_URL/flatpak/|" "$tmp/$app.flatpakrepo"
flatpak remote-add --user --if-not-exists openchime "$tmp/$app.flatpakrepo"
for a in x86_64 aarch64; do
    refs=$(flatpak remote-ls --user --app --arch="$a" --columns=ref openchime) \
        || fail "flatpak cannot read the repository's signed summary for $a"
    grep -qxF "app/$app/$a/stable" <<<"$refs" \
        || fail "the repository does not list $app for $a (it lists: $(tr '\n' ' ' <<<"$refs"))"
    ostree --repo="$FLATPAK_USER_DIR/repo" pull --commit-metadata-only openchime "app/$app/$a/stable" > "$tmp/pull.log" 2>&1 \
        || { cat "$tmp/pull.log" >&2; fail "the repository's $a commit has no valid signature from its key"; }
done
ok "the Flatpak repository adds from its .flatpakrepo and lists $app for x86_64 and aarch64, its summary and each commit signed"
