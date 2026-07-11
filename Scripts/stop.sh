#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "Scripts/stop.sh: stopping the platform (local DB and MinIO data preserved)..."
docker compose down
