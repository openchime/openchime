#!/usr/bin/env bash
# scripts/test/merge/e2e.sh
#
# The built daemon, started from its environment as a deployment starts it --
# the one thing the in-process suites cannot show, since they link the
# daemon's code without its main(): /healthz, then the protocol over TLS with
# the e2e client (sign in on the daemon's page, send, receive), then a push
# token registered after sign-in. Development mode, because the test makes its
# users, which only that mode reads (CONFIG.md "Mode"). Its ports are E2E_PORT
# and E2E_HEALTH_PORT (18443, 18080), clear of a dev daemon on 8443.
#
# Needs: curl; openchimed, build/e2e_client, build/demo_client.
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
need curl "curl"
for f in openchimed build/e2e_client build/demo_client; do have "$f" "make test builds it"; done
port=${E2E_PORT:-18443} hport=${E2E_HEALTH_PORT:-18080}
dir=$(mktemp -d)
mkdir -p "$dir/data"
OPENCHIME_DATA_DIR="$dir/data" OPENCHIME_PORT="$port" OPENCHIME_HEALTH_PORT="$hport" \
OPENCHIME_MODE=development OPENCHIME_DEV_USERS='e2e-alice:e2e-pw:owner,e2e-bob:e2e-pw:member' \
    ./openchimed >"$dir/daemon.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -rf "$dir"' EXIT
if ! wait_healthz "http://127.0.0.1:$hport/healthz" 60 "$pid"; then
    cat "$dir/daemon.log" >&2
    fail "the daemon did not become healthy"
fi
./build/e2e_client 127.0.0.1 "$port"
out=$(./build/demo_client 127.0.0.1 "$port" e2e-bob e2e-pw token fcm e2e-device-token)
echo "$out"
grep -q "registered fcm token" <<<"$out" || fail "demo_client did not register a push token"
# A reply in a thread is acknowledged as stored, behind what the daemon pushes
# after sign-in, as a send is.
out=$(./build/demo_client 127.0.0.1 "$port" e2e-bob e2e-pw send 1 "a message to reply to")
echo "$out"
mid=$(sed -n 's/^demo_client: sent message \([0-9]*\) .*/\1/p' <<<"$out")
[ -n "$mid" ] || fail "demo_client did not send a message"
out=$(./build/demo_client 127.0.0.1 "$port" e2e-bob e2e-pw reply 1 "$mid" "a reply")
echo "$out"
grep -q "replied in thread $mid" <<<"$out" || fail "demo_client did not report its reply"
ok "the daemon served the protocol over TLS"
