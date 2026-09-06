// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "text_util.h"

namespace nshmqtt
{

int default_thread_count()
{
    constexpr int kFloor = 4;
    constexpr int kCap = 20;
    constexpr int kFallback = 8; // hardware_concurrency() is allowed to return 0 when it can't tell

    unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0)
    {
        return kFallback;
    }

    int n = static_cast<int>(detected);
    if (n < kFloor)
    {
        return kFloor;
    }
    if (n > kCap)
    {
        return kCap;
    }
    return n;
}

std::string default_client_id()
{
    // "nshmqtt_" + 8 random hex digits (32 bits) -- generated once, here,
    // not per connection (mqtt_pool_size's own "-N" suffix already
    // separates connections within one process; this separates whole
    // processes/deployments from each other and from anything unrelated).
    // The hostname was considered and rejected: it's often shared by
    // every instance in a scaled/multi-replica deployment (they'd all
    // still collide), whereas a random suffix is unique per process
    // start regardless of how many instances share a host. 32 bits is
    // already far past what the birthday paradox needs for however many
    // nshmqtt processes could realistically ever share one broker (you'd
    // need tens of thousands of them running at once for a real chance of
    // a collision) -- not a cryptographic requirement, so std::random_device
    // is used directly (no PRNG in between) since this runs once at
    // startup, not on a hot path. Keeps the "nshmqtt_" prefix so the
    // identity is still recognizable at a glance in a broker's own client
    // list/logs, unlike a bare random string, and keeps the whole ID
    // short and readable there too.
    constexpr int kSuffixBytes = 4; // 32 bits
    constexpr char kHexDigits[] = "0123456789abcdef";

    std::random_device rd;
    std::string id = "nshmqtt_";
    id.reserve(id.size() + static_cast<std::size_t>(kSuffixBytes) * 2);
    for (int i = 0; i < kSuffixBytes; ++i)
    {
        unsigned int byte = rd() & 0xFFu;
        id += kHexDigits[(byte >> 4) & 0xFu];
        id += kHexDigits[byte & 0xFu];
    }
    return id;
}

