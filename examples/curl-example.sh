#!/bin/sh
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Manual smoke test against nshmqtt through NGINX -- the normal way to
# reach it (see README's "Architecture"), and what actually works
# out of the box against `docker compose up -d` in this project: the
# compose stack's nginx talks to nshmqtt over the internal compose
# network, not a shared UNIX socket, so this is the one that's runnable
# as-is against it. For the direct-socket case (bare-metal/systemd, or
# exec'ing into the nshmqtt container itself), see
# examples/curl-example-socket.sh instead -- same API, same operations,
# just addressed differently.
#
# Defaults to the compose stack's plain-HTTP port for the write API/
# health, and its separate dedicated metrics port (9100 -- the exporter
# convention, see README's "Docker") for /metrics and /metrics-state;
# override either independently, e.g.:
#   NSHMQTT_URL=https://localhost:8444 ./examples/curl-example.sh
# For HTTPS, this looks for tls/ca.crt (see README's "TLS") next to this
# script's project root and passes it as --cacert automatically; override
# with NSHMQTT_CACERT if it lives elsewhere. 9100 has no TLS listener yet
# -- NSHMQTT_METRICS_URL stays plain http:// unless that changes.
#
# If the running instance has http_auth_tokens configured, set
# NSHMQTT_AUTH_TOKEN to one of them so this script's requests are
# authorized:
#   NSHMQTT_AUTH_TOKEN=some-long-random-token ./examples/curl-example.sh
# Left unset, every call below runs exactly as it would against an
# instance with auth disabled (the default).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASE_URL="${NSHMQTT_URL:-http://localhost:8081}"
METRICS_URL="${NSHMQTT_METRICS_URL:-http://localhost:9100}"
AUTH_TOKEN="${NSHMQTT_AUTH_TOKEN:-}"
CACERT="${NSHMQTT_CACERT:-$SCRIPT_DIR/../tls/ca.crt}"

# The token goes in the X-Mqtt-Api-Key header on every call, never in the JSON
# body and never as a query parameter -- see README's "Authentication"
# for why: an event's body is published to MQTT byte-for-byte unchanged,
# so a token embedded there would get published straight onto the broker
# along with it. A helper function (not a shell alias) so every curl call
# below picks this up uniformly, including --cacert for an https:// URL,
# without repeating either conditional.
curl_nshmqtt()
{
    if [ -n "$AUTH_TOKEN" ]; then
        set -- -H "X-Mqtt-Api-Key: $AUTH_TOKEN" "$@"
    fi
    case "$BASE_URL" in
        https://*)
            curl -sS -i --cacert "$CACERT" "$@"
            ;;
        *)
            curl -sS -i "$@"
            ;;
    esac
}

echo "--- health (never needs a token, even when auth is configured) ---"
curl_nshmqtt "$BASE_URL/health"
echo
echo

echo "--- POST event, JSON body (payload published to MQTT unchanged) ---"
curl_nshmqtt -X POST "$BASE_URL/event/domino/server1/backup" \
    -H 'Content-Type: application/json' \
    -d '{"status":"completed","duration":127}'
echo
echo

echo "--- confirms the event above does NOT show up in /metrics-state (see README's 'Events' section --"
echo "    POST /event only publishes to MQTT, it never touches the current-state store on its own) ---"
if curl -sS "$METRICS_URL/metrics-state" | grep -q 'domino_server1_backup'; then
    echo "unexpected: found a domino_server1_backup series -- see README's 'Events' section"
else
    echo "confirmed absent, as expected"
fi
echo

echo "--- POST event, overriding qos and retain for this one publish (?qos=, ?retain=) ---"
echo "    body stays the payload -- these can only be query parameters, never body fields ---"
curl_nshmqtt -X POST "$BASE_URL/event/domino/server1/backup?qos=2&retain=true" \
    -H 'Content-Type: application/json' \
    -d '{"status":"completed","duration":127}'
echo
echo

echo "--- same override, as headers instead (X-Mqtt-Qos, X-Mqtt-Retain) -- either form works, and if"
echo "    both are somehow given on the same request, the header wins ---"
curl_nshmqtt -X POST "$BASE_URL/event/domino/server1/backup" \
    -H 'Content-Type: application/json' \
    -H 'X-Mqtt-Qos: 2' -H 'X-Mqtt-Retain: true' \
    -d '{"status":"completed","duration":127}'
echo
echo

echo "--- POST event, plain text body ---"
curl_nshmqtt -X POST "$BASE_URL/event/server1/status" \
    -H 'Content-Type: text/plain' \
    -d 'completed'
echo
echo

echo "--- GET simple event API (?value=...), with the same ?qos=/?retain= overrides ---"
curl_nshmqtt "$BASE_URL/event/server1/status?value=completed&qos=0&retain=false"
echo
echo

echo "--- invalid qos on an event (expect 400) ---"
curl_nshmqtt "$BASE_URL/event/server1/status?value=completed&qos=9"
echo
echo

echo "--- PUT metric, plain text value ---"
curl_nshmqtt -X PUT "$BASE_URL/metric/server1/load" -d '17.3'
echo
echo

echo "--- unlike an event, this one DOES show up in /metrics-state right away ---"
if curl -sS "$METRICS_URL/metrics-state" | grep -q 'server1_load 17.3'; then
    echo "confirmed present, as expected -- PUT /metric updates the current-state store directly"
else
    echo "unexpected: server1_load 17.3 not found -- see README's 'Current state / metrics' section"
fi
echo

echo "--- PUT metric, JSON body ---"
curl_nshmqtt -X PUT "$BASE_URL/metric/room/temperature" \
    -H 'Content-Type: application/json' \
    -d '{"value": 22.7}'
echo
echo

echo "--- GET simple metric API (?value=...) ---"
curl_nshmqtt "$BASE_URL/metric/kitchen/temperature?value=19.5"
echo
echo

echo "--- DELETE metric (removes state, clears retained MQTT message) ---"
curl_nshmqtt -X DELETE "$BASE_URL/metric/kitchen/temperature"
echo
echo

echo "--- DELETE again (expect 404 -- already gone) ---"
curl_nshmqtt -X DELETE "$BASE_URL/metric/kitchen/temperature"
echo
echo

echo "--- invalid metric value (expect 400) ---"
curl_nshmqtt -X PUT "$BASE_URL/metric/bad" -d 'not-a-number'
echo
echo

echo "--- unsupported method on /event (expect 405) ---"
curl_nshmqtt -X DELETE "$BASE_URL/event/x"
echo
echo

echo "--- /metrics on nginx's dedicated :9100 -- service-only, never needs a token ---"
curl -sS -i "$METRICS_URL/metrics"
echo
echo

echo "--- /metrics-state on :9100 -- actual current values -- needs a token if configured ---"
if [ -n "$AUTH_TOKEN" ]; then
    curl -sS -i -H "X-Mqtt-Api-Key: $AUTH_TOKEN" "$METRICS_URL/metrics-state"
else
    curl -sS -i "$METRICS_URL/metrics-state"
fi
echo

if [ -z "$AUTH_TOKEN" ]; then
    echo
    echo "--- if this instance has http_auth_tokens configured, a request with no"
    echo "    X-Mqtt-Api-Key at all gets 401 (uncomment to try against such an instance) ---"
    # curl -sS -i "$METRICS_URL/metrics-state"
fi
