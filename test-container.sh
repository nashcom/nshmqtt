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
#
# NSHMQTT_TEST_FORCE_FAIL=1 ./test-container.sh   # deliberately fail one
# unit test (see tests/test_nshmqtt.cpp's own comment on it) -- for
# re-verifying, whenever you want to, that a real failure actually stops
# the run and comes back out as a non-zero exit code here, instead of
# trusting that by reasoning alone.
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
# curl/mosquitto/mosquitto-clients/python3/bash: what integration_test.sh
# itself needs (every HTTP check, its own private broker, mosquitto_pub/
# sub, the webhook mock, and the script's own shell) -- none of these
# are in the runtime image either, it's deliberately minimal -- see that
# script's own comment.
# `sh -c` below runs in its own fresh shell process (a new container),
# not a subshell of this script -- it does NOT inherit the `set -eu`
# above at all, so it needs its own `set -e` to stop on the first
# failure. Without it, a failing `make test` would still fall through to
# running the integration test anyway (they're separate statements, not
# `&&`-chained to the whole rest of the script), and the exit code CI
# actually sees would end up reflecting only whichever command ran last
# -- silently losing a real unit-test failure whenever the integration
# test still happened to pass on its own. `chown` is the one exception,
# explicitly allowed to fail (`|| true`) since it's just a host-ownership
# nicety, not a real test result.
#
# `rm -f tests/test_nshmqtt` before `make test`: `-v "$SCRIPT_DIR:/src"`
# bind-mounts the live host directory, shared with whatever native
# `make`/`make test` runs happen there too -- Make's own up-to-date
# check (mtime-based) can't be trusted against that: it's seen a stale
# `tests/test_nshmqtt` as already current and skipped recompiling it
# entirely, then failed trying to run a binary that (from this
# container's view, at that moment) didn't exist. Removing it first
# forces a real rebuild every time, which is what a throwaway test
# container should be doing anyway. Deliberately not `make clean` --
# that also removes the top-level `nshmqtt` binary, which, over this
# same bind mount, would delete the *host's* own native build too.
docker run --rm --user root -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
    -e NSHMQTT_TEST_FORCE_FAIL \
    -v "$SCRIPT_DIR:/src" -w /src --entrypoint sh "$IMAGE_TAG" -c '
    set -e
    apk add --no-cache bash curl g++ make mosquitto mosquitto-clients python3
    rm -f tests/test_nshmqtt
    make test
    chown "$HOST_UID:$HOST_GID" tests/test_nshmqtt 2>/dev/null || true
    NSHMQTT_BIN=/nshmqtt bash tests/integration_test.sh
'
