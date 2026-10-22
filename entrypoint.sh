#!/bin/sh
set -eu

# The daemon opens its own database and applies its own migrations on boot
# (daemon/dbwriter.c, ARCH-27/49), so there is nothing to seed here. An earlier
# version of this script created the file with the sqlite3 CLI and inserted a
# placeholder row; that was never needed, and it was the only reason the runtime
# image carried a SQLite command-line tool at all.
#
# Creating the directory IS still ours: the daemon creates the database, not the
# directory holding it, and on a fresh volume that path does not exist.
: "${OPENCHIME_DATA_DIR:=/data}"
export OPENCHIME_DATA_DIR
mkdir -p "$OPENCHIME_DATA_DIR"

# Arguments are forwarded, so `podman run <image> --version` reports the release
# instead of silently starting a daemon. Without "$@" the entrypoint swallows
# whatever it was given, which makes the image impossible to interrogate without
# overriding the entrypoint. Nothing below runs for an interrogation: it starts
# no replicator and touches no storage.
if [ "$#" -gt 0 ]; then
  exec /usr/local/bin/openchimed "$@"
fi

# --- Replication (ARCH-3, ARCH-39) --------------------------------------------
# What the boot did with the replica is said on one line of stable tokens,
# "entrypoint: <key>=<value>", beside the sentence for a person: a harness, a
# log query or the release-candidate's image check reads the tokens, never the
# words. They are:
#   entrypoint: replica=off            no replication files; the daemon as before
#   entrypoint: replica=misconfigured  files there but wrong; not started (exit 1)
#   entrypoint: replica=on             replicating
#   entrypoint: restore=ok             no database; the replica's copy restored
#   entrypoint: restore=empty          no database; the replica holds no copy yet
#   entrypoint: restore=failed         no database; the restore failed (exit 3)
# An existing database is left alone and says no restore= token.
#
# On only when the deployment provides it, as files the hosted deployment
# writes under /etc/openchime (mode 0600): replica-bucket, replica-prefix (the
# workspace's area in the bucket), replica-accesskey and replica-secretkey, all
# four or none, and optionally replica-endpoint. The files are the only way in:
# no variable turns replication on. A run without them -- every self-hosted
# one -- starts the daemon exactly as before, with no replicator. The values
# are never printed.
CREDS=/etc/openchime
present=""
missing=""
for f in replica-bucket replica-prefix replica-accesskey replica-secretkey; do
  if [ -e "$CREDS/$f" ]; then present="$present $f"; else missing="$missing $f"; fi
done

if [ -z "$present" ]; then
  echo "entrypoint: replica=off"
  echo "entrypoint: starting openchimed"
  exec /usr/local/bin/openchimed
fi

# Some of the four but not all is a deployment that meant to replicate and got
# it wrong: starting unreplicated would hide that until the box was lost.
fail() {
  echo "entrypoint: replica=misconfigured" >&2
  echo "entrypoint: $1, so the daemon is not started" >&2
  exit 1
}
[ -z "$missing" ] || fail "replication needs all four of replica-bucket, replica-prefix, replica-accesskey and replica-secretkey under $CREDS, and this box lacks$missing"
for f in replica-bucket replica-prefix replica-accesskey replica-secretkey; do
  [ -r "$CREDS/$f" ] && [ -s "$CREDS/$f" ] || fail "$CREDS/$f is empty or cannot be read"
done
bucket=$(cat "$CREDS/replica-bucket")
prefix=$(cat "$CREDS/replica-prefix")
access_key=$(cat "$CREDS/replica-accesskey")
secret_key=$(cat "$CREDS/replica-secretkey")
endpoint=https://fly.storage.tigris.dev
if [ -e "$CREDS/replica-endpoint" ]; then
  endpoint=$(cat "$CREDS/replica-endpoint")
fi

# The bucket, the prefix and the endpoint are written into Litestream's
# configuration below, so each is held to the characters it can have: nothing
# that YAML, or Litestream's own $-expansion of its configuration, would read
# as anything but the value. The prefix is one path segment.
case "$bucket" in ''|*[!a-z0-9.-]*) fail "$CREDS/replica-bucket is not a bucket name (a-z, 0-9, '.' and '-')" ;; esac
case "$prefix" in ''|*[!a-z0-9-]*) fail "$CREDS/replica-prefix is not a single path segment (a-z, 0-9 and '-')" ;; esac
case "$endpoint" in
  http://?*|https://?*) ;;
  *) fail "$CREDS/replica-endpoint is not an http:// or https:// URL" ;;
