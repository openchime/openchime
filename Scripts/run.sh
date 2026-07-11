#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f .env ]; then
  echo "Scripts/run.sh: no .env found, copying .env.example"
  cp .env.example .env
fi

echo "Scripts/run.sh: building and starting the platform..."
docker compose up --build -d

echo "Scripts/run.sh: waiting for the daemon's health check..."
for i in $(seq 1 30); do
  if curl -sf http://localhost:8080/healthz >/dev/null 2>&1; then
    echo "Scripts/run.sh: daemon is healthy."
    break
  fi
  if [ "$i" -eq 30 ]; then
    echo "Scripts/run.sh: daemon did not become healthy in time" >&2
    docker compose logs daemon
    exit 1
  fi
  sleep 1
done

cat <<EOF

OpenChime local platform is running:
  daemon health check:  http://localhost:8080/healthz
  MinIO console:         http://localhost:9001  (login from .env)

  View logs:   docker compose logs -f
  Stop:        Scripts/stop.sh
  Full reset:  Scripts/reset.sh   (also wipes local DB + MinIO data)
EOF
