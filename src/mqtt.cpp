// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "mqtt.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace nshmqtt
{

MqttClient::MqttClient(Config cfg, Metrics &metrics, MessageHandler on_message)
    : cfg_(std::move(cfg)), metrics_(metrics), on_message_(std::move(on_message))
{}

MqttClient::~MqttClient()
{
    stop();
}

bool MqttClient::start(std::string &err)
{
    running_.store(true, std::memory_order_relaxed);

    for (int i = 0; i < cfg_.mqtt_pool_size; ++i)
    {
        auto conn = std::make_unique<Connection>(*this, i, /*is_primary=*/i == 0);
        std::string conn_err;
        if (!conn->start(conn_err))
        {
            err = "connection " + std::to_string(i) + ": " + conn_err;
            running_.store(false, std::memory_order_relaxed);
            for (auto &started : connections_)
            {
                started->stop();
            }
            connections_.clear();
            return false;
        }
        connections_.push_back(std::move(conn));
    }

    // Either feature alone (or both) needs the subscribe-processing
    // thread running -- see config.h's own comment on why these are two
    // independent things sharing one underlying subscription mechanism.
    bool needs_subscribe_worker = (cfg_.subscribe_enabled && !cfg_.subscribe_topics.empty()) ||
                                  (cfg_.webhook_enabled && !cfg_.webhook_topics.empty());
    if (needs_subscribe_worker)
    {
        subscribe_worker_ = std::thread(&MqttClient::subscribe_worker_loop, this);
    }

    log_info("MQTT pool started: " + std::to_string(cfg_.mqtt_pool_size) + " connection(s), client_id '" +
             cfg_.mqtt_client_id + "' (+'-N' suffix on connections beyond the first)");
    return true;
}

void MqttClient::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    queue_cv_.notify_all();
    incoming_cv_.notify_all();

    for (auto &conn : connections_)
    {
        conn->stop();
    }
    connections_.clear();

    if (subscribe_worker_.joinable())
    {
        subscribe_worker_.join();
    }

    // Any jobs still queued at this point never ran; wake their waiters
    // (still inside publish()'s wait_for, or already timed out and gone --
    // either way harmless to touch) with a definite Failed rather than
    // leaving them to time out on their own during shutdown.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (auto &job : queue_)
        {
            std::lock_guard<std::mutex> job_lock(job->done_mutex);
            job->done = true;
            job->outcome = PublishOutcome::Failed;
            job->done_cv.notify_all();
        }
        queue_.clear();
    }
}

bool MqttClient::connected() const
{
    for (const auto &conn : connections_)
    {
        if (conn->connected())
        {
            return true;
        }
    }
    return false;
}

int MqttClient::connections_active() const
{
    int n = 0;
    for (const auto &conn : connections_)
    {
        if (conn->connected())
        {
            ++n;
        }
    }
    return n;
}

int MqttClient::queue_depth() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(queue_mutex_));
    return static_cast<int>(queue_.size());
}

PublishOutcome MqttClient::publish(const std::string &topic, const std::string &payload, int qos, bool retain,
                                   std::chrono::milliseconds wait_timeout)
{
    auto job = std::make_shared<Job>();
    job->topic = topic;
    job->payload = payload;
    job->qos = qos;
    job->retain = retain;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (static_cast<int>(queue_.size()) >= cfg_.mqtt_queue_size)
        {
            metrics_.mqtt_queue_full_total.fetch_add(1, std::memory_order_relaxed);
            return PublishOutcome::QueueFull;
        }
        queue_.push_back(job);
    }
    queue_cv_.notify_all();

    std::unique_lock<std::mutex> lock(job->done_mutex);
    if (!job->done_cv.wait_for(lock, wait_timeout, [&job] { return job->done; }))
    {
        // The job may still be queued or in flight from some connection's
        // point of view and complete later -- its result is simply
        // discarded at that point. The caller only gets a definite
        // Timeout here so an HTTP request never blocks longer than
        // wait_timeout regardless of how long the broker side takes.
        return PublishOutcome::Timeout;
    }
    return job->outcome;
}

