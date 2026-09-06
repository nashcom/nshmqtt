#!/bin/bash
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Process/protocol-level integration tests for nshmqtt, run against a real
# running daemon over its actual UNIX socket, and a real Mosquitto broker.
# Complements the pure-function unit tests in test_nshmqtt.cpp.
#
# Configuration (all optional):
#   NSHMQTT_BIN     path to the nshmqtt binary (default: ./nshmqtt)
#   MOSQUITTO_BIN   path to mosquitto (default: mosquitto, found via PATH)
#   MOSQUITTO_PORT  port for the test broker (default: 18830, not 1883, so
#                   this never collides with a real broker already running)
#   WEBHOOK_PORT    port for the mock webhook HTTP receiver used by the
#                   webhook-forwarding tests (default: 18832)
#
# If mosquitto/mosquitto_sub aren't found, MQTT-dependent tests are skipped
# (SKIP, not FAIL) but all protocol-level HTTP tests that don't need a
# working broker (status codes, malformed requests, /health, /metrics
# shape) still run. Webhook-forwarding tests need both mosquitto (to
# publish the test messages) and python3 (for a minimal mock HTTP
# receiver) -- skipped if either is missing.
#
# Every "poll until ready" loop below (process startup, MQTT round-trip
# settling, webhook delivery) uses the same `seq 1 100` / `sleep 0.1`
# shape -- a 10s ceiling, deliberately generous. The loop always breaks
# the instant its condition is met, so this costs nothing on a normal
# run; it only matters on a slower/busier machine (a shared CI runner,
# say), where too tight a ceiling reads whatever's there yet -- possibly
# nothing -- instead of actually waiting for it.

set -u

TESTS_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$TESTS_DIR/.." && pwd)"

NSHMQTT_BIN="${NSHMQTT_BIN:-$PROJECT_ROOT/nshmqtt}"
MOSQUITTO_BIN="${MOSQUITTO_BIN:-mosquitto}"
MOSQUITTO_PORT="${MOSQUITTO_PORT:-18830}"
WEBHOOK_PORT="${WEBHOOK_PORT:-18832}" # a mock HTTP receiver for the webhook-forwarding tests, not a real broker port

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "SKIP: $1"; }

WORKDIR="$(mktemp -d /tmp/nshmqtt_test.XXXXXX)"
SOCK="$WORKDIR/nshmqtt.sock"
CONF="$WORKDIR/nshmqtt.conf"
STATE_FILE="$WORKDIR/state.json"
NSHMQTT_PID=""
MOSQUITTO_PID=""
WEBHOOK_MOCK_PID=""

