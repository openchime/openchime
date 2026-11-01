#!/usr/bin/env bash
# scripts/test/release/db_restore.sh <image>
#
# The replicated start (ARCH-3, ARCH-39): the image given its replica-* files
# streams its database off the box, a box that finds no database takes it
# back, and one that cannot read the bucket does not start. The bucket is a
# directory served as S3 by rclone -- a pinned release checked against its
# published digest, a process rather than a container (ARCH-38) -- so what
# was replicated is read straight off disk. The containers share the host's
# network to reach it on loopback.
#
#   A  the replica-* files as a deployment writes them under /etc/openchime,
#      an empty volume and an empty bucket: it starts, its database appears in
#      the bucket under <prefix>/db/ and its identity under <identity
#      prefix>/db/.
#   B  the same files, a new empty volume: it restores both before the daemon
#      starts, and presents the same certificate as A -- the identity
#      (ARCH-66b) came back.
#   D  the same identity prefix with a data prefix never used, a new empty
#      volume: the data begins again and the identity is restored, so it
#      presents A's certificate on an empty database.
#   E  the same files with a restore time before any copy: the restore finds
#      nothing, and the boot stops (exit 3) rather than starting empty.
#   C  the same files with a wrong secret, a new empty volume: the restore
#      fails, the boot exits non-zero, and no database or identity is created.
#   F  the files without replica-identity-prefix: the boot stops as
#      misconfigured (exit 1), since the data would be replicated and the
#      identity not; and the files without replica-endpoint, which has no
#      default: misconfigured too.
#   G  A's volume with its schema version raised past this daemon's: the boot
#      stops with the refusal's own status (exit 4) and token, before
#      Litestream starts.
#
# An image for another architecture is not started.
#
# Needs: podman, curl, unzip, python3; <image> in podman's store; ports 18081,
# 18443 and 19000 free; network, to fetch the pinned rclone.
# Run by: make test-release (PART=image).
. "$(dirname "$0")/../lib/common.sh"
need podman "podman"; need curl "curl"; need unzip "unzip"; need python3 "python3"
IMAGE=${1:?usage: db_restore.sh <image>}
if ! image_runnable "$IMAGE"; then echo "$TEST: $IMAGE is not for this machine ($(uname -m)); not started"; exit 0; fi
RCLONE=v1.75.2
RCLONE_SHA256=349ac8fba6ff65d6247043f1750cdcb518ec5d500ef91463a10d37c0ccdf3702
W=$(mktemp -d)
BUCKET=oc-replica
PREFIX=p/data/1
IDPREFIX=p/identity/1
mkdir -p "$W/s3/$BUCKET" "$W/creds" "$W/creds-c" "$W/creds-d"

curl -fsSL --retry 3 -o "$W/rclone.zip" \
  "https://github.com/rclone/rclone/releases/download/${RCLONE}/rclone-${RCLONE}-linux-amd64.zip"
echo "${RCLONE_SHA256}  $W/rclone.zip" | sha256sum -c -
unzip -q -j "$W/rclone.zip" "rclone-${RCLONE}-linux-amd64/rclone" -d "$W"
# Buckets are the directories under s3/; the one key pair is the only
# one accepted, so a wrong secret is refused as a real provider would.
"$W/rclone" serve s3 --addr 127.0.0.1:19000 --auth-key ci-key,ci-secret "$W/s3" > "$W/rclone.log" 2>&1 &
RCLONE_PID=$!
trap 'podman rm -f oc-a oc-b oc-d >/dev/null 2>&1 || true; kill "$RCLONE_PID" 2>/dev/null || true; podman unshare rm -rf "$W" 2>/dev/null || rm -rf "$W"' EXIT
for i in $(seq 1 30); do
  curl -s -o /dev/null http://127.0.0.1:19000/ && break
  sleep 1
done

# One box: host network, its own data directory, ports clear of
# image.sh's. Arguments after the data directory go to podman.
start() {
  local name="$1" data="$2"; shift 2
  mkdir -p "$data"
  podman run -d --name "$name" --network host -v "$data:/data" \
    -e OPENCHIME_WORKSPACE_NAME=Image -e OPENCHIME_HEALTH_PORT=18081 -e OPENCHIME_PORT=18443 \
    "$@" "$IMAGE" >/dev/null
}
healthz() {
  wait_healthz http://127.0.0.1:18081/healthz 60 && return 0
  echo "--- $1's log"; podman logs "$1" 2>&1 | tail -40
  error "$1 did not answer /healthz within 60 s"; return 1
}
# Only what lands under the two prefixes counts, and anything outside
# them is an error: each database is db/ under its own prefix.
replicas() { find "$W/s3/$BUCKET/${1:-$PREFIX}/db" -type f -name '*.ltx' 2>/dev/null | wc -l; }
stray() { find "$W/s3/$BUCKET" -type f ! -path "$W/s3/$BUCKET/$PREFIX/db/*" ! -path "$W/s3/$BUCKET/$IDPREFIX/db/*" | head -5; }
# The deployment's files, 0600 as a deployment writes them.
creds() {
  local dir="$1" secret="$2" data="${3:-$PREFIX}"
  printf '%s' "$BUCKET" > "$dir/replica-bucket"
  printf '%s' "$data" > "$dir/replica-prefix"
  printf '%s' "$IDPREFIX" > "$dir/replica-identity-prefix"
  printf '%s' ci-key > "$dir/replica-accesskey"
  printf '%s' "$secret" > "$dir/replica-secretkey"
  printf '%s' http://127.0.0.1:19000 > "$dir/replica-endpoint"
  chmod 600 "$dir"/replica-*
}
creds "$W/creds" ci-secret
creds "$W/creds-c" not-the-secret
creds "$W/creds-d" ci-secret p/data/2

