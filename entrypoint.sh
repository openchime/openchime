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
#   entrypoint: replica=off              no replication files; the daemon as before
#   entrypoint: replica=misconfigured    files there but wrong; not started (exit 1)
#   entrypoint: replica=on               replicating
#   entrypoint: identity-restore=ok      no identity; the replica's copy restored
#   entrypoint: identity-restore=empty   no identity; the replica holds no copy yet
#   entrypoint: identity-restore=failed  no identity; the restore failed (exit 3)
#   entrypoint: restore=ok               no database; the replica's copy restored
#   entrypoint: restore=empty            no database; the replica holds no copy yet
#   entrypoint: restore=failed           no database; the restore failed (exit 3)
#   entrypoint: attachments=s3           attachments kept beside the replica
# An existing database or identity is left alone and says no restore token.
#
# On only when the deployment provides it, as files the hosted deployment
# writes under /etc/openchime (mode 0600): replica-bucket, replica-prefix (the
# data's place in the bucket), replica-accesskey and replica-secretkey, all
# four or none, and optionally:
#   replica-endpoint           the S3 endpoint (default: Tigris)
#   replica-identity-prefix    the identity's own place (ARCH-66b): replicated
#                              apart from the data, so the data can be begun
#                              again or restored while the identity stays
#   replica-restore-at         a time (RFC 3339): a database restored here is
#                              the copy as it was then, not the latest
#   replica-restore-from       where a database restored here is read from,
#                              when not replica-prefix: an earlier copy is
#                              read from its own place and the box replicates
#                              to a new one, never on top of the old history
#   replica-blob-prefix        attachments in the bucket under it (ARCH-70),
#                              with the same key, instead of on the volume
# The files are the only way in: no variable turns replication on. A run
# without them -- every self-hosted one -- starts the daemon exactly as before,
# with no replicator. The values are never printed.
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
for f in replica-endpoint replica-identity-prefix replica-restore-at replica-restore-from replica-blob-prefix; do
  # An optional file, if there, must say something: an empty one is a mistake.
  [ ! -e "$CREDS/$f" ] || { [ -r "$CREDS/$f" ] && [ -s "$CREDS/$f" ]; } || fail "$CREDS/$f is empty or cannot be read"
done
optional() { if [ -e "$CREDS/$1" ]; then cat "$CREDS/$1"; fi; }
bucket=$(cat "$CREDS/replica-bucket")
prefix=$(cat "$CREDS/replica-prefix")
access_key=$(cat "$CREDS/replica-accesskey")
secret_key=$(cat "$CREDS/replica-secretkey")
endpoint=$(optional replica-endpoint)
[ -n "$endpoint" ] || endpoint=https://fly.storage.tigris.dev
identity_prefix=$(optional replica-identity-prefix)
restore_at=$(optional replica-restore-at)
restore_from=$(optional replica-restore-from)
blob_prefix=$(optional replica-blob-prefix)

# The bucket, the prefixes and the endpoint are written into Litestream's
# configuration below, so each is held to the characters it can have: nothing
# that YAML, or Litestream's own $-expansion of its configuration, would read
# as anything but the value. A prefix is path segments of a-z, 0-9 and '-'.
is_prefix() {
  case "$1" in ''|/*|*/|*//*|*[!a-z0-9/-]*) return 1 ;; esac
  return 0
}
case "$bucket" in ''|*[!a-z0-9.-]*) fail "$CREDS/replica-bucket is not a bucket name (a-z, 0-9, '.' and '-')" ;; esac
is_prefix "$prefix" || fail "$CREDS/replica-prefix is not a path in the bucket (segments of a-z, 0-9 and '-')"
[ -z "$identity_prefix" ] || is_prefix "$identity_prefix" || fail "$CREDS/replica-identity-prefix is not a path in the bucket (segments of a-z, 0-9 and '-')"
[ -z "$blob_prefix" ] || is_prefix "$blob_prefix" || fail "$CREDS/replica-blob-prefix is not a path in the bucket (segments of a-z, 0-9 and '-')"
[ -z "$restore_from" ] || is_prefix "$restore_from" || fail "$CREDS/replica-restore-from is not a path in the bucket (segments of a-z, 0-9 and '-')"
[ "$identity_prefix" != "$prefix" ] || fail "$CREDS/replica-identity-prefix is the data's own replica-prefix; they must differ"
case "$restore_at" in *[!0-9TZ:.+-]*) fail "$CREDS/replica-restore-at is not a time (RFC 3339, as 2026-10-22T09:45:00Z)" ;; esac
case "$endpoint" in
  http://?*|https://?*) ;;
  *) fail "$CREDS/replica-endpoint is not an http:// or https:// URL" ;;
