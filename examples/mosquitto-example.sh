#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Both directions of the HTTP <-> MQTT boundary, with a native MQTT
# client -- the counterpart to examples/curl-example.sh, which only ever
# talks to nshmqtt's HTTP API. There are two independent paths into
# nshmqtt's current-state store (see README's "MQTT subscriptions" for
# the full picture): this script exercises both, and both directions of
# reading a value back out too.
#
# Requires: mosquitto_pub, mosquitto_sub (the mosquitto-clients package on
# most distros).
#
# Defaults to the docker-compose stack's plain MQTT port through NGINX's
# stream{} passthrough (see README's "Architecture"); override for a
# bare-metal/systemd mosquitto instead, e.g.:
#   NSHMQTT_MQTT_HOST=127.0.0.1 NSHMQTT_MQTT_PORT=1883 ./examples/mosquitto-example.sh
# For MQTT/TLS (8883, once ./gen-cert.sh has populated tls/), also set:
#   NSHMQTT_MQTT_CAFILE=tls/ca.crt
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MQTT_HOST="${NSHMQTT_MQTT_HOST:-localhost}"
MQTT_PORT="${NSHMQTT_MQTT_PORT:-1883}"
CAFILE="${NSHMQTT_MQTT_CAFILE:-}"

mosquitto_pub_nshmqtt()
{
    if [ -n "$CAFILE" ]; then
        mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" --cafile "$CAFILE" "$@"
    else
        mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" "$@"
    fi
}

mosquitto_sub_nshmqtt()
{
    if [ -n "$CAFILE" ]; then
        mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" --cafile "$CAFILE" "$@"
    else
        mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" "$@"
    fi
}

echo "=== PUSH: getting a value INTO nshmqtt's current-state store ==="
echo

echo "--- path 1 of 2: through nshmqtt's HTTP API (PUT /metric) ---"
curl -sS -X PUT "http://localhost:8081/metric/server1/load" -d '17.3'
echo
echo

echo "--- path 2 of 2: a native MQTT publish, with retain (-r) -- nshmqtt never sees an HTTP request"
echo "    at all here. Picked up by nshmqtt's own subscription (subscribe_enabled=true,"
echo "    subscribe_topics=# in this stack) -- a sensor, an IoT device, or any other service already"
echo "    on the broker can push a value this way, with no code calling nshmqtt directly. ---"
mosquitto_pub_nshmqtt -t 'sensors/kitchen/temperature' -m '21.4' -r
echo

echo "--- confirms nshmqtt's own subscription picked up that native publish -- it shows up in"
echo "    /metrics-state within one publish, no restart, nothing needing to know about it ahead of"
echo "    time (see README's 'MQTT subscriptions') ---"
if curl -sS "http://localhost:9100/metrics-state" | grep -q 'sensors_kitchen_temperature 21.4'; then
    echo "confirmed present, as expected"
else
    echo "unexpected: sensors_kitchen_temperature 21.4 not found -- see README's 'MQTT subscriptions' section"
fi
echo

echo "=== CONSUME: reading a value back OUT with a native MQTT client ==="
echo

echo "--- getting the CURRENT value of one topic, without watching for future updates ---"
echo "    '-C 1' exits after exactly one message. Because the value above was published retained, the"
echo "    broker delivers the last known value immediately on subscribe -- this is the native-MQTT"
echo "    equivalent of GET /metrics-state for a single series, no HTTP round trip needed. ---"
mosquitto_sub_nshmqtt -t 'server1/load' -C 1 -v
echo

echo "--- same, but for every current value (retained messages on every topic anyone has published"
echo "    come back immediately, one per topic, then the command exits on its own once no more arrive"
echo "    within a couple of seconds -- there's no server-side signal for 'that's all of them', so a"
echo "    fixed count isn't the right tool here the way -C 1 was above) ---"
mosquitto_sub_nshmqtt -t '#' -v -W 2
echo

echo "--- watching everything LIVE, retained and new alike, until interrupted (Ctrl-C) --"
echo "    this is the one to leave running in a second terminal while you exercise the HTTP API,"
echo "    or while a native MQTT device publishes directly ---"
echo "    (commented out by default so this script still exits on its own -- uncomment to run it)"
# mosquitto_sub_nshmqtt -t '#' -v