# A: the files, an empty volume, an empty bucket.
start oc-a "$W/data-a" -v "$W/creds:/etc/openchime:ro"
healthz oc-a
# The log goes to a file first: under pipefail, grep -q stops reading at
# its first match and `podman logs` then fails on the closed pipe.
podman logs oc-a > "$W/a-boot.log" 2>&1
# The entrypoint's stable tokens (entrypoint.sh), never its sentences.
grep -qxF 'entrypoint: restore=empty' "$W/a-boot.log" \
  || { tail -40 "$W/a-boot.log"; error "A did not report an empty replica on its first boot"; exit 1; }
grep -qxF 'entrypoint: replica=on' "$W/a-boot.log" \
  || { tail -40 "$W/a-boot.log"; error "A did not start replicating"; exit 1; }
# Litestream syncs every 60 s (the configuration entrypoint.sh writes).
ok=0
for i in $(seq 1 150); do
  if [ "$(replicas)" -gt 0 ] && [ "$(replicas "$IDPREFIX")" -gt 0 ]; then echo "A: replicated after ${i} s"; ok=1; break; fi
  sleep 1
done
[ "$ok" = 1 ] || { podman logs oc-a 2>&1 | tail -40; tail -20 "$W/rclone.log"; find "$W/s3" -type f | head -20; error "nothing reached $PREFIX/db/ and $IDPREFIX/db/ in the bucket within 150 s"; exit 1; }
# A stop is SIGTERM to Litestream, which passes it to the daemon and
# syncs once more after the daemon has exited.
podman stop -t 30 oc-a >/dev/null
echo "A: exited $(podman inspect -f '{{.State.ExitCode}}' oc-a) on SIGTERM; $(replicas) LTX files in the bucket"
echo "--- A's log"; podman logs oc-a 2>&1 | tail -20
podman rm oc-a >/dev/null
[ -s "$W/data-a/cert.pem" ] || { error "A left no cert.pem"; exit 1; }
[ -z "$(stray)" ] || { stray; error "A wrote to the bucket outside $PREFIX/db/ and $IDPREFIX/db/"; exit 1; }

# B: the same files, onto an empty volume.
start oc-b "$W/data-b" -v "$W/creds:/etc/openchime:ro"
healthz oc-b
podman logs oc-b > "$W/b-boot.log" 2>&1
grep -qxF 'entrypoint: restore=ok' "$W/b-boot.log" \
  || { tail -40 "$W/b-boot.log"; error "B did not restore the database"; exit 1; }
grep -qxF 'entrypoint: identity-restore=ok' "$W/b-boot.log" \
  || { tail -40 "$W/b-boot.log"; error "B did not restore the identity"; exit 1; }
cmp -s "$W/data-a/cert.pem" "$W/data-b/cert.pem" \
  || { error "B presents a different certificate from A: the restored database did not carry its identity"; exit 1; }
echo "B: restored, and presents A's certificate"
podman rm -f oc-b >/dev/null

# D: the identity kept, the data begun again in a new place.
start oc-d "$W/data-d" -v "$W/creds-d:/etc/openchime:ro"
healthz oc-d
podman logs oc-d > "$W/d-boot.log" 2>&1
grep -qxF 'entrypoint: identity-restore=ok' "$W/d-boot.log" \
  || { tail -40 "$W/d-boot.log"; error "D did not restore the identity"; exit 1; }
grep -qxF 'entrypoint: restore=empty' "$W/d-boot.log" \
  || { tail -40 "$W/d-boot.log"; error "D did not begin its data again"; exit 1; }
cmp -s "$W/data-a/cert.pem" "$W/data-d/cert.pem" \
  || { error "D presents a different certificate from A: the kept identity was not used"; exit 1; }
echo "D: data begun again, and presents A's certificate"
podman rm -f oc-d >/dev/null

