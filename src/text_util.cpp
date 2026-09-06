// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "text_util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace nshmqtt
{

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);

    for (unsigned char c : in)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }

    return out;
}

std::string sanitize_header_value(const std::string &in)
{
    std::string out;
    out.reserve(in.size());

    for (unsigned char c : in)
    {
        // Reject CR, LF, and other control/DEL bytes outright; a plain
        // space (0x20) is the only "whitespace-like" byte allowed through.
        if (c == '\r' || c == '\n' || (c < 0x20) || c == 0x7f)
        {
            continue;
        }
        out += static_cast<char>(c);
    }

    return out;
}

std::string format_double(double v)
{
    std::ostringstream oss;
    oss << std::setprecision(9) << v;
    return oss.str();
}

namespace
{
std::string trim_ws(const std::string &s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
    {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    {
        --end;
    }
    return s.substr(start, end - start);
}
} // namespace

bool parse_double(const std::string &text, double &out)
{
    std::string trimmed = trim_ws(text);
    if (trimmed.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    double v = std::strtod(trimmed.c_str(), &end);
    if (errno != 0 || end == trimmed.c_str() || *end != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

bool parse_int(const std::string &text, long &out)
{
    std::string trimmed = trim_ws(text);
    if (trimmed.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long v = std::strtol(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str() || *end != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

bool parse_bool(const std::string &text, bool &out)
{
    std::string v = trim_ws(text);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

bool constant_time_equals(const std::string &a, const std::string &b)
{
    // A length mismatch is itself observable (nothing to hide there --
    // the configured token's length isn't secret), but every byte of the
    // shorter string is still compared against something so the loop
    // count/timing doesn't vary with the input length either.
    std::size_t len = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (std::size_t i = 0; i < len; ++i)
    {
        unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

} // namespace nshmqtt