esac
case "$endpoint" in *[!A-Za-z0-9.:/_-]*) fail "$CREDS/replica-endpoint holds a character a URL here cannot" ;; esac

DB="$OPENCHIME_DATA_DIR/openchime.db"
# The identity's own database (ARCH-66b): the daemon keeps it in identity/
# beside the data and, when a place is given, it is replicated to its own.
IDENTITY_DB="$OPENCHIME_DATA_DIR/identity/identity.db"
mkdir -p "$OPENCHIME_DATA_DIR/identity"
chmod 700 "$OPENCHIME_DATA_DIR/identity"
# Litestream's configuration, written fresh at every boot from the files above
# and readable by root alone. The key pair is deliberately not in it: Litestream
# reads LITESTREAM_ACCESS_KEY_ID and LITESTREAM_SECRET_ACCESS_KEY from its own
# environment, which this script sets for the Litestream commands only, so a
# secret is never pasted into YAML, where a ':' or '#' in it would change what
# the file says.
CONFIG=/run/litestream.yml
mkdir -p /run
rm -f "$CONFIG"
replica() {
  # $1 the database, $2 its place in the bucket; the copy is db/ under it.
  cat <<EOF
  - path: $1
    replica:
      type: s3
      bucket: $bucket
      path: $2/db
      # Any endpoint makes Litestream address the bucket by path, which both
      # Tigris and a local S3 implementation accept.
      endpoint: $endpoint
      region: auto
      # How often changes leave the box: the recovery point a lost box falls
      # back to is at most this old (REQ-094). The daemon's own ack never waits
      # on it (ARCH-23).
      sync-interval: 60s
EOF
}
(
  umask 077
  {
    echo "# Written by /entrypoint.sh at boot from $CREDS (ARCH-3, ARCH-39)."
    echo "dbs:"
    replica "$DB" "$prefix"
    if [ -n "$identity_prefix" ]; then replica "$IDENTITY_DB" "$identity_prefix"; fi
    cat <<EOF

# A full copy a day, and every change between them, for 14 days: any point in
# that window can be restored. The same window the control plane keeps for its
# own database.
snapshot:
  interval: 24h
  retention: 336h
EOF
  } > "$CONFIG"
)
chmod 600 "$CONFIG"

# A restore read from another place than the box replicates to: a configuration
# of its own naming only the database there, used for the restore alone.
RESTORE_CONFIG="$CONFIG"
if [ -n "$restore_from" ]; then
  RESTORE_CONFIG=/run/litestream-restore.yml
  rm -f "$RESTORE_CONFIG"
  (
    umask 077
    { echo "dbs:"; replica "$DB" "$restore_from"; } > "$RESTORE_CONFIG"
  )
  chmod 600 "$RESTORE_CONFIG"
fi

# The base carries no CA bundle; Litestream verifies the bucket's HTTPS against
# the daemon's own roots, written out as PEM in the image (ARCH-10).
export SSL_CERT_FILE=/usr/share/openchime/ca-roots.pem

