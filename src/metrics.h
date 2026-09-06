// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Process-wide request/MQTT counters, MQTT-topic-to-Prometheus-name
// normalization, and the /metrics (and metrics-textfile) response body
// built from both plus the current StateStore snapshot. Counters are
// incremented from worker threads concurrently (HTTP workers and the one
// MQTT worker, see thread_pool.h and mqtt.h) and read from whichever
// thread handles a /metrics request or the periodic textfile writer --
// plain std::atomic<uint64_t> with relaxed ordering, mirroring nshgeoip's
// own metrics.h (see its comment for the same reasoning: independent
// counters, no ordering relationship to any other memory a reader needs).

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nshmqtt
{

struct Metrics
{
    // Split by path category rather than one flat counter, same rationale
    // as nshgeoip: a health check or a scrape isn't "an event" or "a
    // metric write" in any meaningful sense.
    std::atomic_uint64_t event_requests_total{0};
    std::atomic_uint64_t metric_requests_total{0};
    std::atomic_uint64_t health_requests_total{0};
    std::atomic_uint64_t metrics_requests_total{0};
    std::atomic_uint64_t other_requests_total{0};

    std::atomic_uint64_t responses_2xx{0};
    std::atomic_uint64_t responses_4xx{0};
    std::atomic_uint64_t responses_5xx{0};

    std::atomic_uint64_t mqtt_publish_ok_total{0};
    std::atomic_uint64_t mqtt_publish_failed_total{0};
    std::atomic_uint64_t mqtt_publish_timeout_total{0};
    std::atomic_uint64_t mqtt_queue_full_total{0};
    std::atomic_uint64_t mqtt_reconnects_total{0};
    std::atomic_uint64_t mqtt_messages_received_total{0};
    std::atomic_uint64_t mqtt_messages_dropped_total{0};

    std::atomic_uint64_t webhook_delivered_total{0};
    std::atomic_uint64_t webhook_failed_total{0};
    std::atomic_uint64_t webhook_queue_full_total{0};

    // Increments the request counter matching `path`'s leading segment
    // ("/event/...", "/metric/...", "/health", "/metrics"); anything else
    // (unknown path, a request whose path couldn't even be parsed) counts
    // as other_requests_total.
    void record_request(const std::string &path);

    // Increments responses_2xx/4xx/5xx matching `status / 100`; anything
    // outside 2xx/4xx/5xx (there isn't one today) is dropped.
    void record_response(int status);
};

// Deterministically maps an MQTT-style hierarchical name (e.g.
// "server1/system/cpu/load") to a Prometheus metric name: `topic_prefix`
// (config's state_topic_prefix, empty by default) is prepended to
// `topic_name` first, then every byte of the combined string that isn't
// [A-Za-z0-9_] becomes '_' -- topic_prefix goes through the same
// substitution as the topic name itself, no special-casing -- and a
// leading digit (after `prefix`, if the result would start with one) gets
// an underscore inserted before it, since Prometheus metric names can't
// start with a digit. `prefix` is prepended after normalization, as-is
// (expected to already be a valid Prometheus name prefix, e.g.
// "nshmqtt_") and is not itself sanitized.
std::string normalize_metric_name(const std::string &prefix, const std::string &topic_name,
                                  const std::string &topic_prefix = "");

struct NormalizedMetrics
{
    // Prometheus name -> value, one entry per distinct normalized name
    // that survived collision resolution.
    std::vector<std::pair<std::string, double>> metrics;
    // Topic names that normalized to a Prometheus name already claimed by
    // an earlier (alphabetically first) topic name, and were therefore
    // left out of `metrics` -- see render_prometheus_metrics()'s comment
    // for how this is surfaced instead of silently overwriting a value.
    std::vector<std::string> collisions;
};

// Runs normalize_metric_name() over every entry in `items` (expected to
// already be a name-sorted snapshot, e.g. from StateStore::snapshot(), so
// collision resolution is deterministic between calls: first name
// alphabetically wins, everything after it that collides is reported in
// `collisions` instead of overwriting the first).
NormalizedMetrics normalize_all(const std::string &prefix, const std::vector<std::pair<std::string, double>> &items,
                                const std::string &topic_prefix = "");

// Renders the service-only half of Prometheus output: the fixed set of
// counters above, MQTT connection/pool/queue status, and counts (not
// values) of current state -- nothing here is derived from an actual
// topic name or payload value, so this is always safe to leave
// unauthenticated (see GET /metrics in server.cpp, and README's
// "Security considerations"). `state_entries` and `metric_name_collisions`
// come from the caller's own StateStore::snapshot() and normalize_all()
// (see below) -- passed in rather than computed here so a caller that
// needs both this and render_prometheus_state_metrics() only calls
// normalize_all() once.
std::string render_prometheus_service_metrics(const std::string &prefix, const Metrics &metrics,
                                              const std::string &version, double uptime_seconds, bool mqtt_connected,
                                              int mqtt_connections_active, int mqtt_pool_size, int mqtt_queue_depth,
                                              std::size_t state_entries, std::size_t metric_name_collisions,
                                              int webhook_queue_depth);

// Renders the payload half: one gauge series per current state value,
// already normalized (see normalize_all()) and collision-resolved. This
// is real content -- the actual values callers have published -- so it's
// what GET /metrics-state gates behind http_auth_tokens when configured,
// unlike the service metrics above. Dynamically named series have no
// fixed name to attach a HELP/TYPE line to ahead of time -- emitted
// without one apiece, which the Prometheus text format treats as valid,
// "untyped" metrics.
std::string render_prometheus_state_metrics(const std::vector<std::pair<std::string, double>> &normalized_metrics);

// The /health JSON body (see server.cpp for the plain-text default).
std::string render_health_json(const std::string &version, double uptime_seconds, bool mqtt_connected,
                               int mqtt_connections_active, int mqtt_pool_size, std::size_t state_entries);

} // namespace nshmqtt
