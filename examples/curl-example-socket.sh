#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Minimal smoke test against nshmqtt's UNIX socket directly -- for a
# bare-metal/systemd install (see README's "Installation") running on the
# same host, or from inside the nshmqtt container itself
# (docker compose exec nshmqtt sh). This is nshmqtt's own primary
# interface (README's "Architecture"), but not what the docker-compose
# stack's NGINX actually talks to (it proxies over the compose network
# instead -- see examples/curl-example.sh for the runnable-as-is example
# against `docker compose up -d`). Same API, same operations, just three
# calls instead of the full walkthrough -- enough to prove this path
# works, not a repeat of everything curl-example.sh already covers.
#
# Defaults to the production socket path; override for local testing, e.g.
#   NSHMQTT_SOCKET=/tmp/nshmqtt/nshmqtt.sock ./examples/curl-example-socket.sh
#
# If the running instance has http_auth_tokens configured, set
# NSHMQTT_AUTH_TOKEN to one of them, same as curl-example.sh.
set -eu

SOCKET="${NSHMQTT_SOCKET:-/run/nshmqtt/nshmqtt.sock}"
AUTH_TOKEN="${NSHMQTT_AUTH_TOKEN:-}"

curl_nshmqtt()
{
    if [ -n "$AUTH_TOKEN" ]; then
        curl -sS -i --unix-socket "$SOCKET" -H "X-Mqtt-Api-Key: $AUTH_TOKEN" "$@"
    else
        curl -sS -i --unix-socket "$SOCKET" "$@"
    fi
}

echo "--- health ---"
curl -sS -i --unix-socket "$SOCKET" 'http://localhost/health'
echo
echo

echo "--- POST event ---"
curl_nshmqtt -X POST 'http://localhost/event/domino/server1/backup' \
    -H 'Content-Type: application/json' \
    -d '{"status":"completed","duration":127}'
echo
echo

echo "--- PUT metric ---"
curl_nshmqtt -X PUT 'http://localhost/metric/server1/load' -d '17.3'
echo
echo

echo "--- /metrics-state (note: server1_load is here, domino_server1_backup is not -- an event only"
echo "    publishes to MQTT, it never updates state on its own; see README's 'Events' section) ---"
curl_nshmqtt 'http://localhost/metrics-state'
echo
