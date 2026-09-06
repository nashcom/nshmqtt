// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "webhook.h"

#include <chrono>

#include "httpclient.h"
#include "log.h"
#include "webhook_json.h"

namespace nshmqtt
{

WebhookClient::WebhookClient(Config cfg, Metrics &metrics) : cfg_(std::move(cfg)), metrics_(metrics) {}

WebhookClient::~WebhookClient()
{
    stop();
}

bool WebhookClient::start(std::string &err)
{
    (void)err; // nothing that can fail up front -- kept for interface symmetry with MqttClient::start()
    if (!cfg_.webhook_enabled)
    {
        return true;
    }
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread(&WebhookClient::worker_loop, this);
    log_info("webhook delivery started, POSTing to " + cfg_.webhook_url);
    return true;
}

void WebhookClient::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }
    queue_cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void WebhookClient::enqueue(const std::string &topic, const std::string &payload, bool retained)
{
    if (!running_.load(std::memory_order_relaxed))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (static_cast<int>(queue_.size()) >= cfg_.webhook_queue_size)
        {
            metrics_.webhook_queue_full_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back({topic, payload, retained});
    }
    queue_cv_.notify_one();
}

int WebhookClient::queue_depth() const
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return static_cast<int>(queue_.size());
}

void WebhookClient::worker_loop()
{
    HttpClient http;
    while (running_.load(std::memory_order_relaxed))
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
            if (queue_.empty())
            {
                if (!running_.load(std::memory_order_relaxed))
                {
                    break;
                }
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        auto now = std::chrono::system_clock::now();
        auto epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        std::string body = build_webhook_json(job.topic, job.payload, job.retained, epoch_seconds);

        std::vector<std::pair<std::string, std::string>> headers;
        if (!cfg_.webhook_auth_header.empty())
        {
            headers.emplace_back(cfg_.webhook_auth_header, cfg_.webhook_auth_value);
        }

        HttpResult result =
            http.post(cfg_.webhook_url, body, headers, cfg_.webhook_timeout_seconds, cfg_.webhook_tls_insecure);
        if (result.ok)
        {
            metrics_.webhook_delivered_total.fetch_add(1, std::memory_order_relaxed);
            log_debug("webhook delivered '" + job.topic + "' (status " + std::to_string(result.status_code) + ")");
        }
        else
        {
            metrics_.webhook_failed_total.fetch_add(1, std::memory_order_relaxed);
            log_warn("webhook delivery for '" + job.topic + "' failed: " + result.error);
        }
    }
}

} // namespace nshmqtt
