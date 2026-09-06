// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "state.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "json_util.h"
#include "text_util.h"

namespace nshmqtt
{

void StateStore::set(const std::string &name, double value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    values_[name] = value;
}

bool StateStore::remove(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return values_.erase(name) > 0;
}

bool StateStore::get(const std::string &name, double &out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(name);
    if (it == values_.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

std::vector<std::pair<std::string, double>> StateStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::pair<std::string, double>>(values_.begin(), values_.end());
}

std::size_t StateStore::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return values_.size();
}

bool StateStore::load(const std::string &path, std::string &err)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return true; // no state file yet -- not an error, just a fresh start
    }

    std::ostringstream buf;
    buf << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        err = "failed to read state file: " + path;
        return false;
    }

    std::map<std::string, double> loaded;
    if (!parse_flat_number_object(buf.str(), loaded))
    {
        err = "state file is not a valid flat {\"name\": value, ...} object: " + path;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    values_ = std::move(loaded);
    return true;
}

bool StateStore::save(const std::string &path, std::string &err) const
{
    std::string text;
    {
        // Snapshot under the lock, then build/write the JSON text outside
        // it -- state persistence should never hold the same mutex a live
        // HTTP/MQTT update to the store would need.
        auto items = snapshot();
        std::ostringstream oss;
        oss << "{";
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
            {
                oss << ",";
            }
            oss << "\"" << json_escape(items[i].first) << "\":" << format_double(items[i].second);
        }
        oss << "}";
        text = oss.str();
    }

    std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            err = "failed to open state file for writing: " + tmp_path;
            return false;
        }
        out << text;
        out.flush();
        if (!out)
        {
            err = "failed to write state file: " + tmp_path;
            return false;
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0)
    {
        err = "failed to rename state file into place (" + tmp_path + " -> " + path + ")";
        return false;
    }
    return true;
}

} // namespace nshmqtt