cleanup() {
    if [ -n "$NSHMQTT_PID" ] && kill -0 "$NSHMQTT_PID" 2>/dev/null; then
        kill -TERM "$NSHMQTT_PID" 2>/dev/null
        wait "$NSHMQTT_PID" 2>/dev/null
    fi
    if [ -n "$MOSQUITTO_PID" ] && kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
        kill -TERM "$MOSQUITTO_PID" 2>/dev/null
        wait "$MOSQUITTO_PID" 2>/dev/null
    fi
    if [ -n "$WEBHOOK_MOCK_PID" ] && kill -0 "$WEBHOOK_MOCK_PID" 2>/dev/null; then
        kill -TERM "$WEBHOOK_MOCK_PID" 2>/dev/null
        wait "$WEBHOOK_MOCK_PID" 2>/dev/null
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required to run this test suite" >&2
    exit 1
fi
if [ ! -x "$NSHMQTT_BIN" ]; then
    echo "nshmqtt binary not found/executable: $NSHMQTT_BIN (build it first: make)" >&2
    exit 1
fi

HAVE_MOSQUITTO=1
command -v "$MOSQUITTO_BIN" >/dev/null 2>&1 || HAVE_MOSQUITTO=0
command -v mosquitto_sub >/dev/null 2>&1 || HAVE_MOSQUITTO=0

HAVE_PYTHON3=1
command -v python3 >/dev/null 2>&1 || HAVE_PYTHON3=0

http_status() {
    curl -sS -o /dev/null -w '%{http_code}' --unix-socket "$SOCK" "$1" \
        ${2:+-X "$2"} ${3:+-d "$3"}
}

start_broker() {
    cat > "$WORKDIR/mosquitto.conf" <<EOF
listener $MOSQUITTO_PORT 127.0.0.1
allow_anonymous true
EOF
    "$MOSQUITTO_BIN" -c "$WORKDIR/mosquitto.conf" >"$WORKDIR/mosquitto.log" 2>&1 &
    MOSQUITTO_PID=$!
    for _ in $(seq 1 100); do
        kill -0 "$MOSQUITTO_PID" 2>/dev/null || return 1
        # A plain TCP connect probe is enough to know the listener is up.
        (exec 3<>"/dev/tcp/127.0.0.1/$MOSQUITTO_PORT") 2>/dev/null && exec 3<&- 3>&- && return 0
        sleep 0.1
    done
    return 1
}

# A minimal mock HTTP receiver for the webhook-forwarding tests -- not a
# real webhook service, just enough to record what nshmqtt actually POSTed
# (path, the auth header if any, and the raw body) so the tests below can
# grep it, one line per request in WEBHOOK_CAPTURE.
WEBHOOK_CAPTURE="$WORKDIR/webhook_capture.log"
start_webhook_mock() {
    cat > "$WORKDIR/webhook_mock.py" <<'EOF'
import http.server, sys

port = int(sys.argv[1])
capture_path = sys.argv[2]

class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length).decode('utf-8', 'replace')
        auth = self.headers.get('X-Hook-Secret', '')
        with open(capture_path, 'a') as f:
            f.write(self.path + '\t' + auth + '\t' + body + '\n')
        self.send_response(200)
        self.end_headers()

    def log_message(self, *args):
        pass

http.server.HTTPServer(('127.0.0.1', port), Handler).serve_forever()
EOF
    : > "$WEBHOOK_CAPTURE"
    python3 "$WORKDIR/webhook_mock.py" "$WEBHOOK_PORT" "$WEBHOOK_CAPTURE" >"$WORKDIR/webhook_mock.log" 2>&1 &
    WEBHOOK_MOCK_PID=$!
    for _ in $(seq 1 100); do
        kill -0 "$WEBHOOK_MOCK_PID" 2>/dev/null || return 1
        (exec 3<>"/dev/tcp/127.0.0.1/$WEBHOOK_PORT") 2>/dev/null && exec 3<&- 3>&- && return 0
        sleep 0.1
    done
    return 1
}

write_conf() {
    {
        echo "socket=$SOCK"
        echo "socket_mode=0660"
        echo "threads=4"
        echo "state_enabled=true"
        echo "state_file=$STATE_FILE"
        if [ "$HAVE_MOSQUITTO" = "1" ]; then
            echo "mqtt_host=127.0.0.1"
            echo "mqtt_port=$MOSQUITTO_PORT"
        else
            # An unreachable port -- broker-down behavior is still testable
            # (predictable failure/timeout responses) without a real broker.
            echo "mqtt_host=127.0.0.1"
            echo "mqtt_port=1"
        fi
        echo "mqtt_connect_timeout_seconds=2"
        echo "mqtt_publish_timeout_seconds=2"
    } > "$CONF"
}

