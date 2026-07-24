#!/usr/bin/env bash
# Local federated demo (ARCH-84 enrollment + ARCH-85 push), end to end.
#
# Stands up a daemon in the FEDERATED model against an already-running control
# plane (the openchime-saas repo), enrolls it, then drives a client: one user
# registers a push device token, another sends a message, and the control-plane
# push gateway relays a contentless notification. Proves the two daemon->central
# outbound wires (enrollment, push) interoperate — the C daemon signs with mbedTLS,
# the .NET control plane verifies (CP-12).
#
# Prereqs:
#   - The control plane running with the dev log push provider, e.g. from openchime-saas:
#       ConnectionStrings__ControlPlane=... Database__MigrateOnStartup=true \
#       Push__Log__Enabled=true ASPNETCORE_URLS=http://localhost:5176 \
#       dotnet run --project src/OpenChime.ControlPlane.Web
#   - curl, sqlite3 optional.
#
# Usage: scripts/demo-federated.sh [control-plane-url]   (default http://localhost:5176)
set -euo pipefail

CP="${1:-http://localhost:5176}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
PROTO=18443; HEALTH=18080
trap 'kill "${DPID:-0}" 2>/dev/null || true; rm -rf "$WORK"' EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Building daemon + demo client"
( cd "$ROOT" && make openchimed >/dev/null && make demo-client >/dev/null )

daemon_env=(
  OPENCHIME_DB_PATH="$WORK/demo.db" OPENCHIME_TLS_CERT="$WORK/cert.pem" OPENCHIME_TLS_KEY="$WORK/key.pem"
  OPENCHIME_BLOB_DIR="$WORK/blobs" OPENCHIME_PROTO_PORT="$PROTO" OPENCHIME_HEALTH_PORT="$HEALTH"
  OC_BOOTSTRAP_USERS="alice:pw:owner,bob:pw:member"
  OC_ENROLL_URL="$CP" OC_PUSH_URL="$CP"
)
mkdir -p "$WORK/blobs"

boot() { env "${daemon_env[@]}" "$ROOT/openchimed" >>"$WORK/daemon.log" 2>&1 & DPID=$!; }
wait_health() { for _ in $(seq 1 40); do curl -sf "http://localhost:$HEALTH/healthz" >/dev/null 2>&1 && return 0; sleep 0.25; done; return 1; }

say "Boot 1: generate keypair + audience, print enrollment code (pending)"
: >"$WORK/daemon.log"; boot; sleep 3
OCE="$(grep -oE 'oce1\.[A-Za-z0-9_-]+' "$WORK/daemon.log" | head -1)"
kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true
[ -n "$OCE" ] || { echo "no enrollment code produced"; exit 1; }
echo "audience code: ${OCE:0:24}..."

say "Reserve the code in the console (operator step, scripted)"
J="$WORK/cookies.txt"
tok() { grep -oE 'name="__RequestVerificationToken"[^>]*value="[^"]*"' "$1" | head -1 | sed -E 's/.*value="([^"]*)".*/\1/'; }
OP="op-$RANDOM@example.com"
curl -sf -c "$J" "$CP/account/register" -o "$WORK/reg.html"
curl -sf -b "$J" -c "$J" -o /dev/null \
  -d "Input.Email=$OP" --data-urlencode "Input.Password=Passw0rd!" \
  --data-urlencode "Input.ConfirmPassword=Passw0rd!" \
  --data-urlencode "__RequestVerificationToken=$(tok "$WORK/reg.html")" "$CP/account/register"
curl -sf -b "$J" -c "$J" "$CP/dashboard/enroll" -o "$WORK/enroll.html"
curl -sf -b "$J" -c "$J" -o /dev/null \
  --data-urlencode "Code=$OCE" --data-urlencode "__RequestVerificationToken=$(tok "$WORK/enroll.html")" \
  "$CP/dashboard/enroll"
echo "reserved."

say "Boot 2: prove possession -> activate; enable the push emitter"
boot; wait_health || { echo "daemon did not come up"; tail -20 "$WORK/daemon.log"; exit 1; }
grep -q "enrollment activated" "$WORK/daemon.log" || { echo "activation failed"; tail -20 "$WORK/daemon.log"; exit 1; }
grep -iE "enrollment activated|push emitter enabled|listening on :$PROTO" "$WORK/daemon.log"

say "Client flow: bob registers a device token, alice sends a message"
"$ROOT/build/demo_client" 127.0.0.1 "$PROTO" bob pw token apns tok-bob-demo
"$ROOT/build/demo_client" 127.0.0.1 "$PROTO" alice pw send 1 "hello from the federated demo"

say "Done. Watch the control-plane log for a line like:"
echo "  push[Apns] would notify token tok-bob-demo (channel 1, ...)"
echo "That is the daemon->central push wire firing end to end (signed CP-12, contentless)."