esac
case "$endpoint" in *[!A-Za-z0-9.:/_-]*) fail "$CREDS/replica-endpoint holds a character a URL here cannot" ;; esac

DB="$OPENCHIME_DATA_DIR/openchime.db"
# Litestream's configuration, written fresh at every boot from the files above
# and readable by root alone. The key pair is deliberately not in it: Litestream
# reads LITESTREAM_ACCESS_KEY_ID and LITESTREAM_SECRET_ACCESS_KEY from its own
# environment, which this script sets for the two Litestream commands only, so
# a secret is never pasted into YAML, where a ':' or '#' in it would change what
# the file says.
CONFIG=/run/litestream.yml
mkdir -p /run
rm -f "$CONFIG"
(
  umask 077
  cat > "$CONFIG" <<EOF
# Written by /entrypoint.sh at boot from $CREDS (ARCH-3, ARCH-39).
dbs:
  - path: $DB
    replica:
      type: s3
      bucket: $bucket
      # The workspace's area in the bucket is its prefix; the database is db/
      # under it.
      path: $prefix/db
      # Any endpoint makes Litestream address the bucket by path, which both
      # Tigris and a local S3 implementation accept.
      endpoint: $endpoint
      region: auto
      # How often changes leave the box: the recovery point a lost box falls
      # back to is at most this old (REQ-094). The daemon's own ack never waits
      # on it (ARCH-23).
      sync-interval: 60s

# A full copy a day, and every change between them, for 14 days: any point in
# that window can be restored. The same window the control plane keeps for its
# own database.
snapshot:
  interval: 24h
  retention: 336h
EOF
)
chmod 600 "$CONFIG"

# The base carries no CA bundle; Litestream verifies the bucket's HTTPS against
# the daemon's own roots, written out as PEM in the image (ARCH-10).
export SSL_CERT_FILE=/usr/share/openchime/ca-roots.pem

# A box that finds no database -- a new volume, or the host it was on lost --
# takes the latest copy before the daemon opens anything. A bucket holding no
# copy is the very first boot and not an error. Any other failure stops the
# boot: a daemon started empty here would mint a new identity and enrollment,
# and a managed box's one-time enrollment ticket cannot claim it a second time,
# so the workspace would be lost rather than recovered. An existing database is
# left alone.
if [ ! -e "$DB" ]; then
  echo "entrypoint: no database at $DB; restoring the latest copy from the replica, if one exists"
  if ! LITESTREAM_ACCESS_KEY_ID="$access_key" LITESTREAM_SECRET_ACCESS_KEY="$secret_key" \
      /usr/local/bin/litestream restore -config "$CONFIG" -if-db-not-exists -if-replica-exists \
      -integrity-check full "$DB"; then
    echo "entrypoint: restore=failed" >&2
    echo "entrypoint: the restore from the replica failed, so the daemon is not started: a daemon started on an empty database would replace this workspace's identity and enrollment rather than recover them, and its enrollment ticket cannot be used twice. Fix the bucket, the key or the endpoint and start the box again." >&2
    exit 3
  fi
  if [ -e "$DB" ]; then
    echo "entrypoint: restore=ok"
    echo "entrypoint: restored $DB from the replica"
  else
    echo "entrypoint: restore=empty"
    echo "entrypoint: the replica holds no copy yet; the daemon creates a new database"
  fi
fi

# Litestream runs the daemon as its child and replicates beside it; it forwards
# the platform's stop signal to the daemon and makes a final sync once the
# daemon has exited. The key is exported only now, for Litestream, which this
# shell becomes; the daemon is given an environment without it: it has no use
# for it, and a process that serves the network should not hold a credential it
# never needs. (Litestream copies the key to the AWS_* names for its SDK, so
# those go too.)
echo "entrypoint: replica=on"
echo "entrypoint: starting openchimed, replicated to the bucket by Litestream"
export LITESTREAM_ACCESS_KEY_ID="$access_key" LITESTREAM_SECRET_ACCESS_KEY="$secret_key"
exec /usr/local/bin/litestream replicate -config "$CONFIG" -exec "/usr/bin/env -u LITESTREAM_ACCESS_KEY_ID -u LITESTREAM_SECRET_ACCESS_KEY -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY /usr/local/bin/openchimed"
