// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Builds the JSON envelope webhook.cpp POSTs to webhook_url. Kept as a
// separate, pure (no I/O, no threading) module from webhook.cpp itself so
// its output shape is directly unit-testable without pulling in libcurl or
// the queue/worker-thread machinery around it.

#include <cstdint>
#include <string>

namespace nshmqtt
{

// Renders one forwarded MQTT message as a JSON object:
// {"topic":"<topic>","payload":"<payload>","retain":<bool>,"timestamp":<epoch seconds>}
// `payload` is always emitted as a JSON string field, whatever the original
// MQTT payload looked like (number, JSON, plain text, or arbitrary bytes)
// -- a uniform shape regardless of what was published, and one that can
// never itself produce invalid JSON no matter what bytes came in over MQTT
// (see json_escape()). `timestamp_epoch_seconds` is whole seconds, not
// fractional -- sub-second precision isn't meaningful for this purpose.
std::string build_webhook_json(const std::string &topic, const std::string &payload, bool retained,
                               std::int64_t timestamp_epoch_seconds);

} // namespace nshmqtt