start_daemon() {
    "$NSHMQTT_BIN" --config "$CONF" >"$WORKDIR/stdout.log" 2>"$WORKDIR/stderr.log" &
    NSHMQTT_PID=$!
    for _ in $(seq 1 100); do
        # A socket file existing only means bind() has happened, not that
        # listen() has -- there's a narrow window between the two where a
        # connect() attempt gets ECONNREFUSED even though `-S` already
        # sees the file. An actual successful request is the only real
        # readiness signal.
        if [ -S "$SOCK" ] && [ "$(http_status 'http://localhost/health')" = "200" ]; then
            return 0
        fi
        kill -0 "$NSHMQTT_PID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# ---------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------

if [ "$HAVE_MOSQUITTO" = "1" ]; then
    if start_broker; then
        pass "test Mosquitto broker started on 127.0.0.1:$MOSQUITTO_PORT"
    else
        fail "test Mosquitto broker failed to start -- MQTT-dependent tests will fail, not skip"
        cat "$WORKDIR/mosquitto.log" >&2
    fi
else
    skip "mosquitto/mosquitto_sub not found -- MQTT-dependent tests skipped"
fi

write_conf
if start_daemon; then
    pass "nshmqtt starts and creates its UNIX socket"
else
    fail "nshmqtt failed to start -- aborting remaining tests"
    cat "$WORKDIR/stdout.log" "$WORKDIR/stderr.log" >&2
    echo "$PASS passed, $FAIL failed, $SKIP skipped"
    exit 1
fi

# ---------------------------------------------------------------------
# Basic HTTP shape
# ---------------------------------------------------------------------

[ "$(http_status 'http://localhost/health')" = "200" ] \
    && pass "GET /health returns 200" || fail "GET /health returns 200"

[ "$(http_status 'http://localhost/nonexistent')" = "404" ] \
    && pass "unknown path returns 404" || fail "unknown path returns 404"

[ "$(http_status 'http://localhost/event/x' DELETE)" = "405" ] \
    && pass "DELETE /event/<topic> returns 405" || fail "DELETE /event/<topic> returns 405"

[ "$(http_status 'http://localhost/event/')" = "400" ] \
    && pass "POST /event/ (empty topic) returns 400" || fail "POST /event/ (empty topic) returns 400"

[ "$(http_status 'http://localhost/event/x?qos=9' POST 'y')" = "400" ] \
    && pass "POST /event with an out-of-range ?qos= returns 400" || fail "POST /event with an out-of-range ?qos= returns 400"

[ "$(http_status 'http://localhost/event/x?retain=maybe' POST 'y')" = "400" ] \
    && pass "POST /event with an unrecognized ?retain= returns 400" || fail "POST /event with an unrecognized ?retain= returns 400"

qos_retain_body="$(curl -sS --unix-socket "$SOCK" -X POST 'http://localhost/event/x?qos=2&retain=true' -d 'y')"
echo "$qos_retain_body" | grep -q '"qos":2,"retain":true' \
    && pass "POST /event echoes back the ?qos=/?retain= actually used" \
    || fail "POST /event echoes back the ?qos=/?retain= actually used (got '$qos_retain_body')"

qos_retain_header_body="$(curl -sS --unix-socket "$SOCK" -X POST 'http://localhost/event/x' \
    -H 'X-Mqtt-Qos: 2' -H 'X-Mqtt-Retain: true' -d 'y')"
echo "$qos_retain_header_body" | grep -q '"qos":2,"retain":true' \
    && pass "POST /event honors X-Mqtt-Qos/X-Mqtt-Retain headers, same as the query params" \
    || fail "POST /event honors X-Mqtt-Qos/X-Mqtt-Retain headers (got '$qos_retain_header_body')"

qos_header_wins_body="$(curl -sS --unix-socket "$SOCK" -X POST 'http://localhost/event/x?qos=0&retain=false' \
    -H 'X-Mqtt-Qos: 2' -H 'X-Mqtt-Retain: true' -d 'y')"
echo "$qos_header_wins_body" | grep -q '"qos":2,"retain":true' \
    && pass "when both are given, the X-Mqtt-Qos/X-Mqtt-Retain headers win over the query params" \
    || fail "header should win over query param (got '$qos_header_wins_body')"

[ "$(http_status 'http://localhost/metric/x' PUT 'not-a-number')" = "400" ] \
    && pass "PUT /metric with a non-numeric body returns 400" || fail "PUT /metric with a non-numeric body returns 400"

[ "$(http_status 'http://localhost/metric/does-not-exist' DELETE)" = "404" ] \
    && pass "DELETE of a never-set metric returns 404" || fail "DELETE of a never-set metric returns 404"

metrics_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics')"
echo "$metrics_body" | grep -q '^nshmqtt_up 1$' \
    && pass "GET /metrics includes nshmqtt_up 1" || fail "GET /metrics includes nshmqtt_up 1"
echo "$metrics_body" | grep -q 'mqtt_connections_active' \
    && pass "GET /metrics includes pool connection status" || fail "GET /metrics includes pool connection status"

# ---------------------------------------------------------------------
# Metric write/read/delete round trip
# ---------------------------------------------------------------------

put_status="$(http_status 'http://localhost/metric/test/load' PUT '17.3')"
[ "$put_status" = "200" ] && pass "PUT /metric/test/load returns 200" || fail "PUT /metric/test/load returns 200 (got $put_status)"

metrics_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics')"
echo "$metrics_body" | grep -q 'mqtt_test_load' \
    && fail "state values do not appear in the service-only /metrics" || pass "state values do not appear in the service-only /metrics"

state_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics-state')"
echo "$state_body" | grep -q '^mqtt_test_load 17.3$' \
    && pass "written metric appears in /metrics-state" || fail "written metric appears in /metrics-state"

del_status="$(http_status 'http://localhost/metric/test/load' DELETE)"
[ "$del_status" = "200" ] && pass "DELETE of an existing metric returns 200" || fail "DELETE of an existing metric returns 200 (got $del_status)"

state_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics-state')"
echo "$state_body" | grep -q 'mqtt_test_load' \
    && fail "deleted metric no longer appears in /metrics-state" || pass "deleted metric no longer appears in /metrics-state"

# ---------------------------------------------------------------------
# State persistence across a restart
# ---------------------------------------------------------------------

http_status 'http://localhost/metric/persist/me' PUT '5' >/dev/null
[ -f "$STATE_FILE" ] && pass "state file created after a metric write" || fail "state file created after a metric write"
grep -q '"persist/me":5' "$STATE_FILE" \
    && pass "state file content matches the written value" || fail "state file content matches the written value"

kill -TERM "$NSHMQTT_PID" 2>/dev/null
wait "$NSHMQTT_PID" 2>/dev/null
NSHMQTT_PID=""

if start_daemon; then
    pass "nshmqtt restarts cleanly"
    restored_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics-state')"
    echo "$restored_body" | grep -q '^mqtt_persist_me 5$' \
        && pass "state restored from state file after restart" || fail "state restored from state file after restart"
else
    fail "nshmqtt failed to restart"
fi

# ---------------------------------------------------------------------
# Real MQTT round trip (skipped if no local Mosquitto)
# ---------------------------------------------------------------------

if [ "$HAVE_MOSQUITTO" = "1" ] && kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
    received="$(timeout 5 mosquitto_sub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'integration/test/event' -C 1 2>/dev/null &
                 SUB_PID=$!
                 sleep 0.3
                 curl -sS --unix-socket "$SOCK" -X POST 'http://localhost/event/integration/test/event' -d 'hello-mqtt' >/dev/null
                 wait "$SUB_PID" 2>/dev/null)"
    [ "$received" = "hello-mqtt" ] \
        && pass "POST /event publishes to MQTT and is received by a real subscriber" \
        || fail "POST /event publishes to MQTT and is received by a real subscriber (got '$received')"

    event_status="$(http_status 'http://localhost/event/integration/test/event2' POST 'x')"
    [ "$event_status" = "200" ] && pass "event publish against a live broker returns 200" \
        || fail "event publish against a live broker returns 200 (got $event_status)"

    # ?retain=true on an event is honored by the broker itself: a late,
    # one-shot subscriber (subscribing well after the publish) still gets
    # it, which is not true of an ordinary (default, unretained) event.
    curl -sS --unix-socket "$SOCK" -X POST 'http://localhost/event/integration/test/retained?retain=true' \
        -d 'retained-value' >/dev/null
    late_received="$(timeout 3 mosquitto_sub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'integration/test/retained' -C 1 2>/dev/null)"
    [ "$late_received" = "retained-value" ] \
        && pass "POST /event with ?retain=true is actually retained on the broker" \
        || fail "POST /event with ?retain=true is actually retained on the broker (got '$late_received')"
