// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "event_placeholders.h"
#include "json_util.h"
#include "log.h"
#include "text_util.h"
#include "version.h"

namespace nshmqtt
{

namespace
{

std::string errno_str()
{
    return std::strerror(errno);
}

std::string mode_octal(mode_t mode)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04o", mode & 07777);
    return std::string(buf);
}

bool write_all(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool content_type_is_json(const std::string &content_type)
{
    std::string lower = content_type;
    for (char &c : lower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find("application/json") != std::string::npos;
}

} // namespace

Server::Server(Config cfg, MqttClient &mqtt, WebhookClient &webhook, StateStore &state, Metrics &metrics)
    : cfg_(std::move(cfg)), mqtt_(mqtt), webhook_(webhook), state_(state), metrics_(metrics),
      start_time_(std::chrono::steady_clock::now())
{}

Server::~Server()
{
    if (pool_)
    {
        pool_->shutdown();
        pool_.reset();
    }
    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
    }
    for (int fd : tcp_listen_fds_)
    {
        close(fd);
    }
    if (signal_fd_ >= 0)
    {
        close(signal_fd_);
    }
    if (socket_created_)
    {
        unlink(cfg_.socket_path.c_str());
    }
}

bool Server::init(std::string &err)
{
    if (!setup_signals(err))
    {
        return false;
    }

    if (!setup_socket(err))
    {
        return false;
    }

    if (!setup_tcp_listeners(err))
    {
        return false;
    }

    pool_ =
        std::make_unique<ThreadPool>(static_cast<std::size_t>(cfg_.threads), [this](int fd) { handle_connection(fd); });
    log_info("HTTP worker pool started with " + std::to_string(cfg_.threads) + " threads");

    return true;
}

bool Server::setup_signals(std::string &err)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
    {
        err = "sigprocmask() failed: " + errno_str();
        return false;
    }

    signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd_ < 0)
    {
        err = "signalfd() failed: " + errno_str();
        return false;
    }

    return true;
}

bool Server::path_has_live_listener(const std::string &path) const
{
    int probe_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe_fd < 0)
    {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int rc = connect(probe_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    close(probe_fd);
    return rc == 0;
}

bool Server::ensure_socket_directory(const std::string &socket_path, std::string &err)
{
    size_t slash = socket_path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
    {
        return true;
    }
    std::string dir = socket_path.substr(0, slash);

    if (mkdir(dir.c_str(), 0755) == 0)
    {
        log_info("created socket directory: " + dir);
        return true;
    }
    if (errno != EEXIST)
    {
        err = "failed to create socket directory '" + dir + "': " + errno_str() +
              " (if running under systemd, set RuntimeDirectory=nshmqtt; "
              "otherwise create it manually, e.g. 'mkdir -p " +
              dir + "')";
        return false;
    }

    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0)
    {
        err = "failed to stat socket directory '" + dir + "': " + errno_str();
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        err = "socket directory path '" + dir + "' exists but is not a directory";
        return false;
    }
    if (access(dir.c_str(), W_OK) != 0)
    {
        err = "socket directory '" + dir + "' exists but is not writable: " + errno_str();
        return false;
    }
    return true;
}

bool Server::setup_socket(std::string &err)
{
    const std::string &path = cfg_.socket_path;

    if (!ensure_socket_directory(path, err))
    {
        return false;
    }

    struct stat st{};
    if (lstat(path.c_str(), &st) == 0)
    {
        if (!S_ISSOCK(st.st_mode))
        {
            err = "refusing to remove non-socket file at " + path;
            return false;
        }
        if (path_has_live_listener(path))
        {
            err = "another process is already listening on " + path +
                  " -- refusing to remove its socket. Stop that instance "
                  "first, or configure a different socket= path.";
            return false;
        }
        if (unlink(path.c_str()) != 0)
        {
            err = "failed to remove stale socket " + path + ": " + errno_str();
            return false;
        }
        log_warn("removed stale socket: " + path);
    }
    else if (errno != ENOENT)
    {
        err = "failed to stat socket path " + path + ": " + errno_str();
        return false;
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0)
    {
        err = "socket() failed: " + errno_str();
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        err = "socket path too long: " + path;
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        err = "bind(" + path + ") failed: " + errno_str();
        return false;
    }
    socket_created_ = true;

    if (chmod(path.c_str(), cfg_.socket_mode) != 0)
    {
        err = "chmod(" + path + ") failed: " + errno_str();
        return false;
    }

    if (listen(listen_fd_, SOMAXCONN) != 0)
    {
        err = "listen() failed: " + errno_str();
        return false;
    }

    log_info("listening on unix socket " + path + " (mode " + mode_octal(cfg_.socket_mode) + ")");
    return true;
}

