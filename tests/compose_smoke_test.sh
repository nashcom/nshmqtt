#!/bin/bash
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Smoke test for the docker-compose stack itself (container wiring,
# volumes, NGINX proxying, TLS) -- distinct from tests/integration_test.sh,
# which tests the nshmqtt binary directly, not how the containers are
# wired together. Assumes `docker compose up -d` has already been run in
# the project root; does not bring the stack up or down itself, so
# re-running this doesn't disturb whatever state you already have.
#
# Requires: docker compose, curl, mosquitto_pub/mosquitto_sub, python3
# (for JSON parsing of Prometheus's own API -- already a soft dependency
# nowhere else in this project, but the simplest way to check scrape
# target health without a real Prometheus query language dependency).
#
# TLS checks (8444, 8883) run only if tls/ca.crt exists (see
# ./gen-cert.sh) -- skipped, not failed, otherwise. Prometheus's own
# checks are also skipped, not failed, if it isn't running at all (the
# "monitoring" compose profile is optional) -- and always reached over
# TLS (nginx-monitoring's :9090), never plain HTTP, since that's the
# only way its own UI/API is published at all.

set -u

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
CACERT="$PROJECT_ROOT/tls/ca.crt"

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "SKIP: $1"; }

cd "$PROJECT_ROOT" || exit 1


# Only the core stack is required here -- nshmqtt-prometheus is not.
# Prometheus is optional (the "monitoring" compose profile); its own
# checks below skip gracefully if it isn't running, same as the
# mosquitto_pub/mosquitto_sub and tls/ca.crt checks already do for their
# own optional pieces. Requiring it here would defeat that entirely.
for c in nshmqtt nshmqtt-nginx nshmqtt-mosquitto; do
    if ! docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null | grep -q true; then
        echo "container '$c' is not running -- run 'docker compose up -d' first" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------
# Shared UNIX socket volume: same file, visible with the same relaxed
# permissions from both containers that need it.
# ---------------------------------------------------------------------

nshmqtt_sock_line="$(docker compose exec -T nshmqtt sh -c 'ls -la /run/nshmqtt/nshmqtt.sock 2>&1')"
echo "$nshmqtt_sock_line" | grep -q '^srw' \
    && pass "nshmqtt.sock exists as a socket inside the nshmqtt container" \
    || fail "nshmqtt.sock exists as a socket inside the nshmqtt container (got: $nshmqtt_sock_line)"

nginx_sock_line="$(docker compose exec -T nginx sh -c 'ls -la /run/nshmqtt/nshmqtt.sock 2>&1')"
echo "$nginx_sock_line" | grep -q '^srw' \
    && pass "the same socket is visible inside the nginx container (shared volume)" \
    || fail "the same socket is visible inside the nginx container (got: $nginx_sock_line)"

# nshmqtt's own TCP listener (9100 inside its container, feeding nginx's
# dedicated :9100 below -- see docker-compose.yml/nginx.conf's own
# comments) is not published to the host itself; this checks it directly
# from inside the nginx container, independent of nginx's own routing.
# Service metrics use prometheus_prefix (nshmqtt_, this stack's default --
# not prometheus_mqtt_state_prefix, which is mqtt_ here and applies only
# to /metrics-state's content).
tcp_metrics_line="$(docker compose exec -T nginx sh -c 'wget -qO- http://nshmqtt:9100/metrics 2>&1' | head -1)"
echo "$tcp_metrics_line" | grep -q 'HELP nshmqtt_up' \
    && pass "nshmqtt's own TCP listener (9100) independently serves /metrics" \
    || fail "nshmqtt's own TCP listener (9100) independently serves /metrics (got: $tcp_metrics_line)"

# ---------------------------------------------------------------------
# HTTP through NGINX -- write API/health on the general port (8081) over
# the UNIX socket; /metrics and /metrics-state on NGINX's own *dedicated*
# :9100 listener instead, published to the host under the same port
# number nshmqtt uses internally (see nginx.conf's own comment). The two
# genuinely don't overlap -- checked both ways below.
# ---------------------------------------------------------------------

status="$(curl -sS -o /dev/null -w '%{http_code}' 'http://localhost:8081/health')"
[ "$status" = "200" ] && pass "GET /health through NGINX (plain, 8081) returns 200" \
    || fail "GET /health through NGINX (plain, 8081) returns 200 (got $status)"

put_status="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT 'http://localhost:8081/metric/smoke/test' -d '1')"
[ "$put_status" = "200" ] && pass "PUT /metric through NGINX returns 200" \
    || fail "PUT /metric through NGINX returns 200 (got $put_status)"

metrics_on_8081="$(curl -sS -o /dev/null -w '%{http_code}' 'http://localhost:8081/metrics')"
[ "$metrics_on_8081" = "404" ] && pass "/metrics is NOT reachable on the general API port (8081)" \
    || fail "/metrics is NOT reachable on the general API port (8081) (got $metrics_on_8081)"

