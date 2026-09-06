// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Owns nshmqtt's MQTT connection(s) (Eclipse Paho's synchronous
// MQTTClient API -- see the class comment on MqttClient for why each
// individual connection is only ever touched by its own dedicated worker
// thread, never a pool of threads sharing it) and the small amount of
// queueing needed to keep each connection's own blocking/reconnect
// behavior off the HTTP worker threads (server.cpp) and off Paho's own
// callback thread.

#include <MQTTClient.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "metrics.h"

namespace nshmqtt
{

enum class PublishOutcome
{
    Ok,        // broker accepted it (QoS 0: handed to the socket; QoS 1/2: acknowledged)
    QueueFull, // the bounded queue to the MQTT worker(s) was already full -- broker isn't keeping up
    Timeout,   // queued, but no result within the caller's wait_timeout (may still complete later)
    Failed,    // an MQTT worker attempted it and Paho reported a definite failure
};

// Invoked (from a dedicated subscribe-processing thread -- never from
// Paho's own callback thread, and never from a publish worker thread) for
// each message arriving on a subscribed topic. `retained` is the MQTT
// retain flag the message actually arrived with (not whether nshmqtt's own
// state writes use retain -- see README's "MQTT subscriptions" for that
// asymmetry). One handler serves both subscribe_topics and webhook_topics
// (see config.h): Paho never tells a caller which subscription matched,
// so the handler itself checks the arrived topic against each feature's
// own list (mqtt_topic.h's topic_matches_any()) and acts independently on
// either, both, or neither.
using MessageHandler = std::function<void(const std::string &topic, const std::string &payload, bool retained)>;

// Owns a pool of `mqtt_pool_size` independent MQTT connections (Config's
// default is 1, i.e. today's original single-connection behavior) and
// serializes every call into a given connection's Paho handle through
// that connection's own dedicated worker thread. This is not a stylistic
// choice -- MQTTClient.h's own top-of-file documentation states plainly:
// "The MQTTClient API is not thread safe, whereas the MQTTAsync API is."
// A pool of threads all calling MQTTClient_publish concurrently on the
// *same* handle would be a real data race. What this class pools instead
// is whole connections: each one gets its own MQTTClient handle, owned
// exclusively by its own worker thread, so multiple publishes can
// genuinely run in parallel (on separate TCP connections/MQTT sessions)
// without ever touching a Paho handle from more than one thread.
//
// All connections pull from one shared bounded publish queue (whichever
// connection is idle and currently connected takes the next job -- see
// Connection::worker_loop()), so HTTP worker threads (server.cpp) don't
// need to know how many connections exist or pick one themselves.
//
// Subscribing is different: only connection index 0 ever subscribes,
// regardless of pool size. If every connection subscribed to the same
// topic filter, the broker would deliver each incoming message once per
// subscribing session -- N copies of every message for an N-connection
// pool, corrupting whatever the message handler does with it (e.g.
// StateStore::set() would just see redundant, harmless-but-wasteful
// repeats for a plain value, but a future stateful consumer could not
// assume "one message in = one update"). Keeping subscription on exactly
// one connection avoids that by construction rather than by deduplicating
// after the fact.
//
// A second, independent thread (subscribe_worker_loop()) drains messages
// arriving via that one subscription. It exists separately from the
// publish workers because Paho's message-arrived callback must stay fast
// and non-blocking (see MQTTClient_setCallbacks' own documentation, and
// the general Paho callback advice this follows: copy the data, enqueue
// it, return immediately) -- if it shared a publish worker's thread, a
// slow publish() in flight could delay MQTT-level PINGREQ/keepalive
// handling closely enough to risk a spurious disconnect.
class MqttClient
{
public:
    MqttClient(Config cfg, Metrics &metrics, MessageHandler on_message);
    ~MqttClient();

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    // Creates every connection's Paho client and starts its worker
    // thread(s). Each connection's first connect attempt happens
    // asynchronously on its own worker thread -- start() itself does not
    // block waiting for the broker, so a broker that's down at startup
    // does not delay nshmqtt's own readiness.
    // Returns false only for a setup failure that retrying can't fix
    // (e.g. MQTTClient_create() rejecting a connection's client_id).
    bool start(std::string &err);

    // Stops every connection's worker thread(s) and disconnects cleanly.
    // Returns quickly even if the broker is currently unreachable.
    void stop();

    // Enqueues a publish job onto the shared queue and blocks the calling
    // thread until whichever connection picks it up reports an outcome,
    // or `wait_timeout` elapses. A full queue returns QueueFull
    // immediately without blocking at all.
    PublishOutcome publish(const std::string &topic, const std::string &payload, int qos, bool retain,
                           std::chrono::milliseconds wait_timeout);

    // True if at least one connection in the pool is currently connected
    // -- what /health and GET /metrics report as overall MQTT
    // connectivity. See connections_active() for the per-connection count.
    bool connected() const;

    // How many of the pool's connections are currently connected.
    int connections_active() const;

    int queue_depth() const;

private:
    struct Job
    {
        std::string topic;
        std::string payload;
        int qos = 0;
        bool retain = false;

        std::mutex done_mutex;
        std::condition_variable done_cv;
        bool done = false;
        PublishOutcome outcome = PublishOutcome::Failed;
    };

    struct IncomingMessage
    {
        std::string topic;
        std::string payload;
        bool retained = false;
    };

    // One physical MQTT connection: its own Paho handle, its own worker
    // thread, its own connect/reconnect state. Pulls publish jobs from
    // the owning MqttClient's shared queue whenever it's connected and
    // idle; only the primary connection (index 0) subscribes and feeds
    // the owning MqttClient's shared incoming-message queue.
    class Connection
    {
    public:
        Connection(MqttClient &owner, int index, bool is_primary);

        bool start(std::string &err);
        void stop();

        bool connected() const
        {
            return connected_.load(std::memory_order_relaxed);
        }

    private:
        void worker_loop();
        bool try_connect();
        void run_job(Job &job);

        static void on_connection_lost(void *context, char *cause);
        static int on_message_arrived(void *context, char *topic_name, int topic_len, MQTTClient_message *message);

        MqttClient &owner_;
        int index_;
        bool is_primary_;

        MQTTClient client_ = nullptr;
        std::thread worker_;
        std::atomic_bool connected_{false};
    };

    void subscribe_worker_loop();

    Config cfg_;
    Metrics &metrics_;
    MessageHandler on_message_;

    std::atomic_bool running_{false};
    std::vector<std::unique_ptr<Connection>> connections_;

    // Shared across every connection -- see the class comment for why one
    // queue (not one per connection) is the right shape here.
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<Job>> queue_;

    std::thread subscribe_worker_;
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;
    std::deque<IncomingMessage> incoming_queue_;
};

} // namespace nshmqtt
