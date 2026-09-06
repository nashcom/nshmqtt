// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "metrics.h"

#include <map>
#include <sstream>

#include "text_util.h"

namespace nshmqtt
{

void Metrics::record_request(const std::string &path)
{
    if (path.rfind("/event/", 0) == 0)
    {
        event_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else if (path.rfind("/metric/", 0) == 0)
    {
        metric_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else if (path == "/health")
    {
        health_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else if (path == "/metrics" || path == "/metrics-state")
    {
        metrics_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        other_requests_total.fetch_add(1, std::memory_order_relaxed);
    }
}

void Metrics::record_response(int status)
{
    if (status >= 200 && status < 300)
    {
        responses_2xx.fetch_add(1, std::memory_order_relaxed);
    }
    else if (status >= 400 && status < 500)
    {
        responses_4xx.fetch_add(1, std::memory_order_relaxed);
    }
    else if (status >= 500 && status < 600)
    {
        responses_5xx.fetch_add(1, std::memory_order_relaxed);
    }
}

std::string normalize_metric_name(const std::string &prefix, const std::string &topic_name,
                                  const std::string &topic_prefix)
{
    std::string raw = topic_prefix + topic_name;
    std::string body;
    body.reserve(raw.size());
    for (unsigned char c : raw)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        body += ok ? static_cast<char>(c) : '_';
    }
    if (!body.empty() && body[0] >= '0' && body[0] <= '9')
    {
        body = "_" + body;
    }
    return prefix + body;
}

NormalizedMetrics normalize_all(const std::string &prefix, const std::vector<std::pair<std::string, double>> &items,
                                const std::string &topic_prefix)
{
    NormalizedMetrics result;
    std::map<std::string, std::string> claimed; // Prometheus name -> topic name that claimed it first

    for (const auto &item : items)
    {
        std::string prom_name = normalize_metric_name(prefix, item.first, topic_prefix);
        auto it = claimed.find(prom_name);
        if (it != claimed.end())
        {
            result.collisions.push_back(item.first);
            continue;
        }
        claimed.emplace(prom_name, item.first);
        result.metrics.emplace_back(std::move(prom_name), item.second);
    }

    return result;
}

namespace
{

void write_counter(std::ostringstream &oss, const std::string &name, const std::string &help, const std::string &labels,
                   std::uint64_t value)
{
    oss << "# HELP " << name << " " << help << "\n";
    oss << "# TYPE " << name << " counter\n";
    oss << name << (labels.empty() ? "" : "{" + labels + "}") << " " << value << "\n";
}

void write_gauge(std::ostringstream &oss, const std::string &name, const std::string &help, double value)
{
    oss << "# HELP " << name << " " << help << "\n";
    oss << "# TYPE " << name << " gauge\n";
    oss << name << " " << format_double(value) << "\n";
}

} // namespace

std::string render_prometheus_service_metrics(const std::string &prefix, const Metrics &metrics,
                                              const std::string &version, double uptime_seconds, bool mqtt_connected,
                                              int mqtt_connections_active, int mqtt_pool_size, int mqtt_queue_depth,
                                              std::size_t state_entries, std::size_t metric_name_collisions,
                                              int webhook_queue_depth)
{
    std::ostringstream oss;

    oss << "# HELP " << prefix << "up 1 if this nshmqtt process is running (always 1 if you can scrape it)\n";
    oss << "# TYPE " << prefix << "up gauge\n";
    oss << prefix << "up 1\n";

    write_gauge(oss, prefix + "uptime_seconds", "Seconds since nshmqtt started", uptime_seconds);
    write_gauge(oss, prefix + "mqtt_connected", "1 if at least one pool connection is currently connected, 0 otherwise",
                mqtt_connected ? 1.0 : 0.0);
    write_gauge(oss, prefix + "mqtt_connections_active", "Number of MQTT pool connections currently connected",
                static_cast<double>(mqtt_connections_active));
    write_gauge(oss, prefix + "mqtt_pool_size", "Configured MQTT connection pool size (mqtt_pool_size)",
                static_cast<double>(mqtt_pool_size));
    write_gauge(oss, prefix + "mqtt_publish_queue_depth",
                "Publish jobs currently queued, shared across the connection pool",
                static_cast<double>(mqtt_queue_depth));
    write_gauge(oss, prefix + "state_entries", "Current number of stored metric values",
                static_cast<double>(state_entries));

    // Not "_total": this is recomputed from the current state snapshot on
    // every render, so it can go back down (e.g. a colliding entry gets
    // deleted) -- a live gauge, not a monotonic counter, despite counting
    // something that sounds cumulative. The count itself (not which
    // topics collided) carries no payload content, so it stays here in
    // the always-open service metrics rather than in the state metrics.
    write_gauge(oss, prefix + "metric_name_collisions",
                "Distinct topic names in the current state that normalize to an already-used Prometheus metric name "
                "and are dropped from the state metrics endpoint",
                static_cast<double>(metric_name_collisions));

    oss << "# HELP " << prefix << "http_requests_total HTTP requests received, by category\n";
    oss << "# TYPE " << prefix << "http_requests_total counter\n";
    oss << prefix << "http_requests_total{category=\"event\"} " << metrics.event_requests_total.load() << "\n";
    oss << prefix << "http_requests_total{category=\"metric\"} " << metrics.metric_requests_total.load() << "\n";
    oss << prefix << "http_requests_total{category=\"health\"} " << metrics.health_requests_total.load() << "\n";
    oss << prefix << "http_requests_total{category=\"metrics\"} " << metrics.metrics_requests_total.load() << "\n";
    oss << prefix << "http_requests_total{category=\"other\"} " << metrics.other_requests_total.load() << "\n";

    oss << "# HELP " << prefix << "http_responses_total HTTP responses sent, by status class\n";
    oss << "# TYPE " << prefix << "http_responses_total counter\n";
    oss << prefix << "http_responses_total{class=\"2xx\"} " << metrics.responses_2xx.load() << "\n";
    oss << prefix << "http_responses_total{class=\"4xx\"} " << metrics.responses_4xx.load() << "\n";
    oss << prefix << "http_responses_total{class=\"5xx\"} " << metrics.responses_5xx.load() << "\n";

    oss << "# HELP " << prefix << "mqtt_publish_total MQTT publish attempts, by outcome\n";
    oss << "# TYPE " << prefix << "mqtt_publish_total counter\n";
    oss << prefix << "mqtt_publish_total{outcome=\"ok\"} " << metrics.mqtt_publish_ok_total.load() << "\n";
    oss << prefix << "mqtt_publish_total{outcome=\"failed\"} " << metrics.mqtt_publish_failed_total.load() << "\n";
    oss << prefix << "mqtt_publish_total{outcome=\"timeout\"} " << metrics.mqtt_publish_timeout_total.load() << "\n";

    write_counter(oss, prefix + "mqtt_queue_full_total",
                  "Publish requests rejected because the MQTT worker queue was full", "",
                  metrics.mqtt_queue_full_total.load());
    write_counter(oss, prefix + "mqtt_reconnects_total", "MQTT reconnect attempts since startup", "",
                  metrics.mqtt_reconnects_total.load());
    write_counter(oss, prefix + "mqtt_messages_received_total", "MQTT messages received via subscription", "",
                  metrics.mqtt_messages_received_total.load());
    write_counter(oss, prefix + "mqtt_messages_dropped_total",
                  "MQTT messages dropped because the subscribe processing queue was full", "",
                  metrics.mqtt_messages_dropped_total.load());

    write_gauge(oss, prefix + "webhook_queue_depth", "Webhook delivery jobs currently queued",
                static_cast<double>(webhook_queue_depth));
    write_counter(oss, prefix + "webhook_delivered_total", "Webhook POSTs that completed with a 2xx response", "",
                  metrics.webhook_delivered_total.load());
    write_counter(oss, prefix + "webhook_failed_total",
                  "Webhook POSTs that failed (connection error, timeout, or non-2xx response)", "",
                  metrics.webhook_failed_total.load());
    write_counter(oss, prefix + "webhook_queue_full_total",
                  "Messages dropped because the webhook delivery queue was full", "",
                  metrics.webhook_queue_full_total.load());

    (void)version; // reserved for a future nshmqtt_build_info series, not emitted yet
    return oss.str();
}

std::string render_prometheus_state_metrics(const std::vector<std::pair<std::string, double>> &normalized_metrics)
{
    std::ostringstream oss;
    // See the header comment: dynamically-named series have no fixed name
    // to attach a per-series HELP/TYPE line to ahead of time -- emitted
    // without one apiece, which the Prometheus text format treats as
    // valid, "untyped" metrics. This is a plain `#` comment, not a `#
    // HELP` directive -- the latter requires a real metric name
    // immediately after it (Prometheus's parser rejects a placeholder
    // like "<name>" outright: "expected metric name after HELP").
    oss << "# Current value of each stored metric below (untyped; one series per name)\n";
    for (const auto &m : normalized_metrics)
    {
        oss << m.first << " " << format_double(m.second) << "\n";
    }
    return oss.str();
}

std::string render_health_json(const std::string &version, double uptime_seconds, bool mqtt_connected,
                               int mqtt_connections_active, int mqtt_pool_size, std::size_t state_entries)
{
    std::ostringstream oss;
    oss << "{";
    oss << "\"status\":\"ok\"";
    oss << ",\"version\":\"" << json_escape(version) << "\"";
    oss << ",\"uptime_seconds\":" << format_double(uptime_seconds);
    oss << ",\"mqtt_connected\":" << (mqtt_connected ? "true" : "false");
    oss << ",\"mqtt_connections_active\":" << mqtt_connections_active;
    oss << ",\"mqtt_pool_size\":" << mqtt_pool_size;
    oss << ",\"state_entries\":" << state_entries;
    oss << "}";
    return oss.str();
}

} // namespace nshmqtt