namespace
{

std::string trim(const std::string &s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
    {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }
    return s.substr(start, end - start);
}

bool parse_long(const std::string &value, long &out, int base = 10)
{
    if (value.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long v = std::strtol(value.c_str(), &end, base);
    if (errno != 0 || end == value.c_str() || *end != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

// Splits a comma-separated list into trimmed, non-empty entries -- used
// for subscribe_topics=. An empty or all-blank value yields an empty
// vector (the caller treats that as "nothing to subscribe to", distinct
// from the unset-default which is {"#"}).
std::vector<std::string> split_csv(const std::string &value)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size())
    {
        size_t comma = value.find(',', start);
        std::string item =
            trim((comma == std::string::npos) ? value.substr(start) : value.substr(start, comma - start));
        if (!item.empty())
        {
            out.push_back(item);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return out;
}

} // namespace

bool parse_port(const std::string &text, int &out)
{
    long v = 0;
    if (!parse_long(text, v) || v < 1 || v > 65535)
    {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

namespace
{

// One entry per recognized config-file key: applies `value` to the right
// Config field, or fills `err` and returns false. Shared verbatim between
// load_config() (line-numbered errors) and apply_env_overrides() (plain
// errors) so the two never drift out of sync on what counts as valid.
bool apply_key(Config &cfg, const std::string &key, const std::string &value, std::string &err)
{
    if (key == "socket")
    {
        cfg.socket_path = value;
    }
    else if (key == "socket_mode")
    {
        long mode = 0;
        if (!parse_long(value, mode, 8) || mode < 0 || mode > 07777)
        {
            err = "invalid socket_mode (expected octal, e.g. 0660)";
            return false;
        }
        cfg.socket_mode = static_cast<mode_t>(mode);
    }
    else if (key == "threads")
    {
        long threads = 0;
        if (!parse_long(value, threads) || threads < 1 || threads > 256)
        {
            err = "invalid threads (expected 1-256)";
            return false;
        }
        cfg.threads = static_cast<int>(threads);
    }
    else if (key == "max_request_bytes")
    {
        long bytes = 0;
        if (!parse_long(value, bytes) || bytes < 256 || bytes > (1 << 20))
        {
            err = "invalid max_request_bytes (expected 256-1048576)";
            return false;
        }
        cfg.max_request_bytes = static_cast<std::size_t>(bytes);
    }
    else if (key == "max_body_bytes")
    {
        long bytes = 0;
        if (!parse_long(value, bytes) || bytes < 1 || bytes > (16 << 20))
        {
            err = "invalid max_body_bytes (expected 1-16777216)";
            return false;
        }
        cfg.max_body_bytes = static_cast<std::size_t>(bytes);
    }
    else if (key == "debug_log")
    {
        bool debug = false;
        if (!parse_bool(value, debug))
        {
            err = "invalid debug_log (expected true/false)";
            return false;
        }
        cfg.debug_log = debug;
    }
    else if (key == "tcp_port")
    {
        int port = 0;
        if (!value.empty() && value != "0")
        {
            if (!parse_port(value, port))
            {
                err = "invalid tcp_port (expected 1-65535, or 0 to disable)";
                return false;
            }
        }
        cfg.tcp_port = port;
    }
    else if (key == "tcp_address")
    {
        cfg.tcp_address = value;
    }
    else if (key == "simple_get")
    {
        bool enabled = false;
        if (!parse_bool(value, enabled))
        {
            err = "invalid simple_get (expected true/false)";
            return false;
        }
        cfg.simple_get = enabled;
    }
    else if (key == "http_auth_tokens")
    {
        cfg.http_auth_tokens = split_csv(value);
    }
    else if (key == "mqtt_host")
    {
        cfg.mqtt_host = value;
    }
    else if (key == "mqtt_port")
    {
        if (!parse_port(value, cfg.mqtt_port))
        {
            err = "invalid mqtt_port (expected 1-65535)";
            return false;
        }
    }
    else if (key == "mqtt_client_id")
    {
        cfg.mqtt_client_id = value;
    }
    else if (key == "mqtt_qos")
    {
        long qos = 0;
        if (!parse_long(value, qos) || qos < 0 || qos > 2)
        {
            err = "invalid mqtt_qos (expected 0, 1, or 2)";
            return false;
        }
        cfg.mqtt_qos = static_cast<int>(qos);
    }
    else if (key == "mqtt_pool_size")
    {
        long n = 0;
        if (!parse_long(value, n) || n < 1 || n > 64)
        {
            err = "invalid mqtt_pool_size (expected 1-64)";
            return false;
        }
        cfg.mqtt_pool_size = static_cast<int>(n);
    }
    else if (key == "mqtt_keepalive_seconds")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 5 || v > 3600)
        {
            err = "invalid mqtt_keepalive_seconds (expected 5-3600)";
            return false;
        }
        cfg.mqtt_keepalive_seconds = static_cast<int>(v);
    }
    else if (key == "mqtt_connect_timeout_seconds")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 300)
        {
            err = "invalid mqtt_connect_timeout_seconds (expected 1-300)";
            return false;
        }
        cfg.mqtt_connect_timeout_seconds = static_cast<int>(v);
    }
    else if (key == "mqtt_publish_timeout_seconds")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 300)
        {
            err = "invalid mqtt_publish_timeout_seconds (expected 1-300)";
            return false;
        }
        cfg.mqtt_publish_timeout_seconds = static_cast<int>(v);
    }
    else if (key == "mqtt_queue_size")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 65536)
        {
            err = "invalid mqtt_queue_size (expected 1-65536)";
            return false;
        }
        cfg.mqtt_queue_size = static_cast<int>(v);
    }
    else if (key == "mqtt_username")
    {
        cfg.mqtt_username = value;
    }
    else if (key == "mqtt_password")
    {
        cfg.mqtt_password = value;
    }
    else if (key == "subscribe_enabled")
    {
        bool enabled = false;
        if (!parse_bool(value, enabled))
        {
            err = "invalid subscribe_enabled (expected true/false)";
            return false;
        }
        cfg.subscribe_enabled = enabled;
    }
    else if (key == "subscribe_topics")
    {
        cfg.subscribe_topics = split_csv(value);
    }
    else if (key == "webhook_enabled")
    {
        bool enabled = false;
        if (!parse_bool(value, enabled))
        {
            err = "invalid webhook_enabled (expected true/false)";
            return false;
        }
        cfg.webhook_enabled = enabled;
    }
    else if (key == "webhook_url")
    {
        cfg.webhook_url = value;
    }
    else if (key == "webhook_topics")
    {
        cfg.webhook_topics = split_csv(value);
    }
    else if (key == "webhook_auth_header")
    {
        cfg.webhook_auth_header = value;
    }
    else if (key == "webhook_auth_value")
    {
        cfg.webhook_auth_value = value;
    }
    else if (key == "webhook_tls_insecure")
    {
        bool insecure = false;
        if (!parse_bool(value, insecure))
        {
            err = "invalid webhook_tls_insecure (expected true/false)";
            return false;
        }
        cfg.webhook_tls_insecure = insecure;
    }
    else if (key == "webhook_timeout_seconds")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 300)
        {
            err = "invalid webhook_timeout_seconds (expected 1-300)";
            return false;
        }
        cfg.webhook_timeout_seconds = static_cast<int>(v);
    }
    else if (key == "webhook_queue_size")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 65536)
        {
            err = "invalid webhook_queue_size (expected 1-65536)";
            return false;
        }
        cfg.webhook_queue_size = static_cast<int>(v);
    }
    else if (key == "state_enabled")
    {
        bool enabled = false;
        if (!parse_bool(value, enabled))
        {
            err = "invalid state_enabled (expected true/false)";
            return false;
        }
        cfg.state_enabled = enabled;
    }
    else if (key == "state_file")
    {
        cfg.state_file = value;
    }
    else if (key == "prometheus_http")
    {
        bool enabled = false;
        if (!parse_bool(value, enabled))
        {
            err = "invalid prometheus_http (expected true/false)";
            return false;
        }
        cfg.prometheus_http = enabled;
    }
    else if (key == "prometheus_textfile")
    {
        cfg.prometheus_textfile = value;
    }
    else if (key == "prometheus_textfile_interval_seconds")
    {
        long v = 0;
        if (!parse_long(value, v) || v < 1 || v > 86400)
        {
            err = "invalid prometheus_textfile_interval_seconds (expected 1-86400)";
            return false;
        }
        cfg.prometheus_textfile_interval_seconds = static_cast<int>(v);
    }
    else if (key == "prometheus_prefix")
    {
        cfg.prometheus_prefix = value;
    }
    else if (key == "prometheus_mqtt_state_prefix")
    {
        cfg.prometheus_mqtt_state_prefix = value;
    }
    else if (key == "state_topic_prefix")
    {
        cfg.state_topic_prefix = value;
    }
    else if (key == "prometheus_mqtt_json_topics")
    {
        cfg.prometheus_mqtt_json_topics = split_csv(value);
    }
    else
    {
        return false; // unknown key -- caller decides whether that's fatal
    }
    return true;
}

} // namespace

