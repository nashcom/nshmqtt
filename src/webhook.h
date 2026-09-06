// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Delivers MQTT messages to an HTTP(S) webhook -- the HTTP-to-MQTT
// gateway's other direction. Structurally mirrors MqttClient's own
// publish path (mqtt.h): a bounded queue plus one dedicated worker thread,
// so a slow or unreachable webhook receiver can never block the MQTT
// subscribe callback that feeds it (that callback must stay fast and
// non-blocking, same constraint documented in mqtt.h). A full queue drops
// the new message and counts it -- fire-and-forget, no retry, same
// philosophy as an unreachable MQTT broker not blocking HTTP requests
// elsewhere in this project.
//
// Completely independent of MqttClient's own subscribe_enabled/
// subscribe_topics: this class knows nothing about MQTT at all, just
// receives (topic, payload, retained) tuples handed to it by whatever
// decided they matched webhook_topics (see main.cpp and mqtt_topic.h).

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "metrics.h"

namespace nshmqtt
{

class WebhookClient
{
public:
    WebhookClient(Config cfg, Metrics &metrics);
    ~WebhookClient();

    WebhookClient(const WebhookClient &) = delete;
    WebhookClient &operator=(const WebhookClient &) = delete;

    // Starts the worker thread if cfg.webhook_enabled; a no-op (returns
    // true, nothing running) otherwise. Never fails on its own -- unlike
    // MqttClient, there's no upfront connection to establish here, just a
    // thread to start.
    bool start(std::string &err);
    void stop();

    // Queues one message for delivery. Safe to call from any thread;
    // never blocks beyond the queue mutex. A no-op if the worker isn't
    // running (webhook_enabled=false) -- callers don't need to check that
    // themselves. Drops and counts (webhook_queue_full_total) if the
    // bounded queue is already full.
    void enqueue(const std::string &topic, const std::string &payload, bool retained);

    // Current queue depth, for the service metrics gauge.
    int queue_depth() const;

private:
    struct Job
    {
        std::string topic;
        std::string payload;
        bool retained;
    };

    void worker_loop();

    Config cfg_;
    Metrics &metrics_;

    std::atomic_bool running_{false};
    std::thread worker_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;
};

} // namespace nshmqtt
