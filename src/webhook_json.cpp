// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "webhook_json.h"

#include "text_util.h"

namespace nshmqtt
{

std::string build_webhook_json(const std::string &topic, const std::string &payload, bool retained,
                               std::int64_t timestamp_epoch_seconds)
{
    std::string out;
    out.reserve(topic.size() + payload.size() + 48);
    out += "{\"topic\":\"";
    out += json_escape(topic);
    out += "\",\"payload\":\"";
    out += json_escape(payload);
    out += "\",\"retain\":";
    out += retained ? "true" : "false";
    out += ",\"timestamp\":";
    out += std::to_string(timestamp_epoch_seconds);
    out += "}";
    return out;
}

} // namespace nshmqtt