state_body="$(curl -sS 'http://localhost:9100/metrics-state')"
echo "$state_body" | grep -q '^mqtt_smoke_test 1$' \
    && pass "written metric appears in /metrics-state on NGINX's dedicated :9100" \
    || fail "written metric appears in /metrics-state on NGINX's dedicated :9100"

write_on_9100="$(curl -sS -o /dev/null -w '%{http_code}' 'http://localhost:9100/health')"
[ "$write_on_9100" = "404" ] && pass "/health is NOT reachable on the dedicated metrics port (9100)" \
    || fail "/health is NOT reachable on the dedicated metrics port (9100) (got $write_on_9100)"

if [ -f "$CACERT" ]; then
    https_status="$(curl -sS -o /dev/null -w '%{http_code}' --cacert "$CACERT" 'https://localhost:8444/health')"
    [ "$https_status" = "200" ] && pass "GET /health through NGINX (TLS, 8444) returns 200" \
        || fail "GET /health through NGINX (TLS, 8444) returns 200 (got $https_status)"
else
    skip "no tls/ca.crt -- run ./gen-cert.sh to also check the HTTPS listener (8444)"
fi

# ---------------------------------------------------------------------
# MQTT through NGINX's stream{} passthrough -> mosquitto
# ---------------------------------------------------------------------

if command -v mosquitto_pub >/dev/null 2>&1 && command -v mosquitto_sub >/dev/null 2>&1; then
    received="$(timeout 5 mosquitto_sub -h localhost -p 1883 -t 'smoke/test/plain' -C 1 2>/dev/null &
                 SUB_PID=$!
                 sleep 0.3
                 mosquitto_pub -h localhost -p 1883 -t 'smoke/test/plain' -m 'hello-plain'
                 wait "$SUB_PID" 2>/dev/null)"
    [ "$received" = "hello-plain" ] && pass "MQTT round trip through NGINX (plain, 1883)" \
        || fail "MQTT round trip through NGINX (plain, 1883) (got '$received')"

    if [ -f "$CACERT" ]; then
        received_tls="$(timeout 5 mosquitto_sub -h localhost -p 8883 --cafile "$CACERT" -t 'smoke/test/tls' -C 1 2>/dev/null &
                          SUB_PID=$!
                          sleep 0.3
                          mosquitto_pub -h localhost -p 8883 --cafile "$CACERT" -t 'smoke/test/tls' -m 'hello-tls'
                          wait "$SUB_PID" 2>/dev/null)"
        [ "$received_tls" = "hello-tls" ] && pass "MQTT round trip through NGINX (TLS, 8883)" \
            || fail "MQTT round trip through NGINX (TLS, 8883) (got '$received_tls')"
    else
        skip "no tls/ca.crt -- run ./gen-cert.sh to also check the MQTT/TLS listener (8883)"
    fi
else
    skip "mosquitto_pub/mosquitto_sub not found -- MQTT round-trip checks skipped"
fi

# ---------------------------------------------------------------------
# Prometheus: both jobs healthy, scraping through nginx (not nshmqtt
# directly -- see prometheus/prometheus.yml)
# ---------------------------------------------------------------------

if command -v python3 >/dev/null 2>&1; then
    if [ -f "$CACERT" ]; then
        # Prometheus's own UI/API is TLS-only through nginx-monitoring
        # (see nginx-monitoring.conf) -- there is no plain-HTTP path to it
        # at all, unlike the core stack's other listeners above. Prometheus
        # is also optional (the "monitoring" compose profile) -- if it's
        # simply not running, that's a SKIP, not a FAIL. A curl failure
        # here (connection refused, or -f rejecting a non-2xx response)
        # means exactly that; only actually reaching it and getting a
        # per-job health value that isn't "up" is a real failure.
        if targets_json="$(curl -sS -f --cacert "$CACERT" 'https://localhost:9090/api/v1/targets' 2>/dev/null)" && [ -n "$targets_json" ]; then
            for job in nshmqtt nshmqtt-state; do
                health="$(echo "$targets_json" | python3 -c "
import json, sys
d = json.load(sys.stdin)
for t in d['data']['activeTargets']:
    if t['labels']['job'] == '$job':
        print(t['health'])
        break
else:
    print('missing')
" 2>/dev/null)"
                [ "$health" = "up" ] && pass "Prometheus job '$job' is healthy" \
                    || fail "Prometheus job '$job' is healthy (got '$health')"
            done
        else
            for job in nshmqtt nshmqtt-state; do
                skip "Prometheus job '$job' is healthy -- Prometheus not reachable on :9090 (run with --profile monitoring to include it)"
            done
        fi
    else
        for job in nshmqtt nshmqtt-state; do
            skip "Prometheus job '$job' is healthy -- no tls/ca.crt (run ./gen-cert.sh; Prometheus is TLS-only)"
        done
    fi
else
    skip "python3 not found -- Prometheus target health checks skipped"
fi

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
