#!/usr/bin/env bash
# Local OIDC-relay verification: the last cross-repo wire (ARCH-56/57).
#
# Proves that the control plane (openchime-saas) mints an ES256 identity token and
# a daemon in OIDC mode VERIFIES it against the central key it pins — end to end,
# credential-free (no real Google). It generates an ES256 keypair, gives the
# private half to the control plane (the signing key) and the public half to the
# daemon (pinned), mints a token via the dev endpoint, and has demo_client present
# it. The upstream-IdP + browser flow is bypassed on purpose: this exercises the
# daemon's verification, which is the untested half; mint↔verify is unit-tested too.
#
# Prereqs: the sibling ../openchime-saas, dotnet, Postgres (this script starts the
# compose db), curl, openssl.
#
# Usage: scripts/demo-oidc.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SAAS="$ROOT/../openchime-saas"
WORK="$(mktemp -d)"
CP=http://localhost:5176
ISS=http://openchime-demo
AUD=ws_demo_oidc
PROTO=18444; HEALTH=18081
CP_PID=""; D_PID=""
trap 'kill "$CP_PID" "$D_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Generate an ES256 keypair (central signs with the private half; the daemon pins the public half)"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$WORK/priv.pem" 2>/dev/null
openssl pkey -in "$WORK/priv.pem" -pubout -out "$WORK/pub.pem" 2>/dev/null

say "Start Postgres + the control plane (with the signing key + the dev token-mint)"
( cd "$SAAS" && docker compose up -d db >/dev/null 2>&1 )
( cd "$SAAS" && ConnectionStrings__ControlPlane="Host=localhost;Port=5432;Database=openchime_cp;Username=openchime;Password=openchime_dev" \
  Database__MigrateOnStartup=true \
  Oidc__SigningKeyPem="$(cat "$WORK/priv.pem")" Oidc__Kid=demo-kid Oidc__Issuer="$ISS" Oidc__DevMintEnabled=true \
  ASPNETCORE_URLS="$CP" ASPNETCORE_ENVIRONMENT=Development \
  dotnet run --project src/OpenChime.ControlPlane.Web -c Debug --no-launch-profile >"$WORK/cp.log" 2>&1 ) &
CP_PID=$!
for _ in $(seq 1 60); do [ "$(curl -s -o /dev/null -w '%{http_code}' "$CP/healthz" 2>/dev/null)" = "200" ] && break; sleep 1; done

say "Start a daemon in OIDC mode, pinned to the central public key"
( cd "$ROOT" && make openchimed >/dev/null && make demo-client >/dev/null )
OPENCHIME_DB_PATH="$WORK/oidc.db" OPENCHIME_TLS_CERT="$WORK/cert.pem" OPENCHIME_TLS_KEY="$WORK/key.pem" \
OPENCHIME_BLOB_DIR="$WORK/blobs" OPENCHIME_PROTO_PORT="$PROTO" OPENCHIME_HEALTH_PORT="$HEALTH" \
OPENCHIME_AUTH_MODE=oidc OPENCHIME_OIDC_ISSUER="$ISS" OPENCHIME_OIDC_AUDIENCE="$AUD" OPENCHIME_OIDC_PUBKEY_FILE="$WORK/pub.pem" \
"$ROOT/openchimed" >"$WORK/daemon.log" 2>&1 &
D_PID=$!
for _ in $(seq 1 40); do curl -sf "http://localhost:$HEALTH/healthz" >/dev/null 2>&1 && break; sleep 0.3; done
grep -iE "OIDC mode|listening on :$PROTO" "$WORK/daemon.log" || { echo "daemon did not come up"; tail "$WORK/daemon.log"; exit 1; }

say "Mint a token (dev endpoint) and present it to the daemon"
JWT="$(curl -s -X POST "$CP/api/dev/oidc/token" -H 'Content-Type: application/json' \
  -d "{\"audienceId\":\"$AUD\",\"subject\":\"alice-oidc\",\"email\":\"alice@demo.example\",\"name\":\"Alice\"}" \
  | sed -E 's/.*"token":"([^"]*)".*/\1/')"
[ -n "$JWT" ] || { echo "mint failed"; exit 1; }

"$ROOT/build/demo_client" 127.0.0.1 "$PROTO" --oidc "$JWT" whoami \
  && echo && echo "OK: the daemon verified a central-minted ES256 token and provisioned the OIDC user."
