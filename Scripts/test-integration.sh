#!/usr/bin/env bash
# End-to-end integration harness (docs/TESTING.md §3): bring up the real
# compose stack and exercise the deployed daemon as a black box —
#   1. health check responds (ARCH-25)
#   2. the protocol vertical works over TLS: two clients AUTH + SEND + BROADCAST
# Backup/replication is not exercised here: it is a hosted-model concern and
# lives in the openchime-saas repo, not this one.
# A non-zero exit fails CI. Run locally with `make integration`.
set -euo pipefail

cd "$(dirname "$0")/.."
export COMPOSE_PROJECT_NAME=openchime
[ -f .env ] || cp .env.example .env

cleanup() { docker compose down -v >/dev/null 2>&1 || true; }
trap cleanup EXIT

wait_healthz() {
  for _ in $(seq 1 60); do
    curl -sf http://localhost:8080/healthz >/dev/null 2>&1 && return 0
    sleep 2
  done
  echo "[itest] daemon did not become healthy" >&2
  docker compose logs
  return 1
}

echo "[itest] building e2e client + starting the stack"
make -s build/e2e_client
docker compose up --build -d

echo "[itest] 1/2 waiting for /healthz"
wait_healthz
echo "[itest] 1/2 healthz OK"

echo "[itest] 2/2 protocol vertical over TLS"
./build/e2e_client 127.0.0.1 8443
echo "[itest] 2/2 protocol vertical OK"

echo "[itest] ALL INTEGRATION CHECKS PASSED"
