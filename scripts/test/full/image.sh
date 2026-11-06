#!/usr/bin/env bash
# scripts/test/full/image.sh <image>
#
# The image starts and answers /healthz: its entrypoint, its data directory,
# the binary on this base and the data beside it hold together. The plain
# start, with no bucket and so no replicator (db_restore.sh is the replicated
# one). An image for another architecture is not started.
#
# Needs: podman, curl; <image> in podman's store; port 18080 free.
# Run by: make test-all (PART=image).
. "$(dirname "$0")/../lib/common.sh"
need podman "podman"; need curl "curl"
image=${1:?usage: image.sh <image>}
if ! image_runnable "$image"; then echo "$TEST: $image is not for this machine ($(uname -m)); not started"; exit 0; fi
podman run -d --name oc-image-check -p 127.0.0.1:18080:8080 -e OPENCHIME_WORKSPACE_NAME=Image "$image" >/dev/null
trap 'podman rm -f oc-image-check >/dev/null 2>&1 || true' EXIT
if ! wait_healthz http://127.0.0.1:18080/healthz 60; then
    podman logs oc-image-check 2>&1 | tail -30 >&2
    fail "the image did not answer /healthz within 60 s"
fi
ok "$image starts and answers /healthz"
