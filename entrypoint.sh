#!/bin/sh
set -eu

: "${OPENCHIME_DB_PATH:=/data/openchime.db}"
mkdir -p "$(dirname "$OPENCHIME_DB_PATH")"

if [ ! -f "$OPENCHIME_DB_PATH" ]; then
  echo "entrypoint: no local DB found, attempting restore from replica..."
  litestream restore -if-replica-exists -config /etc/litestream.yml "$OPENCHIME_DB_PATH" || true
fi

if [ ! -f "$OPENCHIME_DB_PATH" ]; then
  echo "entrypoint: no replica to restore from, initializing a fresh database"
  sqlite3 "$OPENCHIME_DB_PATH" \
    "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS _placeholder (id INTEGER PRIMARY KEY, created_at TEXT DEFAULT CURRENT_TIMESTAMP); INSERT INTO _placeholder DEFAULT VALUES;"
fi

echo "entrypoint: starting litestream replicate"
litestream replicate -config /etc/litestream.yml &

echo "entrypoint: starting openchimed"
exec /usr/local/bin/openchimed
