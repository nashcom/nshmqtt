// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// MQTT topic-filter matching (`+`/`#` wildcards), independent of any
// broker/library -- pure string logic, no MQTT connection involved. Needed
// because Paho's message-arrived callback hands back only the concrete
// topic a message was published to, never which of a client's own
// subscriptions matched it (see mqtt.h). Two independent features
// (subscribe_topics for the current-state store, webhook_topics for HTTP
// forwarding) can each have their own topic list watching the same
// underlying MQTT session, so nshmqtt does this matching itself, once per
// arrived message, against each feature's own list.

#include <string>
#include <vector>

namespace nshmqtt
{

// True if `topic` (a concrete topic a message was published to, never
// containing wildcards itself) matches `filter` (an MQTT subscription
// filter, which may contain `+` and `#`). Follows the MQTT 3.1.1 spec:
// `+` matches exactly one topic level; `#` matches any number of
// remaining levels and is only valid as the filter's last level; a topic
// whose first level starts with `$` (e.g. broker-internal `$SYS/...`
// topics) never matches a filter whose first level is a wildcard (`+` or
// `#`), matching real broker behavior -- an explicit `$SYS/#` filter
// still matches those topics normally, only a bare leading wildcard is
// excluded.
bool topic_matches_filter(const std::string &topic, const std::string &filter);

// True if `topic` matches any filter in `filters` -- a plain OR over
// topic_matches_filter(), used to check "does this feature's topic list
// care about this arrived message" without needing to know which
// underlying subscription actually caused the broker to deliver it.
bool topic_matches_any(const std::string &topic, const std::vector<std::string> &filters);

} // namespace nshmqtt
