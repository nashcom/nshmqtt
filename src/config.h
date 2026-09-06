// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Parser for the deliberately simple nshmqtt.conf key=value format -- same
// format and precedence rules (environment > config file > built-in
// default) as nshgeoip. Keys are prefixed by domain (mqtt_, subscribe_,
// state_, prometheus_) since nshmqtt's config spans more areas than
// nshgeoip's did; server/http keys keep nshgeoip's own bare names
// (socket, tcp_port, threads, ...) where the equivalent setting already
// existed there.

#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace nshmqtt
{

// See config.cpp for the reasoning (identical to nshgeoip's own
// default_thread_count()): CPU core count, clamped to [4, 20].
int default_thread_count();

struct Config
{
    // --- server / HTTP listener (mirrors nshgeoip's own keys) ----------
    std::string socket_path = "/run/nshmqtt/nshmqtt.sock";
    mode_t socket_mode = 0660;
    int threads = default_thread_count(); // HTTP worker pool size
    std::size_t max_request_bytes = 8192; // header block cap
    std::size_t max_body_bytes = 65536;   // request body cap (event/metric payloads)
    bool debug_log = false;

    // Optional TCP/IP listener, off by default. 0 = disabled, same
    // semantics as nshgeoip's tcp_port.
    int tcp_port = 0;
    std::string tcp_address;

    // --- HTTP API behavior ------------------------------------------------
    // Enables GET /event/<topic>?value=... and GET /metric/<name>?value=...
    // -- a side-effecting GET, which is not normal HTTP semantics, so this
    // exists as an explicit opt-out for deployments that don't want it.
    bool simple_get = true;

    // Comma-separated list of valid tokens for nshmqtt's own request
    // authentication -- checked against the X-Mqtt-Api-Key header,
    // independent of and in addition to whatever NGINX does with the
    // standard Authorization header (the two are deliberately different
    // headers so both checks can be layered without collision -- see
    // README's "Security considerations"). Empty (default) = disabled.
    // Applies to every payload-bearing endpoint (/event/*, /metric/*,
    // /metrics-state) but never to /health or the service-only /metrics
    // -- those carry no payload data and stay reachable (e.g. for
    // Prometheus or a container health check) without a token.
    std::vector<std::string> http_auth_tokens;

    // --- MQTT connectivity --------------------------------------------
    std::string mqtt_host = "127.0.0.1";
    int mqtt_port = 1883;
    std::string mqtt_client_id = "nshmqtt";
    int mqtt_qos = 1; // default QoS for publishes that don't specify their own
    // Number of independent MQTT connections publishing in parallel (see
    // mqtt.h's MqttClient class comment for why this is a pool of whole
    // connections, not a pool of threads sharing one). Each connection
    // beyond the first gets client_id suffixed "-<index>" -- the broker
    // sees N distinct sessions. Only the first (index 0) ever subscribes,
    // regardless of this value, so subscribe_enabled's messages are never
    // delivered more than once. 1 keeps today's single-connection
    // behavior exactly.
    int mqtt_pool_size = 1;
    int mqtt_keepalive_seconds = 60;
    int mqtt_connect_timeout_seconds = 10;
    // Bound on how long a single publish may block a calling HTTP worker
    // thread waiting for the MQTT worker to complete it -- see mqtt.h for
    // why this exists (Paho's synchronous client is not itself
    // thread-safe, so all publishes are serialized through one dedicated
    // worker thread and a bounded queue; this timeout is what keeps a
    // wedged broker from also wedging the HTTP layer).
    int mqtt_publish_timeout_seconds = 5;
    // Depth of the bounded queue between HTTP worker threads and the MQTT
    // worker thread. A full queue means the MQTT side can't keep up (or
    // the broker is down) -- new publishes fail fast with 503 rather than
    // growing this without bound.
    int mqtt_queue_size = 256;
    std::string mqtt_username; // empty = no authentication
    std::string mqtt_password; // empty = no authentication; never logged

    // --- MQTT subscription ----------------------------------------------
    bool subscribe_enabled = false;
    // Comma-separated MQTT topic filters, e.g. "sensors/#,server1/status".
    std::vector<std::string> subscribe_topics = {"#"};

    // --- Webhook forwarding (MQTT -> HTTP), independent of the above ------
    // A completely separate feature from subscribe_enabled/subscribe_topics
    // above -- either can be configured and run without the other. Where
    // subscribe_enabled feeds numeric-only payloads into the current-state
    // store, webhook forwarding POSTs every matching message (any payload,
    // not just numbers) as JSON to webhook_url. Both can watch overlapping
    // or entirely disjoint topic sets; Paho's message-arrived callback
    // never tells a caller which subscription matched, so nshmqtt does its
    // own topic-filter matching (see mqtt_topic.h) against each feature's
    // own list independently for every arrived message.
    bool webhook_enabled = false;
    std::string webhook_url;
    // Comma-separated MQTT topic filters, same syntax as subscribe_topics.
    std::vector<std::string> webhook_topics = {"#"};
    // Optional header sent on every webhook POST, e.g. webhook_auth_header=
    // Authorization, webhook_auth_value=Bearer some-token -- for a receiver
    // that requires its own credential. Both empty (default) = no header
    // sent. Deliberately a single configurable header, not a fixed scheme,
    // since a webhook receiver's own auth convention isn't nshmqtt's to
    // assume (unlike http_auth_tokens above, which nshmqtt itself defines).
    std::string webhook_auth_header;
    std::string webhook_auth_value;
    // Skips TLS certificate verification for an https:// webhook_url --
    // local/self-signed testing only, off by default. Using this against a
    // real endpoint defeats the point of https:// in the first place.
    bool webhook_tls_insecure = false;
    int webhook_timeout_seconds = 5;
    // Depth of the bounded queue between the MQTT subscribe worker and the
    // webhook worker thread -- same fail-fast-rather-than-grow-unbounded
    // reasoning as mqtt_queue_size above. A full queue means the webhook
    // receiver can't keep up (or is unreachable); new messages are dropped
    // and counted, never retried -- see README's "MQTT subscriptions" for
    // why nshmqtt treats a slow/unreachable downstream this way throughout.
    int webhook_queue_size = 256;

    // --- current-state persistence ---------------------------------------
    bool state_enabled = true;
    std::string state_file = "/var/lib/nshmqtt/state.json";

    // --- Prometheus output -----------------------------------------------
    // GET /metrics through the normal HTTP interface (UNIX socket, and TCP
    // if enabled) -- independent of the textfile output below, both may be
    // on at once.
    bool prometheus_http = true;
    // Path to periodically write Prometheus exposition format to, for
    // node_exporter's textfile collector or similar. Empty = disabled
    // (default), same pattern as nshgeoip's metrics_file.
    std::string prometheus_textfile;
    int prometheus_textfile_interval_seconds = 60;
    // Two independent prefixes, not one -- service metrics (GET
    // /metrics: request counters, MQTT connection/pool status, ...) and
    // state/content metrics (GET /metrics-state: one series per current
    // value, named from the topic) are different namespaces on purpose,
    // and default to different prefixes for the same reason: /metrics is
    // data *about* nshmqtt itself, /metrics-state is data nshmqtt is
    // *exposing* on behalf of whatever published it -- distinct enough in
    // nature that they default to looking distinct too, not just
    // independently configurable. A single shared prefix also means a
    // topic that happens to be named e.g. "up" or "mqtt_connected"
    // normalizes to the exact same Prometheus name as one of nshmqtt's
    // own reserved service-metric names -- a real collision risk, not
    // just a readability one, especially once both halves land in the
    // same prometheus_textfile. Set prometheus_mqtt_state_prefix alone to
    // rename content metrics (e.g. back to "nshmqtt_" for a single shared
    // namespace, or anything else) without touching service metrics.
    std::string prometheus_prefix = "nshmqtt_";         // service (GET /metrics)
    std::string prometheus_mqtt_state_prefix = "mqtt_"; // state/content (GET /metrics-state)

    // Optional, off by default. Prepended to the raw topic/metric name
    // before the byte-by-byte normalization pass (see
    // normalize_metric_name() in metrics.cpp) -- so it goes through the
    // same substitution as the rest of the name, no special-casing.
    // Render-time only: does not affect state.json, /metric/<name>
    // addressing, or DELETE. Exists as a just-in-case escape hatch for a
    // deployment that needs every state/content metric under one
    // additional namespace segment beyond prometheus_mqtt_state_prefix.
    std::string state_topic_prefix;

    // Comma-separated list of exact MQTT topics (not filters -- no `+`/`#`
    // wildcard matching here, deliberately, for this first implementation)
    // whose payload should be treated as a JSON object and flattened into
    // one state entry per leaf, rather than parsed as a single bare
    // number. Only meaningful for a topic already reached via
    // subscribe_enabled/subscribe_topics above -- this list doesn't add a
    // subscription of its own, it only changes how a matching topic's
    // payload already received that way is interpreted. Empty (default)
    // = today's numeric-only behavior, completely unchanged. See
    // json_util.h's flatten_json_object() for the conversion rules, and
    // README's "Prometheus support" for the AWTRIX-style example this was
    // built for (though nothing here is AWTRIX-specific).
    std::vector<std::string> prometheus_mqtt_json_topics;
};

// Parses `text` as a TCP port number (1-65535). Returns false (leaving
// `out` unchanged) if it isn't a valid integer in range.
bool parse_port(const std::string &text, int &out);

// Loads key=value pairs from `path` into `cfg`. Returns false and fills
// `err` with a human-readable message on failure. Unknown keys are ignored,
// not an error, so the config format can grow without breaking older
// config files -- but each one is appended to `warnings` (if given) so the
// caller can log it.
bool load_config(const std::string &path, Config &cfg, std::string &err, std::vector<std::string> *warnings = nullptr);

// Overlays NSHMQTT_* environment variables onto `cfg`, one per config-file
// key (e.g. NSHMQTT_MQTT_HOST for mqtt_host=). Only variables actually set
// in the environment override; meant to be called after load_config() so
// environment variables take precedence, the intended way to configure
// nshmqtt in a container without mounting a config file at all. Returns
// false and fills `err` if a set variable has an invalid value.
bool apply_env_overrides(Config &cfg, std::string &err);

} // namespace nshmqtt