bool Server::bind_one_tcp(int family, const sockaddr *addr, socklen_t addrlen, std::string &err)
{
    int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        err = "socket() failed for TCP listener: " + errno_str();
        return false;
    }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
    {
        err = "setsockopt(SO_REUSEADDR) failed: " + errno_str();
        close(fd);
        return false;
    }

    if (family == AF_INET6)
    {
        int v6only = 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0)
        {
            err = "setsockopt(IPV6_V6ONLY) failed: " + errno_str();
            close(fd);
            return false;
        }
    }

    if (bind(fd, addr, addrlen) != 0)
    {
        err = "bind() failed for TCP listener: " + errno_str();
        close(fd);
        return false;
    }

    if (listen(fd, SOMAXCONN) != 0)
    {
        err = "listen() failed for TCP listener: " + errno_str();
        close(fd);
        return false;
    }

    char host[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (family == AF_INET)
    {
        const auto *a4 = reinterpret_cast<const sockaddr_in *>(addr);
        inet_ntop(AF_INET, &a4->sin_addr, host, sizeof(host));
        port = ntohs(a4->sin_port);
    }
    else
    {
        const auto *a6 = reinterpret_cast<const sockaddr_in6 *>(addr);
        inet_ntop(AF_INET6, &a6->sin6_addr, host, sizeof(host));
        port = ntohs(a6->sin6_port);
    }

    tcp_listen_fds_.push_back(fd);
    log_info("listening on tcp " + std::string(host) + ":" + std::to_string(port));
    return true;
}

bool Server::setup_tcp_listeners(std::string &err)
{
    if (cfg_.tcp_port == 0)
    {
        return true; // disabled by default
    }

    if (cfg_.tcp_address.empty())
    {
        sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (!bind_one_tcp(AF_INET, reinterpret_cast<sockaddr *>(&addr4), sizeof(addr4), err))
        {
            return false;
        }

        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        addr6.sin6_addr = in6addr_loopback;
        if (!bind_one_tcp(AF_INET6, reinterpret_cast<sockaddr *>(&addr6), sizeof(addr6), err))
        {
            return false;
        }

        return true;
    }

    sockaddr_in addr4{};
    if (inet_pton(AF_INET, cfg_.tcp_address.c_str(), &addr4.sin_addr) == 1)
    {
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        return bind_one_tcp(AF_INET, reinterpret_cast<sockaddr *>(&addr4), sizeof(addr4), err);
    }

    sockaddr_in6 addr6{};
    if (inet_pton(AF_INET6, cfg_.tcp_address.c_str(), &addr6.sin6_addr) == 1)
    {
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(static_cast<uint16_t>(cfg_.tcp_port));
        return bind_one_tcp(AF_INET6, reinterpret_cast<sockaddr *>(&addr6), sizeof(addr6), err);
    }

    err = "invalid tcp_address: " + cfg_.tcp_address;
    return false;
}

double Server::uptime_seconds() const
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now() - start_time_).count();
}

bool Server::is_authorized(const HttpRequest &req) const
{
    if (cfg_.http_auth_tokens.empty())
    {
        return true; // no tokens configured -- auth not in use, same as today
    }
    for (const auto &token : cfg_.http_auth_tokens)
    {
        if (constant_time_equals(req.auth_token, token))
        {
            return true;
        }
    }
    return false;
}

HttpResponse Server::event_outcome_response(const std::string &topic, std::size_t payload_bytes, int qos, bool retain,
                                            PublishOutcome outcome)
{
    HttpResponse r;
    r.format = ResponseFormat::Json;
    switch (outcome)
    {
    case PublishOutcome::Ok:
        r.status = 200;
        r.body = "{\"topic\":\"" + json_escape(topic) + "\",\"payload_bytes\":" + std::to_string(payload_bytes) +
                 ",\"qos\":" + std::to_string(qos) + ",\"retain\":" + (retain ? "true" : "false") +
                 ",\"published\":true}";
        break;
    case PublishOutcome::QueueFull:
        r.status = 503;
        r.body = "{\"error\":\"mqtt publish queue full\"}";
        break;
    case PublishOutcome::Timeout:
        r.status = 504;
        r.body = "{\"error\":\"mqtt publish timed out\"}";
        break;
    case PublishOutcome::Failed:
        r.status = 502;
        r.body = "{\"error\":\"mqtt publish failed\"}";
        break;
    }
    return r;
}

