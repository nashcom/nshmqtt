#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Builds the Alpine nshmqtt image (same as build.sh) and runs the unit and
# integration test suites *inside* a container from it, against the actual
# shipped binary -- Alpine/musl, the real libpaho-mqtt-c/libcurl versions
# that ship, not a separately-built native binary on the host that could
# use a different libc/library version than what actually ships. This is
# what CI runs (see .github/workflows/ci.yml); run it the same way locally
# to reproduce a CI failure without needing g++/libpaho-mqtt-dev/
# libcurl4-openssl-dev installed on your own machine at all -- only Docker.
#
# Usage:
#   ./test-container.sh
#
# IMAGE_TAG=myregistry/nshmqtt:1.0 ./test-container.sh   # override the image tag
set -eu

IMAGE_TAG="${IMAGE_TAG:-nshmqtt:latest}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

docker build -t "$IMAGE_TAG" "$SCRIPT_DIR"
echo "built image: $IMAGE_TAG"

# --user root: `apk add` needs it, and dropping back to the image's own
# unprivileged user just for two test commands isn't worth the extra
# complexity in a throwaway test container -- the shipped image itself
# (what actually gets pushed/deployed) is untouched, this only affects
# this one ephemeral container.
#
# g++/make: for tests/test_nshmqtt's own compile step (`make test`) --
# the runtime image ships only the already-built nshmqtt binary, not a
# toolchain, and that unit-test binary doesn't link against Paho/curl at
# all (see the Makefile), so no *-dev packages are needed for it.
# mosquitto/mosquitto-clients/python3/bash: what integration_test.sh
# itself needs (its own private broker, mosquitto_pub/sub, the webhook
# mock, and the script's own shell) -- see that script's own comment.
docker run --rm --user root -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
    -v "$SCRIPT_DIR:/src" -w /src --entrypoint sh "$IMAGE_TAG" -c '
    apk add --no-cache bash g++ make mosquitto mosquitto-clients python3 &&
    make test &&
    chown "$HOST_UID:$HOST_GID" tests/test_nshmqtt 2>/dev/null
    NSHMQTT_BIN=/nshmqtt bash tests/integration_test.sh
'
