// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The current-state store: one numeric value per metric name (see README's
// event-vs-state distinction). Deliberately just name -> double -- no
// history, no metadata beyond the value itself, per the spec's "keep the
// initial representation deliberately small" -- and no MQTT/HTTP awareness
// of its own; server.cpp and mqtt.cpp both read and write through this one
// class so both input paths (HTTP PUT/GET and an incoming MQTT publish)
// converge on one current value per name, per README's architecture.

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nshmqtt
{

class StateStore
{
public:
    // Sets (or replaces) the current value for `name`.
    void set(const std::string &name, double value);

    // Removes `name`'s current value entirely -- distinct from never
    // having been set, per the spec's DELETE semantics. Returns true if it
    // existed.
    bool remove(const std::string &name);

    // Looks up the current value for `name`; returns false if unset.
    bool get(const std::string &name, double &out) const;

    // A stable, name-sorted snapshot of every current value -- used by
    // both the Prometheus renderer and save() so their output ordering is
    // deterministic between calls.
    std::vector<std::pair<std::string, double>> snapshot() const;

    std::size_t size() const;

    // Loads state.json (the flat {"name": value, ...} object -- see
    // json_util.h's parse_flat_number_object()) into this store, replacing
    // whatever was in it. A missing file is not an error (fresh start);
    // returns false only for a file that exists but fails to parse.
    bool load(const std::string &path, std::string &err);

    // Writes the current snapshot to `path` atomically: a temp file in the
    // same directory, flushed and closed, then rename()'d into place --
    // see server.cpp's write_metrics_file() in nshgeoip for the same
    // pattern this mirrors. A crash mid-write leaves the previous
    // state.json untouched.
    bool save(const std::string &path, std::string &err) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, double> values_;
};

} // namespace nshmqtt