HttpResponse Server::handle_event(const HttpRequest &req)
{
    static const std::string prefix = "/event/";
    if (req.path.size() <= prefix.size() || req.path.compare(0, prefix.size(), prefix) != 0)
    {
        return make_error_response(400, "missing topic", ResponseFormat::Json);
    }
    std::string topic = req.path.substr(prefix.size());
    if (topic.empty())
    {
        return make_error_response(400, "missing topic", ResponseFormat::Json);
    }

    std::string payload;
    if (req.method == "POST")
    {
        payload = req.body;
    }
    else if (req.method == "GET" && cfg_.simple_get)
    {
        std::string value;
        if (!find_query_param(req, "value", value))
        {
            return make_error_response(400, "missing value parameter", ResponseFormat::Json);
        }
        payload = value;
    }
    else
    {
        HttpResponse r = make_error_response(405, "method not allowed", ResponseFormat::Json);
        r.headers.push_back({"Allow", cfg_.simple_get ? "POST, GET" : "POST"});
        return r;
    }

    // Opt-in (see config.h) -- applies to the payload regardless of which
    // of the two forms above produced it. The response below reports the
    // substituted payload's own size, since that's what's actually
    // published, not the size of whatever the caller originally sent.
    if (cfg_.event_placeholders_enabled)
    {
        payload = substitute_event_placeholders(payload);
    }

    // Both apply to POST and the simple GET form alike, as either a header
    // (X-Mqtt-Qos, X-Mqtt-Retain) or a query parameter (?qos=, ?retain=) --
    // the header wins if a request somehow sends both. ?qos=/X-Mqtt-Qos
    // overrides mqtt_qos for this one publish; ?retain=/X-Mqtt-Retain
    // overrides the default (events are unretained unless asked otherwise,
    // see README, "events vs. current state"). Neither is available on PUT
    // /metric -- see README, "Current state / metrics" for why state writes
    // keep retain fixed at true.
    int qos = cfg_.mqtt_qos;
    std::string qos_value;
    bool qos_present = !req.qos_header.empty();
    if (qos_present)
    {
        qos_value = req.qos_header;
    }
    else
    {
        qos_present = find_query_param(req, "qos", qos_value);
    }
    if (qos_present)
    {
        long parsed = 0;
        if (!parse_int(qos_value, parsed) || parsed < 0 || parsed > 2)
        {
            return make_error_response(400, "invalid qos (expected 0, 1, or 2)", ResponseFormat::Json);
        }
        qos = static_cast<int>(parsed);
    }

    bool retain = false;
    std::string retain_value;
    bool retain_present = !req.retain_header.empty();
    if (retain_present)
    {
        retain_value = req.retain_header;
    }
    else
    {
        retain_present = find_query_param(req, "retain", retain_value);
    }
    if (retain_present)
    {
        if (!parse_bool(retain_value, retain))
        {
            return make_error_response(400, "invalid retain (expected true/false, 1/0, yes/no, or on/off)",
                                       ResponseFormat::Json);
        }
    }

    PublishOutcome outcome =
        mqtt_.publish(topic, payload, qos, retain, std::chrono::seconds(cfg_.mqtt_publish_timeout_seconds));
    return event_outcome_response(topic, payload.size(), qos, retain, outcome);
}

HttpResponse Server::handle_metric_write(const std::string &name, double value)
{
    state_.set(name, value);

    if (cfg_.state_enabled)
    {
        std::string err;
        if (!state_.save(cfg_.state_file, err))
        {
            log_error("failed to persist state file: " + err);
        }
    }

    // State has its own persistence and is already durable locally at this
    // point, so a slow/unreachable broker does not fail this request --
    // MQTT publish here is a best-effort mirror, reported back to the
    // caller rather than turned into an HTTP error. See README's "events
    // vs. current state" for why this differs from handle_event() above.
    PublishOutcome outcome = mqtt_.publish(name, format_double(value), cfg_.mqtt_qos, /*retain=*/true,
                                           std::chrono::seconds(cfg_.mqtt_publish_timeout_seconds));
    if (outcome != PublishOutcome::Ok)
    {
        log_warn("metric '" + name + "' stored locally but MQTT publish did not succeed");
    }

    HttpResponse r;
    r.status = 200;
    r.format = ResponseFormat::Json;
    r.body = "{\"name\":\"" + json_escape(name) + "\",\"value\":" + format_double(value) +
             ",\"mqtt_published\":" + (outcome == PublishOutcome::Ok ? "true" : "false") + "}";
    return r;
}

