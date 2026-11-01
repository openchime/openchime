#!/usr/bin/env bash
# sign_windows.sh <file>...
#
# Authenticode-signs Windows executables and installers from Linux with jsign
# (scripts/build/jsign.sh) against Azure Trusted Signing, so no Windows machine
# signs a release. The credentials are the release environment's:
# TRUSTED_SIGNING_ENDPOINT (e.g. https://eus.codesigning.azure.net),
# TRUSTED_SIGNING_ACCOUNT, TRUSTED_SIGNING_PROFILE, and the service
# principal AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET, exchanged
# here for an access token. SHA-256 file digest, RFC 3161 timestamp.
#
# Needs curl, python3 and a Java runtime.
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: sign_windows.sh <file>..." >&2; exit 2; }
cd "$(dirname "$0")/../.."
for v in TRUSTED_SIGNING_ENDPOINT TRUSTED_SIGNING_ACCOUNT TRUSTED_SIGNING_PROFILE \
         AZURE_TENANT_ID AZURE_CLIENT_ID AZURE_CLIENT_SECRET; do
  [ -n "${!v:-}" ] || { echo "sign_windows.sh: $v is not set" >&2; exit 1; }
done
command -v java >/dev/null || { echo "sign_windows.sh: needs a Java runtime" >&2; exit 1; }
scripts/build/jsign.sh >/dev/null
jar=$(scripts/build/jsign.sh --path)

token=$(curl -fsS --retry 3 "https://login.microsoftonline.com/${AZURE_TENANT_ID}/oauth2/v2.0/token" \
  --data-urlencode "client_id=${AZURE_CLIENT_ID}" \
  --data-urlencode "client_secret=${AZURE_CLIENT_SECRET}" \
  --data-urlencode "grant_type=client_credentials" \
  --data-urlencode "scope=https://codesigning.azure.net/.default" \
  | python3 -I -c 'import json, sys; print(json.load(sys.stdin)["access_token"])')
[ -n "${GITHUB_ACTIONS:-}" ] && echo "::add-mask::$token"

host=${TRUSTED_SIGNING_ENDPOINT#https://}
host=${host%/}
java -jar "$jar" --storetype TRUSTEDSIGNING --keystore "$host" --storepass "$token" \
  --alias "${TRUSTED_SIGNING_ACCOUNT}/${TRUSTED_SIGNING_PROFILE}" \
  --alg SHA-256 --tsaurl http://timestamp.acs.microsoft.com --tsmode RFC3161 "$@"
