#!/bin/bash
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0


# Clears all persisted current-state for the docker-compose stack: both
# nshmqtt's own state.json (the nshmqtt-state volume) AND mosquitto's
# retained messages (its own persistence file, mosquitto.db, in the
# mosquitto-data volume). Doesn't touch Prometheus's history or Grafana's
# dashboards -- each of those lives in its own separate volume, untouched
# here.
#
# Both stores have to be cleared together, not just nshmqtt's: nshmqtt
# publishes every metric write with the MQTT retain flag set (see
# README's "Current state / metrics"), and mosquitto persists retained
# messages independently of anything nshmqtt does. Clear only
# state.json, restart nshmqtt with subscribe_enabled=true (the default in
# this stack), and MQTT's own protocol immediately redelivers every
# still-retained message to nshmqtt's fresh subscription the moment it
# resubscribes to '#' -- repopulating exactly what was just cleared. This
# was found by testing this script against a real stack, not guessed.
#
# A restart is required for the same reason: GET /metrics-state reflects
# nshmqtt's in-memory current-state store, which only reloads from
# state.json at startup -- clearing files while the processes keep
# running would just get overwritten by whatever's still in memory on
# the next write. So this stops both nshmqtt and mosquitto (only those
# two -- nginx/prometheus/grafana keep running), clears both files, and
# restarts whichever ones were running to begin with. Safe to run
# whether the stack is up or down.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Found by label, not a hardcoded "<project>_<name>" volume name -- the
# project name prefix depends on the compose project name (normally the
# directory name), which isn't guaranteed to always be "nshmqtt" (a clone
# into a differently-named directory, or an explicit -p, would change
# it). The label is what Compose itself uses to track ownership, so it's
# stable regardless.
NSHMQTT_VOLUME="$(docker volume ls --filter label=com.docker.compose.volume=nshmqtt-state -q | head -1)"
MOSQUITTO_VOLUME="$(docker volume ls --filter label=com.docker.compose.volume=mosquitto-data -q | head -1)"

if [ -z "$NSHMQTT_VOLUME" ] || [ -z "$MOSQUITTO_VOLUME" ]; then
    echo "nshmqtt-state and/or mosquitto-data volume not found -- nothing to clear" >&2
    echo "(has 'docker compose up' ever been run here?)" >&2
    exit 1
fi

nshmqtt_was_running=0
if docker inspect -f '{{.State.Running}}' nshmqtt >/dev/null 2>&1; then
    nshmqtt_was_running=1
fi
mosquitto_was_running=0
if docker inspect -f '{{.State.Running}}' nshmqtt-mosquitto >/dev/null 2>&1; then
    mosquitto_was_running=1
fi

if [ "$nshmqtt_was_running" = "1" ]; then
    echo "stopping nshmqtt..."
    docker compose stop nshmqtt
fi
if [ "$mosquitto_was_running" = "1" ]; then
    echo "stopping mosquitto..."
    docker compose stop mosquitto
fi

echo "clearing nshmqtt's state.json..."
docker run --rm -v "${NSHMQTT_VOLUME}:/data" alpine:latest sh -c 'rm -f /data/state.json'

echo "clearing mosquitto's retained messages (mosquitto.db)..."
docker run --rm -v "${MOSQUITTO_VOLUME}:/data" alpine:latest sh -c 'rm -f /data/mosquitto.db'

# mosquitto first if either was running -- nshmqtt's own depends_on
# expects it reachable at startup, same order the stack normally starts
# in.
if [ "$mosquitto_was_running" = "1" ]; then
    echo "restarting mosquitto..."
    docker compose up -d mosquitto
fi
if [ "$nshmqtt_was_running" = "1" ]; then
    echo "restarting nshmqtt..."
    docker compose up -d nshmqtt
fi

echo "done -- current state and retained messages are both cleared"
