#!/usr/bin/env bash
# End-to-end integration harness (docs/TESTING.md §3): bring up the real
# compose stack and exercise the deployed daemon as a black box —
#   1. health check responds (ARCH-25)
#   2. Litestream replication reaches MinIO (ARCH-23)
#   3. the protocol vertical works over TLS: two clients AUTH + SEND + BROADCAST
#   4. restore-on-boot recovers from the replica (ARCH-24)
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

echo "[itest] 1/4 waiting for /healthz"
wait_healthz
echo "[itest] 1/4 healthz OK"

echo "[itest] 2/4 verifying replication reached MinIO"
sleep 20   # past the 15s Litestream sync interval
out=$(docker compose run --rm --no-deps --entrypoint sh minio-init -c '
  until mc alias set local http://minio:9000 "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null 2>&1; do sleep 1; done
  mc ls --recursive local/"$LITESTREAM_BUCKET"/openchime
')
if [ -z "$(printf '%s' "$out" | tr -d '[:space:]')" ]; then
  echo "[itest] replication prefix empty" >&2; exit 1
fi
echo "[itest] 2/4 replication OK"

echo "[itest] 3/4 protocol vertical over TLS"
./build/e2e_client 127.0.0.1 8443
echo "[itest] 3/4 protocol vertical OK"

echo "[itest] 4/4 restore-on-boot"
docker compose stop daemon
docker compose rm -f daemon
docker volume rm openchime_daemon-data
docker compose up -d daemon
wait_healthz
# Poll the logs rather than grepping once: healthz can go green a beat before
# the restore line is flushed to the log stream (the source of an earlier flake).
wait_log() {
  for _ in $(seq 1 30); do
    docker compose logs daemon 2>&1 | grep -q "$1" && return 0
    sleep 1
  done
  return 1
}
wait_log "attempting restore from replica" \
  || { echo "[itest] restore-on-boot log line missing" >&2; docker compose logs daemon; exit 1; }
echo "[itest] 4/4 restore-on-boot OK"

echo "[itest] ALL INTEGRATION CHECKS PASSED"
