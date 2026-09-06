#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Builds nshmqtt on Alpine, linked against Alpine's shared libpaho-mqtt3c.so
# (see the Dockerfile's own comment for why this isn't a fully static
# build the way nshgeoip's is: Alpine has no static Paho package).
# Compiles docker/fortify_shim.cpp in too -- see that file's comment for
# why Alpine/musl needs it even for this ordinary dynamic build. Meant to
# run inside an Alpine container with g++, make, and paho-mqtt-c-dev
# already installed (see ../Dockerfile).
#
# Usage: docker/compile_alpine.sh [output-path]
# (default output path: ./nshmqtt, i.e. the project root)
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
OUT="${1:-$PROJECT_ROOT/nshmqtt}"

cd "$PROJECT_ROOT"

# -Os/-flto/-ffunction-sections/-fdata-sections + --gc-sections at link time
# (below): same size-over-speed tradeoff as nshgeoip's static build script,
# for the same reason -- nshmqtt's own workload is short synchronous
# request handling, not CPU-bound. -fno-exceptions/-fno-rtti are NOT used
# here (unlike nshgeoip's script): nshmqtt links against Paho, a C library
# with no exceptions of its own, but nshmqtt's own code has no throw/catch
# either, so this would likely be safe too -- left enabled for now simply
# because it hasn't been verified end-to-end the way nshgeoip's was before
# that flag was added there.
CXXFLAGS="-std=c++17 -Wall -Wextra -Os -flto -pthread -ffunction-sections -fdata-sections"

echo "==> compiling nshmqtt object files"
OBJS=""
for f in src/*.cpp; do
    OBJS="$OBJS ${f%.cpp}.o"
done

make $OBJS CXXFLAGS="$CXXFLAGS"

echo "==> compiling fortify shim"
g++ $CXXFLAGS -c "$SCRIPT_DIR/fortify_shim.cpp" -o "$SCRIPT_DIR/fortify_shim.o"

echo "==> linking -> $OUT"
# -s: strip the symbol table/relocation info at link time.
# -Wl,--gc-sections: the dead-code removal -ffunction-sections/
# -fdata-sections (above) made possible.
g++ $CXXFLAGS -s -Wl,--gc-sections -o "$OUT" \
    $OBJS \
    "$SCRIPT_DIR/fortify_shim.o" \
    -lpaho-mqtt3c -lcurl

echo "==> done: $OUT"
file "$OUT" 2>/dev/null || true