else
    skip "no live test broker -- MQTT delivery round trip skipped"
fi

# ---------------------------------------------------------------------
# HTTP auth (X-Mqtt-Api-Key / http_auth_tokens)
# ---------------------------------------------------------------------

kill -TERM "$NSHMQTT_PID" 2>/dev/null
wait "$NSHMQTT_PID" 2>/dev/null
NSHMQTT_PID=""

{
    echo "http_auth_tokens=secret-one,secret-two"
} >> "$CONF"

if start_daemon; then
    pass "nshmqtt restarts with http_auth_tokens configured"

    [ "$(http_status 'http://localhost/health')" = "200" ] \
        && pass "GET /health stays open with auth configured" || fail "GET /health stays open with auth configured"

    [ "$(http_status 'http://localhost/metrics')" = "200" ] \
        && pass "GET /metrics (service-only) stays open with auth configured" \
        || fail "GET /metrics (service-only) stays open with auth configured"

    [ "$(http_status 'http://localhost/metrics-state')" = "401" ] \
        && pass "GET /metrics-state without a token returns 401" \
        || fail "GET /metrics-state without a token returns 401"

    [ "$(http_status 'http://localhost/event/x' POST 'y')" = "401" ] \
        && pass "POST /event without a token returns 401" || fail "POST /event without a token returns 401"

    [ "$(http_status 'http://localhost/metric/x' PUT '1')" = "401" ] \
        && pass "PUT /metric without a token returns 401" || fail "PUT /metric without a token returns 401"

    wrong_status="$(curl -sS -o /dev/null -w '%{http_code}' --unix-socket "$SOCK" \
        -H 'X-Mqtt-Api-Key: not-a-real-token' 'http://localhost/metrics-state')"
    [ "$wrong_status" = "401" ] && pass "GET /metrics-state with a wrong token returns 401" \
        || fail "GET /metrics-state with a wrong token returns 401 (got $wrong_status)"

    right_status="$(curl -sS -o /dev/null -w '%{http_code}' --unix-socket "$SOCK" \
        -H 'X-Mqtt-Api-Key: secret-two' 'http://localhost/metrics-state')"
    [ "$right_status" = "200" ] && pass "GET /metrics-state with a configured token (second of two) returns 200" \
        || fail "GET /metrics-state with a configured token returns 200 (got $right_status)"

    put_authed_status="$(curl -sS -o /dev/null -w '%{http_code}' --unix-socket "$SOCK" -X PUT \
        -H 'X-Mqtt-Api-Key: secret-one' 'http://localhost/metric/authed' -d '9')"
    [ "$put_authed_status" = "200" ] && pass "PUT /metric with a valid token returns 200" \
        || fail "PUT /metric with a valid token returns 200 (got $put_authed_status)"
