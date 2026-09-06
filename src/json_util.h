// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Deliberately minimal JSON reading -- nshmqtt's own design rule is not to
// interpret application JSON (see README, "preserve payloads"), so these
// functions are the only JSON *parsing* nshmqtt does at all, each scoped to
// one narrow, self-imposed shape rather than general JSON:
//
//  - extract_json_number_field(): pulls one named number out of a client's
//    small metric body, e.g. {"value": 17.3}.
//  - parse_flat_number_object(): reads back the flat name->number object
//    that StateStore itself writes to state.json -- not arbitrary JSON.
//  - flatten_json_object(): recursively flattens a (possibly nested) JSON
//    object into name->number leaves, for prometheus_mqtt_json_topics (see
//    config.h) -- the one function here that does descend into nested
//    objects, since that is the whole point of it.
//
// The first two are meant for small, well-formed, non-nested input,
// matching the "no JSON library" spirit of http.cpp's own hand-written
// parsing; flatten_json_object() reuses the same hand-written tokenizing
// rather than pulling one in for this either.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nshmqtt
{

// Extracts the numeric value of `field` from a JSON object text, e.g.
// {"value": 17.3} -> 17.3 for field="value". Looks for a top-level
// "<field>" key (first match) followed by ':' and a JSON number; returns
// false if the field is missing, not a bare number, or `text` doesn't
// start with '{'.
bool extract_json_number_field(const std::string &text, const std::string &field, double &out);

// Parses a flat JSON object of "name": number pairs, in the exact shape
// StateStore::save() writes (see state.h). Returns false on any structural
// problem (not an object, a nested object/array value, a non-numeric
// value, trailing data after the closing brace); `out` may be partially
// filled in that case and should be discarded by the caller.
bool parse_flat_number_object(const std::string &text, std::map<std::string, double> &out);

// Recursively flattens a JSON object into a flat list of (path, value)
// leaves, for prometheus_mqtt_json_topics (see config.h and README's
// "Prometheus support"). `out` is appended to, not cleared first, in
// object-order (not sorted) -- the caller decides what to do with
// duplicate names, same division of labor as normalize_all() elsewhere.
// Conversion rules, applied to each value found:
//   - a number             -> kept as-is
//   - a boolean             -> 1.0 (true) or 0.0 (false)
//   - a nested object       -> descended into; each of its own keys is
//                              joined to the parent key with '_', e.g.
//                              {"wifi":{"enabled":true}} yields the single
//                              leaf "wifi_enabled" -> 1.0
//   - a string, null, or array -> ignored entirely (not added to `out`)
// Returns false only for structurally malformed JSON (not an object to
// begin with, an unterminated string, unbalanced nesting, or a value that
// matches none of the shapes above) -- `out` may be partially filled in
// that case and should be discarded by the caller, same contract as
// parse_flat_number_object().
bool flatten_json_object(const std::string &text, std::vector<std::pair<std::string, double>> &out);

} // namespace nshmqtt