HttpResponse Server::handle_metric_delete(const std::string &name)
{
    bool existed = state_.remove(name);
    if (!existed)
    {
        return make_error_response(404, "not found", ResponseFormat::Json);
    }

    if (cfg_.state_enabled)
    {
        std::string err;
        if (!state_.save(cfg_.state_file, err))
        {
            log_error("failed to persist state file: " + err);
        }
    }

    // Clear the broker's retained message for this name too: an empty
    // retained publish is MQTT's own convention for "no retained message
    // here anymore" -- otherwise a new subscriber would still see the
    // value nshmqtt itself now considers deleted. Best-effort, same as
    // handle_metric_write() above: the local delete has already happened.
    PublishOutcome outcome = mqtt_.publish(name, "", cfg_.mqtt_qos, /*retain=*/true,
                                           std::chrono::seconds(cfg_.mqtt_publish_timeout_seconds));
    if (outcome != PublishOutcome::Ok)
    {
        log_warn("metric '" + name + "' deleted locally but clearing MQTT retained state did not succeed");
    }

    HttpResponse r;
    r.status = 200;
    r.format = ResponseFormat::Json;
    r.body = "{\"name\":\"" + json_escape(name) +
             "\",\"deleted\":true,\"mqtt_cleared\":" + (outcome == PublishOutcome::Ok ? "true" : "false") + "}";
    return r;
}

HttpResponse Server::handle_metric(const HttpRequest &req)
{
    static const std::string prefix = "/metric/";
    if (req.path.size() <= prefix.size() || req.path.compare(0, prefix.size(), prefix) != 0)
    {
        return make_error_response(400, "missing metric name", ResponseFormat::Json);
    }
    std::string name = req.path.substr(prefix.size());
    if (name.empty())
    {
        return make_error_response(400, "missing metric name", ResponseFormat::Json);
    }

    if (req.method == "DELETE")
    {
        return handle_metric_delete(name);
    }

    if (req.method == "PUT")
    {
        double value = 0.0;
        bool ok = content_type_is_json(req.content_type) ? extract_json_number_field(req.body, "value", value)
                                                         : parse_double(req.body, value);
        if (!ok)
        {
            return make_error_response(400, "invalid metric value", ResponseFormat::Json);
        }
        return handle_metric_write(name, value);
    }

    if (req.method == "GET" && cfg_.simple_get)
    {
        std::string value_text;
        if (!find_query_param(req, "value", value_text))
        {
            return make_error_response(400, "missing value parameter", ResponseFormat::Json);
        }
        double value = 0.0;
        if (!parse_double(value_text, value))
        {
            return make_error_response(400, "invalid metric value", ResponseFormat::Json);
        }
        return handle_metric_write(name, value);
    }

    HttpResponse r = make_error_response(405, "method not allowed", ResponseFormat::Json);
    r.headers.push_back({"Allow", cfg_.simple_get ? "PUT, DELETE, GET" : "PUT, DELETE"});
    return r;
}