void MqttClient::subscribe_worker_loop()
{
    while (running_.load(std::memory_order_relaxed))
    {
        IncomingMessage msg;
        {
            std::unique_lock<std::mutex> lock(incoming_mutex_);
            incoming_cv_.wait(lock, [this] { return !running_.load() || !incoming_queue_.empty(); });
            if (incoming_queue_.empty())
            {
                if (!running_.load(std::memory_order_relaxed))
                {
                    break;
                }
                continue;
            }
            msg = std::move(incoming_queue_.front());
            incoming_queue_.pop_front();
        }

        if (on_message_)
        {
            on_message_(msg.topic, msg.payload, msg.retained);
        }
    }
}

// --- Connection ---------------------------------------------------------

MqttClient::Connection::Connection(MqttClient &owner, int index, bool is_primary)
    : owner_(owner), index_(index), is_primary_(is_primary)
{}

bool MqttClient::Connection::start(std::string &err)
{
    std::string client_id =
        (index_ == 0) ? owner_.cfg_.mqtt_client_id : (owner_.cfg_.mqtt_client_id + "-" + std::to_string(index_));
    std::string server_uri = "tcp://" + owner_.cfg_.mqtt_host + ":" + std::to_string(owner_.cfg_.mqtt_port);

    int rc = MQTTClient_create(&client_, server_uri.c_str(), client_id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        err =
            "MQTTClient_create(" + server_uri + ", client_id=" + client_id + ") failed (rc=" + std::to_string(rc) + ")";
        return false;
    }

    // messageArrived must be non-NULL for every connection -- Paho's own
    // MQTTClient_setCallbacks() rejects a NULL one outright
    // (MQTTCLIENT_NULL_PARAMETER), even for a connection that will never
    // subscribe to anything. Wiring on_message_arrived() up uniformly is
    // still safe: it only ever fires in response to an actual PUBLISH
    // delivery, and non-primary connections never call
    // MQTTClient_subscribe(), so there is nothing for the broker to
    // deliver to them in the first place.
    rc = MQTTClient_setCallbacks(client_, this, on_connection_lost, on_message_arrived, nullptr);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        err = "MQTTClient_setCallbacks failed (rc=" + std::to_string(rc) + ")";
        MQTTClient_destroy(&client_);
        client_ = nullptr;
        return false;
    }

    worker_ = std::thread(&Connection::worker_loop, this);
    return true;
}

void MqttClient::Connection::stop()
{
    owner_.queue_cv_.notify_all(); // wake this connection's worker_loop() out of its wait
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (client_ != nullptr)
    {
        MQTTClient_destroy(&client_);
        client_ = nullptr;
    }
    connected_.store(false, std::memory_order_relaxed);
}

