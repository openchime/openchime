#!/usr/bin/env bash
# scripts/release/served.sh
#
# Waits until the address the repositories are verified at (DIST_VERIFY_URL)
# serves release VERSION's indexes consistently, so the smoke test's installs
# read what was published rather than what the storage was still serving a
# moment after the upload: the apt index's signed top (InRelease) verifies
# against the published keyring, each architecture's Packages matches the hash
# it lists and names this version, and the rpm index (repomd.xml) verifies
# against its signature. The storage behind that address can serve the
# previous release's signed files beside the new indexes for some minutes.
#
# Polls every SMOKE_SERVE_POLL seconds (default 20) for at most
# SMOKE_SERVE_WAIT seconds (default 600). Exits 0 once consistent; 1, naming
# what is still stale, if it never is.
#
# Needs: curl, gpgv, sha256sum; DIST_VERIFY_URL, VERSION.
# Run by: scripts/release/smoke.sh, before the repository installs.
set -uo pipefail
: "${DIST_VERIFY_URL:?DIST_VERIFY_URL is the address the published repository is verified at}"
: "${VERSION:?VERSION is the release being verified}"
wait_s=${SMOKE_SERVE_WAIT:-600}
poll_s=${SMOKE_SERVE_POLL:-20}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Prints what is stale, one reason per line; prints nothing when all is served.
stale() {
    local u=$DIST_VERIFY_URL a exp got
    curl -fsS --max-time 30 -o "$work/key.gpg" "$u/openchime-archive-keyring.gpg" || { echo "the keyring is not served"; return; }
    if ! curl -fsS --max-time 30 -o "$work/InRelease" "$u/apt/dists/stable/InRelease" \
       || ! gpgv --keyring "$work/key.gpg" "$work/InRelease" >/dev/null 2>&1; then
        echo "apt: InRelease does not verify"
    else
        for a in amd64 arm64; do
            exp=$(sed -n '/^SHA256:/,/^[A-Z]/p' "$work/InRelease" | awk -v f="main/binary-$a/Packages" '$3 == f {print $1}')
            if ! curl -fsS --max-time 30 -o "$work/Packages-$a" "$u/apt/dists/stable/main/binary-$a/Packages"; then
                echo "apt: $a Packages is not served"; continue
            fi
            got=$(sha256sum < "$work/Packages-$a" | cut -c1-64)
            [ -n "$exp" ] && [ "$exp" = "$got" ] || echo "apt: $a Packages is not the one InRelease lists"
            grep -qx "Version: $VERSION" "$work/Packages-$a" || echo "apt: $a Packages does not list version $VERSION"
        done
    fi
    if ! curl -fsS --max-time 30 -o "$work/repomd.xml" "$u/rpm/repodata/repomd.xml" \
       || ! curl -fsS --max-time 30 -o "$work/repomd.xml.asc" "$u/rpm/repodata/repomd.xml.asc" \
       || ! gpgv --keyring "$work/key.gpg" "$work/repomd.xml.asc" "$work/repomd.xml" >/dev/null 2>&1; then
        echo "rpm: repomd.xml does not verify against its signature"
    fi
}

start=$(date +%s)
while true; do
    reasons=$(stale)
    waited=$(( $(date +%s) - start ))
    if [ -z "$reasons" ]; then
        echo "served: release $VERSION's indexes are served consistently at $DIST_VERIFY_URL (after ${waited}s)"
        exit 0
    fi
    if [ "$waited" -ge "$wait_s" ]; then
        echo "served: after ${waited}s, $DIST_VERIFY_URL still serves stale indexes:"
        sed 's/^/  /' <<<"$reasons"
        exit 1
    fi
    echo "served: not yet (${waited}s): $(tr '\n' ';' <<<"$reasons")"
    sleep "$poll_s"
done
