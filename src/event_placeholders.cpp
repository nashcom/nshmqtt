// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "event_placeholders.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <random>

namespace nshmqtt
{

namespace
{

// Same approach as config.cpp's default_client_id(): std::random_device
// used directly, no PRNG in between, since this runs at most a handful of
// times per HTTP request, not on a hot path. `hex_chars` must be even (a
// whole number of bytes) -- true for every size this module actually
// calls it with (8/16/32/64).
std::string random_hex(int hex_chars)
{
    constexpr char kHexDigits[] = "0123456789abcdef";

    std::random_device rd;
    std::string out;
    out.reserve(static_cast<std::size_t>(hex_chars));
    for (int i = 0; i < hex_chars / 2; ++i)
    {
        unsigned int byte = rd() & 0xFFu;
        out += kHexDigits[(byte >> 4) & 0xFu];
        out += kHexDigits[byte & 0xFu];
    }
    return out;
}

// RFC 4122 UUIDv4: 16 random bytes, version nibble forced to 4, variant
// bits forced to 10xx, formatted as the canonical 8-4-4-4-12 hex string.
std::string random_uuid_v4()
{
    constexpr char kHexDigits[] = "0123456789abcdef";

    std::random_device rd;
    unsigned char bytes[16];
    for (unsigned char &b : bytes)
    {
        b = static_cast<unsigned char>(rd() & 0xFFu);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u); // variant 10xx

    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            out += '-';
        }
        out += kHexDigits[(bytes[i] >> 4) & 0xFu];
        out += kHexDigits[bytes[i] & 0xFu];
    }
    return out;
}

template <typename Duration>
std::string timestamp_string()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<Duration>(now).count());
}

// strftime()'s own return value distinguishes "truncated" from "failed" by
// magnitude, not by a clean success/failure split -- but every format
// string used here produces a small, fixed-length result well under
// buf's size, so a 0 return only means the (impossible, for these
// formats) truncation case; no separate error path is worth adding for
// it.
std::string utc_strftime(const char *format)
{
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);

    char buf[32];
    std::size_t n = std::strftime(buf, sizeof(buf), format, &tm_utc);
    return std::string(buf, n);
}

// Finds every occurrence of `token` in `text` and replaces each one with
// its own call to `generate()` -- never the same generated value reused
// across repeated occurrences of the same token. Advances by the
// replacement's own length, not the token's, so a longer or shorter
// generated value can never cause a skipped or re-scanned match.
void replace_all_with_fresh_values(std::string &text, const std::string &token,
                                    const std::function<std::string()> &generate)
{
    std::string::size_type pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos)
    {
        std::string value = generate();
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
}

} // namespace

std::string substitute_event_placeholders(const std::string &payload)
{
    std::string result = payload;

    replace_all_with_fresh_values(result, "NSHMQTT_RANDOM_HEX8", []() { return random_hex(8); });
    replace_all_with_fresh_values(result, "NSHMQTT_RANDOM_HEX16", []() { return random_hex(16); });
    replace_all_with_fresh_values(result, "NSHMQTT_RANDOM_HEX32", []() { return random_hex(32); });
    replace_all_with_fresh_values(result, "NSHMQTT_RANDOM_HEX64", []() { return random_hex(64); });
    replace_all_with_fresh_values(result, "NSHMQTT_RANDOM_UUID", []() { return random_uuid_v4(); });

    replace_all_with_fresh_values(result, "NSHMQTT_TIMESTAMP_S", []() { return timestamp_string<std::chrono::seconds>(); });
    replace_all_with_fresh_values(result, "NSHMQTT_TIMESTAMP_MS",
                                   []() { return timestamp_string<std::chrono::milliseconds>(); });
    replace_all_with_fresh_values(result, "NSHMQTT_TIMESTAMP_US",
                                   []() { return timestamp_string<std::chrono::microseconds>(); });
    replace_all_with_fresh_values(result, "NSHMQTT_TIMESTAMP_NS",
                                   []() { return timestamp_string<std::chrono::nanoseconds>(); });

    replace_all_with_fresh_values(result, "NSHMQTT_DATETIME_UTC", []() { return utc_strftime("%Y-%m-%dT%H:%M:%SZ"); });
    replace_all_with_fresh_values(result, "NSHMQTT_DATE_UTC", []() { return utc_strftime("%Y-%m-%d"); });
    replace_all_with_fresh_values(result, "NSHMQTT_TIME_UTC", []() { return utc_strftime("%H:%M:%S"); });

    return result;
}

} // namespace nshmqtt