# E: a restore asked for -- to a time before any copy -- finds none.
# It must stop the boot, not start the workspace empty.
mkdir -p "$W/creds-e" "$W/data-e"
creds "$W/creds-e" ci-secret
printf '%s' 2000-01-01T00:00:00Z > "$W/creds-e/replica-restore-at"
chmod 600 "$W/creds-e"/replica-*
rc=0
podman run --rm --name oc-e --network host -v "$W/data-e:/data" \
  -e OPENCHIME_WORKSPACE_NAME=Image -e OPENCHIME_HEALTH_PORT=18081 -e OPENCHIME_PORT=18443 \
  -v "$W/creds-e:/etc/openchime:ro" \
  "$IMAGE" > "$W/e.log" 2>&1 || rc=$?
echo "--- E's log"; tail -12 "$W/e.log"
[ "$rc" -eq 3 ] && grep -qxF 'entrypoint: restore=failed' "$W/e.log" \
  || { error "E (a restore to a time with no copy) exited ${rc}, not 3 at the restore"; exit 1; }
[ ! -e "$W/data-e/openchime.db" ] || { error "E started a database after its asked-for restore found nothing"; exit 1; }
echo "E: a restore with nothing to restore stopped the boot"

# C: the same files with a wrong secret. The boot must fail, before
# the daemon starts.
rc=0
mkdir -p "$W/data-c"
podman run --rm --name oc-c --network host -v "$W/data-c:/data" \
  -e OPENCHIME_WORKSPACE_NAME=Image -e OPENCHIME_HEALTH_PORT=18081 -e OPENCHIME_PORT=18443 \
  -v "$W/creds-c:/etc/openchime:ro" \
  "$IMAGE" > "$W/c.log" 2>&1 || rc=$?
echo "--- C's log"; tail -20 "$W/c.log"
[ "$rc" -ne 0 ] || { error "a boot that could not read its bucket exited 0"; exit 1; }
# podman run passes the entrypoint's status through: 3 is the restore's.
# The identity is restored first, so that is where it stops.
[ "$rc" -eq 3 ] && grep -qxF 'entrypoint: identity-restore=failed' "$W/c.log" \
  || { error "C failed (exit ${rc}), but not at the restore"; exit 1; }
[ ! -e "$W/data-c/openchime.db" ] && [ ! -e "$W/data-c/identity/identity.db" ] \
  || { error "C created a database after its restore failed"; exit 1; }
echo "C: exited ${rc} at the restore, with no database created"

# F: replication asked for with no place for the identity, and with
# no endpoint.
for missing in replica-identity-prefix replica-endpoint; do
  rm -rf "$W/creds-f" "$W/data-f"
  mkdir -p "$W/creds-f" "$W/data-f"
  creds "$W/creds-f" ci-secret
  rm "$W/creds-f/$missing"
  rc=0
  podman run --rm --name oc-f --network host -v "$W/data-f:/data" \
    -e OPENCHIME_WORKSPACE_NAME=Image -e OPENCHIME_HEALTH_PORT=18081 -e OPENCHIME_PORT=18443 \
    -v "$W/creds-f:/etc/openchime:ro" \
    "$IMAGE" > "$W/f.log" 2>&1 || rc=$?
  echo "--- F's log, without $missing"; tail -6 "$W/f.log"
  [ "$rc" -eq 1 ] && grep -qxF 'entrypoint: replica=misconfigured' "$W/f.log" \
    || { error "F (replication without $missing) exited ${rc}, not 1 as misconfigured"; exit 1; }
  [ ! -e "$W/data-f/openchime.db" ] || { error "F started a database without $missing"; exit 1; }
  echo "F: replication without $missing refused"
done

# G: data a newer daemon wrote. A's volume, copied, its schema
# version raised past the newest this daemon has.
cp -a "$W/data-a" "$W/data-g"
python3 -I -c 'import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute("INSERT INTO schema_version(version) VALUES((SELECT MAX(version) FROM schema_version) + 1000)")
c.commit()' "$W/data-g/openchime.db"
rc=0
podman run --rm --name oc-g --network host -v "$W/data-g:/data" \
  -e OPENCHIME_WORKSPACE_NAME=Image -e OPENCHIME_HEALTH_PORT=18081 -e OPENCHIME_PORT=18443 \
  -v "$W/creds:/etc/openchime:ro" \
  "$IMAGE" > "$W/g.log" 2>&1 || rc=$?
echo "--- G's log"; tail -6 "$W/g.log"
[ "$rc" -eq 4 ] && grep -qxF 'entrypoint: data=too-new' "$W/g.log" \
  || { error "G (data a newer daemon wrote) exited ${rc}, not 4 with its token"; exit 1; }
! grep -qF 'entrypoint: replica=on' "$W/g.log" \
  || { error "G started replicating data it refused"; exit 1; }
echo "G: newer data refused with exit 4, before replication"
ok "the database and identity replicate and restore, and every refusal holds"
