#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Same operations as examples/mosquitto-example.sh, but run *inside* the
# mosquitto container via `docker compose exec` instead of a host-installed
# mosquitto_pub/mosquitto_sub -- nothing to install on the host at all,
# since the eclipse-mosquitto image already bundles both. Useful when the
# host running this script doesn't have (and shouldn't need) the
# mosquitto-clients package, e.g. a workstation that only has Docker. See
# README's "Mosquitto test commands" for the three ways to run these
# commands and when each applies.
#
# Requires: docker compose, and the compose stack already up
# (`docker compose up -d` from the project root). Run this script from the
# project root too, or set COMPOSE_PROJECT_DIR.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR="${COMPOSE_PROJECT_DIR:-$SCRIPT_DIR/..}"

mosquitto_exec()
{
    (cd "$PROJECT_DIR" && docker compose exec -T mosquitto "$@")
}

echo "=== PUSH: getting a value INTO nshmqtt's current-state store ==="
echo

echo "--- path 1 of 2: through nshmqtt's HTTP API (PUT /metric) ---"
curl -sS -X PUT "http://localhost:8081/metric/server1/load" -d '17.3'
echo
echo

echo "--- path 2 of 2: a native MQTT publish, with retain (-r) -- nshmqtt never sees an HTTP request"
echo "    at all here, picked up by its own subscription instead (see README's 'MQTT subscriptions') ---"
mosquitto_exec mosquitto_pub -t 'sensors/kitchen/temperature' -m '21.4' -r
echo

echo "--- confirms nshmqtt's own subscription picked up that native publish ---"
if curl -sS "http://localhost:9100/metrics-state" | grep -q 'sensors_kitchen_temperature 21.4'; then
    echo "confirmed present, as expected"
else
    echo "unexpected: sensors_kitchen_temperature 21.4 not found -- see README's 'MQTT subscriptions' section"
fi
echo

echo "=== CONSUME: reading a value back OUT with a native MQTT client ==="
echo

echo "--- getting the CURRENT value of one topic, without watching for future updates ---"
echo "    same '-C 1' one-shot trick as examples/mosquitto-example.sh, run inside the container ---"
mosquitto_exec mosquitto_sub -t 'server1/load' -C 1 -v
echo

echo "--- same, but for every current value nshmqtt has published (exits once quiet for 2s) ---"
mosquitto_exec mosquitto_sub -t '#' -v -W 2
echo

echo "--- watching everything LIVE, retained and new alike, until interrupted (Ctrl-C) ---"
echo "    (commented out by default so this script still exits on its own -- uncomment to run it)"
# mosquitto_exec mosquitto_sub -t '#' -v