bool load_config(const std::string &path, Config &cfg, std::string &err, std::vector<std::string> *warnings)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        err = "cannot open config file: " + path;
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line))
    {
        ++line_no;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            err = path + ":" + std::to_string(line_no) + ": expected key=value";
            return false;
        }

        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));

        std::string key_err;
        if (!apply_key(cfg, key, value, key_err))
        {
            if (key_err.empty())
            {
                // Unknown key: ignored, not fatal, so newer/older config
                // files stay compatible. Caller may still want to log this.
                if (warnings != nullptr)
                {
                    warnings->push_back(path + ":" + std::to_string(line_no) + ": unknown config key '" + key + "'");
                }
                continue;
            }
            err = path + ":" + std::to_string(line_no) + ": " + key_err;
            return false;
        }
    }

    return true;
}

namespace
{

// Config-file key <-> NSHMQTT_<UPPERCASE> environment variable name.
struct EnvKey
{
    const char *config_key;
    const char *env_name;
};

// clang-format off
const EnvKey kEnvKeys[] = {
    {"socket",                              "NSHMQTT_SOCKET"},
    {"socket_mode",                         "NSHMQTT_SOCKET_MODE"},
    {"threads",                             "NSHMQTT_THREADS"},
    {"max_request_bytes",                   "NSHMQTT_MAX_REQUEST_BYTES"},
    {"max_body_bytes",                      "NSHMQTT_MAX_BODY_BYTES"},
    {"debug_log",                           "NSHMQTT_DEBUG_LOG"},
    {"tcp_port",                            "NSHMQTT_TCP_PORT"},
    {"tcp_address",                         "NSHMQTT_TCP_ADDRESS"},
    {"simple_get",                          "NSHMQTT_SIMPLE_GET"},
    {"http_auth_tokens",                    "NSHMQTT_HTTP_AUTH_TOKENS"},
    {"mqtt_host",                           "NSHMQTT_MQTT_HOST"},
    {"mqtt_port",                           "NSHMQTT_MQTT_PORT"},
    {"mqtt_client_id",                      "NSHMQTT_MQTT_CLIENT_ID"},
    {"mqtt_qos",                            "NSHMQTT_MQTT_QOS"},
    {"mqtt_pool_size",                      "NSHMQTT_MQTT_POOL_SIZE"},
    {"mqtt_keepalive_seconds",              "NSHMQTT_MQTT_KEEPALIVE_SECONDS"},
    {"mqtt_connect_timeout_seconds",        "NSHMQTT_MQTT_CONNECT_TIMEOUT_SECONDS"},
    {"mqtt_publish_timeout_seconds",        "NSHMQTT_MQTT_PUBLISH_TIMEOUT_SECONDS"},
    {"mqtt_queue_size",                     "NSHMQTT_MQTT_QUEUE_SIZE"},
    {"mqtt_username",                       "NSHMQTT_MQTT_USERNAME"},
    {"mqtt_password",                       "NSHMQTT_MQTT_PASSWORD"},
    {"subscribe_enabled",                   "NSHMQTT_SUBSCRIBE_ENABLED"},
    {"subscribe_topics",                    "NSHMQTT_SUBSCRIBE_TOPICS"},
    {"webhook_enabled",                     "NSHMQTT_WEBHOOK_ENABLED"},
    {"webhook_url",                         "NSHMQTT_WEBHOOK_URL"},
    {"webhook_topics",                      "NSHMQTT_WEBHOOK_TOPICS"},
    {"webhook_auth_header",                 "NSHMQTT_WEBHOOK_AUTH_HEADER"},
    {"webhook_auth_value",                  "NSHMQTT_WEBHOOK_AUTH_VALUE"},
    {"webhook_tls_insecure",                "NSHMQTT_WEBHOOK_TLS_INSECURE"},
    {"webhook_timeout_seconds",             "NSHMQTT_WEBHOOK_TIMEOUT_SECONDS"},
    {"webhook_queue_size",                  "NSHMQTT_WEBHOOK_QUEUE_SIZE"},
    {"state_enabled",                       "NSHMQTT_STATE_ENABLED"},
    {"state_file",                          "NSHMQTT_STATE_FILE"},
    {"prometheus_http",                     "NSHMQTT_PROMETHEUS_HTTP"},
    {"prometheus_textfile",                 "NSHMQTT_PROMETHEUS_TEXTFILE"},
    {"prometheus_textfile_interval_seconds","NSHMQTT_PROMETHEUS_TEXTFILE_INTERVAL_SECONDS"},
    {"prometheus_prefix",                   "NSHMQTT_PROMETHEUS_PREFIX"},
    {"prometheus_mqtt_state_prefix",             "NSHMQTT_PROMETHEUS_MQTT_STATE_PREFIX"},
    {"state_topic_prefix",                  "NSHMQTT_STATE_TOPIC_PREFIX"},
    {"prometheus_mqtt_json_topics",         "NSHMQTT_PROMETHEUS_MQTT_JSON_TOPICS"},
};
// clang-format on

} // namespace

bool apply_env_overrides(Config &cfg, std::string &err)
{
    for (const auto &k : kEnvKeys)
    {
        const char *v = std::getenv(k.env_name);
        if (v == nullptr)
        {
            continue;
        }
        std::string key_err;
        if (!apply_key(cfg, k.config_key, v, key_err))
        {
            err = std::string(k.env_name) + ": " + (key_err.empty() ? "invalid value" : key_err);
            return false;
        }
    }
    return true;
}

} // namespace nshmqtt
