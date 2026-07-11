#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "Scripts/reset.sh: stopping the platform and wiping local volumes (DB + MinIO data)..."
docker compose down -v
