#!/usr/bin/env bash
# scripts/smoke/image.sh
#
# The released image, pulled by its version tag from the build repository and
# run: a tag that did not land, or an image that does not start on this base,
# is found here rather than by an operator.
#
# Needs: podman; BUILD_IMAGE, VERSION, IMAGE_REGISTRY_USER,
# IMAGE_REGISTRY_TOKEN.
# Run by: scripts/smoketest.sh.
. "$(dirname "$0")/../test/lib/common.sh"
need podman "podman"
: "${BUILD_IMAGE:?BUILD_IMAGE is the image repository}" "${VERSION:?VERSION is the release number}"
: "${IMAGE_REGISTRY_USER:?IMAGE_REGISTRY_USER is the registry login}"
[ -n "${IMAGE_REGISTRY_TOKEN:-}" ] || fail "IMAGE_REGISTRY_TOKEN is not set"
printf '%s' "$IMAGE_REGISTRY_TOKEN" | podman login -u "$IMAGE_REGISTRY_USER" --password-stdin "${BUILD_IMAGE%%/*}"
podman pull "${BUILD_IMAGE}:${VERSION}"
podman run --rm "${BUILD_IMAGE}:${VERSION}" --version
ok "${BUILD_IMAGE}:${VERSION} pulls and runs"