bool MqttClient::Connection::try_connect()
{
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.keepAliveInterval = owner_.cfg_.mqtt_keepalive_seconds;
    conn_opts.cleansession = 1;
    conn_opts.connectTimeout = owner_.cfg_.mqtt_connect_timeout_seconds;
    if (!owner_.cfg_.mqtt_username.empty())
    {
        conn_opts.username = owner_.cfg_.mqtt_username.c_str();
        conn_opts.password = owner_.cfg_.mqtt_password.c_str();
    }

    int rc = MQTTClient_connect(client_, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        log_warn("MQTT connection " + std::to_string(index_) + " to " + owner_.cfg_.mqtt_host + ":" +
                 std::to_string(owner_.cfg_.mqtt_port) + " failed (rc=" + std::to_string(rc) + "), will retry");
        return false;
    }

    log_info("MQTT connection " + std::to_string(index_) + " connected to " + owner_.cfg_.mqtt_host + ":" +
             std::to_string(owner_.cfg_.mqtt_port));
    connected_.store(true, std::memory_order_relaxed);

    // subscribe_topics (current-state store) and webhook_topics (HTTP
    // forwarding) are independent config lists, so both are subscribed to
    // here if enabled -- the arrived-message handler (main.cpp) sorts out
    // afterwards which list(s) a given topic actually matches (see
    // mqtt.h's MessageHandler comment). If the two lists happen to overlap,
    // the broker may deliver an affected message more than once; that's a
    // documented, accepted edge case (see README's "MQTT subscriptions"),
    // not a bug nshmqtt tries to prevent.
    if (is_primary_)
    {
        if (owner_.cfg_.subscribe_enabled)
        {
            for (const auto &topic : owner_.cfg_.subscribe_topics)
            {
                int src = MQTTClient_subscribe(client_, topic.c_str(), owner_.cfg_.mqtt_qos);
                if (src != MQTTCLIENT_SUCCESS)
                {
                    log_error("MQTT subscribe to '" + topic + "' failed (rc=" + std::to_string(src) + ")");
                }
                else
                {
                    log_info("MQTT subscribed to '" + topic + "' (qos=" + std::to_string(owner_.cfg_.mqtt_qos) + ")");
                }
            }
        }
        if (owner_.cfg_.webhook_enabled)
        {
            for (const auto &topic : owner_.cfg_.webhook_topics)
            {
                int src = MQTTClient_subscribe(client_, topic.c_str(), owner_.cfg_.mqtt_qos);
                if (src != MQTTCLIENT_SUCCESS)
                {
                    log_error("MQTT subscribe (webhook) to '" + topic + "' failed (rc=" + std::to_string(src) + ")");
                }
                else
                {
                    log_info("MQTT subscribed (webhook) to '" + topic +
                             "' (qos=" + std::to_string(owner_.cfg_.mqtt_qos) + ")");
                }
            }
        }
    }

    return true;
}

void MqttClient::Connection::worker_loop()
{
    int backoff_seconds = 1;
    constexpr int kMaxBackoffSeconds = 30;

    while (owner_.running_.load(std::memory_order_relaxed))
    {
        if (!connected_.load(std::memory_order_relaxed))
        {
            if (try_connect())
            {
                backoff_seconds = 1;
                continue;
            }

            owner_.metrics_.mqtt_reconnects_total.fetch_add(1, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(owner_.queue_mutex_);
            owner_.queue_cv_.wait_for(lock, std::chrono::seconds(backoff_seconds),
                                      [this] { return !owner_.running_.load(); });
            backoff_seconds = std::min(backoff_seconds * 2, kMaxBackoffSeconds);
            continue;
        }

        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(owner_.queue_mutex_);
            // A short, bounded wait rather than an unbounded one: this is
            // also the loop's only chance to notice connected_ having
            // flipped to false (set by on_connection_lost() or a failed
            // publish in run_job()) and start reconnecting promptly even
            // when no new job has arrived to wake it.
            owner_.queue_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !owner_.running_.load() || !owner_.queue_.empty() || !connected_.load();
            });
            if (!owner_.running_.load(std::memory_order_relaxed))
            {
                break;
            }
            if (owner_.queue_.empty())
            {
                continue;
            }
            job = owner_.queue_.front();
            owner_.queue_.pop_front();
        }

        run_job(*job);
    }

    if (connected_.load(std::memory_order_relaxed))
    {
        MQTTClient_disconnect(client_, 1000);
        connected_.store(false, std::memory_order_relaxed);
    }
}