# A box that finds no identity or no database -- a new volume, or the host it
# was on lost -- takes the copy before the daemon opens anything. A bucket
# holding no copy is the very first boot and not an error. Any other failure
# stops the boot: a daemon started empty here would mint a new identity and
# enrollment, and a managed box's one-time enrollment ticket cannot claim it a
# second time, so the workspace would be lost rather than recovered. What
# exists is left alone. The identity is always the latest copy; the data is
# the copy as of replica-restore-at when that is given. A restore that was
# asked for -- a time, or a place to read from -- is required: with no copy
# there, starting empty would put an empty workspace where a restored one was
# wanted, so it stops the boot like any other failure.
restore() {
  # $1 the database, $2 the configuration it is restored by, $3 the token's
  # name, $4 what it is, $5 "required" or "if-any"; then extra flags.
  db=$1 config=$2 token=$3 what=$4 need=$5
  shift 5
  [ ! -e "$db" ] || return 0
  echo "entrypoint: no $what at $db; restoring it from the replica, if a copy exists"
  if_any=-if-replica-exists
  [ "$need" != required ] || if_any=
  # $if_any is one flag or none, so it is left unquoted on purpose.
  # shellcheck disable=SC2086
  if ! LITESTREAM_ACCESS_KEY_ID="$access_key" LITESTREAM_SECRET_ACCESS_KEY="$secret_key" \
      /usr/local/bin/litestream restore -config "$config" -if-db-not-exists $if_any \
      -integrity-check full "$@" "$db" || { [ "$need" = required ] && [ ! -e "$db" ]; }; then
    echo "entrypoint: $token=failed" >&2
    echo "entrypoint: the restore of the $what from the replica failed, so the daemon is not started: a daemon started without it would replace this workspace's identity and enrollment rather than recover them, and its enrollment ticket cannot be used twice. Fix the bucket, the key, the endpoint or the restore time and start the box again." >&2
    exit 3
  fi
  if [ -e "$db" ]; then
    echo "entrypoint: $token=ok"
    echo "entrypoint: restored the $what at $db from the replica"
  else
    echo "entrypoint: $token=empty"
    echo "entrypoint: the replica holds no copy of the $what yet; the daemon creates it"
  fi
}
if [ -n "$identity_prefix" ]; then restore "$IDENTITY_DB" "$CONFIG" identity-restore identity if-any; fi
if [ -n "$restore_at" ]; then
  restore "$DB" "$RESTORE_CONFIG" restore database required -timestamp "$restore_at"
elif [ -n "$restore_from" ]; then
  restore "$DB" "$RESTORE_CONFIG" restore database required
else
  restore "$DB" "$RESTORE_CONFIG" restore database if-any
fi
rm -f /run/litestream-restore.yml

# Attachments beside the replica (ARCH-70): the daemon's S3 store, under the
# blob prefix, with the same key. Its URL is written to a file of root's alone
# and named to the daemon by OPENCHIME_S3_FILE, so the key is not in the
# daemon's environment. The key and secret are percent-encoded for the URL.
S3_FILE=/run/openchime-s3
rm -f "$S3_FILE"
if [ -n "$blob_prefix" ]; then
  enc() { printf '%s' "$1" | sed -e 's/%/%25/g' -e 's|/|%2F|g' -e 's/+/%2B/g' -e 's/@/%40/g' -e 's/:/%3A/g'; }
  case "$endpoint" in
    https://*) scheme=s3; host=${endpoint#https://} ;;
    *)         scheme=http+s3; host=${endpoint#http://} ;;
  esac
  host=${host%%/*}
  (
    umask 077
    printf '%s://%s:%s@%s/%s/%s?region=auto\n' "$scheme" "$(enc "$access_key")" "$(enc "$secret_key")" \
      "$host" "$bucket" "$blob_prefix" > "$S3_FILE"
  )
  chmod 600 "$S3_FILE"
  export OPENCHIME_S3_FILE="$S3_FILE"
  echo "entrypoint: attachments=s3"
fi

# Litestream runs the daemon as its child and replicates beside it; it forwards
# the platform's stop signal to the daemon and makes a final sync once the
# daemon has exited. The key is exported only now, for Litestream, which this
# shell becomes; the daemon is given an environment without it: a process that
# serves the network should not hold a credential in its environment.
# (Litestream copies the key to the AWS_* names for its SDK, so those go too.)
# The attachments' copy of it is the file above.
echo "entrypoint: replica=on"
echo "entrypoint: starting openchimed, replicated to the bucket by Litestream"
export LITESTREAM_ACCESS_KEY_ID="$access_key" LITESTREAM_SECRET_ACCESS_KEY="$secret_key"
exec /usr/local/bin/litestream replicate -config "$CONFIG" -exec "/usr/bin/env -u LITESTREAM_ACCESS_KEY_ID -u LITESTREAM_SECRET_ACCESS_KEY -u AWS_ACCESS_KEY_ID -u AWS_SECRET_ACCESS_KEY /usr/local/bin/openchimed"