HttpResponse Server::route(const HttpRequest &req)
{
    const std::string &path = req.path;

    // Every payload-bearing route -- events, individual metric read/write/
    // delete, and the bulk per-topic Prometheus view -- goes through
    // is_authorized() here, in one place, rather than each handler
    // remembering to check for itself. /health and the service-only
    // /metrics deliberately never reach this: neither carries a caller's
    // own data, so both stay reachable without a token (a container health
    // check or Prometheus's own scrape of service metrics has no way to
    // supply one) -- see README's "Security considerations".
    bool needs_auth = path == "/event" || path.rfind("/event/", 0) == 0 || path == "/metric" ||
                      path.rfind("/metric/", 0) == 0 || path == "/metrics-state";
    if (needs_auth && !is_authorized(req))
    {
        return make_error_response(401, "unauthorized", ResponseFormat::Json);
    }

    if (path == "/event" || path.rfind("/event/", 0) == 0)
    {
        return handle_event(req);
    }
    if (path == "/metric" || path.rfind("/metric/", 0) == 0)
    {
        return handle_metric(req);
    }
    if (path == "/metrics" || path == "/metrics-state")
    {
        if (req.method != "GET" && req.method != "HEAD")
        {
            HttpResponse r = make_error_response(405, "method not allowed", ResponseFormat::Json);
            r.headers.push_back({"Allow", "GET, HEAD"});
            return r;
        }
        if (!cfg_.prometheus_http)
        {
            return make_error_response(404, "not found", ResponseFormat::Json);
        }
        HttpResponse r;
        r.status = 200;
        r.format = ResponseFormat::Prometheus;
        if (path == "/metrics")
        {
            // Collisions are inherently a state-prefix concern (only
            // content metric names can collide with each other) -- the
            // count is reported here, in the service metrics, but
            // computed against prometheus_mqtt_state_prefix, not
            // prometheus_prefix.
            NormalizedMetrics normalized =
                normalize_all(cfg_.prometheus_mqtt_state_prefix, state_.snapshot(), cfg_.state_topic_prefix);
            r.body = render_prometheus_service_metrics(cfg_.prometheus_prefix, metrics_, NSHMQTT_VERSION,
                                                       uptime_seconds(), mqtt_.connected(), mqtt_.connections_active(),
                                                       cfg_.mqtt_pool_size, mqtt_.queue_depth(), state_.size(),
                                                       normalized.collisions.size(), webhook_.queue_depth());
        }
        else
        {
            NormalizedMetrics normalized =
                normalize_all(cfg_.prometheus_mqtt_state_prefix, state_.snapshot(), cfg_.state_topic_prefix);
            r.body = render_prometheus_state_metrics(normalized.metrics);
        }
        return r;
    }
    if (path == "/health")
    {
        if (req.method != "GET" && req.method != "HEAD")
        {
            HttpResponse r = make_error_response(405, "method not allowed", ResponseFormat::Json);
            r.headers.push_back({"Allow", "GET, HEAD"});
            return r;
        }
        HttpResponse r;
        r.status = 200;
        if (accept_wants_json(req.accept))
        {
            r.format = ResponseFormat::Json;
            r.body = render_health_json(NSHMQTT_VERSION, uptime_seconds(), mqtt_.connected(),
                                        mqtt_.connections_active(), cfg_.mqtt_pool_size, state_.size());
        }
        else
        {
            r.format = ResponseFormat::Text;
            r.body = std::string("status=ok\n") + "mqtt_connected=" + (mqtt_.connected() ? "true" : "false") + "\n" +
                     "mqtt_connections_active=" + std::to_string(mqtt_.connections_active()) + "/" +
                     std::to_string(cfg_.mqtt_pool_size) + "\n";
        }
        return r;
    }

    return make_error_response(404, "not found", ResponseFormat::Json);
}

void Server::handle_connection(int fd)
{
    constexpr int kTimeoutSeconds = 10;

    std::string raw;
    ReadResult rr = read_http_head(fd, cfg_.max_request_bytes, kTimeoutSeconds, raw);

    HttpResponse resp;

    switch (rr)
    {
    case ReadResult::Ok:
        break;
    case ReadResult::TooLarge:
        metrics_.record_request("");
        resp = make_error_response(400, "request headers too large", ResponseFormat::Json);
        metrics_.record_response(resp.status);
        write_all(fd, build_http_response(resp));
        close(fd);
        return;
    case ReadResult::ConnectionClosed:
    case ReadResult::Timeout:
    case ReadResult::IoError:
        close(fd);
        return;
    }

    HttpRequest req;
    std::string leftover_body;
    bool have_request = parse_http_request(raw, req, leftover_body);

    if (!have_request)
    {
        log_warn("rejected malformed HTTP request");
        metrics_.record_request("");
        resp = make_error_response(400, "malformed HTTP request", ResponseFormat::Json);
    }
    else
    {
        metrics_.record_request(req.path);

        if (req.chunked)
        {
            resp = make_error_response(411, "chunked request bodies are not supported", ResponseFormat::Json);
        }
        else if (req.content_length > 0 && static_cast<std::size_t>(req.content_length) > cfg_.max_body_bytes)
        {
            resp = make_error_response(413, "request body too large", ResponseFormat::Json);
        }
        else
        {
            if (req.content_length > 0)
            {
                req.body = std::move(leftover_body);
                std::size_t need = static_cast<std::size_t>(req.content_length);
                if (req.body.size() > need)
                {
                    req.body.resize(need);
                }
                if (req.body.size() < need)
                {
                    ReadResult br = read_http_body(fd, need - req.body.size(), kTimeoutSeconds, req.body);
                    if (br != ReadResult::Ok)
                    {
                        close(fd); // same treatment as a head-read failure -- no well-formed request to answer
                        return;
                    }
                }
            }

            resp = route(req);
        }
    }

    metrics_.record_response(resp.status);
    bool is_head = have_request && req.method == "HEAD";
    write_all(fd, build_http_response(resp, /*include_body=*/!is_head));
    close(fd);
}

