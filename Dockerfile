# Small Alpine build of nshmqtt.
#
# Unlike nshgeoip's "FROM scratch" static build, this is a normal (dynamic)
# Alpine build: Alpine's paho-mqtt-c-dev package ships only shared
# libraries (libpaho-mqtt3c.so, no libpaho-mqtt3c.a) -- there is no static
# Paho package to link against the way nshgeoip statically links
# libmaxminddb-static. Building Paho itself from source to get a static
# .a was judged not worth the extra build-stage complexity for what it
# buys here; the runtime image below is still just Alpine + one small
# shared library, not a large image. Revisit if a fully static binary
# becomes a real requirement.
#
# The actual compile/link steps live in docker/compile_alpine.sh, which
# also links in docker/fortify_shim.cpp -- see that file's comment for why
# Alpine/musl needs it (glibc-style _FORTIFY_SOURCE wrapper symbols
# libstdc++ emits but musl doesn't provide, plus two more nshmqtt-specific
# ones nshgeoip's own build never needed).
ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION} AS build
RUN apk add --no-cache g++ make file paho-mqtt-c-dev curl-dev

WORKDIR /src

COPY Makefile ./
COPY src ./src
COPY docker ./docker
RUN ./docker/compile_alpine.sh

FROM alpine:${ALPINE_VERSION} AS runtime
# libstdc++/libgcc: nshmqtt is C++, dynamically linked against them (unlike
# nshgeoip's fully static binary, which needs neither at runtime).
#
# nshgeoip's own Dockerfile creates its runtime user's /run ownership by
# COPY-ing an empty, pre-chowned directory over it -- which only works
# because nshgeoip's runtime image is "FROM scratch", where /run doesn't
# exist yet and that COPY creates it fresh. This image isn't scratch (it
# needs Alpine's own libc/libstdc++), so /run already exists (root-owned,
# from the base image) by the time any COPY would target it -- COPY only
# sets ownership on what it actually copies in, not a pre-existing
# destination directory, so that trick would silently no-op here. mkdir +
# chown as root, before dropping to USER, is the direct equivalent.
RUN apk add --no-cache libstdc++ libgcc paho-mqtt-c libcurl \
    && addgroup -S nshmqtt \
    && adduser -S -G nshmqtt -H nshmqtt \
    && mkdir -p /run/nshmqtt /var/lib/nshmqtt \
    && chown nshmqtt:nshmqtt /run/nshmqtt /var/lib/nshmqtt

COPY --from=build /src/nshmqtt /nshmqtt

USER nshmqtt:nshmqtt

ENTRYPOINT ["/nshmqtt"]

HEALTHCHECK CMD ["/nshmqtt", "--health-check"]
