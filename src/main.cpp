// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "config.h"
#include "json_util.h"
#include "log.h"
#include "metrics.h"
#include "mqtt.h"
#include "mqtt_topic.h"
#include "server.h"
#include "state.h"
#include "text_util.h"
#include "version.h"
#include "webhook.h"

namespace
{

const char *kDefaultConfigPath = "/etc/nshmqtt/nshmqtt.conf";

struct ConfigParam
{
    const char *key;
    const char *env;
    const char *desc;
};

// clang-format off
const ConfigParam kParams[] = {
    {"socket=",                              "NSHMQTT_SOCKET",                               "UNIX socket path"},
    {"socket_mode=",                         "NSHMQTT_SOCKET_MODE",                          "UNIX socket permissions (octal)"},
    {"threads=",                             "NSHMQTT_THREADS",                              "HTTP worker pool size (1-256)"},
    {"max_request_bytes=",                   "NSHMQTT_MAX_REQUEST_BYTES",                    "Max request header bytes"},
    {"max_body_bytes=",                      "NSHMQTT_MAX_BODY_BYTES",                       "Max request body bytes"},
    {"debug_log=",                           "NSHMQTT_DEBUG_LOG",                            "Log rejected/malformed requests"},
    {"tcp_port=",                            "NSHMQTT_TCP_PORT",                             "Optional TCP listener port, 0=disabled"},
    {"tcp_address=",                         "NSHMQTT_TCP_ADDRESS",                          "TCP bind address, empty=loopback"},
    {"simple_get=",                          "NSHMQTT_SIMPLE_GET",                           "Enable GET .../event|metric/<name>?value=..."},
    {"http_auth_tokens=",                    "NSHMQTT_HTTP_AUTH_TOKENS",                     "Comma-separated X-Mqtt-Api-Key tokens, empty=disabled"},
    {"mqtt_host=",                           "NSHMQTT_MQTT_HOST",                            "MQTT broker host"},
    {"mqtt_port=",                           "NSHMQTT_MQTT_PORT",                            "MQTT broker port"},
    {"mqtt_client_id=",                      "NSHMQTT_MQTT_CLIENT_ID",                       "MQTT client id"},
    {"mqtt_qos=",                            "NSHMQTT_MQTT_QOS",                             "Default MQTT QoS (0-2)"},
    {"mqtt_pool_size=",                      "NSHMQTT_MQTT_POOL_SIZE",                       "Parallel MQTT connections (1-64)"},
    {"mqtt_keepalive_seconds=",              "NSHMQTT_MQTT_KEEPALIVE_SECONDS",                "MQTT keepalive interval"},
    {"mqtt_connect_timeout_seconds=",        "NSHMQTT_MQTT_CONNECT_TIMEOUT_SECONDS",          "MQTT connect timeout"},
    {"mqtt_publish_timeout_seconds=",        "NSHMQTT_MQTT_PUBLISH_TIMEOUT_SECONDS",          "MQTT publish completion timeout"},
    {"mqtt_queue_size=",                     "NSHMQTT_MQTT_QUEUE_SIZE",                       "Bounded MQTT worker queue depth"},
    {"mqtt_username=",                       "NSHMQTT_MQTT_USERNAME",                        "MQTT username, empty=none"},
    {"mqtt_password=",                       "NSHMQTT_MQTT_PASSWORD",                        "MQTT password, empty=none"},
    {"subscribe_enabled=",                   "NSHMQTT_SUBSCRIBE_ENABLED",                     "Subscribe to MQTT topics into the state store"},
    {"subscribe_topics=",                    "NSHMQTT_SUBSCRIBE_TOPICS",                      "Comma-separated topic filters"},
    {"webhook_enabled=",                     "NSHMQTT_WEBHOOK_ENABLED",                       "Subscribe to MQTT topics and POST them to webhook_url"},
    {"webhook_url=",                         "NSHMQTT_WEBHOOK_URL",                           "Webhook HTTP(S) endpoint URL"},
    {"webhook_topics=",                      "NSHMQTT_WEBHOOK_TOPICS",                        "Comma-separated topic filters, independent of subscribe_topics"},
    {"webhook_auth_header=",                 "NSHMQTT_WEBHOOK_AUTH_HEADER",                   "Header name sent with each webhook POST, empty=none"},
    {"webhook_auth_value=",                  "NSHMQTT_WEBHOOK_AUTH_VALUE",                    "Header value sent with each webhook POST"},
    {"webhook_tls_insecure=",                "NSHMQTT_WEBHOOK_TLS_INSECURE",                  "Skip TLS cert verification for webhook_url (testing only)"},
    {"webhook_timeout_seconds=",             "NSHMQTT_WEBHOOK_TIMEOUT_SECONDS",               "Webhook POST timeout"},
    {"webhook_queue_size=",                  "NSHMQTT_WEBHOOK_QUEUE_SIZE",                    "Bounded queue depth between MQTT subscribe and the webhook worker"},
    {"state_enabled=",                       "NSHMQTT_STATE_ENABLED",                         "Persist current state to state_file"},
    {"state_file=",                          "NSHMQTT_STATE_FILE",                            "Current-state JSON file path"},
    {"prometheus_http=",                     "NSHMQTT_PROMETHEUS_HTTP",                       "Enable GET /metrics"},
    {"prometheus_textfile=",                 "NSHMQTT_PROMETHEUS_TEXTFILE",                   "Path to periodically write Prometheus metrics to"},
    {"prometheus_textfile_interval_seconds=","NSHMQTT_PROMETHEUS_TEXTFILE_INTERVAL_SECONDS",  "How often to write it"},
    {"prometheus_prefix=",                   "NSHMQTT_PROMETHEUS_PREFIX",                     "Service metric name prefix (GET /metrics)"},
    {"prometheus_mqtt_state_prefix=",             "NSHMQTT_PROMETHEUS_MQTT_STATE_PREFIX",               "State metric name prefix (GET /metrics-state)"},
    {"prometheus_mqtt_json_topics=",         "NSHMQTT_PROMETHEUS_MQTT_JSON_TOPICS",           "Exact topics whose payload is a JSON object to flatten"},
};
// clang-format on

// Joins a list of topics with ", " for a single log line -- these aren't
// secrets (unlike http_auth_tokens, logged as a count instead), so showing
// them plainly is worth more for a deployment that got the topic name
// slightly wrong than a count ever would be.
std::string join_topics(const std::vector<std::string> &topics)
{
    std::string out;
    for (std::size_t i = 0; i < topics.size(); ++i)
    {
        if (i > 0)
        {
            out += ", ";
        }
        out += topics[i];
    }
    return out;
}

void print_help(const char *argv0)
{
    std::printf("nshmqtt is a small HTTP-to-MQTT gateway and MQTT-to-Prometheus bridge. It\n"
                "answers 'POST /event/<topic>', 'PUT /metric/<name>', 'DELETE /metric/<name>'\n"
                "(and, if simple_get is enabled, the GET equivalents with a ?value=...\n"
                "query parameter) over a UNIX domain socket (and, optionally, TCP). It also\n"
                "always answers 'GET /health' and, unless disabled, 'GET /metrics' (service\n"
                "metrics only) and 'GET /metrics-state' (current values, gated by\n"
                "http_auth_tokens if configured).\n"
                "\n"
                "Usage: %s [--config PATH] | --health-check [--socket PATH | --port PORT]\n"
                "                | --version | --help\n"
                "\n"
                "  --config PATH        Path to config file (default: %s)\n"
                "  --health-check       Query a running instance's /health over its UNIX socket (or\n"
                "                       TCP with --port) and exit 0 (HTTP 200) or 1 (anything else)\n"
                "  --socket PATH        UNIX socket to check -- only with --health-check\n"
                "                       (default: %s)\n"
                "  --port PORT          Check via TCP 127.0.0.1:PORT instead of a UNIX socket --\n"
                "                       only with --health-check\n"
                "  --version            Print version and exit\n"
                "  --help               Print this help and exit\n"
                "\n"
                "Every config file key below also has an NSHMQTT_<KEY> environment variable\n"
                "that overrides it. The config file itself is optional at the default path\n"
                "(but not when --config names a path explicitly), so a container can run on\n"
                "environment variables alone. Precedence: environment > config file > default.\n"
                "\n",
                argv0, kDefaultConfigPath, nshmqtt::Config().socket_path.c_str());

    std::printf("%-40s %-45s %s\n", "CONFIG KEY", "ENVIRONMENT VARIABLE", "DESCRIPTION");
    for (const auto &p : kParams)
    {
        std::printf("%-40s %-45s %s\n", p.key, p.env, p.desc);
    }
}

constexpr int kHealthCheckTimeoutSeconds = 3;

bool check_health(const std::string &socket_path, int tcp_port, std::string &err)
{
    int fd;
    if (tcp_port != 0)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            err = std::string("socket() failed: ") + strerror(errno);
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(tcp_port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            err = "connect to 127.0.0.1:" + std::to_string(tcp_port) + " failed: " + strerror(errno);
            close(fd);
            return false;
        }
    }
    else
    {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            err = std::string("socket() failed: ") + strerror(errno);
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (socket_path.size() >= sizeof(addr.sun_path))
        {
            err = "socket path too long: " + socket_path;
            close(fd);
            return false;
        }
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            err = "connect to " + socket_path + " failed: " + strerror(errno);
            close(fd);
            return false;
        }
    }

    timeval tv{kHealthCheckTimeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static const char kRequest[] = "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (write(fd, kRequest, sizeof(kRequest) - 1) < 0)
    {
        err = std::string("write failed: ") + strerror(errno);
        close(fd);
        return false;
    }

    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
    {
        err = (n < 0) ? (std::string("read failed: ") + strerror(errno)) : "connection closed with no response";
        return false;
    }
    buf[n] = '\0';

    if (std::strncmp(buf, "HTTP/1.1 200", 12) != 0)
    {
        std::string line(buf);
        err = "unexpected response: " + line.substr(0, line.find("\r\n"));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::string config_path = kDefaultConfigPath;
    bool config_path_explicit = false;

    bool health_check_requested = false;
    std::string health_check_socket;
    int health_check_port = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--version")
        {
            std::printf("nshmqtt %s\n", NSHMQTT_VERSION);
            return 0;
        }
        if (arg == "--help" || arg == "-h")
        {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "--config")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--config requires a path argument\n");
                return 2;
            }
            config_path = argv[++i];
            config_path_explicit = true;
            continue;
        }
        if (arg == "--health-check")
        {
            health_check_requested = true;
            continue;
        }
        if (arg == "--socket")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--socket requires a path argument\n");
                return 2;
            }
            health_check_socket = argv[++i];
            continue;
        }
        if (arg == "--port")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--port requires a value\n");
                return 2;
            }
            if (!nshmqtt::parse_port(argv[++i], health_check_port))
            {
                std::fprintf(stderr, "invalid --port value: %s\n", argv[i]);
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
        print_help(argv[0]);
        return 2;
    }

    if (!health_check_requested && (!health_check_socket.empty() || health_check_port != 0))
    {
        std::fprintf(stderr, "--socket and --port only apply together with --health-check\n");
        return 2;
    }

    if (health_check_requested)
    {
        if (!health_check_socket.empty() && health_check_port != 0)
        {
            std::fprintf(stderr, "--socket and --port are mutually exclusive\n");
            return 2;
        }
        std::string socket_path = health_check_socket.empty() ? nshmqtt::Config().socket_path : health_check_socket;
        std::string hc_err;
        if (!check_health(socket_path, health_check_port, hc_err))
        {
            std::fprintf(stderr, "status=fail: %s\n", hc_err.c_str());
            return 1;
        }
        std::printf("status=ok\n");
        return 0;
    }

    // A worker thread's write() to a client that has already closed its
    // end of the socket must not kill the process.
    signal(SIGPIPE, SIG_IGN);

    nshmqtt::log_info(std::string("nshmqtt ") + NSHMQTT_VERSION + " starting");

    nshmqtt::Config cfg;
    std::string err;

    if (access(config_path.c_str(), F_OK) == 0)
    {
        nshmqtt::log_info("using config file: " + config_path);
        std::vector<std::string> warnings;
        if (!nshmqtt::load_config(config_path, cfg, err, &warnings))
        {
            nshmqtt::log_fatal("failed to load config: " + err);
            return 1;
        }
        for (const auto &w : warnings)
        {
            nshmqtt::log_warn(w);
        }
    }
    else if (config_path_explicit)
    {
        nshmqtt::log_fatal("config file not found: " + config_path);
        return 1;
    }
    else
    {
        nshmqtt::log_info("no config file at " + config_path + ", using defaults and environment variables");
    }

    if (!nshmqtt::apply_env_overrides(cfg, err))
    {
        nshmqtt::log_fatal("failed to apply environment variables: " + err);
        return 1;
    }

    nshmqtt::Logger::instance().set_debug_enabled(cfg.debug_log);

    nshmqtt::log_info("socket = " + cfg.socket_path);
    nshmqtt::log_info("threads = " + std::to_string(cfg.threads));
    nshmqtt::log_info("tcp_port = " + (cfg.tcp_port != 0 ? std::to_string(cfg.tcp_port) : std::string("(disabled)")));
    nshmqtt::log_info("simple_get = " + std::string(cfg.simple_get ? "true" : "false"));
    nshmqtt::log_info("http_auth_tokens = " +
                      (cfg.http_auth_tokens.empty()
                           ? "(disabled)"
                           : ("enabled (" + std::to_string(cfg.http_auth_tokens.size()) + " token(s) configured)")));
    nshmqtt::log_info("mqtt_host:port = " + cfg.mqtt_host + ":" + std::to_string(cfg.mqtt_port));
    nshmqtt::log_info("mqtt_client_id = " + cfg.mqtt_client_id);
    nshmqtt::log_info("mqtt_pool_size = " + std::to_string(cfg.mqtt_pool_size));
    nshmqtt::log_info("subscribe_enabled = " + std::string(cfg.subscribe_enabled ? "true" : "false"));
    nshmqtt::log_info("state_enabled = " + std::string(cfg.state_enabled ? "true" : "false") +
                      (cfg.state_enabled ? (", state_file = " + cfg.state_file) : std::string()));
    nshmqtt::log_info("prometheus_http = " + std::string(cfg.prometheus_http ? "true" : "false"));
    nshmqtt::log_info("prometheus_prefix = " + cfg.prometheus_prefix);
    nshmqtt::log_info("prometheus_mqtt_state_prefix = " + cfg.prometheus_mqtt_state_prefix);
    nshmqtt::log_info("prometheus_mqtt_json_topics = " + (cfg.prometheus_mqtt_json_topics.empty()
                                                              ? "(none)"
                                                              : join_topics(cfg.prometheus_mqtt_json_topics)));
    nshmqtt::log_info("prometheus_textfile = " +
                      (cfg.prometheus_textfile.empty() ? "(disabled)" : cfg.prometheus_textfile));
    nshmqtt::log_info("webhook_enabled = " + std::string(cfg.webhook_enabled ? "true" : "false") +
                      (cfg.webhook_enabled ? (", webhook_url = " + cfg.webhook_url) : std::string()));

    // curl_global_init() itself is not thread-safe and must run exactly
    // once before any other libcurl call, including from a background
    // thread -- done here, before the webhook worker thread (which does
    // make libcurl calls) exists at all. Harmless overhead when
    // webhook_enabled is false: curl_global_cleanup() at the bottom is the
    // only other libcurl call in that case.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    nshmqtt::StateStore state;
    if (cfg.state_enabled)
    {
        std::string state_err;
        if (!state.load(cfg.state_file, state_err))
        {
            nshmqtt::log_fatal("failed to load state file: " + state_err);
            return 1;
        }
        nshmqtt::log_info("loaded " + std::to_string(state.size()) + " state entries from " + cfg.state_file);
    }

    nshmqtt::Metrics metrics;

    // Started before mqtt below so its worker thread is already running
    // by the time any message could possibly arrive -- enqueue() silently
    // no-ops before start(), which would otherwise drop messages received
    // in the narrow window right after startup.
    nshmqtt::WebhookClient webhook(cfg, metrics);
    if (!webhook.start(err))
    {
        nshmqtt::log_fatal("failed to start webhook delivery: " + err);
        return 1;
    }

    // Incoming MQTT publishes feed two independent, optional consumers --
    // see config.h and README's "MQTT subscriptions" for why these are
    // kept separate rather than one combined subscription:
    //   - subscribe_topics: numeric-only payloads update the same
    //     current-state store HTTP metric writes use. A non-numeric
    //     message is a normal thing to see when subscribe_topics is broad
    //     (e.g. "#") and simply isn't meaningful as a metric value, so
    //     it's dropped rather than treated as an error. A topic explicitly
    //     listed in prometheus_mqtt_json_topics is the one exception: its
    //     payload is expected to be a JSON object instead, flattened into
    //     one state entry per leaf (see json_util.h's flatten_json_object()
    //     and README's "Prometheus support") rather than parsed as a
    //     single number. That list is exact-topic-only, not a filter --
    //     it doesn't add a subscription of its own, it only changes how a
    //     topic already reached via subscribe_topics is interpreted.
    //   - webhook_topics: every matching message, any payload, is
    //     forwarded to webhook_url as JSON.
    // Neither Paho nor MqttClient itself knows which of these a given
    // arrived topic is "for" -- topic_matches_any() decides that here,
    // independently for each, so either, both, or neither can fire per
    // message.
    nshmqtt::MqttClient mqtt(
        cfg, metrics, [&state, &cfg, &webhook](const std::string &topic, const std::string &payload, bool retained) {
            if (cfg.subscribe_enabled && nshmqtt::topic_matches_any(topic, cfg.subscribe_topics))
            {
                auto save_state_file = [&cfg, &state]() {
                    if (cfg.state_enabled)
                    {
                        std::string save_err;
                        if (!state.save(cfg.state_file, save_err))
                        {
                            nshmqtt::log_error("failed to persist state file: " + save_err);
                        }
                    }
                };

                bool is_json_topic =
                    std::find(cfg.prometheus_mqtt_json_topics.begin(), cfg.prometheus_mqtt_json_topics.end(), topic) !=
                    cfg.prometheus_mqtt_json_topics.end();
                if (is_json_topic)
                {
                    std::vector<std::pair<std::string, double>> leaves;
                    if (nshmqtt::flatten_json_object(payload, leaves))
                    {
                        for (const auto &leaf : leaves)
                        {
                            state.set(topic + "/" + leaf.first, leaf.second);
                        }
                        save_state_file();
                    }
                    else
                    {
                        nshmqtt::log_debug("ignoring malformed JSON MQTT message on '" + topic + "'");
                    }
                }
                else
                {
                    double value = 0.0;
                    if (nshmqtt::parse_double(payload, value))
                    {
                        state.set(topic, value);
                        save_state_file();
                    }
                    else
                    {
                        nshmqtt::log_debug("ignoring non-numeric MQTT message on '" + topic + "'");
                    }
                }
            }
            if (cfg.webhook_enabled && nshmqtt::topic_matches_any(topic, cfg.webhook_topics))
            {
                webhook.enqueue(topic, payload, retained);
            }
        });

    if (!mqtt.start(err))
    {
        nshmqtt::log_fatal("failed to start MQTT client: " + err);
        webhook.stop();
        return 1;
    }

    nshmqtt::Server server(cfg, mqtt, webhook, state, metrics);
    if (!server.init(err))
    {
        nshmqtt::log_fatal("startup failed: " + err);
        mqtt.stop();
        webhook.stop();
        return 1;
    }

    int rc = server.run();
    mqtt.stop();
    webhook.stop();
    curl_global_cleanup();
    return rc;
}
