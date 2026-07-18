#!/bin/sh
set -eu

: "${OPENCHIME_DB_PATH:=/data/openchime.db}"
mkdir -p "$(dirname "$OPENCHIME_DB_PATH")"

if [ ! -f "$OPENCHIME_DB_PATH" ]; then
  echo "entrypoint: no database found, initializing a fresh one"
  sqlite3 "$OPENCHIME_DB_PATH" \
    "PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS _placeholder (id INTEGER PRIMARY KEY, created_at TEXT DEFAULT CURRENT_TIMESTAMP); INSERT INTO _placeholder DEFAULT VALUES;"
fi

echo "entrypoint: starting openchimed"
exec /usr/local/bin/openchimed
