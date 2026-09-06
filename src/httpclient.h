// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A minimal, encapsulated wrapper around libcurl's easy API for making one
// outbound HTTP(S) POST request. Used by webhook.cpp to deliver forwarded
// MQTT messages; kept generic (no webhook-specific knowledge) so it's a
// small, independently reviewable unit.
//
// Requires curl_global_init() to have been called once at process start
// (see main.cpp) and curl_global_cleanup() at shutdown -- neither is
// thread-safe and both are explicitly the caller's responsibility, per
// libcurl's own documentation. post() itself is safe to call from any
// thread: it creates and destroys its own libcurl "easy" handle per call
// rather than sharing one, so there is no state to synchronize across
// threads (a libcurl easy handle itself is not safe to share concurrently).

#include <string>
#include <utility>
#include <vector>

namespace nshmqtt
{

struct HttpResult
{
    bool ok = false;      // true only for a completed request with a 2xx status
    long status_code = 0; // 0 if the request never completed at all (connect/timeout/TLS failure)
    std::string error;    // libcurl's own error string; empty when ok
};

class HttpClient
{
public:
    // Posts `body` to `url` with `headers` added verbatim (each pair
    // rendered as "Name: Value"), plus a fixed Content-Type of
    // application/json. `timeout_seconds` bounds the whole request
    // (connect + transfer). `tls_insecure` skips certificate verification
    // for an https:// url -- local/self-signed testing only.
    HttpResult post(const std::string &url, const std::string &body,
                    const std::vector<std::pair<std::string, std::string>> &headers, int timeout_seconds,
                    bool tls_insecure);
};

} // namespace nshmqtt
