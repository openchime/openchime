#!/bin/sh
set -eu

# The daemon opens its own database and applies its own migrations on boot
# (daemon/dbwriter.c, ARCH-27/49), so there is nothing to seed here. An earlier
# version of this script created the file with the sqlite3 CLI and inserted a
# placeholder row; that was never needed, and it was the only reason the runtime
# image carried a SQLite command-line tool at all.
#
# Creating the directory IS still ours: the daemon creates the database, not the
# path leading to it, and on a fresh volume that path does not exist.
: "${OPENCHIME_DB_PATH:=/data/openchime.db}"
mkdir -p "$(dirname "$OPENCHIME_DB_PATH")"

# Arguments are forwarded, so `docker run <image> --version` reports the release
# instead of silently starting a daemon. Without "$@" the entrypoint swallows
# whatever it was given, which makes the image impossible to interrogate without
# overriding the entrypoint.
if [ "$#" -gt 0 ]; then
  exec /usr/local/bin/openchimed "$@"
fi

echo "entrypoint: starting openchimed"
exec /usr/local/bin/openchimed
