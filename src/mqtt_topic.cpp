// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "mqtt_topic.h"

namespace nshmqtt
{

namespace
{

// Splits on '/', preserving empty levels (e.g. "a//b" -> {"a", "", "b"})
// -- MQTT topics/filters treat an empty level as a real, matchable level,
// not something to collapse away.
std::vector<std::string> split_levels(const std::string &s)
{
    std::vector<std::string> levels;
    std::size_t start = 0;
    while (true)
    {
        std::size_t slash = s.find('/', start);
        if (slash == std::string::npos)
        {
            levels.push_back(s.substr(start));
            break;
        }
        levels.push_back(s.substr(start, slash - start));
        start = slash + 1;
    }
    return levels;
}

} // namespace

bool topic_matches_filter(const std::string &topic, const std::string &filter)
{
    if (filter.empty() || topic.empty())
    {
        return false;
    }

    std::vector<std::string> topic_levels = split_levels(topic);
    std::vector<std::string> filter_levels = split_levels(filter);

    // A topic starting with '$' (broker-internal, e.g. "$SYS/...") never
    // matches a filter whose first level is a bare wildcard -- an explicit
    // "$SYS/#" still matches those topics normally via the loop below,
    // this only excludes an unqualified leading "+"/"#".
    if (!topic_levels[0].empty() && topic_levels[0][0] == '$' && (filter_levels[0] == "+" || filter_levels[0] == "#"))
    {
        return false;
    }

    std::size_t ti = 0;
    std::size_t fi = 0;
    while (fi < filter_levels.size())
    {
        const std::string &flevel = filter_levels[fi];
        if (flevel == "#")
        {
            // Only valid as the filter's last level; matches every
            // remaining topic level, including zero of them.
            return fi == filter_levels.size() - 1;
        }
        if (ti >= topic_levels.size())
        {
            return false; // filter has more levels left than the topic has
        }
        if (flevel != "+" && flevel != topic_levels[ti])
        {
            return false;
        }
        ++ti;
        ++fi;
    }
    return ti == topic_levels.size(); // both exhausted at the same point
}

bool topic_matches_any(const std::string &topic, const std::vector<std::string> &filters)
{
    for (const auto &filter : filters)
    {
        if (topic_matches_filter(topic, filter))
        {
            return true;
        }
    }
    return false;
}

} // namespace nshmqtt