void MqttClient::Connection::run_job(Job &job)
{
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = const_cast<char *>(job.payload.data());
    pubmsg.payloadlen = static_cast<int>(job.payload.size());
    pubmsg.qos = job.qos;
    pubmsg.retained = job.retain ? 1 : 0;

    MQTTClient_deliveryToken token = 0;
    int rc = MQTTClient_publishMessage(client_, job.topic.c_str(), &pubmsg, &token);

    PublishOutcome outcome;
    if (rc != MQTTCLIENT_SUCCESS)
    {
        outcome = PublishOutcome::Failed;
        owner_.metrics_.mqtt_publish_failed_total.fetch_add(1, std::memory_order_relaxed);
        log_warn("MQTT publish (connection " + std::to_string(index_) + ") to '" + job.topic +
                 "' failed (rc=" + std::to_string(rc) + ")");
        // A publish call failing outright (not just a slow ack) is a
        // strong signal this connection is no longer good even if Paho
        // hasn't invoked on_connection_lost() yet -- treat it as
        // disconnected so the loop above reconnects instead of repeatedly
        // failing publishes against a half-dead socket.
        connected_.store(false, std::memory_order_relaxed);
    }
    else if (job.qos == 0)
    {
        // No acknowledgement to wait for at QoS 0 -- publishMessage()
        // succeeding means it was handed off to the OS socket buffer.
        outcome = PublishOutcome::Ok;
        owner_.metrics_.mqtt_publish_ok_total.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        int wrc = MQTTClient_waitForCompletion(
            client_, token, static_cast<unsigned long>(owner_.cfg_.mqtt_publish_timeout_seconds) * 1000UL);
        if (wrc == MQTTCLIENT_SUCCESS)
        {
            outcome = PublishOutcome::Ok;
            owner_.metrics_.mqtt_publish_ok_total.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            outcome = PublishOutcome::Timeout;
            owner_.metrics_.mqtt_publish_timeout_total.fetch_add(1, std::memory_order_relaxed);
            log_warn("MQTT publish (connection " + std::to_string(index_) + ") to '" + job.topic +
                     "' did not complete within " + std::to_string(owner_.cfg_.mqtt_publish_timeout_seconds) +
                     "s (rc=" + std::to_string(wrc) + ")");
            // Same reasoning as the outright-failure branch above: a
            // publish that never completes within the configured timeout
            // means this connection can no longer be trusted, wedged
            // broker or not -- reconnect rather than keep pulling more
            // work into a connection that may not actually be delivering
            // it.
            connected_.store(false, std::memory_order_relaxed);
        }
    }

    {
        std::lock_guard<std::mutex> lock(job.done_mutex);
        job.done = true;
        job.outcome = outcome;
    }
    job.done_cv.notify_all();
}

void MqttClient::Connection::on_connection_lost(void *context, char *cause)
{
    auto *self = static_cast<Connection *>(context);
    self->connected_.store(false, std::memory_order_relaxed);
    log_warn("MQTT connection " + std::to_string(self->index_) + " lost" +
             (cause != nullptr ? (": " + std::string(cause)) : std::string()));
    // Wakes this connection's worker_loop() out of its poll tick
    // immediately instead of waiting up to a second to notice.
    self->owner_.queue_cv_.notify_all();
}

int MqttClient::Connection::on_message_arrived(void *context, char *topic_name, int topic_len,
                                               MQTTClient_message *message)
{
    auto *self = static_cast<Connection *>(context);

    // Per Paho's own callback guidance (and the class comment in mqtt.h):
    // copy the data out and return quickly, never do real processing here
    // -- this function runs on Paho's own receive thread, which also owns
    // MQTT protocol housekeeping (PINGREQ/keepalive) for this connection.
    std::string topic =
        (topic_len > 0) ? std::string(topic_name, static_cast<std::size_t>(topic_len)) : std::string(topic_name);
    std::string payload(static_cast<const char *>(message->payload), static_cast<std::size_t>(message->payloadlen));
    bool retained = message->retained != 0;

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);

    self->owner_.metrics_.mqtt_messages_received_total.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(self->owner_.incoming_mutex_);
        if (static_cast<int>(self->owner_.incoming_queue_.size()) >= self->owner_.cfg_.mqtt_queue_size)
        {
            self->owner_.metrics_.mqtt_messages_dropped_total.fetch_add(1, std::memory_order_relaxed);
            // Still return 1 (message considered delivered): dropping is
            // our own bounded-queue policy, not an MQTT-level delivery
            // failure Paho should retry.
            return 1;
        }
        self->owner_.incoming_queue_.push_back({std::move(topic), std::move(payload), retained});
    }
    self->owner_.incoming_cv_.notify_one();

    return 1;
}

} // namespace nshmqtt
