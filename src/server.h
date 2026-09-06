// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// HTTP listener and request routing -- the AF_UNIX/TCP accept loop, worker
// pool, and signal handling here are carried over from nshgeoip's own
// server.h/.cpp near verbatim (see their comments for the reasoning that
// still applies unchanged); what's new for nshmqtt is the route table
// itself (/event, /metric, /metrics, /health instead of /lookup) and the
// StateStore/MqttClient it now routes into.

#include <sys/socket.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "http.h"
#include "metrics.h"
#include "mqtt.h"
#include "state.h"
#include "thread_pool.h"
#include "webhook.h"

namespace nshmqtt
{

class Server
{
public:
    Server(Config cfg, MqttClient &mqtt, WebhookClient &webhook, StateStore &state, Metrics &metrics);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Opens the listening socket(s). Returns false and fills `err` on
    // failure (nothing partially set up is left behind).
    bool init(std::string &err);

    // Runs the accept loop until SIGTERM/SIGINT. Returns a process exit
    // code (0 on clean shutdown).
    int run();

private:
    bool setup_signals(std::string &err);
    bool setup_socket(std::string &err);
    bool ensure_socket_directory(const std::string &socket_path, std::string &err);
    bool path_has_live_listener(const std::string &path) const;
    bool setup_tcp_listeners(std::string &err);
    bool bind_one_tcp(int family, const sockaddr *addr, socklen_t addrlen, std::string &err);
    void handle_connection(int fd);
    void cleanup();

    HttpResponse route(const HttpRequest &req);
    HttpResponse handle_event(const HttpRequest &req);
    HttpResponse handle_metric(const HttpRequest &req);
    HttpResponse handle_metric_write(const std::string &name, double value);
    HttpResponse handle_metric_delete(const std::string &name);
    HttpResponse event_outcome_response(const std::string &topic, std::size_t payload_bytes, int qos, bool retain,
                                        PublishOutcome outcome);

    // True if cfg_.http_auth_tokens is empty (no auth configured -- always
    // authorized) or req.auth_token exactly matches one of the configured
    // tokens (constant-time compare, see text_util.h). Callers apply this
    // only to payload-bearing routes -- see route()'s own comment for
    // which ones and why /health and /metrics are exempt.
    bool is_authorized(const HttpRequest &req) const;

    double uptime_seconds() const;
    // Writes the combined service + state Prometheus output (the same
    // content /metrics and /metrics-state serve, concatenated) to
    // cfg_.prometheus_textfile, atomically (temp file + rename()). No-op
    // if that path is empty. A local file has no HTTP-level auth to speak
    // of, so unlike the two endpoints this is never split.
    void write_prometheus_textfile();

    Config cfg_;
    MqttClient &mqtt_;
    WebhookClient &webhook_;
    StateStore &state_;
    Metrics &metrics_;
    std::unique_ptr<ThreadPool> pool_;
    std::chrono::steady_clock::time_point start_time_;

    int listen_fd_ = -1;
    std::vector<int> tcp_listen_fds_;
    int signal_fd_ = -1;
    bool socket_created_ = false;
};

} // namespace nshmqtt