else
    fail "nshmqtt failed to restart with http_auth_tokens configured"
fi

# ---------------------------------------------------------------------
# Webhook forwarding (MQTT -> HTTP), independent of subscribe_enabled
# ---------------------------------------------------------------------

if [ "$HAVE_MOSQUITTO" = "1" ] && [ "$HAVE_PYTHON3" = "1" ] && kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
    if start_webhook_mock; then
        pass "mock webhook receiver started on 127.0.0.1:$WEBHOOK_PORT"

        kill -TERM "$NSHMQTT_PID" 2>/dev/null
        wait "$NSHMQTT_PID" 2>/dev/null
        NSHMQTT_PID=""

        {
            echo "webhook_enabled=true"
            echo "webhook_url=http://127.0.0.1:$WEBHOOK_PORT/hook"
            echo "webhook_topics=webhook/#"
            echo "webhook_auth_header=X-Hook-Secret"
            echo "webhook_auth_value=hook-secret-value"
        } >> "$CONF"

        if start_daemon; then
            pass "nshmqtt restarts with webhook_enabled configured"

            # start_daemon() only confirms the HTTP layer is up -- the MQTT
            # connect + subscribe happens on its own background thread and
            # can still be in flight at that point. Publishing before the
            # subscription is actually active would just never be
            # delivered (no retained catch-up for a plain publish), so wait
            # for mqtt_connected=1 first rather than racing it.
            mqtt_ready=""
            for _ in $(seq 1 100); do
                if curl -sS --unix-socket "$SOCK" 'http://localhost/metrics' 2>/dev/null | grep -q 'nshmqtt_mqtt_connected 1'; then
                    mqtt_ready="1"
                    break
                fi
                sleep 0.1
            done
            [ -n "$mqtt_ready" ] || fail "nshmqtt's MQTT connection came up before publishing the webhook test messages"
            # mqtt_connected flips true right as the connect succeeds, a
            # moment before MQTTClient_subscribe's own SUBACK round-trip
            # actually completes -- same class of race as the mosquitto_sub
            # readiness sleep elsewhere in this file, same fix.
            sleep 0.3

            mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'webhook/test' -m 'hello-webhook'
            mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'other/topic' -m 'should-not-forward'

            delivered=""
            for _ in $(seq 1 100); do
                if grep -q 'hello-webhook' "$WEBHOOK_CAPTURE" 2>/dev/null; then
                    delivered="1"
                    break
                fi
                sleep 0.1
            done
            [ -n "$delivered" ] && pass "a message on a webhook_topics topic is POSTed to the webhook" \
                || fail "a message on a webhook_topics topic is POSTed to the webhook"

            captured_line="$(grep 'hello-webhook' "$WEBHOOK_CAPTURE" 2>/dev/null | head -1)"
            echo "$captured_line" | grep -q '"topic":"webhook/test"' \
                && pass "the webhook JSON body includes the correct topic" \
                || fail "the webhook JSON body includes the correct topic (got '$captured_line')"
            echo "$captured_line" | grep -q 'hook-secret-value' \
                && pass "the webhook request carries the configured auth header" \
                || fail "the webhook request carries the configured auth header (got '$captured_line')"

            sleep 0.5
            grep -q 'should-not-forward' "$WEBHOOK_CAPTURE" 2>/dev/null \
                && fail "a message NOT matching webhook_topics is not forwarded" \
                || pass "a message NOT matching webhook_topics is not forwarded"

            webhook_metrics_body="$(curl -sS --unix-socket "$SOCK" 'http://localhost/metrics')"
            echo "$webhook_metrics_body" | grep -q 'nshmqtt_webhook_delivered_total [1-9]' \
                && pass "webhook_delivered_total is reflected in /metrics" \
                || fail "webhook_delivered_total is reflected in /metrics"
        else
            fail "nshmqtt failed to restart with webhook_enabled configured"
        fi
    else
        fail "mock webhook receiver failed to start"
    fi
