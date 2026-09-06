#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Builds the Alpine nshmqtt image and extracts the compiled binary to disk
# -- see Dockerfile and docker/compile_alpine.sh for what the build itself
# does. Unlike nshgeoip's build.sh, the extracted binary is dynamically
# linked (against musl libc, libpaho-mqtt3c.so, libstdc++/libgcc) rather
# than fully static -- see the Dockerfile's own comment for why.
#
# Usage:
#   ./build.sh                    # build image, extract binary to ./nshmqtt
#   ./build.sh ./out/nshmqtt      # ...to a specific path instead
#
# IMAGE_TAG=myregistry/nshmqtt:1.0 ./build.sh   # override the image tag
set -eu

IMAGE_TAG="${IMAGE_TAG:-nshmqtt:latest}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="${1:-$SCRIPT_DIR/nshmqtt}"

# Belt and suspenders alongside .dockerignore's *.o/*.d exclusion: any
# .o left on disk from a host build (g++/glibc) would otherwise risk
# being copied into the Alpine/musl build context and reused by `make`
# (mtime-"up to date"), silently linking glibc-compiled object code into
# a musl binary -- a real ABI mismatch, not a cosmetic issue. Removing
# them here means the image build can never depend on .dockerignore
# alone to keep that from happening.
make -C "$SCRIPT_DIR" clean >/dev/null 2>&1 || true

docker build --no-cache --progress=plain -t "$IMAGE_TAG" "$SCRIPT_DIR"
echo "built image: $IMAGE_TAG"

CID="$(docker create "$IMAGE_TAG")"
docker cp "$CID:/nshmqtt" "$OUT"
docker rm "$CID" >/dev/null
echo "extracted binary to $OUT"