void Server::write_prometheus_textfile()
{
    if (cfg_.prometheus_textfile.empty())
    {
        return;
    }

    NormalizedMetrics normalized =
        normalize_all(cfg_.prometheus_mqtt_state_prefix, state_.snapshot(), cfg_.state_topic_prefix);
    std::string text = render_prometheus_service_metrics(
                           cfg_.prometheus_prefix, metrics_, NSHMQTT_VERSION, uptime_seconds(), mqtt_.connected(),
                           mqtt_.connections_active(), cfg_.mqtt_pool_size, mqtt_.queue_depth(), state_.size(),
                           normalized.collisions.size(), webhook_.queue_depth()) +
                       render_prometheus_state_metrics(normalized.metrics);

    std::string tmp_path = cfg_.prometheus_textfile + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out)
        {
            log_warn("failed to open prometheus textfile for writing: " + tmp_path);
            return;
        }
        out << text;
        if (!out)
        {
            log_warn("failed to write prometheus textfile: " + tmp_path);
            return;
        }
    }

    if (std::rename(tmp_path.c_str(), cfg_.prometheus_textfile.c_str()) != 0)
    {
        log_warn("failed to rename prometheus textfile into place (" + tmp_path + " -> " + cfg_.prometheus_textfile +
                 "): " + errno_str());
    }
}

void Server::cleanup()
{
    log_info("shutting down");

    if (pool_)
    {
        pool_->shutdown();
        pool_.reset();
    }

    if (listen_fd_ >= 0)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    for (int fd : tcp_listen_fds_)
    {
        close(fd);
    }
    tcp_listen_fds_.clear();

    if (socket_created_)
    {
        if (unlink(cfg_.socket_path.c_str()) != 0 && errno != ENOENT)
        {
            log_warn("failed to remove socket " + cfg_.socket_path + ": " + errno_str());
        }
        else
        {
            log_info("removed socket " + cfg_.socket_path);
        }
        socket_created_ = false;
    }

    if (signal_fd_ >= 0)
    {
        close(signal_fd_);
        signal_fd_ = -1;
    }

    log_info("shutdown complete");
}

int Server::run()
{
    log_info("nshmqtt ready");

    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (int fd : tcp_listen_fds_)
    {
        fds.push_back({fd, POLLIN, 0});
    }
    const size_t signal_idx = fds.size();
    fds.push_back({signal_fd_, POLLIN, 0});

    auto next_textfile_write = std::chrono::steady_clock::now();

    bool running = true;
    while (running)
    {
        int poll_timeout_ms = -1;
        if (!cfg_.prometheus_textfile.empty())
        {
            auto remaining = next_textfile_write - std::chrono::steady_clock::now();
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            poll_timeout_ms = static_cast<int>(remaining_ms > 0 ? remaining_ms : 0);
        }

        int pr = poll(fds.data(), fds.size(), poll_timeout_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            log_error("poll() failed: " + errno_str());
            break;
        }

        for (size_t i = 0; i < signal_idx; ++i)
        {
            if (!(fds[i].revents & POLLIN))
            {
                continue;
            }

            for (;;)
            {
                int client_fd = accept4(fds[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (client_fd < 0)
                {
                    if (errno != EINTR && errno != EAGAIN)
                    {
                        log_warn("accept() failed: " + errno_str());
                    }
                    break;
                }
                pool_->submit(client_fd);
            }
        }

        if (fds[signal_idx].revents & POLLIN)
        {
            signalfd_siginfo si{};
            ssize_t n = read(signal_fd_, &si, sizeof(si));
            if (n == static_cast<ssize_t>(sizeof(si)))
            {
                if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                {
                    log_info(std::string("received signal ") + (si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT") +
                             ", shutting down");
                    running = false;
                }
            }
        }

        if (!cfg_.prometheus_textfile.empty() && std::chrono::steady_clock::now() >= next_textfile_write)
        {
            write_prometheus_textfile();
            next_textfile_write =
                std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.prometheus_textfile_interval_seconds);
        }
    }

    cleanup();
    return 0;
}

} // namespace nshmqtt