else
    skip "no live test broker or python3 -- webhook forwarding tests skipped"
fi

# ---------------------------------------------------------------------
# Prometheus MQTT JSON topics (prometheus_mqtt_json_topics)
# ---------------------------------------------------------------------

if [ "$HAVE_MOSQUITTO" = "1" ] && kill -0 "$MOSQUITTO_PID" 2>/dev/null; then
    kill -TERM "$NSHMQTT_PID" 2>/dev/null
    wait "$NSHMQTT_PID" 2>/dev/null
    NSHMQTT_PID=""

    {
        echo "subscribe_enabled=true"
        echo "subscribe_topics=#"
        echo "prometheus_mqtt_json_topics=json/test/device"
    } >> "$CONF"

    if start_daemon; then
        pass "nshmqtt restarts with subscribe_enabled and prometheus_mqtt_json_topics configured"

        # Same subscribe-readiness race as the webhook block above.
        mqtt_ready=""
        for _ in $(seq 1 100); do
            if curl -sS --unix-socket "$SOCK" 'http://localhost/metrics' 2>/dev/null | grep -q 'nshmqtt_mqtt_connected 1'; then
                mqtt_ready="1"
                break
            fi
            sleep 0.1
        done
        [ -n "$mqtt_ready" ] || fail "nshmqtt's MQTT connection came up before publishing the JSON-topic test messages"
        sleep 0.3

        # A plain numeric payload on a topic NOT in prometheus_mqtt_json_topics
        # still behaves exactly as before this feature existed.
        mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'plain/test/value' -m '17.3'

        # The AWTRIX-style JSON object on the one configured JSON topic.
        mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'json/test/device' -m \
            '{"batteryPercent":91,"batteryVoltage":4.12,"matrixPower":false,"currentApp":"Weather","extra":null,"indicators":[1,2,3],"wifi":{"enabled":true,"attempts":0}}'

        # A structurally identical JSON object, but on a topic that was
        # never added to prometheus_mqtt_json_topics -- not a bare number
        # either, so it's ignored exactly like any other non-numeric
        # message on a broad subscription, not flattened.
        mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'json/test/not-configured' -m '{"batteryPercent":50}'

        # Malformed JSON on the configured JSON topic -- rejected, not
        # partially applied, and no crash.
        mosquitto_pub -h 127.0.0.1 -p "$MOSQUITTO_PORT" -t 'json/test/device' -m '{not valid json'

        # Messages are processed strictly in order by one subscribe worker
        # (see mqtt.h) -- waiting for wifi_attempts (the last field of the
        # last message that should actually apply) guarantees the plain
        # value and the rest of the JSON object were already processed
        # too, not just published. The malformed message after it either
        # already failed or hasn't been dequeued yet either way -- neither
        # changes any of the state this test asserts on.
        # http_auth_tokens is still active from the earlier auth block above
        # (config lines accumulate across restarts in this test) -- needed
        # here too, same as every other /metrics-state call after that point.
        state_body=""
        for _ in $(seq 1 100); do
            state_body="$(curl -sS --unix-socket "$SOCK" -H 'X-Mqtt-Api-Key: secret-one' \
                'http://localhost/metrics-state' 2>/dev/null)"
            echo "$state_body" | grep -q 'json_test_device_wifi_attempts' && break
            sleep 0.1
        done

        echo "$state_body" | grep -q '^mqtt_plain_test_value 17.3$' \
            && pass "a plain numeric payload on a non-JSON topic is unaffected by this feature" \
            || fail "a plain numeric payload on a non-JSON topic is unaffected by this feature (got: $state_body)"

        echo "$state_body" | grep -q 'json_test_device_batteryPercent 91' \
            && pass "a number leaf in the JSON object is flattened into its own series" \
            || fail "a number leaf in the JSON object is flattened into its own series"
        echo "$state_body" | grep -q 'json_test_device_batteryVoltage 4.12' \
            && pass "a float leaf in the JSON object is flattened into its own series" \
            || fail "a float leaf in the JSON object is flattened into its own series"
        echo "$state_body" | grep -q 'json_test_device_matrixPower 0' \
            && pass "a false boolean leaf becomes 0" || fail "a false boolean leaf becomes 0"
        echo "$state_body" | grep -q 'json_test_device_wifi_enabled 1' \
            && pass "a nested object is flattened with '_', and true becomes 1" \
            || fail "a nested object is flattened with '_', and true becomes 1"
        echo "$state_body" | grep -q 'json_test_device_wifi_attempts 0' \
            && pass "a second leaf inside the same nested object is also flattened" \
            || fail "a second leaf inside the same nested object is also flattened"
        echo "$state_body" | grep -qE 'json_test_device_(currentApp|extra|indicators)' \
            && fail "a string/null/array leaf must not appear as a series" \
            || pass "a string/null/array leaf does not appear as a series"

        echo "$state_body" | grep -q 'json_test_not_configured' \
            && fail "a JSON payload on a topic NOT in prometheus_mqtt_json_topics is not flattened" \
            || pass "a JSON payload on a topic NOT in prometheus_mqtt_json_topics is not flattened"
    else
        fail "nshmqtt failed to restart with prometheus_mqtt_json_topics configured"
    fi
else
    skip "no live test broker -- prometheus_mqtt_json_topics tests skipped"
fi

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
