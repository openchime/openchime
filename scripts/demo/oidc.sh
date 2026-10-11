#!/usr/bin/env bash
# Local OIDC-relay verification: the last cross-repo wire (ARCH-56/57).
#
# Proves that the control plane (openchime-saas) mints an ES256 identity token and
# a daemon in relay mode VERIFIES it with the keys central publishes — end to end,
# credential-free (no real Google). It generates an ES256 keypair, gives the
# private half to the control plane (the signing key, which it publishes at
# /oidc/jwks), enrols a daemon with it (OPENCHIME_ENROLL_URL is the relay: its
# origin is the issuer and its JWKS the keys), mints a token for the daemon's own
# enrollment audience via the dev endpoint, and has demo_client present it. The
# upstream-IdP + browser flow is bypassed on purpose: this exercises the daemon's
# verification, which is the untested half; mint<->verify is unit-tested too.
#
# Prereqs: the sibling ../openchime-saas, dotnet, curl, openssl, and a Postgres
# already listening on localhost:5432 with the openchime role/database below.
# This script used to start that Postgres itself via the sibling repo's compose
# stack; the project no longer uses Docker anywhere, so bringing the database up
# is now the caller's job -- same convention as scripts/demo/federated.sh, which
# has always expected an already-running control plane.
#
# Usage: scripts/demo/oidc.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAAS="$ROOT/../openchime-saas"
WORK="$(mktemp -d)"
CP=http://localhost:5176
ISS="$CP"   # the relay's issuer is the enroll URL's origin
PROTO=18444; HEALTH=18081
CP_PID=""; D_PID=""
trap 'kill "$CP_PID" "$D_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Generate an ES256 keypair (central signs with the private half and publishes the public half)"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$WORK/priv.pem" 2>/dev/null

say "Check Postgres is up (start it yourself -- see the prereqs at the top)"
if ! (exec 3<>/dev/tcp/localhost/5432) 2>/dev/null; then
  echo "demo-oidc: nothing listening on localhost:5432." >&2
  echo "  Start Postgres with the openchime role/database, then re-run." >&2
  exit 1
fi

say "Start the control plane (with the signing key + the dev token-mint)"
( cd "$SAAS" && ConnectionStrings__ControlPlane="Host=localhost;Port=5432;Database=openchime_cp;Username=openchime;Password=openchime_dev" \
  Database__MigrateOnStartup=true \
  Oidc__SigningKeyPem="$(cat "$WORK/priv.pem")" Oidc__Kid=demo-kid Oidc__Issuer="$ISS" Oidc__DevMintEnabled=true \
  ASPNETCORE_URLS="$CP" ASPNETCORE_ENVIRONMENT=Development \
  dotnet run --project src/OpenChime.ControlPlane.Web -c Debug --no-launch-profile >"$WORK/cp.log" 2>&1 ) &
CP_PID=$!
for _ in $(seq 1 60); do [ "$(curl -s -o /dev/null -w '%{http_code}' "$CP/healthz" 2>/dev/null)" = "200" ] && break; sleep 1; done

say "Start a daemon in relay mode, enrolled with central (its keys come from $CP/oidc/jwks)"
( cd "$ROOT" && make DEMO=1 openchimed >/dev/null && make build/demo_client >/dev/null )
mkdir -p "$WORK/data"
OPENCHIME_DATA_DIR="$WORK/data" OPENCHIME_PORT="$PROTO" OPENCHIME_HEALTH_PORT="$HEALTH" \
OPENCHIME_AUTH=relay OPENCHIME_ENROLL_URL="$CP" \
"$ROOT/openchimed" >"$WORK/daemon.log" 2>&1 &
D_PID=$!
for _ in $(seq 1 40); do curl -sf "http://localhost:$HEALTH/healthz" >/dev/null 2>&1 && break; sleep 0.3; done
grep -iE "OIDC mode|listening on :$PROTO" "$WORK/daemon.log" || { echo "daemon did not come up"; tail "$WORK/daemon.log"; exit 1; }
AUD="$(sed -nE 's/.*OIDC mode \(issuer=.* audience=([^)]*)\).*/\1/p' "$WORK/daemon.log" | head -1)"
[ -n "$AUD" ] || { echo "no audience in the log"; exit 1; }
sleep 2   # the relay's published keys are fetched at boot

say "Mint a token (dev endpoint) and present it to the daemon"
JWT="$(curl -s -X POST "$CP/api/dev/oidc/token" -H 'Content-Type: application/json' \
  -d "{\"audienceId\":\"$AUD\",\"subject\":\"alice-oidc\",\"email\":\"alice@demo.example\",\"name\":\"Alice\"}" \
  | sed -E 's/.*"token":"([^"]*)".*/\1/')"
[ -n "$JWT" ] || { echo "mint failed"; exit 1; }

"$ROOT/build/demo_client" 127.0.0.1 "$PROTO" --oidc "$JWT" whoami \
  && echo && echo "OK: the daemon verified a central-minted ES256 token with the relay's published keys and provisioned the user."
