// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Small text-sanitization helpers shared by the JSON body and header
// writers. Topic names and payload text come from HTTP clients and MQTT
// peers -- both untrusted input -- and are sanitized before being echoed
// back into a response or a log line.

#include <string>

namespace nshmqtt
{

// Escapes a UTF-8 string for embedding inside a JSON string literal
// (quotes, backslashes, and C0 control characters).
std::string json_escape(const std::string &in);

// Strips CR, LF, and other control characters from a value before it is
// placed into an HTTP header or log line, so an MQTT topic/payload can
// never be used to inject a header or split a log entry.
std::string sanitize_header_value(const std::string &in);

// Formats a double with its natural precision (no trailing zero padding),
// e.g. 17.3 -> "17.3", not std::to_string's "17.300000". Shared by state
// persistence, Prometheus rendering, and MQTT state-topic payloads, so all
// three agree on one canonical text form for the same value.
std::string format_double(double v);

// Parses `text` as a double after trimming surrounding whitespace. Returns
// false if `text` isn't entirely a valid number once trimmed (no trailing
// garbage allowed) -- stricter than strtod() alone, which would silently
// accept "17.3garbage".
bool parse_double(const std::string &text, double &out);

// Parses `text` as a plain base-10 integer -- no trailing garbage allowed
// (stricter than strtol() alone, which would silently accept "2garbage").
bool parse_int(const std::string &text, long &out);

// Parses `text` as a bool: "1"/"true"/"yes"/"on" (case-insensitive) -> true,
// "0"/"false"/"no"/"off" -> false. Returns false (leaving `out` untouched)
// for anything else. Shared by config file/env parsing and HTTP query
// parameter parsing (e.g. ?retain=), so both agree on one set of spellings.
bool parse_bool(const std::string &text, bool &out);

// Compares two strings for exact equality in time that depends only on
// their lengths, never on where the first differing byte is -- an
// ordinary `a == b` short-circuits at the first mismatch, which leaks
// (via response timing) how many leading bytes of a guessed token were
// correct. Used for checking a caller-supplied token against a
// configured one (see server.cpp's auth check); not needed, and not
// used, anywhere comparison timing isn't a secrecy concern.
bool constant_time_equals(const std::string &a, const std::string &b);

} // namespace nshmqtt
